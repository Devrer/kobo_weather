#include "ui_internal.h"
#include "weather.h"
#include "sysutil.h"   /* sysutil_log for the --debug field-change line */
#include <stdio.h>
#include <string.h>

/* ---- Geometry helpers ---------------------------------------------- */

static CurGeom compute_cur_geom(const UIState *ui) {
    CurGeom g;
    int y0   = ui->home_top;
    g.icon_x = ui->pad + sc(UI_REF_CUR_SIDE_INSET);
    g.icon_y = y0 + sc(UI_REF_CUR_ICON_TOP);

    /* Condition occupies a fixed two-line-tall slot starting at cond_y (its
     * top edge, anchored the same gap below the icon as a single line's
     * cap-top would be) so that nothing below it ever moves, whether the
     * text wraps or not — see draw_text_centered_wrapped(). */
    const FontBand *b_cond = band(UI_FONT_COND_PT);
    g.cond_y = g.icon_y + ui->big_icon
             + sc(UI_REF_CUR_COND_ICON_GAP)
             - b_cond->cap_off;

    /* Separator below the condition slot's fixed bottom edge. The slot is
     * 2*em_h + cond_gap tall so it matches what draw_text_centered_wrapped
     * draws (line 2 lands at slot_y + em_h + cond_gap). */
    int cond_gap = sc(UI_REF_COND_LINE_GAP);
    g.sep_y  = g.cond_y + 2 * b_cond->em_h + cond_gap + sc(UI_REF_CUR_COND_SEP_GAP);

    /* Right-aligned stack, bottom-up from sep (all gaps = exact visible px). */
    g.wind_y   = g.sep_y
               - sc(UI_REF_CUR_SEP_WIND_GAP)
               - band(UI_FONT_WIND_PT)->base_off;
    /* maxmin_y anchors the larger "-max°C" piece (UI_FONT_TEMP_HI_PT); the
     * smaller "min" piece is baseline-aligned to it in draw_cur_maxmin(). */
    g.maxmin_y = g.wind_y
               + band(UI_FONT_WIND_PT)->cap_off
               - sc(UI_REF_CUR_WIND_MAXMIN_GAP)
               - band(UI_FONT_TEMP_HI_PT)->base_off;
    g.temp_y   = g.maxmin_y
               + band(UI_FONT_TEMP_HI_PT)->cap_off
               - sc(UI_REF_CUR_MAXMIN_TEMP_GAP)
               - band(UI_FONT_TEMP_BIG_PT)->base_off;

    return g;
}

/* Band height derived from the separator position (top of the 2px line). */
int cur_band_height(const UIState *ui) {
    return compute_cur_geom(ui).sep_y + UI_REF_SEP_THICK - ui->home_top;
}

DayGeom compute_day_geom(const UIState *ui, int col_x, int col_w) {
    DayGeom g;
    int y0    = ui->home_top + ui->current_h;
    g.tx      = col_x + ui->pad;
    g.tw      = col_w - ui->pad * 2;
    g.label_y = y0 + ui->pad + sc(UI_REF_FORECAST_TOP_INSET);
    g.icon_x  = col_x + (col_w - ui->med_icon_w) / 2;

    /* Each gap constant is an exact visible-pixel distance (cap-height→baseline
     * arithmetic, same convention as compute_cur_geom):
     *   icon_y  : label baseline      → icon top
     *   cond_y  : icon bottom         → cond slot top (cond cap-top, single line)
     *   temp_y  : cond slot bottom    → max-temp cap-top
     *   wind_y  : min baseline        → wind cap-top
     * The condition occupies a fixed two-line-tall slot starting at cond_y so
     * that nothing below it ever moves, whether the text wraps or not — see
     * draw_text_centered_wrapped(). */
    const FontBand *b14 = band(UI_FONT_DAY_PT);
    const FontBand *b_cond = band(UI_FONT_DAY_COND_PT);
    const FontBand *b24 = band(UI_FONT_TEMP_HI_PT);       /* max temp */
    const FontBand *b_lo = band(min_temp_pt(ui));         /* min temp (size depends on toggle) */

    g.icon_y = g.label_y + b14->base_off + sc(UI_REF_DAY_ICON_GAP);
    g.cond_y = g.icon_y  + ui->med_icon  + sc(UI_REF_DAY_ICON_COND_GAP) - b_cond->cap_off;
    int cond_gap = sc(UI_REF_COND_LINE_GAP);
    g.temp_y = g.cond_y  + 2 * b_cond->em_h + cond_gap + sc(UI_REF_DAY_COND_TEMP_GAP) - b24->cap_off;

    /* Estimate min_y the same way draw_day_maxmin_el() computes it: sep at
     * em-box bottom of max (temp_y + em_h) + pad, min below sep + pad. min's
     * own size (b22) only matters for its baseline, used below for wind_y. */
    int min_y_est = g.temp_y + b24->em_h
                  + 2 * sc(UI_REF_DAY_MAXMIN_SEP_PAD) + sc(UI_REF_SEP_THICK);
    g.wind_y = min_y_est + b_lo->base_off - b14->cap_off + sc(UI_REF_DAY_MIN_WIND_GAP);

    return g;
}

/* ---- Per-element draw functions (return their bounding Rect) --------- */
static Rect draw_cur_icon(UIState *ui, const CurGeom *g, const WeatherHour *cur, int mode) {
    int do_clear = mode != DRAW_PAINT;
    Rect r = { g->icon_x, g->icon_y, ui->big_icon_w, ui->big_icon };
    if (do_clear) {
        fb_fill_rect(&g_disp.fb, r, UI_BG);
    }
    if (cur && cur->has_symbol)
        draw_icon(cur->symbol, cur->valid_time, g->icon_x, g->icon_y, 0, ICON_LARGE);
    return r;
}

static Rect draw_cur_cond(UIState *ui, const CurGeom *g, const WeatherHour *cur, int mode) {
    int do_clear = mode != DRAW_PAINT;
    Rect old = ui->refresh.drawn_main.last_cur_cond_rect;
    if (do_clear && old.w > 0)
        fb_fill_rect(&g_disp.fb, old, UI_BG);
    char buf[32]; fmt_cond((Lang)ui->persisted.lang, buf, sizeof buf, cur);
    Rect measured = (Rect){0,0,0,0};
    if (buf[0]) {
        int m = sc(UI_REF_CUR_COND_SIDE_MARGIN);
        int cond_gap = sc(UI_REF_COND_LINE_GAP);
        measured = draw_text_centered_wrapped(UI_FONT_COND_PT,
                                              (Rect){g->icon_x - m, g->cond_y,
                                                     ui->big_icon_w + 2 * m,
                                                     2 * band(UI_FONT_COND_PT)->em_h + cond_gap},
                                              cond_gap, buf, FNT_REGULAR);
    }
    ui->refresh.drawn_main.last_cur_cond_rect = measured;
    return rect_union(old, measured);
}

/* The big current temperature is right-aligned, so its trailing "°C" unit always
 * lands at the same x. We draw the unit and the digits as two separate
 * right-aligned draws (the digits' right edge meets the unit's pen start, so the
 * result is identical to one combined string). The unit is repainted only on a
 * full redraw (do_clear==0); a partial temp update (do_clear==1) repaints just
 * the digits and leaves "°C" untouched on the panel — no needless wear on it.
 * last_cur_temp_rect therefore tracks the DIGIT rect only. */
static Rect draw_cur_temp(UIState *ui, const CurGeom *g, const WeatherHour *cur, int mode) {
    int do_clear = mode != DRAW_PAINT;
    int right_x = ui->screen_w - ui->pad - sc(UI_REF_CUR_SIDE_INSET);
    int min_x   = g->icon_x + ui->big_icon_w + ui->pad;

    char buf[32]; fmt_temp(buf, sizeof buf, cur, ui->state->units);
    /* Split off the trailing "°C" (degree sign = UTF-8 \xc2\xb0). */
    const char *deg = strstr(buf, "\xc2\xb0");
    char digits[32];
    const char *unit;
    if (deg) {
        size_t n = (size_t)(deg - buf);
        memcpy(digits, buf, n); digits[n] = '\0';
        unit = deg;
    } else {
        snprintf(digits, sizeof digits, "%s", buf);
        unit = "";
    }

    FONT_STYLE_T st = temp_style(ui->persisted.bold_cur_temp);
    int digit_right = right_x - measure_text_w_styled(UI_FONT_TEMP_BIG_PT, unit, st);

    /* Unit: always on a full redraw; on a partial update only when the unit string
     * itself changed (e.g. a °C↔°F switch). Skipping it on value-only ticks avoids
     * needless panel wear. cur_temp in the snapshot still holds the previous string
     * at this point (snapshot_drawn_main runs after the partial pass). */
    int unit_changed = 0;
    if (do_clear && unit[0]) {
        const char *old_deg  = strstr(ui->refresh.drawn_main.cur_temp, "\xc2\xb0");
        const char *old_unit = old_deg ? old_deg : "";
        unit_changed = (strcmp(old_unit, unit) != 0);
    }
    Rect unit_dirty = {0, 0, 0, 0};
    if ((!do_clear || unit_changed) && unit[0]) {
        if (do_clear && ui->refresh.drawn_main.last_cur_unit_rect.w > 0) {
            Rect u = ui->refresh.drawn_main.last_cur_unit_rect;
            fb_fill_rect(&g_disp.fb, u, UI_BG);
            unit_dirty = u;
        }
        draw_text_right_styled(UI_FONT_TEMP_BIG_PT, right_x, g->temp_y, min_x, unit, st);
        ui->refresh.drawn_main.last_cur_unit_rect = fbink_last_rect();
        unit_dirty = rect_union(unit_dirty, ui->refresh.drawn_main.last_cur_unit_rect);
    }

    /* Digits: clear the previous digit rect (partial), then redraw. */
    Rect old = ui->refresh.drawn_main.last_cur_temp_rect;
    if (do_clear && old.w > 0)
        fb_fill_rect(&g_disp.fb, old, UI_BG);
    Rect dr = (Rect){0, 0, 0, 0};
    if (digits[0]) {
        draw_text_right_styled(UI_FONT_TEMP_BIG_PT, digit_right, g->temp_y, min_x, digits, st);
        dr = fbink_last_rect();
    }
    ui->refresh.drawn_main.last_cur_temp_rect = dr;
    return rect_union(rect_union(old, dr), unit_dirty);
}

static Rect draw_cur_wind(UIState *ui, const CurGeom *g, const WeatherHour *cur, int mode) {
    int do_clear = mode != DRAW_PAINT;
    Rect old = ui->refresh.drawn_main.last_cur_wind_rect;
    if (do_clear && old.w > 0)
        fb_fill_rect(&g_disp.fb, old, UI_BG);
    char buf[64]; fmt_day_wind((Lang)ui->persisted.lang, buf, sizeof buf, cur, ui->state->units);
    Rect measured = (Rect){0,0,0,0};
    if (buf[0]) {
        draw_text_right(UI_FONT_WIND_PT,
                        ui->screen_w - ui->pad - sc(UI_REF_CUR_SIDE_INSET),
                        g->wind_y, g->icon_x + ui->big_icon_w + ui->pad, buf);
        measured = fbink_last_rect();
    }
    ui->refresh.drawn_main.last_cur_wind_rect = measured;
    return rect_union(old, measured);
}

/* Today's high/low is shown as "low-high°C" with the high value (and its
 * "-...°C" trappings) drawn larger than the low — e.g. "12" at
 * UI_FONT_TEMP_LO_PT next to "-19°C" at UI_FONT_TEMP_HI_PT. Drawn as two
 * right-anchored pieces whose edges meet exactly (the same technique
 * draw_cur_temp() uses to split off its trailing "°C" unit), with the
 * smaller "low" piece baseline-aligned to the larger "-high°C" piece. */
static Rect draw_cur_maxmin(UIState *ui, const CurGeom *g, int mode) {
    int do_clear = mode != DRAW_PAINT;
    Rect old = ui->refresh.drawn_main.last_cur_maxmin_rect;
    if (do_clear && old.w > 0)
        fb_fill_rect(&g_disp.fb, old, UI_BG);

    char hi[16], lo[16];
    compute_day_maxmin(ui->state, 0, hi, sizeof hi, lo, sizeof lo);
    Rect measured = (Rect){0,0,0,0};
    if (hi[0] && lo[0]) {
        /* The divider "/" trails the low value with no space (e.g. "12°C/");
         * the high value stands alone ("19°C"). Drawn as two right-anchored
         * pieces whose edges meet, the smaller low piece left of the high. */
        char lo_piece[20];
        snprintf(lo_piece, sizeof lo_piece, "%s/", lo);

        const int hi_pt   = UI_FONT_TEMP_HI_PT;
        const int lo_pt   = min_temp_pt(ui);
        const FONT_STYLE_T hi_st = temp_style(ui->persisted.bold_max_temp);
        const int min_x   = g->icon_x + ui->big_icon_w + ui->pad;
        const int right_x = ui->screen_w - ui->pad - sc(UI_REF_CUR_SIDE_INSET);

        /* Baseline-align the smaller "low" piece to the larger "high°C" one
         * (both share band em-top → baseline offsets from g->maxmin_y). */
        int lo_y = g->maxmin_y + band(hi_pt)->base_off - band(lo_pt)->base_off;

        int hi_right = right_x;
        int lo_right = hi_right - measure_text_w_styled(hi_pt, hi, hi_st);

        draw_text_right_styled(hi_pt, hi_right, g->maxmin_y, min_x, hi, hi_st);
        measured = fbink_last_rect();

        draw_text_right(lo_pt, lo_right, lo_y, min_x, lo_piece);
        measured = rect_union(measured, fbink_last_rect());
    }
    ui->refresh.drawn_main.last_cur_maxmin_rect = measured;
    return rect_union(old, measured);
}

static Rect draw_day_icon_el(UIState *ui, const DayGeom *g, int idx, int mode) {
    int do_clear = mode != DRAW_PAINT;
    Rect r = { g->icon_x, g->icon_y, ui->med_icon_w, ui->med_icon };
    if (do_clear) fb_fill_rect(&g_disp.fb, r, UI_BG);
    if (idx >= 0 && ui->state->hours[idx].has_symbol)
        draw_icon(ui->state->hours[idx].symbol, ui->state->hours[idx].valid_time,
                  g->icon_x, g->icon_y, 0, ICON_MEDIUM);
    return r;
}

static Rect draw_day_label_el(UIState *ui, int col_x, int col_w, const DayGeom *g,
                              int day_offset, int mode) {
    int do_clear = mode != DRAW_PAINT;
    int day_col = (col_w > 0) ? (col_x / col_w) : 0;
    if (day_col > 2) day_col = 2;
    Rect old = ui->refresh.drawn_main.last_day_label_rect[day_col];
    if (do_clear && old.w > 0)
        fb_fill_rect(&g_disp.fb, old, UI_BG);
    draw_text_centered(UI_FONT_DAY_PT, g->tx, g->label_y, g->tw, day_label((Lang)ui->persisted.lang, day_offset));
    Rect measured = fbink_last_rect();
    ui->refresh.drawn_main.last_day_label_rect[day_col] = measured;
    return rect_union(old, measured);
}

static Rect draw_day_cond_el(UIState *ui, int col_x, int col_w, const DayGeom *g,
                             int idx, int mode) {
    int do_clear = mode != DRAW_PAINT;
    int day_col = (col_w > 0) ? (col_x / col_w) : 0;
    if (day_col > 2) day_col = 2;
    Rect old = ui->refresh.drawn_main.last_day_cond_rect[day_col];
    if (do_clear && old.w > 0)
        fb_fill_rect(&g_disp.fb, old, UI_BG);
    Rect measured = (Rect){0,0,0,0};
    if (idx >= 0 && ui->state->hours[idx].has_symbol) {
        char buf[32]; fmt_cond((Lang)ui->persisted.lang, buf, sizeof buf, &ui->state->hours[idx]);
        if (buf[0]) {
            int m = sc(UI_REF_DAY_COND_SIDE_MARGIN);
            int cond_gap = sc(UI_REF_COND_LINE_GAP);
            measured = draw_text_centered_wrapped(UI_FONT_DAY_COND_PT,
                                                  (Rect){g->tx - m, g->cond_y,
                                                         g->tw + 2 * m,
                                                         2 * band(UI_FONT_DAY_COND_PT)->em_h + cond_gap},
                                                  cond_gap, buf, FNT_REGULAR);
        }
    }
    ui->refresh.drawn_main.last_day_cond_rect[day_col] = measured;
    return rect_union(old, measured);
}

static Rect draw_day_maxmin_el(UIState *ui, int col_x, int col_w, const DayGeom *g,
                               const char *tmax, const char *tmin, int mode) {
    int do_clear = mode != DRAW_PAINT;
    int day_col = (col_w > 0) ? (col_x / col_w) : 0;
    if (day_col > 2) day_col = 2;
    Rect old = ui->refresh.drawn_main.last_day_maxmin_rect[day_col];
    if (do_clear && old.w > 0)
        fb_fill_rect(&g_disp.fb, old, UI_BG);
    FONT_STYLE_T hi_st = temp_style(ui->persisted.bold_max_temp);
    /* Max shown larger than min — same size pairing as today's "low-high°C". */
    if (tmax[0])
        draw_text_centered_styled(UI_FONT_TEMP_HI_PT, g->tx, g->temp_y, g->tw, tmax, hi_st);
    else {
        char ph[16];
        snprintf(ph, sizeof ph, "---%s", temp_unit_suffix(ui->state->units));
        draw_text_centered_styled(UI_FONT_TEMP_HI_PT, g->tx, g->temp_y, g->tw, ph, hi_st);
    }
    Rect measured;
    if (tmax[0] && tmin[0]) {
        Rect lr_max = fbink_last_rect();
        int sep_y = lr_max.y + lr_max.h + sc(UI_REF_DAY_MAXMIN_SEP_PAD);
        int sep_w = g->tw / 2;
        fb_fill_rect(&g_disp.fb, (Rect){g->tx + (g->tw - sep_w) / 2, sep_y, sep_w, sc(UI_REF_SEP_THICK)}, UI_FG_SOFT);
        int min_y = sep_y + sc(UI_REF_SEP_THICK) + sc(UI_REF_DAY_MAXMIN_SEP_PAD);

        draw_text_centered(min_temp_pt(ui), g->tx, min_y, g->tw, tmin);
        Rect lr_min = fbink_last_rect();
        measured = (Rect){ g->tx, g->temp_y, g->tw, (lr_min.y + lr_min.h) - g->temp_y };
    } else {
        measured = fbink_last_rect();
    }
    ui->refresh.drawn_main.last_day_maxmin_rect[day_col] = measured;
    return rect_union(old, measured);
}

static Rect draw_day_wind_el(UIState *ui, int col_x, int col_w, const DayGeom *g,
                             int idx, int mode) {
    int do_clear = mode != DRAW_PAINT;
    int day_col = (col_w > 0) ? (col_x / col_w) : 0;
    if (day_col > 2) day_col = 2;
    Rect old = ui->refresh.drawn_main.last_day_wind_rect[day_col];
    if (do_clear && old.w > 0)
        fb_fill_rect(&g_disp.fb, old, UI_BG);
    char buf[64];
    fmt_day_wind((Lang)ui->persisted.lang, buf, sizeof buf, idx >= 0 ? &ui->state->hours[idx] : NULL, ui->state->units);
    Rect measured = (Rect){0,0,0,0};
    if (buf[0]) {
        draw_text_centered(UI_FONT_DAY_WIND_PT, g->tx, g->wind_y, g->tw, buf);
        measured = fbink_last_rect();
    }
    ui->refresh.drawn_main.last_day_wind_rect[day_col] = measured;
    return rect_union(old, measured);
}

/* ---- Full-redraw wrappers ------------------------------------------ */

void draw_current_weather(UIState *ui) {
    int y0 = ui->home_top;
    CurGeom g = compute_cur_geom(ui);
    fb_fill_rect(&g_disp.fb, (Rect){0, y0, ui->screen_w, ui->current_h}, UI_BG);
    WeatherHour *cur = find_now(ui->state);
    draw_cur_icon(ui, &g, cur, 0);
    draw_cur_cond(ui, &g, cur, 0);
    draw_cur_maxmin(ui, &g, 0);
    draw_cur_temp(ui, &g, cur, 0);
    draw_cur_wind(ui, &g, cur, 0);
    int sep_w = sc(UI_REF_MAIN_H_SEP_W);
    fb_fill_rect(&g_disp.fb, (Rect){(ui->screen_w - sep_w) / 2,
                 g.sep_y, sep_w, UI_REF_SEP_THICK}, UI_SEP);
}

static DayGeom draw_day_column(UIState *ui, int day_offset, int col_x, int col_w) {
    DayGeom g = compute_day_geom(ui, col_x, col_w);
    int idx = weather_pick_noon(ui->state, day_offset);
    char tmax[16], tmin[16];
    compute_day_maxmin(ui->state, day_offset, tmax, sizeof tmax, tmin, sizeof tmin);
    draw_day_label_el (ui, col_x, col_w, &g, day_offset, 0);
    draw_day_icon_el  (ui, &g, idx, 0);
    draw_day_cond_el  (ui, col_x, col_w, &g, idx, 0);
    draw_day_maxmin_el(ui, col_x, col_w, &g, tmax, tmin, 0);
    draw_day_wind_el  (ui, col_x, col_w, &g, idx, 0);
    return g;
}

void draw_forecast(UIState *ui) {
    int y0 = ui->home_top + ui->current_h;
    fb_fill_rect(&g_disp.fb, (Rect){0, y0, ui->screen_w, ui->forecast_h}, UI_BG);
    int col_w = ui->screen_w / 3;
    for (int d = 0; d < 3; d++) {
        int col_x = d * col_w;
        DayGeom vsep = draw_day_column(ui, d + 1, col_x, col_w);
        if (d < 2) {
            int vsep_top = vsep.icon_y;
            int vsep_bot = vsep.wind_y;
            fb_fill_rect(&g_disp.fb, (Rect){col_x + col_w - UI_REF_SEP_THICK / 2,
                         vsep_top, UI_REF_SEP_THICK,
                         vsep_bot - vsep_top}, UI_SEP);
        }
    }
}

void draw_subtitle(UIState *ui) {
    char buf[96];
    build_subtitle_str(buf, sizeof buf, ui);
    int y = ui->screen_h - sc(UI_REF_SUBTITLE_BOTTOM);
    draw_text_centered(UI_FONT_PLACE_PT, 0, y, ui->screen_w, buf);
    ui->refresh.drawn_main.last_subtitle_rect = fbink_last_rect();
}

/* ---- Partial-update helpers ---------------------------------------- */

/* --debug aid: record which named fields a partial update redrew. The per-field
 * change detection already lives in partial_update_*; this only collects the
 * names so draw_main_screen can log "fields: cur_temp d2_icon …" alongside the
 * merged-bbox REFRESH line, making it obvious which fields fused into one flash
 * rect. Passed by pointer (NULL when --debug is off) rather than via a global,
 * so the accumulator is owned by the single update pass that fills it. */
typedef struct { char buf[256]; size_t len; } FieldLog;

static void field_log_add(FieldLog *fl, const char *name) {
    if (!fl) return;
    size_t avail = sizeof fl->buf - fl->len;
    int n = snprintf(fl->buf + fl->len, avail, "%s%s", fl->len ? " " : "", name);
    if (n > 0 && (size_t)n < avail) fl->len += (size_t)n;
}

static void snapshot_drawn_main(UIState *ui) {
    DrawnMain *d = &ui->refresh.drawn_main;
    Lang lang = (Lang)ui->persisted.lang;
    WeatherHour *cur = find_now(ui->state);
    d->cur_symbol = (cur && cur->has_symbol) ? cur->symbol : -1;
    fmt_temp(d->cur_temp, sizeof d->cur_temp, cur, ui->state->units);
    fmt_cur_maxmin(d->cur_maxmin, sizeof d->cur_maxmin, ui->state);
    fmt_day_wind(lang, d->cur_wind, sizeof d->cur_wind, cur, ui->state->units);
    fmt_cond(lang, d->cur_cond, sizeof d->cur_cond, cur);
    for (int i = 0; i < 3; i++) {
        int idx = weather_pick_noon(ui->state, i + 1);
        const WeatherHour *h = (idx >= 0) ? &ui->state->hours[idx] : NULL;
        d->day_symbol[i] = (h && h->has_symbol) ? h->symbol : -1;
        fmt_cond    (lang, d->day_cond[i], sizeof d->day_cond[i], h);
        fmt_day_wind(lang, d->day_wind[i], sizeof d->day_wind[i], h, ui->state->units);
        compute_day_maxmin(ui->state, i + 1,
                           d->day_tmax[i], sizeof d->day_tmax[i],
                           d->day_tmin[i], sizeof d->day_tmin[i]);
        snprintf(d->day_label_str[i], sizeof d->day_label_str[i],
                 "%s", day_label(lang, i + 1));
    }
    build_subtitle_str(d->subtitle, sizeof d->subtitle, ui);
    d->ok = 1;
}

static void partial_update_cur(UIState *ui, Rect *dirty, int mode, FieldLog *fl) {
    DrawnMain   *d   = &ui->refresh.drawn_main;
    WeatherHour *cur = find_now(ui->state);
    CurGeom      g   = compute_cur_geom(ui);

    int new_sym = (cur && cur->has_symbol) ? cur->symbol : -1;
    if (new_sym != d->cur_symbol) {
        *dirty = rect_union(*dirty, draw_cur_icon(ui, &g, cur, mode));
        field_log_add(fl, "cur_icon");
        d->cur_symbol = new_sym;
    }

    char s[64];
    fmt_cond((Lang)ui->persisted.lang, s, sizeof s, cur);
    if (strcmp(s, d->cur_cond) != 0) {
        *dirty = rect_union(*dirty, draw_cur_cond(ui, &g, cur, mode));
        field_log_add(fl, "cur_cond");
        fmt_cond((Lang)ui->persisted.lang, d->cur_cond, sizeof d->cur_cond, cur);
    }

    fmt_cur_maxmin(s, sizeof s, ui->state);
    if (strcmp(s, d->cur_maxmin) != 0) {
        *dirty = rect_union(*dirty, draw_cur_maxmin(ui, &g, mode));
        field_log_add(fl, "cur_maxmin");
        fmt_cur_maxmin(d->cur_maxmin, sizeof d->cur_maxmin, ui->state);
    }

    fmt_temp(s, sizeof s, cur, ui->state->units);
    if (strcmp(s, d->cur_temp) != 0) {
        *dirty = rect_union(*dirty, draw_cur_temp(ui, &g, cur, mode));
        field_log_add(fl, "cur_temp");
        fmt_temp(d->cur_temp, sizeof d->cur_temp, cur, ui->state->units);
    }

    fmt_day_wind((Lang)ui->persisted.lang, s, sizeof s, cur, ui->state->units);
    if (strcmp(s, d->cur_wind) != 0) {
        *dirty = rect_union(*dirty, draw_cur_wind(ui, &g, cur, mode));
        field_log_add(fl, "cur_wind");
        snprintf(d->cur_wind, sizeof d->cur_wind, "%s", s);
    }
}

static void partial_update_forecast(UIState *ui, Rect *dirty, int mode, FieldLog *fl) {
    DrawnMain *d     = &ui->refresh.drawn_main;
    int        col_w = ui->screen_w / 3;

    Lang lang = (Lang)ui->persisted.lang;
    for (int i = 0; i < 3; i++) {
        int col_x = i * col_w;
        DayGeom day_g = compute_day_geom(ui, col_x, col_w);
        int idx   = weather_pick_noon(ui->state, i + 1);
        const WeatherHour *h = (idx >= 0) ? &ui->state->hours[idx] : NULL;
        char s[64];
        char fn[16];

        const char *lbl = day_label(lang, i + 1);
        if (strcmp(lbl, d->day_label_str[i]) != 0) {
            *dirty = rect_union(*dirty,
                         draw_day_label_el(ui, col_x, col_w, &day_g, i + 1, mode));
            snprintf(fn, sizeof fn, "d%d_label", i + 1); field_log_add(fl, fn);
            snprintf(d->day_label_str[i], sizeof d->day_label_str[i], "%s", lbl);
        }

        int new_sym = (h && h->has_symbol) ? h->symbol : -1;
        if (new_sym != d->day_symbol[i]) {
            *dirty = rect_union(*dirty, draw_day_icon_el(ui, &day_g, idx, mode));
            snprintf(fn, sizeof fn, "d%d_icon", i + 1); field_log_add(fl, fn);
            d->day_symbol[i] = new_sym;
        }

        fmt_cond(lang, s, sizeof s, h);
        if (strcmp(s, d->day_cond[i]) != 0) {
            *dirty = rect_union(*dirty,
                         draw_day_cond_el(ui, col_x, col_w, &day_g, idx, mode));
            snprintf(fn, sizeof fn, "d%d_cond", i + 1); field_log_add(fl, fn);
            fmt_cond(lang, d->day_cond[i], sizeof d->day_cond[i], h);
        }

        char tmax[16], tmin[16];
        compute_day_maxmin(ui->state, i + 1, tmax, sizeof tmax, tmin, sizeof tmin);
        if (strcmp(tmax, d->day_tmax[i]) != 0 || strcmp(tmin, d->day_tmin[i]) != 0) {
            *dirty = rect_union(*dirty,
                         draw_day_maxmin_el(ui, col_x, col_w, &day_g, tmax, tmin, mode));
            snprintf(fn, sizeof fn, "d%d_maxmin", i + 1); field_log_add(fl, fn);
            snprintf(d->day_tmax[i], sizeof d->day_tmax[i], "%s", tmax);
            snprintf(d->day_tmin[i], sizeof d->day_tmin[i], "%s", tmin);
        }

        fmt_day_wind(lang, s, sizeof s, h, ui->state->units);
        if (strcmp(s, d->day_wind[i]) != 0) {
            *dirty = rect_union(*dirty,
                         draw_day_wind_el(ui, col_x, col_w, &day_g, idx, mode));
            snprintf(fn, sizeof fn, "d%d_wind", i + 1); field_log_add(fl, fn);
            fmt_day_wind(lang, d->day_wind[i], sizeof d->day_wind[i], h, ui->state->units);
        }
    }
}

static void partial_update_subtitle(UIState *ui, Rect *dirty, int mode, FieldLog *fl) {
    DrawnMain *d = &ui->refresh.drawn_main;
    char s[96]; build_subtitle_str(s, sizeof s, ui);
    if (strcmp(s, d->subtitle) == 0)
        return;
    field_log_add(fl, "subtitle");

    int do_clear = mode != DRAW_PAINT;
    Rect old = d->last_subtitle_rect;
    if (do_clear && old.w > 0)
        fb_fill_rect(&g_disp.fb, old, UI_BG);
    int y = ui->screen_h - sc(UI_REF_SUBTITLE_BOTTOM);
    draw_text_centered(UI_FONT_PLACE_PT, 0, y, ui->screen_w, s);
    d->last_subtitle_rect = fbink_last_rect();
    snprintf(d->subtitle, sizeof d->subtitle, "%s", s);
    *dirty = rect_union(*dirty, old);
    *dirty = rect_union(*dirty, d->last_subtitle_rect);
}

/* What a full redraw should do, resolved from *why* it was requested.
 * init_wash: run the INIT-on-white clear before content is drawn.
 * style: the commit style for the post-content ui_region_commit. */
typedef struct { bool init_wash; RefreshStyle style; } FullRedraw;

static FullRedraw resolve_full_redraw(FullRedrawReason reason, SleepEntryWash sleep_entry_wash,
                                      RefreshStyle full_style, bool sleep_gc16_flash) {
    switch (reason) {
    case RR_DAILY_WASH:
        /* 06:00 daily wash always gets the deep INIT-on-white clean,
         * regardless of the entry setting. */
        return (FullRedraw){ true, RSTYLE_GC16_NOFLASH };
    case RR_KIOSK_ENTRY:
        /* Going to sleep with a full redraw. The sleep-entry tri-state owns this:
         * WASH = deepest INIT-on-white clean; FLASH = single full-screen GC16
         * flash; NONE = follow the awake full_style (REAGL or GC16). */
        switch (sleep_entry_wash) {
        case SLEEP_ENTRY_WASH:  return (FullRedraw){ true,  RSTYLE_GC16_NOFLASH };
        case SLEEP_ENTRY_FLASH: return (FullRedraw){ false, RSTYLE_GC16_FLASH };
        default:                return (FullRedraw){ false, full_style };
        }
    case RR_TICK_WAKE:
        /* The periodic kiosk wake-tick never INITs; whether it flashes is
         * governed by sleep_gc16_flash (the sleep-context counterpart to the
         * awake-only gc16_flash) instead of a hardcoded never-flash. */
        return (FullRedraw){ false,
            sleep_gc16_flash ? RSTYLE_GC16_FLASH : RSTYLE_GC16_NOFLASH };
    case RR_STARTUP:
        /* The app-startup first paint: never INIT, always a full-screen GC16
         * flash regardless of full_style/gc16_flash. */
        return (FullRedraw){ false, RSTYLE_GC16_FLASH };
    case RR_HOURLY_EXIT_FLASH:
        /* Leaving hourly after a scroll: force the flash regardless of
         * full_style, to clear any residual A2/REAGL ghosting. */
        return (FullRedraw){ false, RSTYLE_GC16_FLASH };
    case RR_POST_FETCH:
    case RR_NONE:
    default:
        /* Settings->home, Hourly->home, and the app-start post-fetch update
         * follow full_style (REAGL or GC16). */
        return (FullRedraw){ false, full_style };
    }
}

void draw_main_screen(UIState *ui) {
    FullRedrawReason reason = ui->refresh.full_redraw_reason;
    ui->refresh.full_redraw_reason = RR_NONE;
    int full = !ui->refresh.drawn_main.ok || reason != RR_NONE;

    if (full) {
        /* First paint with no explicit reason: derive one so the commit
         * style still flows through the single resolver. */
        if (reason == RR_NONE)
            reason = ui->refresh.startup_paint_pending ? RR_STARTUP : RR_POST_FETCH;
        FullRedraw fr = resolve_full_redraw(reason, ui->persisted.sleep_entry_wash, ui->persisted.full_style,
                                            ui->persisted.sleep_gc16_flash);

        fb_fill_rect(&g_disp.fb, (Rect){0, 0, ui->screen_w, ui->screen_h}, UI_BG);

        /* Run the INIT clear on the still-white framebuffer, BEFORE drawing
         * content, so INIT doesn't desync the EPDC working buffer or bleed
         * content mid-wash (see top-of-file note). Content is drawn
         * afterwards. */
        if (fr.init_wash) ui_run_init_wash(ui);

        draw_current_weather(ui);
        draw_forecast(ui);
        draw_subtitle(ui);
        /* Header band ("≡ ☾ ✕" + separator) drawn last so it sits on top of
         * the section fills; at the new geometry the content starts exactly
         * at the header's bottom edge so there's no overlap to worry about. */
        if (!ui->power.kiosk) draw_header(ui, "", 0, 1, 1);

        /* First paint only: give a still-draining Nickel menu-close update
         * time to land before our flash, so the two don't merge into a
         * shortened flash. Off the panel-seize stamp, so the render above
         * already counts toward it. */
        if (ui->refresh.startup_paint_pending)
            settle_since_panel_seized(STARTUP_HANDOVER_SETTLE_MS);
        ui_region_commit(ui, RECT_FULLSCREEN, fr.style);
        ui->refresh.startup_paint_pending = 0;

        ui->refresh.drawn_main.ok = 1;
        snapshot_drawn_main(ui);
    } else {
        Rect weather_dirty  = {0, 0, 0, 0};  /* current + forecast */
        Rect subtitle_dirty = {0, 0, 0, 0};

        /* --debug only: name every field that changed this pass, so the log
         * shows which fields fused into the merged-bbox REFRESH rect below.
         * NULL when not debugging means field_log_add does nothing. */
        FieldLog flbuf = { .len = 0 };
        FieldLog *fl = g_debug ? &flbuf : NULL;

        partial_update_cur     (ui, &weather_dirty, DRAW_CLEAR, fl);
        partial_update_forecast(ui, &weather_dirty, DRAW_CLEAR, fl);
        partial_update_subtitle(ui, &subtitle_dirty, DRAW_CLEAR, fl);

        if (fl && fl->len)
            sysutil_log("MAIN ", false, "partial fields: %s", fl->buf);

        if (weather_dirty.w > 0)
            ui_region_commit(ui, weather_dirty,
                             kiosk_resolve_style(ui, ui->persisted.partial_style));

        if (subtitle_dirty.w > 0) {
            /* Mid-fetch stage text ("Connecting…", "Searching networks…") must
             * never flash — only the final "Updated HH:MM • place" text may,
             * and then only per partial_style/gc16_flash as usual. */
            RefreshStyle subtitle_style = ui->persisted.partial_style;
            if (ui->fetch_in_progress && subtitle_style == RSTYLE_GC16)
                subtitle_style = RSTYLE_GC16_NOFLASH;
            ui_region_commit(ui, subtitle_dirty,
                             kiosk_resolve_style(ui, subtitle_style));
        }
        /* No snapshot_drawn_main() here: each partial_update_* wrote its changed
         * fields through to the snapshot already. */
    }
}
