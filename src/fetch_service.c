#include "fetch_service.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/eventfd.h>

#include "geo_locate.h"
#include "wifi.h"
#include "sysutil.h"
#include "launcher.h"

/* Wakelock held across the fetch so the device can't suspend mid-request.
 * Matches powersave.c's FETCH_LOCK_NAME. */
#define FETCH_WAKE_LOCK "kobo_weather_fetch"

#define fslog(...) sysutil_log("FETCH ", true, __VA_ARGS__)

static void wake_lock_take(void)    { sysutil_write_sysfs("/sys/power/wake_lock",   FETCH_WAKE_LOCK); }
static void wake_lock_release(void) { sysutil_write_sysfs("/sys/power/wake_unlock", FETCH_WAKE_LOCK); }

/* Fresh IP-based geolocation into cfg->loc (+ api_url rebuild). Returns 0 on
 * success or when auto-location is off, -1 on lookup failure. Never downgrades:
 * a coarser result than what we already hold is discarded. */
static int apply_geolocation(Config *cfg, int provider) {
    if (!cfg->auto_location) return 0;
    Location loc;
    if (geo_locate(&loc) != 0) {
        fslog("GEO_LOCATE_FAILED");
        return -1;
    }
    if (loc.precision < cfg->loc.precision) {
        /* Coarser result — keep the existing location/api_url. */
        fslog("GEO_LOCATE kept place=%s (prec %d) over coarser result (prec %d)",
              cfg->loc.place, cfg->loc.precision, loc.precision);
        return 0;
    }
    cfg->loc = loc;  /* atomic: lat+lon+place+precision move together */
    cfg->api_url[0] = '\0';
    config_build_url(cfg);
    fslog("GEO_LOCATE place=%s lat=%.4f lon=%.4f prec=%d provider=%s",
          cfg->loc.place, cfg->loc.lat, cfg->loc.lon, cfg->loc.precision,
          (provider == PROVIDER_OPEN_METEO) ? "open_meteo" : "smhi");
    return 0;
}

/* Shared fetch pipeline, run on whichever thread calls it. on_stage/ctx may
 * differ from req->on_stage/on_stage_ctx (threaded path uses an eventfd trampoline). */
static void fetch_run(const FetchRequest *req, WifiStageFn on_stage, void *ctx,
                      FetchResult *out) {
    memset(out, 0, sizeof *out);

    /* Bounded association in kiosk mode (can't pin the loop); staged connect interactively. */
    int ws = (req->wpa_retries > 0)
           ? wifi_up_bounded(req->wpa_retries, on_stage, ctx)
           : wifi_connect_staged(on_stage, ctx);
    launcher_notify_wifi_resolved();
    out->wifi_stage = ws;
    if (ws == WIFI_STAGE_NEEDS_NICKEL) { out->status = FETCH_ERR_NICKEL; return; }
    if (ws != WIFI_STAGE_CONNECTED)    { out->status = FETCH_ERR_WIFI;   return; }

    /* Work on a private copy so the worker never writes the caller's shared cfg.
     * A resolved location goes out via out->resolved_loc for the caller to adopt. */
    Config local = *req->cfg;
    /* Only publish when geolocation actually runs (auto_location on). With it off
     * apply_geolocation() no-ops, so publishing would make the caller needlessly
     * rebuild api_url and clobber a user-set custom API_URL on a pinned config. */
    if (req->geolocate && local.auto_location) {
        if (apply_geolocation(&local, local.provider) != 0) {
            out->status = FETCH_ERR_GEO;
            return;
        }
        out->resolved_loc     = local.loc;
        out->has_resolved_loc = 1;
    }

    wake_lock_take();
    int rc = weather_fetch_provider(&local, &out->state,
                                    local.provider, (UnitSystem)req->units);
    wake_lock_release();
    out->rc     = rc;
    out->status = (rc == 0) ? FETCH_OK : FETCH_ERR_FETCH;
    /* Copy weather.c's error buffer on this same thread, right after the call
     * that wrote it; other threads read out->err instead. */
    if (rc != 0)
        snprintf(out->err, sizeof out->err, "%s", weather_last_error());
}

/* ---- threaded path ---------------------------------------------------- */

static void stage_trampoline(void *ctx, int stage) {
    FetchManager *fm = (FetchManager *)ctx;
    atomic_store(&fm->stage, stage);
    uint64_t one = 1;
    (void)!write(fm->status_fd, &one, sizeof one);
}

static void *worker(void *arg) {
    FetchManager *fm = (FetchManager *)arg;
    fetch_run(&fm->req, stage_trampoline, fm, &fm->result);
    uint64_t one = 1;
    (void)!write(fm->event_fd, &one, sizeof one);  /* fails only on overflow */
    return NULL;
}

int fetch_manager_init(FetchManager *fm) {
    memset(fm, 0, sizeof *fm);
    atomic_init(&fm->stage, 0);
    fm->event_fd  = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    fm->status_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (fm->event_fd < 0 || fm->status_fd < 0) {
        fetch_manager_cleanup(fm);
        return -1;
    }
    return 0;
}

int fetch_manager_start(FetchManager *fm, const FetchRequest *req) {
    if (fm->in_progress) return -1;          /* one in flight at a time */
    fm->req = *req;
    atomic_store(&fm->stage, 0);
    if (pthread_create(&fm->thread, NULL, worker, fm) != 0) {
        fslog("THREAD_CREATE_FAILED");
        return -1;
    }
    fm->in_progress = true;
    return 0;
}

void fetch_manager_complete(FetchManager *fm, FetchResult *out) {
    if (!fm->in_progress) return;
    pthread_join(fm->thread, NULL);
    /* Drain the completion signal so a stale readable event_fd can't re-trigger the loop. */
    uint64_t drain;
    while (read(fm->event_fd, &drain, sizeof drain) == (ssize_t)sizeof drain) { }
    if (out) *out = fm->result;
    fm->in_progress = false;
}

int fetch_manager_stage(const FetchManager *fm) {
    return atomic_load(&fm->stage);
}

void fetch_manager_cleanup(FetchManager *fm) {
    if (fm->event_fd  >= 0) { close(fm->event_fd);  fm->event_fd  = -1; }
    if (fm->status_fd >= 0) { close(fm->status_fd); fm->status_fd = -1; }
}

/* ---- synchronous path ------------------------------------------------- */

void fetch_blocking(const FetchRequest *req, FetchResult *out) {
    fetch_run(req, req->on_stage, req->on_stage_ctx, out);
}
