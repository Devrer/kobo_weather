#ifndef UI_H
#define UI_H

#include "state.h"
#include "config.h"
#include "ui_params.h"
#include "input.h"
#include "widgets/scroll.h"
#include "fb.h"
#include <stdint.h>

/* Rect/RECT_FULLSCREEN come from fb.h. */

/* Why draw_main_screen forces a full redraw this pass; consumed/cleared there.
 * RR_STARTUP and RR_POST_FETCH are derived for the first paint, never
 * assigned directly. */
typedef enum {
    RR_NONE = 0,           /* no forced full redraw (partial-update path) */
    RR_DAILY_WASH,         /* 06:00: INIT-on-white, then GC16 no-flash */
    RR_KIOSK_ENTRY,        /* kiosk entry full redraw: sleep_entry_wash tri-state —
                              WASH=INIT, FLASH=GC16 flash, NONE=awake full_style */
    RR_TICK_WAKE,          /* periodic RTC redraw tick: GC16 no-flash */
    RR_STARTUP,            /* derived: app-startup first paint (cache or post-fetch), GC16 flash */
    RR_POST_FETCH,         /* derived: Settings/Hourly->home, startup post-fetch */
    RR_HOURLY_EXIT_FLASH,  /* leaving hourly after a scroll: GC16 flash regardless of full_style */
} FullRedrawReason;

/* Pending heavy-refresh override after a button-driven kiosk wake. Set by
 * classify_wake() (powersave.c); consumed by ui_consume_wake_override() on the
 * next Settings/Hourly entry or app close. WASH and FLASH are mutually exclusive. */
typedef enum {
    WAKE_REFRESH_NONE = 0,   /* never slept / awake nav: no override (keep full_style) */
    WAKE_REFRESH_CLEAN,      /* short kiosk sleep: full-screen GC16, no flash (re-baselines the EPDC) */
    WAKE_REFRESH_FLASH,      /* >=wake_flash_min asleep: GC16 flash pending */
    WAKE_REFRESH_WASH,       /* >=wake_wash_min asleep: INIT-wash + GC16 pending */
} WakeRefreshNeed;

/* Snapshot of data last rendered on the main screen, for change-detection so
 * draw_main_screen() repaints only dirty elements. */
typedef struct {
    int  ok;                   /* 0 until first full draw populates all fields */
    int  cur_symbol;           /* wsymb icon number; -1 = missing */
    char cur_temp[16];
    char cur_maxmin[32];
    char cur_wind[64];
    char cur_cond[32];
    int  day_symbol[3];        /* forecast columns [0..2] = day offsets 1..3 */
    char day_tmax[3][16];
    char day_tmin[3][16];
    char day_wind[3][32];
    char day_cond[3][32];
    char day_label_str[3][16]; /* changes at midnight */
    char subtitle[96];

    /* Pixel rects of the most recently rendered text per element; used by
     * partial-update clears to erase precisely where old text was painted. */
    Rect last_cur_temp_rect;
    Rect last_cur_unit_rect;   /* unit glyph; only repainted when unit changes */
    Rect last_cur_maxmin_rect;
    Rect last_cur_wind_rect;
    Rect last_cur_cond_rect;
    Rect last_day_label_rect[3];
    Rect last_day_cond_rect[3];
    Rect last_day_maxmin_rect[3];
    Rect last_day_wind_rect[3];
    Rect last_subtitle_rect;
} DrawnMain;

typedef enum {
    SCREEN_MAIN     = 0,
    SCREEN_HOURLY   = 1,
    SCREEN_SETTINGS = 2,
    SCREEN_DEMO     = 3,
} UIScreen;

/* ScrollMode (SCROLL_FLOAT/SNAP_A2/SNAP_REAGL) is defined in config.h, where
 * the persisted `snap` field is typed with it. */

/* Touch-gesture tracking (all session-only). */
typedef struct {
    int      active;
    int      start_x;
    int      start_y;
    int      committed_scroll;
    /* Set when a button-driven screen change happens on TOUCH_DOWN: swallows
     * the rest of the gesture (and any contact-bounce DOWN) until the finger
     * lifts, so the same physical tap can't also land on a button occupying
     * the same corner of the destination screen. */
    int      swallow;
    int      snap_steps;        /* snap-scroll rows already applied this drag */
    /* Close pressed but waiting for the finger to lift before exiting, so
     * Nickel doesn't inherit the still-pending TOUCH_UP as a tap on its own
     * search button in the same top-right area. */
    int      quit_pending;
} TouchState;

/* Refresh / partial-update bookkeeping: the per-screen "already painted once"
 * flags and the main-screen change-detection snapshot. */
typedef struct {
    /* The next main-screen full paint is the app-startup paint: full-screen
     * GC16 flash regardless of full_style. */
    int              startup_paint_pending;
    /* Set with startup_paint_pending at cache-load startup; the first successful
     * background fetch consumes it to force a full redraw (drawn_main.ok=0). */
    int              startup_wash_pending;
    /* Set after the first static GC16 paint of the hourly screen; cleared on
     * entry and after each scroll gesture so the next static draw flashes. */
    int              drawn_hourly;
    /* Tells the next hourly settle frame to do a full-screen GC16 flash to
     * clear A2/REAGL scroll ghosts, regardless of full_style/snap_scroll. */
    int              hourly_scroll_settle;
    /* Set when a scroll commits on hourly; consumed on leaving hourly (back,
     * kiosk entry, or app close) to force the next full redraw to flash. */
    int              hourly_scrolled;
    /* Set after the first static paint of the settings screen (entry). */
    int              drawn_settings;
    /* Set after the first paint of the demo screen (entry GC16 flash). */
    int              drawn_demo;
    /* Why the next draw_main_screen pass should force a full redraw; consumed
     * and cleared by draw_main_screen. */
    FullRedrawReason full_redraw_reason;
    time_t           daily_wash_after; /* no daily wash before this; reset to next_06am() */
    DrawnMain        drawn_main;
} RefreshState;

/* Session-only settings-screen interaction state (not persisted — the persisted
 * values live in UIState.persisted as a UISettings). */
typedef struct {
    int page;                 /* active settings tab: 0=General,1=Timers,2=Advanced */
    int rearm_timers;         /* a timer setter changed values; main.c re-arms the timerfds */
    /* settings_handle_touch records which row changed (index into the active
     * page's items[], -1 = none) so draw_settings_screen can capture that
     * row's freshly-computed rect instead of re-deriving it. */
    int dirty_item;
    int dirty_sub;            /* ITEM_ROW_GROUP: newly-checked sub_idx */
    int dirty_prev_sub;       /* ITEM_ROW_GROUP: previously-checked sub_idx, or -1 */
    int tab_switch;           /* tab-bar tap: next body redraw uses REAGL */
} SettingsScreenState;

/* Power-save / kiosk "weather frame" mode. */
typedef struct {
    int             kiosk;     /* render headerless */
    int             enter;     /* transient request flag; main.c enters power_save_loop() */
    time_t          slept_at;  /* wall-clock time kiosk sleep began (power_save_loop) */
    WakeRefreshNeed wake_need; /* pending heavy-refresh override for the next
                                   Settings/Hourly entry or app close */
} KioskState;

/* Settings "Demo" mode: cycles every Wsymb2 symbol (1..27) on a simulated
 * sleep screen every 2 s with synthetic data — see weather_fill_demo(). */
typedef struct {
    int enter;    /* request to enter the demo (settings "Play demo" button) */
    int symbol;   /* current Wsymb2 symbol 1..27 */
    int quit;     /* request to leave the demo */
} DemoState;

typedef struct {
    WeatherState  *state;
    const Config  *cfg;

    /* Geometry — scaled at init from UI_REF_* */
    int  screen_w, screen_h;
    int  header_h;
    int  home_top;    /* y where home/kiosk content begins (< header_h; no header band) */
    int  pad;
    int  x_half;
    int  big_icon;    /* large icon height (px) */
    int  big_icon_w;  /* large icon width  (px, differs from height after horizontal crop) */
    int  med_icon;    /* medium icon height (px) */
    int  med_icon_w;  /* medium icon width  (px) */
    int  small_icon;
    int  current_h;
    int  forecast_h;
    int  hourly_row_h;

    /* Runtime state */
    UIScreen   screen;
    int        selected_day;     /* index 0..2 when hourly screen active */
    int        fetch_in_progress;
    int        needs_redraw;

    /* Hourly scroll */
    ScrollState scroll;
    int         hourly_count;
    int         hourly_idx[24];

    const char *settings_path;   /* where the persisted settings live */

    /* Current WiFi/fetch stage (WIFI_STAGE_* from wifi.h); 0 = idle. */
    int         fetch_stage;

    /* Set when WiFi could not be enabled because Nickel has not initialised it
     * this boot. Shown persistently in the subtitle until a fetch succeeds. */
    int         wifi_needs_nickel;

    /* Set by the units toggle to request a fresh fetch on the next main loop. */
    int         trigger_fetch;

    DemoState   demo;

    /* Persisted settings — single source of truth, loaded/saved as one struct.
     * full_style/partial_style live here as int (config.h is UI-agnostic); cast
     * to RefreshStyle at use. */
    UISettings    persisted;

    /* Grouped sub-state (see the typedefs above). */
    TouchState    touch;
    RefreshState  refresh;
    SettingsScreenState settings_ui;   /* session-only settings-screen state */
    KioskState    power;
} UIState;

int  ui_init(UIState *ui, WeatherState *state, const Config *cfg);
void ui_cleanup(UIState *ui);

/* Sets ui->screen, logging the transition under --debug. */
void ui_set_screen(UIState *ui, UIScreen s);

void ui_draw(UIState *ui);

/* Enter the power-save (kiosk) layout from the current screen. Owns the
 * entry-refresh policy: a full redraw (RR_KIOSK_ENTRY) when coming from a
 * non-home layout, or when Entry full wash / sleep GC16 flash force one;
 * otherwise just erases the header band via the awake partial_style, leaving
 * the on-panel weather content untouched. */
void ui_enter_kiosk(UIState *ui);
/* Leave the power-save layout: restore the "≡ ☾ ✕" header band with a partial
 * GC16 no-flash refresh (content below is already on-panel). */
void ui_exit_kiosk(UIState *ui);

/* Debug-only: dump whatever is currently on the panel to `path` as a BMP
 * (see fb_dump_bmp). Returns 0 on success, -1 on failure. */
int  ui_dump_screenshot(const char *path);

/* Handle a touch event. Returns 1 if app should quit, 0 otherwise. */
int  ui_handle_touch(UIState *ui, const TouchEvent *ev);

#endif /* UI_H */
