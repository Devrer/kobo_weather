/* ui_internal.h — private header for the UI subsystem.
 * Included by every ui_*.c / screen_*.c translation unit, not main.c/powersave.c. */
#ifndef UI_INTERNAL_H
#define UI_INTERNAL_H

#include "ui.h"    /* UIState, UIScreen, Rect, DrawnMain, RefreshStyle, FB … */
#include "i18n.h"  /* Lang */
#include <fbink.h> /* FBInkConfig, FBInkOTConfig, FBInkRect, FONT_STYLE_T … */

/* Which icon-pack PNG set to decode from. Resolved once in ui_init(). */
typedef enum { ICONPACK_CLARA = 0, ICONPACK_LIBRA = 1 } IconPack;

/* The display is a single physical resource; owned and defined in ui.c. */
typedef struct {
    FB          fb;
    FBInkConfig cfg;
    int         fbink_fd;
    int         font_loaded;
    int         bold_loaded;
    float       scale;
    IconPack    icon_pack;
    /* When we seized the panel from Nickel, stamped in ui_init(). See
     * settle_since_panel_seized(). */
    struct timespec panel_seized_at;
} Display;
extern Display g_disp;

/* Scale a UI_REF_* reference pixel value to actual screen pixels. */
int sc(int ref);

/* ---- Type definitions shared across UI modules ----------------------------- */

/* Per-point visible ink offsets, measured at startup. Owned by ui_calib.c. */
typedef struct {
    int em_h;           /* full em-box height in px (from fbink_get_last_rect) */
    int ink_top_off;    /* em-top → first inked row of "Åjg0" (accent)    */
    int ink_bottom_off; /* em-top → last inked row+1 of "Åjg0" (descender) */
    int cap_off;        /* em-top → top of capitals/digits ("H0")         */
    int base_off;       /* em-top → baseline ("H0" bottom, no descender)  */
} FontBand;

typedef enum {
    FB_11PT = 0, FB_13PT, FB_14PT, FB_15PT,
    FB_16PT, FB_18PT, FB_20PT, FB_22PT, FB_24PT, FB_40PT, FB_48PT,
    FB_COUNT
} FontBandIdx;

typedef enum {
    WICON_CLEAR_NIGHT = 0, WICON_CLOUDY, WICON_FOG,
    WICON_HEAVY_RAIN, WICON_LIGHT_RAIN, WICON_LIGHT_SNOW,
    WICON_PARTLY_CLOUDY, WICON_RAIN, WICON_SLEET,
    WICON_SNOW, WICON_SUNNY, WICON_THUNDERSTORM,
    WICON_COUNT
} WeatherIconType;

typedef enum { ICON_SMALL = 0, ICON_MEDIUM = 1, ICON_LARGE = 2 } IconSize;

/* Decoded-icon cache entry. Owned by ui_icons.c. */
typedef struct {
    unsigned char *pixels;   /* 1 byte/px grayscale; NULL until first decode */
    int            w, h;
} IconCacheEntry;

/* Draw-mode flag passed to per-element draw functions:
 *   DRAW_PAINT (0): draw new content only; caller already cleared the region.
 *   DRAW_CLEAR (1): erase stale region then draw new content in the same pass. */
enum { DRAW_PAINT = 0, DRAW_CLEAR = 1 };

/* ---- Layout geometry structs (screen_main.c + ui_init) --------------------- */

typedef struct {
    int icon_x, icon_y;   /* top-left of large icon */
    int cond_y;           /* margins.top of condition text (centred below icon) */
    int sep_y;            /* top of the horizontal separator line */
    int wind_y;           /* margins.top of wind text (right-aligned, above sep) */
    int maxmin_y;         /* margins.top of today's high/low text */
    int temp_y;           /* margins.top of current temperature text */
} CurGeom;

typedef struct {
    int tx, tw;           /* text column x and width (inset by pad) */
    int label_y;          /* day-name label */
    int icon_x, icon_y;   /* medium icon */
    int cond_y;           /* condition text row */
    int temp_y;           /* max-temp text */
    int wind_y;           /* wind text */
} DayGeom;

/* Layout for one settings row/section (screen_settings.c). lbx/ltx (checkbox
 * x, label text x) only apply to ITEM_ROW/ITEM_ROW_GROUP. */
typedef struct {
    int lx, ly, lcw;       /* column origin + width */
    int row_h;             /* row/section height */
    int lbx, ltx;          /* checkbox x, label text x (rows only) */
} RowGeom;

/* ---- Prototypes: ui.c (shared helpers) --------------------------------- */
void settle_sleep_ms(int ms);
void settle_since_panel_seized(int target_ms);

/* ---- Prototypes: screen_main.c ----------------------------------------- */
int     cur_band_height(const UIState *ui);
DayGeom compute_day_geom(const UIState *ui, int col_x, int col_w);
void    draw_current_weather(UIState *ui);
void    draw_forecast(UIState *ui);
void    draw_subtitle(UIState *ui);
void    draw_main_screen(UIState *ui);

/* ---- Prototypes: screen_hourly.c --------------------------------------- */
void draw_hourly_screen(UIState *ui);

/* ---- Prototypes: screen_settings.c ------------------------------------- */
void draw_settings_screen(UIState *ui);
void settings_handle_touch(UIState *ui, const TouchEvent *ev);

/* ---- Prototypes: screen_demo.c ----------------------------------------- */
void draw_demo_screen(UIState *ui);

/* ---- Prototypes: ui_refresh.c ------------------------------------------ */

/* Inputs to the entry/in-place/tab-switch style decision, shared across screens. */
typedef struct {
    bool screen_change;   /* first paint of this screen (entry transition)  */
    bool tab_switch;      /* settings: switching tabs (force REAGL)          */
    bool snap_reagl;      /* hourly: snap_scroll == SCROLL_SNAP_REAGL        */
    RefreshStyle full_style, partial_style;
} RefreshCtx;

RefreshStyle resolve_refresh_style(const RefreshCtx *c);

/* The single point where the gc16_flash setting meets a refresh. */
void ui_region_commit(const UIState *ui, Rect r, RefreshStyle style);

/* Resolves a base RefreshStyle to its sleep-context literal variant. */
RefreshStyle kiosk_resolve_style(const UIState *ui, RefreshStyle base);

/* INIT-on-white clear + settle. Must run before content is drawn/re-committed. */
void ui_run_init_wash(UIState *ui);

/* Resolved pending WakeRefreshNeed; override is false when nothing was pending. */
typedef struct { bool override; bool init_wash; RefreshStyle style; } WakeOverride;

/* Consumes (zeroes) ui->power.wake_need, resolving it to a one-shot override
 * for the caller's next full-screen commit. */
WakeOverride ui_consume_wake_override(UIState *ui);

/* ---- Prototypes: ui_calib.c -------------------------------------------- */
const FontBand *band(int pt);
int center_y(int row_y, int row_h, int pt);
void calibrate_fonts(UIState *ui);
int  load_band_cache(const char *font_path);
void save_band_cache(const char *font_path);

/* ---- Prototypes: ui_format.c ------------------------------------------- */
const char  *temp_unit_suffix(UnitSystem u);
const char  *wind_unit_str(UnitSystem u);
const char  *day_label(Lang lang, int day_offset);
void         format_long_date(Lang lang, int day_offset, char *buf, size_t n);
WeatherHour *find_now(const WeatherState *st);
void fmt_temp(char *buf, int n, const WeatherHour *h, UnitSystem units);
void fmt_cond(Lang lang, char *buf, int n, const WeatherHour *h);
void fmt_day_wind(Lang lang, char *buf, int n, const WeatherHour *h, UnitSystem units);
void compute_day_maxmin(const WeatherState *st, int day_offset,
                        char *tmax, int tn, char *tmin, int mn);
void fmt_cur_maxmin(char *buf, int n, const WeatherState *st);
void build_subtitle_str(char *buf, int n, const UIState *ui);

/* ---- Prototypes: ui_widgets.c ------------------------------------------ */
void draw_header(UIState *ui, const char *title, int show_back,
                 int show_settings, int show_power);
int  hit_close_button(UIState *ui, int tx, int ty);
int  hit_powersave_button(UIState *ui, int tx, int ty);
int  hit_top_left_button(UIState *ui, int tx, int ty);

/* ---- Prototypes: ui_icons.c -------------------------------------------- */
void icon_cache_free(void);
int  draw_icon(int wsymb, time_t valid_time, int x, int y, int row_h, IconSize size);

/* ---- Prototypes: ui_text.c --------------------------------------------- */
Rect fbink_last_rect(void);
Rect rect_union(Rect a, Rect b);
void draw_text(int size_pt, int x, int y_top, int max_w, const char *s);
void draw_text_clipped(int size_pt, int x, int y_top, int max_w, int max_h,
                        const char *s);
void draw_text_right_styled(int size_pt, int right_x, int y_top, int min_x,
                            const char *s, FONT_STYLE_T style);
void draw_text_right(int size_pt, int right_x, int y_top, int min_x,
                     const char *s);
int  measure_text_w_styled(int size_pt, const char *s, FONT_STYLE_T style);
int  min_temp_pt(const UIState *ui);
FONT_STYLE_T temp_style(int on);
void draw_text_centered_styled(int size_pt, int x_left, int y_top, int box_w,
                               const char *s, FONT_STYLE_T style);
void draw_text_centered(int size_pt, int x_left, int y_top, int box_w,
                        const char *s);
Rect draw_text_centered_wrapped(int size_pt, Rect slot, int line_gap,
                                const char *s, FONT_STYLE_T style);

#endif /* UI_INTERNAL_H */
