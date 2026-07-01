#include "powersave.h"
#include "sysutil.h"
#include "wifi.h"
#include "fetch_service.h"
#include "geo_locate.h"   /* GeoPrecision (kiosk re-geolocate gate) */
#include "ui.h"
#include "weather.h"
#include "cache.h"
#include "input.h"
#include "fb.h"          /* g_debug */
#include "timeutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strncasecmp */
#include <stdarg.h>
#include <unistd.h>
#include <time.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>

/* Set by main.c's signal handler; lets the loop break out on shutdown. */
extern volatile int g_running;

#define KIOSK_FLAG     "/tmp/kobo_weather_kiosk"
/* launcher.c's session-long lock, held across launch/restore. */
#define WAKELOCK_NAME  "kobo_weather"
/* This loop's per-cycle hold. Must be a distinct name from WAKELOCK_NAME:
 * sharing one name meant release-before-sleep dropped only one of two holds on
 * the same kernel object, so every suspend's first attempt was rejected. */
#define KIOSK_LOCK_NAME "kobo_weather_kiosk"
#define FETCH_LOCK_NAME "kobo_weather_fetch"   /* transient lock main.c uses during fetch */
#define WIFI_SETTLE_US 500000   /* let the WiFi NIC power down before suspend */
/* Cap WPA association in kiosk refresh (vs default 30 s) so a refresh that can't
 * associate doesn't pin the loop/power button; typical association is 5-10 s. */
#define KIOSK_WIFI_WPA_SECS 12
/* Redraw cadence: wake at most hourly to advance the displayed "now" hour from
 * cached data, independent of the (possibly longer) sleep_upd_min fetch interval. */
#define KIOSK_DISPLAY_TICK_MIN 60

/* Give up the kiosk cycle after this many consecutive refused suspends (e.g. a
 * charger permanently holding a wakeup source) rather than retrying forever.
 * ~2 minutes with the 1/2/4/8/16/32/60s backoff. */
#define MAX_SUSPEND_RETRIES 8

/* logging: stderr -> run.log via launcher.c. pserr always; psdbg only --debug. */

#define pserr(...) sysutil_log("PS ", true,  __VA_ARGS__)
#define psdbg(...) sysutil_log("PS ", false, __VA_ARGS__)

static int write_sysfs(const char *p, const char *v) { return sysutil_write_sysfs(p, v); }

static int read_int_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int v = -1;
    if (fscanf(f, "%d", &v) != 1) v = -1;
    fclose(f);
    return v;
}

/* Reads /sys/power/wakeup_count into buf. Returns 0 if the file is missing
 * (older kernel), so the caller can fall back to the old straight-to-suspend
 * path. */
static int read_wakeup_count(char *buf, size_t n) {
    FILE *f = fopen("/sys/power/wakeup_count", "r");
    if (!f) return 0;
    int ok = (fgets(buf, n, f) != NULL);
    fclose(f);
    if (ok) { char *nl = strchr(buf, '\n'); if (nl) *nl = '\0'; }
    return ok;
}

/* Release every active userspace wakelock by name. launcher.c holds
 * "kobo_weather" and main.c holds "kobo_weather_fetch" during fetches, so a
 * hardcoded unlock can miss them; reading the active list is name-independent.
 * (Driver wakeup sources aren't listed here and can't be released this way.) */
static void release_all_wakelocks(void) {
    char buf[1024];
    FILE *f = fopen("/sys/power/wake_lock", "r");
    if (!f) {
        pserr("wake_lock open failed errno=%d (%s)", errno, strerror(errno));
        return;
    }
    size_t len = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[len] = '\0';

    char *saveptr;
    char *lock = strtok_r(buf, " \t\n", &saveptr);
    if (!lock) { psdbg("no active wakelocks"); return; }
    for (; lock; lock = strtok_r(NULL, " \t\n", &saveptr)) {
        psdbg("releasing wakelock: %s", lock);
        write_sysfs("/sys/power/wake_unlock", lock);
    }
}

/* Wakelocks held by other processes (e.g. Nickel, which is only paused, not
 * killed) at kiosk entry. release_all_wakelocks() strips these before
 * suspending, so we snapshot them here and re-acquire them on exit — handing
 * the device's pre-kiosk power state back intact instead of silently
 * dropping foreign locks. */
#define MAX_SAVED_LOCKS 16
#define LOCK_NAME_MAX   64
static char g_saved_locks[MAX_SAVED_LOCKS][LOCK_NAME_MAX];
static int  g_n_saved_locks = 0;

static int is_own_lock(const char *name) {
    return !strcmp(name, WAKELOCK_NAME) || !strcmp(name, KIOSK_LOCK_NAME) ||
           !strcmp(name, FETCH_LOCK_NAME);
}

static void snapshot_foreign_wakelocks(void) {
    g_n_saved_locks = 0;
    char buf[1024];
    FILE *f = fopen("/sys/power/wake_lock", "r");
    if (!f) {
        pserr("wake_lock open failed errno=%d (%s)", errno, strerror(errno));
        return;
    }
    size_t len = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[len] = '\0';

    char *saveptr;
    for (char *lock = strtok_r(buf, " \t\n", &saveptr);
         lock && g_n_saved_locks < MAX_SAVED_LOCKS;
         lock = strtok_r(NULL, " \t\n", &saveptr)) {
        if (is_own_lock(lock)) continue;
        snprintf(g_saved_locks[g_n_saved_locks], LOCK_NAME_MAX, "%s", lock);
        psdbg("saving foreign wakelock: %s", g_saved_locks[g_n_saved_locks]);
        g_n_saved_locks++;
    }
    if (g_n_saved_locks == 0) psdbg("no foreign wakelocks to preserve");
}

static void restore_foreign_wakelocks(void) {
    for (int i = 0; i < g_n_saved_locks; i++) {
        psdbg("restoring foreign wakelock: %s", g_saved_locks[i]);
        write_sysfs("/sys/power/wake_lock", g_saved_locks[i]);
    }
}

/* Classify a button-driven kiosk wake by how long the device was asleep
 * since power_save_loop() armed ui->power.slept_at, arming the
 * WakeRefreshNeed override consumed on the next Settings/Hourly entry or
 * app close. WASH always wins over FLASH (a long sleep implies a medium
 * one), so this is a single source of truth for the threshold comparison —
 * both button-wake exit points below call it identically. */
static void classify_wake(UIState *ui, time_t woke) {
    int slept_min = (int)((woke - ui->power.slept_at) / 60);
    WakeRefreshNeed need =
        slept_min >= ui->persisted.wake_wash_min  ? WAKE_REFRESH_WASH  :
        slept_min >= ui->persisted.wake_flash_min ? WAKE_REFRESH_FLASH :
                                                    WAKE_REFRESH_CLEAN;
    /* Every kiosk button-wake gets at least CLEAN: a full-screen GC16 no-flash.
     * Suspend powers the EPDC down, losing the pixel history later REAGL updates
     * diff against, so a full GC16 re-establishes a correct baseline; longer
     * sleeps escalate. Awake nav stays NONE. Escalate only: a short re-sleep must
     * never downgrade a pending wash. */
    if (need > ui->power.wake_need) ui->power.wake_need = need;
    psdbg("button wake after %dmin asleep -> wake_need=%d", slept_min, ui->power.wake_need);
}

/* On a refused suspend, dump the kernel's reason so we can see why next time. */
static void dump_suspend_diag(void) {
    pserr("---- suspend diag (echo mem refused) ----");
    fflush(stderr);
    (void)!system("for p in state mem_sleep wake_lock wakeup_count; do "
           "printf 'DIAG: PS %s = ' \"$p\"; cat \"/sys/power/$p\" 2>&1; done 1>&2");
    (void)!system("dmesg 2>/dev/null | tail -25 1>&2");
    pserr("---- end suspend diag ----");
}

/* ---- frontlight --------------------------------------------------- */
/* Kobo exposes the frontlight under /sys/class/backlight/<dev>/brightness. */
static char g_bl_path[300] = "";   /* "/sys/class/backlight/<name>/brightness" */
static int  g_bl_saved     = -1;

static int bl_name_is_warm(const char *name) {
    for (const char *p = name; *p; p++)
        if (!strncasecmp(p, "warm", 4) || !strncasecmp(p, "amber", 5))
            return 1;
    return 0;
}

/* ComfortLight Kobos expose a warm channel beside the white one in arbitrary
 * readdir() order. We only fade/restore white, so take the first usable
 * (max_brightness > 0) non-warm device; fall back to the first usable one. */
static void backlight_detect(void) {
    DIR *d = opendir("/sys/class/backlight");
    if (!d) { pserr("no /sys/class/backlight errno=%d (%s)", errno, strerror(errno)); return; }
    char first[300] = "";
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char maxp[300];
        snprintf(maxp, sizeof maxp, "/sys/class/backlight/%s/max_brightness", e->d_name);
        if (read_int_file(maxp) <= 0) continue;
        if (!first[0])
            snprintf(first, sizeof first, "/sys/class/backlight/%s/brightness", e->d_name);
        if (!bl_name_is_warm(e->d_name)) {
            snprintf(g_bl_path, sizeof g_bl_path, "/sys/class/backlight/%s/brightness", e->d_name);
            break;
        }
    }
    closedir(d);
    if (!g_bl_path[0] && first[0])
        snprintf(g_bl_path, sizeof g_bl_path, "%s", first);
    if (g_bl_path[0]) psdbg("backlight = %s", g_bl_path);
    else              pserr("no backlight device found");
}

static void backlight_set(int v) {
    if (!g_bl_path[0]) return;
    FILE *f = fopen(g_bl_path, "w");
    if (!f) { pserr("backlight open %s failed errno=%d (%s)", g_bl_path, errno, strerror(errno)); return; }
    fprintf(f, "%d", v < 0 ? 0 : v);
    if (fclose(f) == EOF)
        pserr("backlight write %s failed errno=%d (%s)", g_bl_path, errno, strerror(errno));
}

/* Smoothly ramp the frontlight from its current level to off over ~1 second. */
static void backlight_fade_out(void) {
    if (!g_bl_path[0]) return;
    int cur = read_int_file(g_bl_path);
    if (cur < 0) cur = 0;
    g_bl_saved = cur;
    psdbg("frontlight fade %d -> 0", cur);
    const int steps = 20;            /* 20 * 50 ms = ~1 s */
    for (int i = steps; i >= 1; i--) {
        backlight_set(cur * (i - 1) / steps);
        usleep(50000);
    }
}

static void backlight_restore(void) {
    if (!g_bl_path[0] || g_bl_saved < 0) return;
    psdbg("frontlight restore -> %d", g_bl_saved);
    backlight_set(g_bl_saved);
}

/* ---- fetch + redraw the headerless frame -------------------------- */

/* Optionally fetch fresh weather, then redraw the panel. do_fetch decides only
 * whether the (blocking) WiFi+fetch step runs first; the redraw always happens
 * and advances the "now" hour from whatever is in ui->state (cached or freshly
 * fetched). No internal time logic — the caller owns the fetch-vs-redraw choice.
 * The fetch is silent: no "(Ansluter…)/(Hämtar…)" subtitle staging — the old
 * frame stays on the panel untouched until the fetch finishes.
 *
 * force_full_redraw distinguishes the two callers:
 *   - the entry call (power_save_loop, right after the screen has already
 *     been put into its kiosk layout by ui_enter_kiosk())
 *     passes 0: any changed data is picked up by draw_main_screen's normal
 *     partial-update path (only the changed icons/text repaint), so the
 *     header-only (or full-wash) update that already ran is never followed by
 *     a redundant second full-screen sweep.
 *   - the periodic wake-tick call (deep in the suspend loop, where nothing
 *     has been drawn yet since the panel went dark) passes 1: a single
 *     full-screen GC16 (no flash, see RSTYLE_GC16_NOFLASH) redraws everything
 *     in one sweep. */
static void refresh_frame(UIState *ui, Config *cfg, const char *cache_path,
                          int do_fetch, int force_full_redraw) {
    if (do_fetch) {
        /* Same shared pipeline as interactive mode (see fetch_service.c), but
         * synchronous, silent (no subtitle stages), with a bounded WPA wait so
         * a refresh that can't associate doesn't pin the kiosk loop — and,
         * unlike before, the WiFi status is actually checked before HTTP.
         * Kiosk re-geolocates only while the known place is still sub-city, to
         * pick up an IP change (e.g. a new ISP lease) that might now resolve to
         * a city; once city-level is reached this is false and kiosk reverts to
         * pure coordinate-reuse, so there's no steady-state extra HTTP roundtrip
         * per wake. apply_geolocation never downgrades, so a coarser re-attempt
         * can't worsen what's shown. The fetch publishes any resolved location in
         * res.resolved_loc; we adopt it here after the call. */
        FetchRequest req = {
            .cfg = cfg, .units = ui->persisted.units,
            .geolocate = (cfg->loc.precision < GEO_PRECISION_CITY),
            .wpa_retries = KIOSK_WIFI_WPA_SECS, .on_stage = NULL,
        };
        FetchResult res;
        fetch_blocking(&req, &res);
        if (res.has_resolved_loc) {
            cfg->loc = res.resolved_loc;
            cfg->api_url[0] = '\0';
            config_build_url(cfg);
        }
        if (res.status == FETCH_OK) {
            *ui->state = res.state;
            cache_save(cache_path, ui->state);
            psdbg("fetch ok (%d hours)", res.state.n_hours);
        } else {
            pserr("fetch failed: status=%d %s", res.status, res.err);
        }
    } else {
        psdbg("redraw-only tick (no fetch, WiFi stays off)");
    }

    /* Set the tick reason first, then let a due daily wash override it below —
     * the wash must win when both are true in the same call. */
    if (force_full_redraw)
        ui->refresh.full_redraw_reason = RR_TICK_WAKE;   /* single full-screen GC16-no-flash sweep */

    /* Daily 06:00 wash — applies whether or not we fetched, since the panel now
     * redraws hourly and the wash must still fire on a no-fetch tick. */
    time_t now_w = time(NULL);
    if (now_w >= ui->refresh.daily_wash_after) {
        ui->refresh.full_redraw_reason = RR_DAILY_WASH;
        ui->refresh.daily_wash_after = next_06am(now_w);
        psdbg("daily wash");
    }

    ui_draw(ui);
}

/* ---- public entry ------------------------------------------------- */

void power_save_mark_kiosk(void) {
    FILE *ff = fopen(KIOSK_FLAG, "w");   /* launcher.c fail-safe guard */
    if (ff) fclose(ff);
}

int power_save_loop(UIState *ui, Config *cfg, int *input_fd,
                    const char *cache_path) {
    backlight_detect();
    if (g_debug) fprintf(stderr, "\n");
    psdbg("ENTER kiosk");

    /* Enter the kiosk layout. ui_enter_kiosk() owns the entry-refresh policy:
     * from home it normally erases just the header band via partial_style
     * (weather content is already on the panel at identical positions); from a
     * non-home layout (hourly/settings), or when Entry full wash / sleep GC16
     * flash force one, it does a full-screen redraw instead (which also clears
     * any A2 ghosts left by hourly scrolling). Either way the screen has now
     * been drawn once; refresh_frame() below only adds a fetch and lets its
     * own (partial) redraw pick up whatever changed — it must not also force a
     * second full-screen sweep on top of what just ran. */
    ui_enter_kiosk(ui);
    /* Entry fetch is age-guarded: WiFi is still up from interactive mode, so
     * fetch unless the cached data is still within the current update bucket.
     * Entry time is arbitrary (not boundary-aligned), so this crossed_boundary
     * check is not exposed to the ~1 s early-RTC edge that affects wake ticks. */
    /* Track whether WiFi is currently up so we only tear it down when it's
     * actually associated — queried directly rather than inferred from
     * whether the entry fetch ran, since WiFi can already be associated from
     * interactive mode even when the cache is fresh enough to skip the fetch
     * (in which case wifi_down() must still run before the first suspend). */
    int wifi_is_up;
    {
        long entry_step = (long)ui->persisted.sleep_upd_min * 60;
        if (entry_step < 60) entry_step = 60;
        int entry_fetch = crossed_boundary(time(NULL), ui->state->fetched_at, entry_step);
        /* force_full_redraw=0: the screen is already in its kiosk layout
         * (header-only update, or full redraw if from_hourly/sleep_entry_wash
         * — see above); any data change from entry_fetch repaints through the
         * normal partial-update path instead of a redundant full sweep. */
        refresh_frame(ui, cfg, cache_path, entry_fetch, 0);
        wifi_is_up = wifi_is_associated();
    }

    /* Debug-only: capture exactly what the panel shows on sleep entry, since
     * the kiosk hidden-corner tap (ui_handle_touch) can't fire here — any
     * touch at this point wakes the device instead of reaching it. Fixed
     * filename: only the most recent sleep entry matters for debugging. */
    if (g_debug) {
        char shot[PATH_MAX];
        ui_dump_screenshot(sysutil_path(shot, sizeof shot, "screenshot_sleep.bmp"));
    }

    /* KIOSK_FLAG already set by power_save_mark_kiosk() at entry. */

    if (*input_fd >= 0) { input_close(*input_fd); *input_fd = -1; }
    /* Capture any foreign wakelocks before we start releasing them, so we can
     * hand them back on exit. */
    snapshot_foreign_wakelocks();
    /* Hold a lock so autosleep can't suspend us mid-setup; released per cycle
     * right before echo mem (alarm already armed) and re-taken on resume. */
    write_sysfs("/sys/power/wake_lock", KIOSK_LOCK_NAME);

    /* Marks the start of "vila" for the button-wake staleness override (see
     * classify_wake() below) — covers the whole kiosk session (every
     * RTC-tick re-suspend), not just the final suspend cycle. */
    ui->power.slept_at = time(NULL);

    /* Decouple the redraw cadence from the fetch cadence. Wake (and redraw) at
     * the display tick — hourly, or sleep_upd_min if that is shorter — but only
     * fetch over WiFi every fetch_every-th wake. All sleep_upd_min menu values
     * are exact multiples of the tick, so the division is exact (15/30/60→1,
     * 180→3, 360→6, 720→12). Settings are unreachable in kiosk mode, so this is
     * fixed for the lifetime of the loop. */
    long tick_min = (long)ui->persisted.sleep_upd_min;
    if (tick_min > KIOSK_DISPLAY_TICK_MIN) tick_min = KIOSK_DISPLAY_TICK_MIN;
    if (tick_min < 1) tick_min = 1;
    int fetch_every = (int)((long)ui->persisted.sleep_upd_min / tick_min);
    if (fetch_every < 1) fetch_every = 1;
    int ticks = 0;
    long step = tick_min * 60;       /* redraw tick, not the fetch interval */

    /* Arm the wake alarm before the frontlight fade rather than right before
     * echo mem, so the RTC IRQ it raises has the fade (plus the handshake
     * below) to settle instead of landing right on the first suspend attempt. */
    time_t next = next_local_boundary(time(NULL), step);
    write_sysfs("/sys/class/rtc/rtc0/wakealarm", "0");
    {
        char alarm[32];
        snprintf(alarm, sizeof alarm, "%ld", (long)next);
        write_sysfs("/sys/class/rtc/rtc0/wakealarm", alarm);
    }
    if (g_debug) {
        struct tm tmv;
        localtime_r(&next, &tmv);
        psdbg("suspend now=%ld alarm=%ld (%02d:%02d)",
              (long)time(NULL), (long)next, tmv.tm_hour, tmv.tm_min);
    }

    /* Last visible action: fade the frontlight out over ~1 s, then suspend. */
    backlight_fade_out();

    /* Watch the power-key node across suspends so we can positively identify a
     * button wake instead of relying only on the RTC/timing heuristic, which
     * misclassifies presses landing within ~1 s of a refresh boundary. Kept open
     * across the whole loop; -1 (no node / Nickel grabbed it) falls back cleanly
     * to the heuristic. */
    int pkey_fd = power_key_open();

    int backoff_s = 1;
    int suspend_retries = 0;
    int alarm_armed = 1;   /* the arming above already covers the first iteration */
    while (g_running) {
        /* Only tear WiFi down if a fetch actually raised it (the entry fetch on
         * the first iteration, or a fetch tick on a later one). Redraw-only
         * ticks never raise WiFi, so wifi_down()'s ~5 busybox shell-outs would
         * be pure waste/battery. */
        if (wifi_is_up) {
            wifi_down();
            usleep(WIFI_SETTLE_US);      /* let the NIC finish powering down */
            wifi_is_up = 0;
        }

        /* Re-arm at the top of the loop (skipped on the first pass, already
         * armed above) so a continue after backoff always re-arms with a
         * fresh next, avoiding a stale/past alarm. */
        if (!alarm_armed) {
            /* Wake at the next local-aligned tick boundary (e.g. the top of the
             * hour). The fetch-vs-redraw decision is made by counting ticks
             * below, not by re-checking the clock, so the ~1 s early-RTC fire
             * can't drop a scheduled fetch. */
            next = next_local_boundary(time(NULL), step);
            write_sysfs("/sys/class/rtc/rtc0/wakealarm", "0");
            char alarm[32];
            snprintf(alarm, sizeof alarm, "%ld", (long)next);
            write_sysfs("/sys/class/rtc/rtc0/wakealarm", alarm);

            if (g_debug) {
                struct tm tmv;
                localtime_r(&next, &tmv);
                psdbg("suspend now=%ld alarm=%ld (%02d:%02d)",
                      (long)time(NULL), (long)next, tmv.tm_hour, tmv.tm_min);
            }
        }
        alarm_armed = 0;

        /* Suspend-to-RAM (KOReader's proven Kobo sequence). echo mem blocks
         * until wake. The RTC alarm is armed above, so even if releasing the
         * wakelocks lets autosleep fire first, the device still wakes on time. */
        write_sysfs("/sys/power/state-extended", "1");
        sleep(2);
        sync();

        /* Release our own wakelocks before the handshake below: a held
         * wakelock is itself an active wakeup source, so wakeup_count can
         * never settle while we hold one. */
        release_all_wakelocks();

        /* wakeup_count handshake: read the count and write it back. The write
         * succeeds only if no wakeup event is pending/in-progress; EINVAL means
         * one is still settling (WiFi-teardown residue, touch controller
         * resuming), in which case echo mem would be refused (EPERM) anyway —
         * skip it and let the retry backoff until the source clears. Missing
         * file (older kernel) falls back to the old straight-through suspend. */
        int wc_ok = 1;
        char wc[32];
        if (read_wakeup_count(wc, sizeof wc))
            wc_ok = (write_sysfs("/sys/power/wakeup_count", wc) == 0);

        int suspended, attempted_mem = 0;
        if (!wc_ok) {
            psdbg("wakeup_count busy - wakeup pending, skipping echo mem");
            suspended = -1;
        } else {
            attempted_mem = 1;
            /* Final line before the panel goes dark. fflush so it reaches the
             * log file before the process freezes inside the blocking mem write. */
            psdbg("========SLEEP========");
            fflush(stderr);

            /* echo mem; blocks until wake. Inlined (not write_sysfs) so its success
             * isn't logged before the WAKE-UP marker — that marker must be the very
             * first line after resume. Errors are still logged, matching write_sysfs. */
            FILE *f = fopen("/sys/power/state", "w");
            if (!f) {
                pserr("sysfs open /sys/power/state failed errno=%d (%s)", errno, strerror(errno));
                suspended = -1;
            } else {
                int ok = (fputs("mem", f) != EOF);
                if (!ok)
                    pserr("sysfs write /sys/power/state failed errno=%d (%s)", errno, strerror(errno));
                if (fclose(f) == EOF) {
                    pserr("sysfs flush /sys/power/state failed errno=%d (%s)", errno, strerror(errno));
                    ok = 0;
                }
                suspended = ok ? 0 : -1;
            }
            if (g_debug) fprintf(stderr, "\n");  /* blank line separates this wake cycle from the sleep above */
            if (suspended == 0)
                psdbg("========WAKE-UP========");   /* first line after resume */
        }
        write_sysfs("/sys/power/state-extended", "0");
        write_sysfs("/sys/power/wake_lock", KIOSK_LOCK_NAME);  /* stay awake while working */
        usleep(100000);

        /* If the kernel refused to suspend (a held wakeup source, charger,
         * WiFi up/down residue, etc.), retry with exponential backoff. The
         * alarm is re-armed at the top of the next iteration with a fresh
         * `next`. After MAX_SUSPEND_RETRIES consecutive rejections, give up
         * and fall through to the normal exit-to-interactive path below. */
        if (suspended != 0) {
            /* Only dump the diag when echo mem was actually attempted; a
             * handshake abort already logged its own reason above. */
            if (g_debug && attempted_mem) dump_suspend_diag();
            pserr("suspend rejected - retry in %ds", backoff_s);
            /* WiFi was already torn down at the top of this iteration (or
             * never raised this cycle) — no need to repeat it here. */
            release_all_wakelocks();
            if (++suspend_retries >= MAX_SUSPEND_RETRIES) {
                pserr("suspend rejected %d times in a row - exiting kiosk", suspend_retries);
                break;
            }
            sleep(backoff_s);
            backoff_s = backoff_s < 32 ? backoff_s * 2 : 60;
            if (!g_running) break;
            continue;
        }
        backoff_s       = 1;   /* successful suspend; reset backoff */
        suspend_retries = 0;

        if (!g_running) break;

        time_t woke = time(NULL);
        /* Drained once — a buffered KEY_POWER read here is a definitive button
         * wake, trusted over the timing heuristic below (which misses presses
         * near a boundary). When the fd is unavailable this is always 0. */
        int key_pressed = power_key_drain_pressed(pkey_fd);

        /* Classify wake source using two independent signals:
         *   early       — woke before the alarm (time proximity)
         *   rtc_cleared — kernel cleared wakealarm on RTC fire; a button/
         *                 other wake leaves it pending with the armed epoch.
         * Both must agree to exit kiosk. On disagreement (spurious IRQ,
         * charger, RTC driver quirk) we re-suspend — biased toward staying
         * asleep because a false "stay" costs one extra button press while a
         * false "exit" reintroduces the bug. */
        int early       = (woke < next - 1);
        int rtc_cleared = (read_int_file("/sys/class/rtc/rtc0/wakealarm") <= 0);
        int by_button   = key_pressed || (early && !rtc_cleared);
        psdbg("woke now=%ld early=%d rtc_cleared=%d key=%d source=%s",
              (long)woke, early, rtc_cleared, key_pressed, by_button ? "button" : "RTC");
        if (by_button) {
            /* Nickel holds EVIOCGRAB on the power key (our grab failed with
             * EBUSY), so the wakeup press is buffered on Nickel's fd and
             * replays as a sleep toggle when Nickel resumes.  Write a
             * synthetic press+release to the same node (no grab required for
             * writes) — the real press + this cancel give two toggle cycles,
             * leaving Nickel awake. */
            classify_wake(ui, woke);
            power_key_inject_cancel();
            break;
        }

        /* RTC woke us for a scheduled hourly tick. Always redraw — that advances
         * the "now" hour and day columns from cached data. Fetch only every
         * fetch_every-th tick, so a long sleep_upd_min still refreshes the panel
         * hourly without WiFi. Counting ticks (not comparing the clock to a
         * boundary) sidesteps the ~1 s early-RTC fire that previously dropped
         * boundary fetches. WiFi (if a fetch raised it) is dropped again at the
         * top of the next iteration. */
        int do_fetch = (++ticks >= fetch_every);
        if (do_fetch) ticks = 0;
        /* Mirror the interactive timer's night_pause skip (main.c) — redraw
         * still happens every tick (advances the hour from cached data), only
         * the WiFi+fetch is suppressed 00:00-06:00 local. */
        if (do_fetch && ui->persisted.night_pause) {
            struct tm lt; localtime_r(&woke, &lt);
            if (lt.tm_hour < 6) {
                do_fetch = 0;
                psdbg("night_pause: skip fetch tick");
            }
        }
        /* force_full_redraw: nothing has been drawn since the panel went
         * dark, so this is the sole redraw for the tick. Normally that means
         * one full-screen sweep, not a partial one — but when sleep_partial
         * is on, the user has opted in to letting draw_main_screen's ordinary
         * partial-update path (diffed against the last-drawn snapshot) handle
         * it instead, so we pass 0 and let force_full_redraw stay unset. */
        refresh_frame(ui, cfg, cache_path, do_fetch, ui->persisted.sleep_partial ? 0 : 1);
        if (do_fetch) wifi_is_up = wifi_is_associated();

        /* The refresh above can block several seconds on WiFi+fetch. If the user
         * pressed power during that window, honour it now rather than dropping
         * back to sleep and making them press again. */
        if (power_key_drain_pressed(pkey_fd)) {
            psdbg("power press during refresh — exiting kiosk");
            classify_wake(ui, woke);
            power_key_inject_cancel();
            break;
        }
    }

    if (pkey_fd >= 0) close(pkey_fd);

    /* Exit back to interactive mode. Redraw the header view first (from cached
     * data) so the app is visible within ~a second of waking; only then bring
     * WiFi back, since udhcpc can block on DHCP and must not gate the redraw. */
    if (g_debug) fprintf(stderr, "\n");
    psdbg("EXIT kiosk");
    pserr("cpu1_online_wake=%d", read_int_file("/sys/devices/system/cpu/cpu1/online"));
    backlight_restore();
    write_sysfs("/sys/power/wake_lock", WAKELOCK_NAME);
    restore_foreign_wakelocks();   /* hand Nickel's locks back, if any */
    *input_fd = input_open();
    input_set_screen(ui->screen_w, ui->screen_h);
    unlink(KIOSK_FLAG);
    /* Wake: repaint only the header band, partial GC16 no-flash. Content is
     * already on-panel; drawn_main stays valid afterwards so the deferred
     * post-wake fetch reconciles via the normal partial-update path. */
    ui_exit_kiosk(ui);
    return 0;
}
