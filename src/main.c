#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>  /* strcasecmp */
#include <stdarg.h>   /* va_list/va_start/va_end (diagf) */
#include <stdbool.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <sys/select.h>
#include <sys/timerfd.h>
#include <time.h>

#include "config.h"
#include "fetch_service.h"
#include "state.h"
#include "weather.h"
#include "cache.h"
#include "input.h"
#include "ui.h"
#include "fb.h"
#include <fbink.h>
#include "powersave.h"
#include "wifi.h"
#include "i18n.h"
#include "timeutil.h"
#include "launcher.h"
#include "sysutil.h"
#include <limits.h>

/* Data files live beside the binary (sysutil_exe_dir()); filled in app_main(). */
static char g_cache_path[PATH_MAX];
static char g_config_path[PATH_MAX];
static char g_settings_path[PATH_MAX];

volatile int g_running = 1;   /* also read by powersave.c via extern */
bool g_debug = false;  /* set by --debug CLI flag; read by fb.c's logging */
/* Fetch thread/eventfds/result live in FetchManager; main only holds timerfds. */
static struct {
    int refresh;    /* active-interval auto-refresh */
    int idle;       /* auto-sleep idle timer */
    int demo;       /* settings "Demo" 1 Hz symbol-cycle tick */
    int liveness;   /* heartbeat to the supervisor fail-safe */
} g_timers = { -1, -1, -1, -1 };

static void sig_handler(int sig) {
    (void)sig;
    g_running = 0;
}

/* Arm a timerfd.  first_s: seconds to first expiry.  interval_s: period
 * (0 = single-shot).  Clamps first_s to at least 1. */
static void arm_timerfd(int fd, long first_s, long interval_s) {
    if (fd < 0) return;
    if (first_s < 1) first_s = 1;
    struct itimerspec its = {
        .it_value    = { .tv_sec = first_s,    .tv_nsec = 0 },
        .it_interval = { .tv_sec = interval_s, .tv_nsec = 0 },
    };
    timerfd_settime(fd, 0, &its, NULL);
}

static void diag(const char *msg) {
    time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
    char ts[16];
    snprintf(ts, sizeof ts, "%02d:%02d:%02d ", tm.tm_hour, tm.tm_min, tm.tm_sec);
    size_t ts_len = strlen(ts);
    size_t len = strlen(msg);
    const char *paths[] = {
        "/mnt/onboard/kw_start.txt",
        "/tmp/kobo_weather.log",
        NULL
    };
    for (int i = 0; paths[i]; i++) {
        int fd = open(paths[i], O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0) {
            fprintf(stderr, "diag: open(%s) errno=%d (%s)\n",
                    paths[i], errno, strerror(errno));
            continue;
        }
        (void)!write(fd, ts, ts_len);
        ssize_t n = write(fd, msg, len);
        if (n != (ssize_t)len) {
            fprintf(stderr, "diag: write(%s) short=%zd errno=%d\n",
                    paths[i], n, errno);
        }
        /* Skip fsync to avoid flash wear (diag fires often); force under --debug. */
        if (g_debug) fsync(fd);
        close(fd);
    }
    fputs(ts, stderr);
    fputs(msg, stderr);
    fflush(stderr);
}

__attribute__((format(printf,1,2)))
static void diagf(const char *fmt, ...) {
    char buf[256];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    diag(buf);
}

/* Build a background fetch request; stage updates flow via the manager's
 * status_fd, not on_stage (left NULL here). */
static FetchRequest make_fetch_request(UIState *ui, Config *cfg) {
    return (FetchRequest){
        .cfg = cfg, .units = ui->persisted.units,
        .geolocate = true, .wpa_retries = 0, .on_stage = NULL,
    };
}

/* Kick off a background fetch; on thread-create failure, clear in-progress
 * rather than leaving the spinner stuck. */
static void start_fetch(UIState *ui, FetchManager *fm, Config *cfg) {
    FetchRequest req = make_fetch_request(ui, cfg);
    if (fetch_manager_start(fm, &req) != 0) {
        diag("DIAG: FETCH_START_FAILED\n");
        ui->fetch_in_progress = 0;
        ui->needs_redraw      = 1;
        return;
    }
    ui->fetch_in_progress = 1;
    ui->fetch_stage       = 0;
    ui->needs_redraw      = 1;
}

/* Adopt a completed fetch's outcome: persist on success, else flag the
 * "enable WiFi in Nickel" hint. */
static void adopt_fetch_result(UIState *ui, Config *cfg, WeatherState *state,
                               const FetchResult *fr) {
    if (fr->has_resolved_loc) {
        /* Geolocation ran on the worker; adopt its result here, single-threaded. */
        cfg->loc = fr->resolved_loc;
        cfg->api_url[0] = '\0';
        config_build_url(cfg);
    }
    if (fr->status == FETCH_OK) {
        *state = fr->state;
        cache_save(g_cache_path, state);
        ui->wifi_needs_nickel = 0;
    } else {
        ui->wifi_needs_nickel = (fr->status == FETCH_ERR_NICKEL);
    }
    ui->fetch_in_progress = 0;
}

/* Re-arm idle/auto-sleep timer; no-op if unset or in kiosk mode (which has
 * its own wake/sleep cycle). */
static void rearm_idle_timer(UIState *ui) {
    if (g_timers.idle >= 0 && !ui->power.kiosk)
        arm_timerfd(g_timers.idle, (long)ui->persisted.sleep_min * 60, 0);
}

/* Re-arm the auto-refresh timer to the next active_min-aligned local-time
 * boundary. No-op if the timer wasn't created. */
static void rearm_refresh_timer(UIState *ui) {
    if (g_timers.refresh < 0) return;
    long step    = (long)ui->persisted.active_min * 60;
    time_t now_t = time(NULL);
    long first_s = (long)(next_local_boundary(now_t, step) - now_t);
    arm_timerfd(g_timers.refresh, first_s, step);
}

/* Create and arm the refresh/idle/demo-tick timerfds. Demo timer starts
 * disarmed (armed on SCREEN_DEMO entry/exit). */
static void setup_timers(UIState *ui) {
    /* Auto-refresh timer aligned to the configured active_min boundary. */
    g_timers.refresh = timerfd_create(CLOCK_BOOTTIME, TFD_NONBLOCK | TFD_CLOEXEC);
    if (g_timers.refresh < 0) {
        diag("DIAG: TIMERFD_FAILED\n");
    } else {
        long step      = (long)ui->persisted.active_min * 60;
        time_t now_t   = time(NULL);
        long initial_s = (long)(next_local_boundary(now_t, step) - now_t);
        arm_timerfd(g_timers.refresh, initial_s, step);
        diagf("DIAG: TIMERFD_INIT initial=%lds interval=%lds\n",
              initial_s, step);
    }

    /* Daily wash: first successful fetch at/after 06:00 local time. */
    ui->refresh.daily_wash_after = next_06am(time(NULL));

    /* Idle auto-sleep timer, re-armed on every touch event. */
    g_timers.idle = timerfd_create(CLOCK_BOOTTIME, TFD_NONBLOCK | TFD_CLOEXEC);
    if (g_timers.idle < 0) {
        diag("DIAG: IDLE_TIMERFD_FAILED\n");
    } else {
        arm_timerfd(g_timers.idle, (long)ui->persisted.sleep_min * 60, 0);
    }

    /* Settings "Demo" tick (2 s); armed/disarmed on SCREEN_DEMO entry/exit. */
    g_timers.demo = timerfd_create(CLOCK_BOOTTIME, TFD_NONBLOCK | TFD_CLOEXEC);
    if (g_timers.demo < 0) diag("DIAG: DEMO_TIMERFD_FAILED\n");

    /* Liveness heartbeat (every 15 s): pets the launcher's 60 s fail-safe so a
     * responsive loop is never killed; a wedged loop stops pinging and gets
     * killed within ~1 min. */
    g_timers.liveness = timerfd_create(CLOCK_BOOTTIME, TFD_NONBLOCK | TFD_CLOEXEC);
    if (g_timers.liveness < 0) diag("DIAG: LIVENESS_TIMERFD_FAILED\n");
    else arm_timerfd(g_timers.liveness, 15, 15);
}

/* Close, sleep 1s, and reopen the touch-input fd after a read/select error
 * (e.g. the input device node was transiently removed/re-created). Re-applies
 * the current screen geometry to the new fd. Returns the new fd. */
static int reopen_input(int old_fd, UIState *ui) {
    input_close(old_fd);
    sleep(1);
    int new_fd = input_open();
    input_set_screen(ui->screen_w, ui->screen_h);
    return new_fd;
}

/* Handle a completed background fetch: adopt result, apply daily-wash check,
 * recalc hourly scroll bounds if needed, and flag redraw. */
static void handle_fetch_complete(UIState *ui, Config *cfg, WeatherState *state, const FetchResult *fr) {
    adopt_fetch_result(ui, cfg, state, fr);
    if (fr->status == FETCH_OK) {
        time_t now_t = time(NULL);
        if (now_t >= ui->refresh.daily_wash_after) {
            ui->refresh.full_redraw_reason = RR_DAILY_WASH;
            ui->refresh.daily_wash_after = next_06am(now_t);
            diag("DIAG: DAILY_WASH\n");
        }
        if (ui->refresh.startup_wash_pending) {
            ui->refresh.drawn_main.ok = 0;
            ui->refresh.startup_wash_pending = 0;
        }
        if (ui->screen == SCREEN_HOURLY) {
            ui->hourly_count = weather_hours_for_day(state, ui->selected_day,
                                                      ui->hourly_idx);
            int total_h   = ui->hourly_count * ui->hourly_row_h;
            int visible_h = ui->screen_h - ui->header_h;
            int page_h    = (visible_h / ui->hourly_row_h) * ui->hourly_row_h;
            ui->scroll.max = total_h > page_h ? total_h - page_h : 0;
            if (ui->scroll.y > ui->scroll.max) ui->scroll.y = ui->scroll.max;
        }
        diag("DIAG: FETCH_OK\n");
    } else {
        diagf("DIAG: FETCH_FAIL_BG status=%d rc=%d %s\n",
              fr->status, fr->rc, fr->err);
    }
    if (ui->screen == SCREEN_MAIN || ui->screen == SCREEN_HOURLY)
        ui->needs_redraw = 1;
}

/* Handle ui->power.enter: join/adopt any in-flight fetch, then block in
 * power_save_loop() until the device wakes. On return, re-arm timers and
 * trigger a catch-up fetch if cached data is stale by an active interval. */
static void handle_enter_power_save(UIState *ui, Config *cfg, WeatherState *state,
                                      int *input_fd, FetchManager *fm) {
    /* Mark kiosk before the blocking join: the loop won't heartbeat again until
     * we return, so the fail-safe must be flag-exempt for the whole entry. */
    power_save_mark_kiosk();
    if (fm->in_progress) {
        FetchResult fr;
        fetch_manager_complete(fm, &fr);
        adopt_fetch_result(ui, cfg, state, &fr);
    }
    /* Disarm idle timer while in kiosk (it has its own wake/sleep cycle). */
    if (g_timers.idle >= 0) {
        struct itimerspec dis = {{0,0},{0,0}};
        timerfd_settime(g_timers.idle, 0, &dis, NULL);
    }
    power_save_loop(ui, cfg, input_fd, g_cache_path);  /* blocks */
    rearm_idle_timer(ui);
    /* CLOCK_BOOTTIME timer accrued expirations across suspend; re-arming
     * discards those and restores clean boundary alignment. */
    time_t kx_now = time(NULL);
    rearm_refresh_timer(ui);
    /* Refetch only if data is older than a full active interval; otherwise
     * wait for the re-armed boundary timer. */
    if (!fm->in_progress && state->fetched_at > 0 &&
        kx_now - state->fetched_at >= (long)ui->persisted.active_min * 60) {
        diag("DIAG: KIOSK_EXIT_STALE_FETCH\n");
        start_fetch(ui, fm, cfg);
    }
}

/* Advance the demo by one Wsymb2 symbol; ends the demo after symbol 27. */
static void handle_demo_tick(UIState *ui, WeatherState *demo_state) {
    if (ui->demo.symbol >= 27) {
        ui->demo.quit = 1;
    } else {
        ui->demo.symbol++;
        weather_fill_demo(demo_state, ui->demo.symbol, ui->persisted.units);
        ui->needs_redraw = 1;
    }
}

/* Enter the demo: swap ui->state to demo_state (saving the old pointer for
 * restore), seed symbol 1, switch screen, arm the 2s tick. */
static void handle_enter_demo(UIState *ui, WeatherState *demo_state, WeatherState **demo_saved_state) {
    *demo_saved_state = ui->state;
    ui->state         = demo_state;
    ui->demo.symbol   = 1;
    weather_fill_demo(demo_state, ui->demo.symbol, ui->persisted.units);
    ui_set_screen(ui, SCREEN_DEMO);
    ui->refresh.drawn_demo    = 0;
    arm_timerfd(g_timers.demo, 2, 2);
    ui->needs_redraw  = 1;
}

/* Exit the demo: disarm tick, restore ui->state, return to Settings. */
static void handle_quit_demo(UIState *ui, WeatherState **demo_saved_state) {
    struct itimerspec dis = {{0,0},{0,0}};
    timerfd_settime(g_timers.demo, 0, &dis, NULL);
    if (*demo_saved_state) {
        ui->state         = *demo_saved_state;
        *demo_saved_state = NULL;
    }
    ui_set_screen(ui, SCREEN_SETTINGS);
    ui->refresh.drawn_settings = 0;
    ui->needs_redraw  = 1;
}

int app_main(int argc, char *argv[]) {
    /* Resolve the data-file paths beside the binary before anything uses them. */
    sysutil_path(g_cache_path,    sizeof g_cache_path,    "state.cache");
    sysutil_path(g_config_path,   sizeof g_config_path,   "weather.conf");
    sysutil_path(g_settings_path, sizeof g_settings_path, "ui_settings.conf");

    const char *conf_path = g_config_path;   /* a positional CLI arg overrides */
    bool use_smhi = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--debug") == 0)
            g_debug = true;
        else if (strcmp(argv[i], "--smhi") == 0)
            use_smhi = true;
        else
            conf_path = argv[i];
    }

    /* Unbuffered so DIAG lines survive an abrupt kill. */
    setvbuf(stderr, NULL, _IONBF, 0);

    diag("DIAG: MAIN_ALIVE\n");
    {
        FILE *_f = fopen("/sys/devices/system/cpu/cpu1/online", "r");
        int   _v = -1;
        if (_f) { if (fscanf(_f, "%d", &_v) != 1) _v = -1; fclose(_f); }
        diagf("DIAG: CPU1_ONLINE_STARTUP=%d\n", _v);
    }

    signal(SIGTERM, sig_handler);
    signal(SIGINT,  sig_handler);
    signal(SIGHUP,  sig_handler);

    FetchManager fm;
    if (fetch_manager_init(&fm) < 0) {
        diag("DIAG: EVENTFD_FAILED\n");
        return 1;
    }

    Config cfg;
    config_load(conf_path, &cfg);
    cfg.provider = use_smhi ? PROVIDER_SMHI : PROVIDER_OPEN_METEO;  /* not from file */
    diagf("DIAG: CONFIG place=%s auto_location=%d\n",
          cfg.loc.place, cfg.auto_location);
    WeatherState state;
    memset(&state, 0, sizeof(state));
    snprintf(state.place, sizeof(state.place), "%s", cfg.loc.place);

    UIState ui;
    if (ui_init(&ui, &state, &cfg) < 0) {
        diag("DIAG: UI_INIT_FAILED\n");
        return 1;
    }
    ui.settings_path = g_settings_path;
    /* Persisted settings load as one struct (single source of truth). */
    settings_load(g_settings_path, &ui.persisted);

    int input_fd = input_open();
    if (input_fd < 0) diag("DIAG: INPUT_OPEN_FAILED\n");
    input_set_screen(ui.screen_w, ui.screen_h);

    /* Resolve the real WiFi interface (e.g. mlan0) before any has_ip() test, so
     * kiosk entry with a fresh cache powers the RF off instead of leaving it on. */
    wifi_detect();

    /* Always boot to the home screen and fetch in the background. With no cache
     * the empty state renders as a dashed skeleton (---°C, Vind: ---, blank
     * icons) while the subtitle shows the live stages; the close button stays
     * reachable the whole time. */
    if (cache_load(g_cache_path, &state) == 0 && state.n_hours > 0) {
        diag("DIAG: CACHE_LOADED\n");
    } else {
        diag("DIAG: NO_CACHE_SKELETON_START\n");
        memset(&state, 0, sizeof state);   /* a late cache_load failure can leave partial state */
        snprintf(state.place, sizeof state.place, "%s", cfg.loc.place);
    }
    ui_set_screen(&ui, SCREEN_MAIN);
    ui.refresh.startup_paint_pending = 1;
    ui.refresh.startup_wash_pending  = 1;
    start_fetch(&ui, &fm, &cfg);

    setup_timers(&ui);

    /* Separate from `state` so the demo never disturbs real cached/fetched weather. */
    WeatherState demo_state;
    WeatherState *demo_saved_state = NULL;

    /* Main event loop: blocks on input/eventfd/timerfd until something fires. */
    while (g_running) {
        if (ui.needs_redraw) ui_draw(&ui);

        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = -1;
        if (input_fd >= 0)           { FD_SET(input_fd,           &rfds); if (input_fd           > maxfd) maxfd = input_fd; }
        if (fm.event_fd >= 0)        { FD_SET(fm.event_fd,       &rfds); if (fm.event_fd        > maxfd) maxfd = fm.event_fd; }
        if (fm.status_fd >= 0)       { FD_SET(fm.status_fd,      &rfds); if (fm.status_fd       > maxfd) maxfd = fm.status_fd; }
        if (g_timers.refresh >= 0)  { FD_SET(g_timers.refresh,  &rfds); if (g_timers.refresh  > maxfd) maxfd = g_timers.refresh; }
        if (g_timers.idle >= 0)     { FD_SET(g_timers.idle,     &rfds); if (g_timers.idle     > maxfd) maxfd = g_timers.idle; }
        if (g_timers.demo >= 0)     { FD_SET(g_timers.demo,     &rfds); if (g_timers.demo     > maxfd) maxfd = g_timers.demo; }
        if (g_timers.liveness >= 0) { FD_SET(g_timers.liveness, &rfds); if (g_timers.liveness > maxfd) maxfd = g_timers.liveness; }

        /* A queued redraw polls with zero timeout so it's serviced immediately.
         * Otherwise block indefinitely — timerfds wake the loop on schedule. */
        struct timeval tv = {0, 0};
        struct timeval *tvp = ui.needs_redraw ? &tv : NULL;

        int sel = select(maxfd + 1, &rfds, NULL, NULL, tvp);
        if (sel < 0) {
            if (errno == EINTR) continue;
            input_fd = reopen_input(input_fd, &ui);
            continue;
        }

        if (input_fd >= 0 && FD_ISSET(input_fd, &rfds)) {
            TouchEvent ev;
            int r;
            while ((r = input_read_nonblock(input_fd, &ev)) == 0) {
                if (ui_handle_touch(&ui, &ev)) { g_running = 0; break; }
                /* Re-arm idle timer on every touch so inactivity is measured
                 * from the last user interaction. */
                rearm_idle_timer(&ui);
            }
            if (r < 0) {
                diag("DIAG: INPUT_READ_ERR\n");
                input_fd = reopen_input(input_fd, &ui);
            }
        }

        /* WiFi stage update from the fetch thread. */
        if (fm.status_fd >= 0 && FD_ISSET(fm.status_fd, &rfds)) {
            uint64_t drain;
            (void)!read(fm.status_fd, &drain, sizeof(drain));
            ui.fetch_stage   = fetch_manager_stage(&fm);
            if (ui.screen == SCREEN_MAIN || ui.screen == SCREEN_HOURLY)
                ui.needs_redraw  = 1;
        }

        if (fm.event_fd >= 0 && FD_ISSET(fm.event_fd, &rfds)) {
            if (fm.in_progress) {
                FetchResult fr;
                fetch_manager_complete(&fm, &fr);
                handle_fetch_complete(&ui, &cfg, &state, &fr);
            }
        }

        if (g_timers.refresh >= 0 && FD_ISSET(g_timers.refresh, &rfds)) {
            uint64_t expirations;
            (void)!read(g_timers.refresh, &expirations, sizeof(expirations));
            time_t now_t = time(NULL);
            /* Guards against a spurious fire after resume (CLOCK_BOOTTIME
             * accrues expirations across suspend) when already up to date. */
            if (!fm.in_progress && ui.screen != SCREEN_DEMO &&
                crossed_boundary(now_t, state.fetched_at, (long)ui.persisted.active_min * 60)) {
                int skip_night = 0;
                if (ui.persisted.night_pause) {
                    struct tm lt; localtime_r(&now_t, &lt);
                    skip_night = (lt.tm_hour < 6);
                }
                if (!skip_night)
                    start_fetch(&ui, &fm, &cfg);
            }
        }

        if (g_timers.demo >= 0 && FD_ISSET(g_timers.demo, &rfds)) {
            uint64_t expirations;
            (void)!read(g_timers.demo, &expirations, sizeof(expirations));
            if (ui.screen == SCREEN_DEMO)
                handle_demo_tick(&ui, &demo_state);
        }

        if (g_timers.liveness >= 0 && FD_ISSET(g_timers.liveness, &rfds)) {
            uint64_t expirations;
            (void)!read(g_timers.liveness, &expirations, sizeof(expirations));
            launcher_notify_liveness();
        }

        /* Idle timer expired — enter power-save if interactive. */
        if (g_timers.idle >= 0 && FD_ISSET(g_timers.idle, &rfds)) {
            uint64_t expirations;
            (void)!read(g_timers.idle, &expirations, sizeof(expirations));
            if (!ui.power.kiosk && (ui.screen == SCREEN_MAIN ||
                                    ui.screen == SCREEN_HOURLY ||
                                    ui.screen == SCREEN_SETTINGS))
                ui.power.enter = 1;   /* idle timeout → enter power-save */
        }

        /* Settings changed timers — re-arm both timerfds. */
        if (ui.settings_ui.rearm_timers) {
            ui.settings_ui.rearm_timers = 0;
            rearm_refresh_timer(&ui);
            rearm_idle_timer(&ui);
        }

        /* Units changed in settings — re-fetch in the new unit system. */
        if (ui.trigger_fetch && !fm.in_progress) {
            ui.trigger_fetch = 0;
            start_fetch(&ui, &fm, &cfg);
        }

        if (ui.power.enter) {
            ui.power.enter = 0;
            handle_enter_power_save(&ui, &cfg, &state, &input_fd, &fm);
        }

        if (ui.demo.enter) {
            ui.demo.enter = 0;
            handle_enter_demo(&ui, &demo_state, &demo_saved_state);
        }

        if (ui.demo.quit) {
            ui.demo.quit = 0;
            handle_quit_demo(&ui, &demo_saved_state);
        }
    }

    /* Teardown (reached when the loop exits on quit/close). */
    /* Join any in-flight background fetch before tearing WiFi down. */
    fetch_manager_complete(&fm, NULL);
    /* Release WiFi so it doesn't fight connmand once the launcher resumes it. */
    wifi_down();
    if (g_timers.refresh >= 0) close(g_timers.refresh);
    if (g_timers.idle    >= 0) close(g_timers.idle);
    if (g_timers.demo    >= 0) close(g_timers.demo);
    if (g_timers.liveness >= 0) close(g_timers.liveness);
    fetch_manager_cleanup(&fm);
    input_close(input_fd);
    ui_cleanup(&ui);
    sys_restore_orphaned();
    diag("DIAG: EXIT\n");
    return 0;
}
