#include "ui.h"
#include "ui_params.h"
#include "fb.h"
#include "weather.h"
#include "i18n.h"
#include "wifi.h"
#include <fbink.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <math.h>
#include <sys/stat.h>
#include "ui_internal.h"

/* WFM_INIT clears the panel to white but also latches whatever is currently
 * in the framebuffer into the EPDC working buffer; if the FB isn't white at
 * that point, a later non-flashing refresh diffs to nothing and is a no-op.
 * Always wash a white FB before INIT. The settle delay below lets INIT's
 * physical flash finish (its completion marker fires early on this panel). */

/* nanosleep wrapper (ms granularity), resumes across EINTR. */
void settle_sleep_ms(int ms) {
    struct timespec req = { ms / 1000, (long)(ms % 1000) * 1000000L };
    while (nanosleep(&req, &req) == -1 && errno == EINTR) { }
}

/* Sleeps only the remainder up to target_ms since the panel was seized from
 * Nickel (g_disp.panel_seized_at), so a still-draining Nickel update can land
 * before our first flash. Work already done since seize counts, so a slow
 * start adds no extra wait. */
void settle_since_panel_seized(int target_ms) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long ms = (now.tv_sec  - g_disp.panel_seized_at.tv_sec)  * 1000
            + (now.tv_nsec - g_disp.panel_seized_at.tv_nsec) / 1000000;
    if (ms < target_ms) settle_sleep_ms((int)(target_ms - ms));
}

Display g_disp = {
    .fbink_fd    = -1,
    .font_loaded = 0,
    .bold_loaded = 0,
    .scale       = 1.0f,
    .icon_pack   = ICONPACK_CLARA,
};

/* Scale a UI_REF_* reference pixel value to the actual screen. */
int sc(int ref) { return (int)((float)ref * g_disp.scale + 0.5f); }

static const char *screen_name(UIScreen s) {
    switch (s) {
    case SCREEN_MAIN:     return "MAIN";
    case SCREEN_HOURLY:   return "HOURLY";
    case SCREEN_SETTINGS: return "SETTINGS";
    case SCREEN_DEMO:     return "DEMO";
    default:              return "?";
    }
}

/* Centralises ui->screen writes so transitions are visible in --debug. */
void ui_set_screen(UIState *ui, UIScreen s) {
    if (g_debug && ui->screen != s) {
        time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
        fprintf(stderr, "%02d:%02d:%02d SCREEN %s -> %s\n",
                tm.tm_hour, tm.tm_min, tm.tm_sec,
                screen_name(ui->screen), screen_name(s));
    }
    ui->screen = s;
}

/* ---- Geometry structs ------------------------------------------------ */


/* Enter the power-save (kiosk) layout. Needs a full redraw from a non-home
 * screen or when sleep_entry_wash is FLASH/WASH; otherwise just blanks the
 * header band (content pixels are already correct on the home screen). */
void ui_enter_kiosk(UIState *ui) {
    bool non_home   = (ui->screen != SCREEN_MAIN);
    bool need_full  = non_home ||
                      ui->persisted.sleep_entry_wash != SLEEP_ENTRY_NONE;
    /* Scrolled on hourly without a later in-place settle clearing it (e.g.
     * idle timer fires mid-scroll): force the flash on this exit too. */
    bool exit_flash = (ui->screen == SCREEN_HOURLY) && ui->refresh.hourly_scrolled;
    ui->refresh.hourly_scrolled = 0;
    ui_set_screen(ui, SCREEN_MAIN);
    ui->power.kiosk = 1;
    if (need_full) {
        ui->refresh.drawn_main.ok      = 0;
        ui->refresh.full_redraw_reason = exit_flash ? RR_HOURLY_EXIT_FLASH : RR_KIOSK_ENTRY;
        draw_main_screen(ui);   /* ui->power.kiosk == 1 -> headerless frame */
    } else {
        fb_fill_rect(&g_disp.fb, (Rect){0, 0, ui->screen_w, ui->header_h}, UI_BG);
        ui_region_commit(ui, (Rect){0, 0, ui->screen_w, ui->header_h}, ui->persisted.partial_style);
    }
}

/* Leave the power-save layout: repaint only the header band, partial GC16
 * no-flash, regardless of settings. Content below header_h is already on-panel
 * (home_top == header_h), the mirror of ui_enter_kiosk's blank-the-band path;
 * drawn_main stays valid so the post-wake fetch reconciles content partially. */
void ui_exit_kiosk(UIState *ui) {
    ui->power.kiosk = 0;
    draw_header(ui, "", 0, 1, 1);   /* home header "≡ ☾ ✕"; paints only [0, header_h) */
    ui_region_commit(ui, (Rect){0, 0, ui->screen_w, ui->header_h}, RSTYLE_GC16_NOFLASH);
}

/* Debug-only screenshot; g_disp.fb is private to this module, so callers
 * outside the UI subsystem go through this. */
int ui_dump_screenshot(const char *path) {
    return fb_dump_bmp(&g_disp.fb, path);
}

/* ---- Drawing: hourly screen --------------------------------------- */

/* ---- Loading/error/demo screens are in screen_demo.c ------------------- */

/* ---- Settings screen (draw + touch) are in screen_settings.c ----------- */

/* ---- Font calibration --------------------------------------------- */


/* ---- Public API --------------------------------------------------- */

int ui_init(UIState *ui, WeatherState *state, const Config *cfg) {
    memset(ui, 0, sizeof(*ui));
    ui->state  = state;
    ui->cfg    = cfg;
    ui_set_screen(ui, SCREEN_MAIN);

    ui->settings_ui.dirty_item     = -1;
    ui->settings_ui.dirty_prev_sub = -1;

    if (fb_open(&g_disp.fb) < 0) return -1;
    ui->screen_w = g_disp.fb.w;
    ui->screen_h = g_disp.fb.h;

    float s = (float)ui->screen_w / (float)UI_REF_W;
    g_disp.scale = s;
#define SC(x) ((int)((x) * s + 0.5f))
    ui->header_h     = SC(UI_REF_HEADER_H);
    ui->home_top     = ui->header_h;
    ui->pad          = SC(UI_REF_PAD);
    ui->x_half       = SC(UI_REF_X_HALF);
    /* Libra-class (wider) screens use the Libra icon pack; resolve before any
     * icon-sized layout below. The packs ARE the resolution adaptation: each is
     * authored at its target screen's size and blitted at native size (no
     * resize in draw_icon), so these slot dims are native — NOT SC()-scaled,
     * so the slot matches the drawn icon. */
    g_disp.icon_pack = (ui->screen_w > UI_REF_W) ? ICONPACK_LIBRA : ICONPACK_CLARA;
    if (g_disp.icon_pack == ICONPACK_LIBRA) {
        ui->big_icon     = UI_REF_BIG_ICON_LIBRA;
        ui->big_icon_w   = UI_REF_BIG_ICON_W_LIBRA;
        ui->med_icon     = UI_REF_MED_ICON_LIBRA;
        ui->med_icon_w   = UI_REF_MED_ICON_W_LIBRA;
        ui->small_icon   = UI_REF_SMALL_ICON_LIBRA;
    } else {
        ui->big_icon     = UI_REF_BIG_ICON_CLARA;
        ui->big_icon_w   = UI_REF_BIG_ICON_W_CLARA;
        ui->med_icon     = UI_REF_MED_ICON_CLARA;
        ui->med_icon_w   = UI_REF_MED_ICON_W_CLARA;
        ui->small_icon   = UI_REF_SMALL_ICON_CLARA;
    }
    /* forecast_h derived after calibration (see below) */
    {
        int ref_rh   = SC(UI_REF_HOURLY_ROW_H);
        int usable_h = ui->screen_h - SC(UI_REF_HEADER_H);
        int n_rows   = usable_h / ref_rh;
        if (n_rows < 1) n_rows = 1;
        ui->hourly_row_h = usable_h / n_rows;
    }
#undef SC

    if (g_debug)
        fprintf(stderr, "Layout scale %.2f (screen %dx%d)\n",
                s, ui->screen_w, ui->screen_h);

    memset(&g_disp.cfg, 0, sizeof(g_disp.cfg));
    g_disp.cfg.no_refresh = true;
    g_disp.fbink_fd = fbink_open();
    if (g_disp.fbink_fd < 0) return -1;
    if (fbink_init(g_disp.fbink_fd, &g_disp.cfg) < 0) return -1;
    fb_paced_init(g_disp.fbink_fd);
    /* Panel seized from Nickel: nothing drawn yet (calibration below is
     * no_refresh). The first startup flash settles a remainder off this
     * stamp so Nickel's last update can land. */
    clock_gettime(CLOCK_MONOTONIC, &g_disp.panel_seized_at);

    static const char *font_fallbacks[] = {
        "./fonts/DejaVuSans.ttf",
        "/usr/local/Kobo/fonts/A-Avenir-Next.ttf",
        "/usr/local/Kobo/fonts/A-Bookerly.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        NULL
    };
    g_disp.font_loaded = 0;
    const char *loaded_font = NULL;
    if (cfg->font_path[0] != '\0') {
        if (fbink_add_ot_font(cfg->font_path, FNT_REGULAR) == 0) {
            g_disp.font_loaded = 1;
            loaded_font   = cfg->font_path;
            if (g_debug)
                fprintf(stderr, "Font from config: %s\n", cfg->font_path);
        }
    }
    for (int i = 0; !g_disp.font_loaded && font_fallbacks[i]; i++) {
        if (access(font_fallbacks[i], R_OK) == 0 &&
            fbink_add_ot_font(font_fallbacks[i], FNT_REGULAR) == 0) {
            g_disp.font_loaded = 1;
            loaded_font   = font_fallbacks[i];
            if (g_debug)
                fprintf(stderr, "Font: %s\n", font_fallbacks[i]);
        }
    }
    if (!g_disp.font_loaded) {
        fprintf(stderr, "WARN: no OT font found — text will be invisible\n");
    }

    /* Bold face for emphasised text; optional — falls back to regular if not found. */
    static const char *bold_font_fallbacks[] = {
        "./fonts/DejaVuSans-Bold.ttf",
        "/usr/local/Kobo/fonts/A-Avenir-Next-Bold.ttf",
        "/usr/local/Kobo/fonts/A-Bookerly-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        NULL
    };
    g_disp.bold_loaded = 0;
    for (int i = 0; !g_disp.bold_loaded && bold_font_fallbacks[i]; i++) {
        if (access(bold_font_fallbacks[i], R_OK) == 0 &&
            fbink_add_ot_font(bold_font_fallbacks[i], FNT_BOLD) == 0) {
            g_disp.bold_loaded = 1;
            if (g_debug)
                fprintf(stderr, "Bold font: %s\n", bold_font_fallbacks[i]);
        }
    }
    if (!g_disp.bold_loaded && g_debug)
        fprintf(stderr, "No bold OT font found — emphasised text falls back to regular\n");

    /* Calibrate font band offsets (cached per font) before computing current_h. */
    if (g_disp.font_loaded && !load_band_cache(loaded_font)) {
        calibrate_fonts(ui);
        save_band_cache(loaded_font);
    }
    ui->current_h = cur_band_height(ui);
    {
        DayGeom g0   = compute_day_geom(ui, 0, ui->screen_w / 3);
        int wind_bot = g0.wind_y + band(UI_FONT_DAY_WIND_PT)->base_off;
        ui->forecast_h = wind_bot + ui->pad - (ui->home_top + ui->current_h);
    }

    scroll_init(&ui->scroll);
    return 0;
}

void ui_cleanup(UIState *ui) {
    WakeOverride wo = ui_consume_wake_override(ui);

    /* Closing while still on hourly after a scroll: force the flash too,
     * unless the wake-staleness override above already takes precedence. */
    bool exit_flash = (ui->screen == SCREEN_HOURLY) && ui->refresh.hourly_scrolled;
    ui->refresh.hourly_scrolled = 0;

    /* Wash BEFORE restoring Nickel's snapshot, else INIT latches the snapshot
     * into the EPDC working buffer and the GC16 below becomes a no-op. */
    if (wo.init_wash) {
        fb_fill_rect(&g_disp.fb, (Rect){0, 0, ui->screen_w, ui->screen_h}, UI_BG);
        ui_run_init_wash(ui);
    }

    /* Restore Nickel's pre-launch screen (it won't repaint on SIGCONT itself). */
    if (!fb_restore_snapshot(&g_disp.fb))
        fb_fill_rect(&g_disp.fb, (Rect){0, 0, ui->screen_w, ui->screen_h}, UI_BG);

    ui_region_commit(ui, RECT_FULLSCREEN,
                      wo.override ? wo.style : (exit_flash ? RSTYLE_GC16_FLASH : RSTYLE_GC16));
    fb_paced_wait();  /* ensure final wash lands before Nickel returns */
    if (g_disp.fbink_fd >= 0) fbink_close(g_disp.fbink_fd);
    fb_close(&g_disp.fb);
    icon_cache_free();
}

/* Per-screen draw dispatch table. Touch handling stays in ui_touch.c. */
typedef struct {
    void (*draw)(UIState *ui);
} ScreenOps;

static const ScreenOps screen_ops[] = {
    [SCREEN_MAIN]     = { draw_main_screen },
    [SCREEN_HOURLY]   = { draw_hourly_screen },
    [SCREEN_SETTINGS] = { draw_settings_screen },
    [SCREEN_DEMO]     = { draw_demo_screen },
};

void ui_draw(UIState *ui) {
    /* Drain any in-flight A2 frame before starting a new draw path. */
    fb_paced_wait();

    if ((unsigned)ui->screen < sizeof screen_ops / sizeof screen_ops[0]
        && screen_ops[ui->screen].draw)
        screen_ops[ui->screen].draw(ui);
    ui->needs_redraw = 0;
}

/* ---- Touch handling is in ui_touch.c ----------------------------------- */
