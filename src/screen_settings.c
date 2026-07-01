#include "ui_internal.h"
#include "config.h"
#include "i18n.h"
#include "input.h"
#include "sysutil.h"
#include <stdio.h>
#include <string.h>

/* Log a settings change (only under --debug). */
#define slog(...) sysutil_log("SET ", false, __VA_ARGS__)

/* Language names always shown in their own language, regardless of UI lang. */
static const char *g_lang_names[LANG_COUNT] = {
    "English", "Svenska", "Espa\xc3\xb1ol", "Portugu\xc3\xaas",
    "Deutsch", "Fran\xc3\xa7" "ais", "Italiano"
};

static void fmt_duration(Lang lang, char *buf, int n, int minutes) {
    if (minutes < 60)
        snprintf(buf, n, "%d %s", minutes, tr(lang, S_UNIT_MIN));
    else
        snprintf(buf, n, "%d %s", minutes / 60, tr(lang, S_UNIT_HOUR));
}

/* "2.5 s" from milliseconds (100 ms granularity). */
static void fmt_settle(char *buf, int n, int ms) {
    snprintf(buf, n, "%d.%d s", ms / 1000, (ms % 1000) / 100);
}

typedef enum { ITEM_SEC, ITEM_ROW, ITEM_ROW_GROUP, ITEM_STEPPER, ITEM_BUTTON } ItemKind;

/* Refresh scope a changed row gets:
 *   CHECKBOX   (default) — just the changed checkbox square(s).
 *   WHOLE_BODY — whole body; language group only, since it re-translates every label.
 *   OWN_RECT   — exactly the item's own rect (stepper/button). */
typedef enum { ROW_SCOPE_CHECKBOX = 0, ROW_SCOPE_WHOLE_BODY, ROW_SCOPE_OWN_RECT } RowRefreshScope;

typedef struct {
    ItemKind kind;
    int      col;                                  /* 0 = left/only, 1 = right (dual pages only) */
    int      count;                               /* ITEM_ROW_GROUP: sub-row count */
    RowRefreshScope scope;
    const char *(*label)(const UIState *ui);       /* static section/row label */
    const char *(*dyn_label)(const UIState *ui);  /* ITEM_BUTTON: dynamic label */
    const char *(*label_for)(const UIState *ui, int idx); /* ITEM_ROW_GROUP: per-entry label */
    int  (*is_checked)(const UIState *ui, int idx);
    void (*on_select)(UIState *ui, int idx);
    void (*fmt_value)(const UIState *ui, char *buf, int n); /* ITEM_STEPPER */
    void (*on_step)(UIState *ui, int delta);
    void (*on_press)(UIState *ui);                /* ITEM_BUTTON */
    bool col_title;                               /* ITEM_SEC: white (UI_BG) fill, not gray band
                                                       (dual-column Wake/Sleep headers) */
} SettingsItem;

/* Flat laid-out band from settings_build_rows(): one entry per visible
 * row/section, ITEM_ROW_GROUP expanded into sub_idx entries. Both draw and
 * hit-test consume it so y-positions are computed once. */
typedef struct {
    int      y, h;        /* band top + height in screen coords */
    int      item_idx;    /* index into the page's items[] */
    int      sub_idx;     /* ROW_GROUP sub-row index (0 for non-group kinds) */
    ItemKind kind;
    int      col;
} SettingsRow;

/* Forward declarations — implemented after the setters. */
static const SettingsItem *settings_page_items(const UIState *ui, int *n_out);
static int settings_sec_total(const SettingsItem *items, int n, int col);
static int settings_row_total(const SettingsItem *items, int n, int col);
static int settings_row_h(int cont_h, int sec_h, int n_sec, int n_row);
static int settings_uniform_row_h(int cont_h, int sec_h);
static int settings_build_rows(const SettingsItem *items, int n_items,
                                int cont_y, int sec_h, int row_h,
                                SettingsRow *out, int cap);

/* Only the Advanced page splits into two columns (Wake/Sleep). */
static bool settings_page_is_dual(int page) { return page == 2; }

/* Layout for one row/section. lbx/ltx (checkbox x, label text x) are only
 * meaningful for ITEM_ROW/ITEM_ROW_GROUP. */
static RowGeom compute_row_geom(UIState *ui, int lx, int ly, int lcw, int row_h) {
    int lbox = row_h / 2;
    RowGeom g = { .lx = lx, .ly = ly, .lcw = lcw, .row_h = row_h };
    g.lbx = lx + ui->pad * 2;
    g.ltx = g.lbx + lbox + ui->pad;
    return g;
}

static void render_section(UIState *ui, const RowGeom *g, const char *lbl, bool col_title) {
    fb_fill_rect(&g_disp.fb, (Rect){g->lx, g->ly, g->lcw, g->row_h}, 0xCC);
    if (col_title) {
        /* Dual-column heading (Wake/Sleep): centered, slightly larger. */
        draw_text_centered(UI_FONT_SETTINGS_COL_TITLE_PT, g->lx,
                           center_y(g->ly, g->row_h, UI_FONT_SETTINGS_COL_TITLE_PT),
                           g->lcw, lbl);
    } else {
        draw_text_clipped(UI_FONT_SETTINGS_SEC_PT, g->lx + ui->pad,
                          center_y(g->ly, g->row_h, UI_FONT_SETTINGS_SEC_PT),
                          g->lcw - ui->pad * 2, g->row_h, lbl);
    }
    /* Bottom divider, matching the other render_* helpers. */
    fb_fill_rect(&g_disp.fb, (Rect){g->lx, g->ly + g->row_h - UI_REF_SEP_THICK,
                 g->lcw, UI_REF_SEP_THICK}, UI_SEP);
}

/* Checkbox square geometry from a row's y/h and column lbx; returned by
 * render_row() so draw_settings_screen can target a refresh at it. */
static Rect row_checkbox_rect(int ly, int row_h, int lbx) {
    int lbox = row_h / 2;
    int by   = ly + row_h / 2 - lbox / 2;
    return (Rect){ lbx, by, lbox, lbox };
}

static Rect render_row(UIState *ui, const RowGeom *g, const char *lbl, int checked) {
    Rect cb = row_checkbox_rect(g->ly, g->row_h, g->lbx);
    fb_draw_rect_outline(&g_disp.fb, cb, UI_FG, 2);
    if (checked)
        fb_fill_rect(&g_disp.fb, (Rect){cb.x + cb.w / 4, cb.y + cb.h / 4,
                     cb.w - cb.w / 2, cb.h - cb.h / 2}, UI_FG);
    draw_text(UI_FONT_SETTINGS_ROW_PT, g->ltx,
              center_y(g->ly, g->row_h, UI_FONT_SETTINGS_ROW_PT),
              g->lcw - (g->ltx - g->lx) - ui->pad, lbl);
    fb_fill_rect(&g_disp.fb, (Rect){g->lx, g->ly + g->row_h - UI_REF_SEP_THICK, g->lcw, UI_REF_SEP_THICK}, UI_SEP);
    return cb;
}

/* The stepper arrows' center x-positions, derived purely from a row's
 * geometry — the single source of truth for both where the arrow glyphs are
 * drawn and where their (wider) touch hit-zones sit. */
static void stepper_arrow_x(UIState *ui, const RowGeom *g, int *lax, int *rax) {
    int arw    = sc(UI_REF_STEPPER_ARROW_W);
    int ctrl_x = g->lx + g->lcw / 2;
    int ctrl_w = g->lcw / 2 - ui->pad;
    *lax = ctrl_x;
    *rax = ctrl_x + ctrl_w - arw;
}

/* The stepper arrows' touch hit-zones (wider than the drawn glyph, per
 * UI_REF_STEPPER_HIT_W) — mirrors row_checkbox_rect()'s pattern so neither
 * draw nor touch dispatch needs a stored global. render_stepper() and
 * settings_handle_touch() both call this fresh off their own RowGeom. */
typedef struct { int lx0, lx1, rx0, rx1; } StepperHit;
static StepperHit compute_stepper_hit(UIState *ui, const RowGeom *g) {
    int lax, rax;
    stepper_arrow_x(ui, g, &lax, &rax);
    int arw  = sc(UI_REF_STEPPER_ARROW_W);
    int hitw = sc(UI_REF_STEPPER_HIT_W);
    return (StepperHit){
        .lx0 = lax - (hitw - arw) / 2, .lx1 = lax + arw + (hitw - arw) / 2,
        .rx0 = rax - (hitw - arw) / 2, .rx1 = rax + arw + (hitw - arw) / 2,
    };
}

static Rect render_stepper(UIState *ui, const RowGeom *g,
                            const char *lbl, const char *val_str) {
    draw_text(UI_FONT_SETTINGS_ROW_PT, g->lx + ui->pad,
              center_y(g->ly, g->row_h, UI_FONT_SETTINGS_ROW_PT),
              g->lcw / 2 - ui->pad, lbl);
    int arh   = sc(UI_REF_STEPPER_ARROW_H);
    int mid_y = g->ly + g->row_h / 2;
    int lax, rax;
    stepper_arrow_x(ui, g, &lax, &rax);
    for (int i = 0; i <= arh; i++) {
        for (int t = -1; t <= 1; t++) {
            fb_set_pixel(&g_disp.fb, lax + i + t, mid_y - i, UI_FG);
            fb_set_pixel(&g_disp.fb, lax + i + t, mid_y + i, UI_FG);
        }
    }
    for (int i = 0; i <= arh; i++) {
        for (int t = -1; t <= 1; t++) {
            fb_set_pixel(&g_disp.fb, rax + arh - i + t, mid_y - i, UI_FG);
            fb_set_pixel(&g_disp.fb, rax + arh - i + t, mid_y + i, UI_FG);
        }
    }
    int inner_x = lax + arh;
    int inner_w  = rax - inner_x;
    draw_text_centered(UI_FONT_SETTINGS_ROW_PT, inner_x,
                       center_y(g->ly, g->row_h, UI_FONT_SETTINGS_ROW_PT),
                       inner_w, val_str);
    fb_fill_rect(&g_disp.fb, (Rect){g->lx, g->ly + g->row_h - UI_REF_SEP_THICK, g->lcw, UI_REF_SEP_THICK}, UI_SEP);
    /* Whole row is the dirty rect: the centered value string's width varies
     * with the digits, so refreshing the full band is the simplest correct
     * scope (mirrors render_row/render_button returning their bounding box). */
    return (Rect){g->lx, g->ly, g->lcw, g->row_h};
}

static Rect render_button(UIState *ui, const RowGeom *g, const char *lbl) {
    int tw = measure_text_w_styled(UI_FONT_SETTINGS_ROW_PT, lbl, FNT_REGULAR);
    int bp = ui->pad / 2;
    int bx = g->lbx;
    int bw = tw + 2 * bp;
    int bh = g->row_h - ui->pad;
    int by = g->ly + (g->row_h - bh) / 2;
    Rect btn = { bx, by, bw, bh };
    fb_draw_rect_outline(&g_disp.fb, btn, UI_FG, 2);
    draw_text(UI_FONT_SETTINGS_ROW_PT, bx + bp,
              center_y(g->ly, g->row_h, UI_FONT_SETTINGS_ROW_PT),
              tw, lbl);
    fb_fill_rect(&g_disp.fb, (Rect){g->lx, g->ly + g->row_h - UI_REF_SEP_THICK, g->lcw, UI_REF_SEP_THICK}, UI_SEP);
    return btn;
}

/* The Demo button on the dual-column Advanced page sits below both columns,
 * spanning the full screen width and horizontally centered — unlike
 * render_button()'s left-aligned, column-width style (used by the °C/°F
 * toggle). Geometry is split from drawing, mirroring row_checkbox_rect()'s
 * pattern, so settings_handle_touch can hit-test the same rect without
 * redrawing. */
static Rect demo_button_rect(UIState *ui, int y, int row_h, const char *lbl) {
    int tw = measure_text_w_styled(UI_FONT_SETTINGS_ROW_PT, lbl, FNT_REGULAR);
    int bp = ui->pad / 2;
    int bw = tw + 2 * bp;
    int bh = row_h - ui->pad;
    int bx = (ui->screen_w - bw) / 2;
    int by = y + (row_h - bh) / 2;
    return (Rect){ bx, by, bw, bh };
}

static void render_demo_button(UIState *ui, int y, int row_h, const char *lbl) {
    Rect btn = demo_button_rect(ui, y, row_h, lbl);
    int bp = ui->pad / 2;
    fb_draw_rect_outline(&g_disp.fb, btn, UI_FG, 2);
    draw_text(UI_FONT_SETTINGS_ROW_PT, btn.x + bp,
              center_y(y, row_h, UI_FONT_SETTINGS_ROW_PT),
              btn.w - bp, lbl);
}

/* The lower bound of a dual-column page's actual content (the greater of the
 * two columns' last-row bottoms) — both the divider's height and the Demo
 * button's y sit here instead of stretching to the container's bottom. */
static int settings_dual_max_y(const SettingsRow *rows, int nrows, int cont_y) {
    int max_y = cont_y;
    for (int i = 0; i < nrows; i++) {
        int bottom = rows[i].y + rows[i].h;
        if (bottom > max_y) max_y = bottom;
    }
    return max_y;
}

void draw_settings_screen(UIState *ui) {
    bool screen_change = !ui->refresh.drawn_settings;
    /* Resolve style once: entry→full_style, tab_switch→REAGL, else→partial_style. */
    RefreshStyle srs = resolve_refresh_style(&(RefreshCtx){
        .screen_change = screen_change,
        .tab_switch    = ui->settings_ui.tab_switch,
        .full_style    = ui->persisted.full_style,
        .partial_style = ui->persisted.partial_style,
    });

    fb_fill_rect(&g_disp.fb, (Rect){0, 0, ui->screen_w, ui->screen_h}, UI_BG);

    /* A long button-driven kiosk wake upgrades this entry's full-screen
     * commit to a GC16 flash or an INIT-wash + GC16 — see
     * ui_consume_wake_override(). Only on entry (not tab switches/in-place
     * redraws), and the wash must run while the panel is still pre-commit. */
    WakeOverride wo = {0};
    if (screen_change) {
        wo = ui_consume_wake_override(ui);
        if (wo.init_wash) ui_run_init_wash(ui);
    }

    draw_header(ui, tr((Lang)ui->persisted.lang, S_SETTINGS_TITLE), 1, 0, 0);

    int avail  = ui->screen_h - ui->header_h;
    int sec_h  = sc(UI_REF_SETTINGS_SEC_H);
    int tab_h  = sec_h;
    int cont_y = ui->header_h + tab_h;
    int cont_h = avail - tab_h;

    /* Every page renders at the same row density — General's — so tabs
     * read consistently; pages with fewer rows just leave blank space
     * below their last row. */
    bool dual = settings_page_is_dual(ui->settings_ui.page);
    int n_items;
    const SettingsItem *items = settings_page_items(ui, &n_items);
    int row_h = settings_uniform_row_h(cont_h, sec_h);

    /* ---- Tab bar ---- */
    {
        int tw3 = ui->screen_w / 3;
        for (int t = 0; t < 3; t++) {
            int tx0    = t * tw3;
            int tw     = (t == 2) ? (ui->screen_w - 2 * tw3) : tw3;
            int active = (ui->settings_ui.page == t);
            const char *lbl = tr((Lang)ui->persisted.lang,
                                 t == 0 ? S_TAB_GENERAL : t == 1 ? S_TAB_TIMERS : S_TAB_ADVANCED);
            /* Active tab: light (UI_BG) so it merges with the content area below —
             * the standard active-tab look. Inactive: light-gray (0xCC) like section
             * headers, so it reads as recessed. Black text on both. */
            fb_fill_rect(&g_disp.fb, (Rect){tx0, ui->header_h, tw, tab_h}, active ? 0xCC : UI_BG);
            if (g_disp.font_loaded) {
                int ty = center_y(ui->header_h, tab_h, UI_FONT_SETTINGS_TAB_PT);
                draw_text_centered(UI_FONT_SETTINGS_TAB_PT, tx0, ty, tw, lbl);
            }
        }
        /* Dividers between the three tab cells */
        fb_fill_rect(&g_disp.fb, (Rect){tw3     - UI_REF_SEP_THICK / 2, ui->header_h,
                     UI_REF_SEP_THICK, tab_h}, UI_FG);
        fb_fill_rect(&g_disp.fb, (Rect){2 * tw3 - UI_REF_SEP_THICK / 2, ui->header_h,
                     UI_REF_SEP_THICK, tab_h}, UI_FG);
    }

    /* ---- Column geometry: half-width per column when dual, full-width
     * otherwise (column 1 simply unused in the single-column case). ---- */
    int lcw = dual ? ui->screen_w / 2 : ui->screen_w;

    /* Build the layout table once; both draw and hit-test consume it. */
    SettingsRow rows[48];
    int nrows = settings_build_rows(items, n_items, cont_y, sec_h, row_h, rows, 48);
    Rect dirty = {0, 0, 0, 0};
    for (int i = 0; i < nrows; i++) {
        const SettingsItem *item = &items[rows[i].item_idx];
        int rh = rows[i].h;
        int lx = (dual && rows[i].col == 1) ? ui->screen_w / 2 : 0;
        RowGeom rg = compute_row_geom(ui, lx, rows[i].y, lcw, rh);
        /* True for the row(s) settings_handle_touch flagged as having just
         * changed — for ITEM_ROW_GROUP this also matches the row whose
         * checkbox needs erasing. Captures the rect render_row/render_button
         * just computed for *this* frame, so it's always current (e.g. a
         * °C/°F button whose width just changed on toggle). */
        int is_dirty = rows[i].item_idx == ui->settings_ui.dirty_item &&
                       (rows[i].kind != ITEM_ROW_GROUP ||
                        rows[i].sub_idx == ui->settings_ui.dirty_sub ||
                        rows[i].sub_idx == ui->settings_ui.dirty_prev_sub);
        switch (rows[i].kind) {
        case ITEM_SEC:
            render_section(ui, &rg,
                           item->label ? item->label(ui) : "",
                           item->col_title);
            break;
        case ITEM_ROW: {
            Rect cb = render_row(ui, &rg,
                       item->label ? item->label(ui) : "",
                       item->is_checked ? item->is_checked(ui, 0) : 0);
            if (is_dirty) dirty = rect_union(dirty, cb);
            break;
        }
        case ITEM_ROW_GROUP: {
            Rect cb = render_row(ui, &rg,
                       item->label_for ? item->label_for(ui, rows[i].sub_idx) : "",
                       item->is_checked ? item->is_checked(ui, rows[i].sub_idx) : 0);
            if (is_dirty) dirty = rect_union(dirty, cb);
            break;
        }
        case ITEM_STEPPER: {
            char vbuf[24];
            if (item->fmt_value) item->fmt_value(ui, vbuf, sizeof vbuf);
            else vbuf[0] = '\0';
            Rect st = render_stepper(ui, &rg,
                           item->label ? item->label(ui) : "", vbuf);
            if (is_dirty) dirty = rect_union(dirty, st);
            break;
        }
        case ITEM_BUTTON: {
            const char *lbl = item->dyn_label ? item->dyn_label(ui)
                            : item->label     ? item->label(ui)
                            : "";
            Rect btn = render_button(ui, &rg, lbl);
            if (is_dirty) dirty = rect_union(dirty, btn);
            break;
        }
        }
    }

    if (dual) {
        int max_y = settings_dual_max_y(rows, nrows, cont_y);
        fb_fill_rect(&g_disp.fb, (Rect){ui->screen_w / 2 - UI_REF_SEP_THICK / 2, cont_y,
                     UI_REF_SEP_THICK, max_y - cont_y}, UI_FG);
        render_demo_button(ui, max_y, row_h, tr(LANG_EN, S_DEMO_RUN));
    }

    if (screen_change) {
        /* Entry: full-screen refresh so the panel picks up the new header
         * (back-arrow + title + separator) along with the body. A pending
         * button-wake staleness override (wo) replaces the normal style.
         * Entry always wins over a stale leftover dirty_item from a
         * previous visit — entry must never take the partial-refresh arm. */
        ui_region_commit(ui, RECT_FULLSCREEN, wo.override ? wo.style : srs);
        ui->settings_ui.dirty_item = -1;
    } else if (dirty.w > 0) {
        /* Refresh only the row(s) settings_handle_touch flagged as changed
         * (a checkbox square, or the °C/°F button) — never the whole body. */
        ui_region_commit(ui, dirty, ui->persisted.partial_style);
        ui->settings_ui.dirty_item = -1;
    } else {
        /* In-place redraw (row tap, tab switch, scroll-mode change, etc.):
         * wash only the body, leave the header alone. Tab switches always use
         * REAGL (flicker-free) regardless of partial_style; other in-place
         * redraws follow partial_style. Both cases resolved by srs. */
        ui->settings_ui.tab_switch = 0;
        ui_region_commit(ui, (Rect){0, ui->header_h, ui->screen_w, ui->screen_h - ui->header_h},
                         srs);
    }
    ui->refresh.drawn_settings = 1;
}

static void persist_settings(UIState *ui) {
    if (!ui->settings_path) return;
    settings_save(ui->settings_path, &ui->persisted);
}

static void set_scroll_mode(UIState *ui, int snap) {
    if (snap < 0) snap = 0;
    if (snap > 2) snap = 2;
    if ((int)ui->persisted.snap != snap) {
        ui->persisted.snap = snap;
        if (snap && ui->hourly_row_h > 0)
            ui->scroll.y = (ui->scroll.y / ui->hourly_row_h) * ui->hourly_row_h;
        persist_settings(ui);
        slog("scroll_mode -> %d", snap);
    }
    ui->needs_redraw = 1;
}

static void set_language(UIState *ui, int lang) {
    if (lang < 0) lang = 0;
    if (lang >= LANG_COUNT) lang = LANG_COUNT - 1;
    if (ui->persisted.lang != lang) {
        ui->persisted.lang          = lang;
        ui->refresh.drawn_main.ok = 0;
        persist_settings(ui);
        slog("language -> %d", lang);
    }
    ui->needs_redraw = 1;
}

/* Flip the unit-system preference: persist it, force a full repaint (so the
 * unit glyph next to the digits is repainted, not left stale by a partial
 * update), and trigger an immediate refetch in the new unit (with the usual
 * "(updating…)" status, via the existing trigger_fetch mechanism). */
static void set_units(UIState *ui, UnitSystem u) {
    if (ui->persisted.units != (int)u) {
        ui->persisted.units         = (int)u;
        ui->refresh.drawn_main.ok = 0;
        persist_settings(ui);
        ui->trigger_fetch = 1;
        slog("units -> %d", (int)u);
    }
    ui->needs_redraw = 1;
}

static void set_bold_cur_temp(UIState *ui, int on) {
    on = on ? 1 : 0;
    if (ui->persisted.bold_cur_temp != on) {
        ui->persisted.bold_cur_temp = on;
        ui->refresh.drawn_main.ok = 0;
        persist_settings(ui);
        slog("bold_cur_temp -> %d", on);
    }
    ui->needs_redraw = 1;
}

static void set_bold_max_temp(UIState *ui, int on) {
    on = on ? 1 : 0;
    if (ui->persisted.bold_max_temp != on) {
        ui->persisted.bold_max_temp = on;
        ui->refresh.drawn_main.ok = 0;
        persist_settings(ui);
        slog("bold_max_temp -> %d", on);
    }
    ui->needs_redraw = 1;
}

static void set_small_min_temp(UIState *ui, int on) {
    on = on ? 1 : 0;
    if (ui->persisted.small_min_temp != on) {
        ui->persisted.small_min_temp = on;
        ui->refresh.drawn_main.ok = 0;
        persist_settings(ui);
        slog("small_min_temp -> %d", on);
    }
    ui->needs_redraw = 1;
}

static void set_gc16_flash(UIState *ui, int on) {
    on = on ? 1 : 0;
    if (ui->persisted.gc16_flash != on) {
        ui->persisted.gc16_flash = on;     /* live effect via ui_region_commit, no restart */
        persist_settings(ui);
        slog("gc16_flash -> %d", on);
    }
    ui->needs_redraw = 1;
}

static void set_sleep_gc16_flash(UIState *ui, int on) {
    on = on ? 1 : 0;
    if (ui->persisted.sleep_gc16_flash != on) {
        ui->persisted.sleep_gc16_flash = on;
        persist_settings(ui);
        slog("sleep_gc16_flash -> %d", on);
    }
    ui->needs_redraw = 1;
}

static void set_sleep_partial(UIState *ui, int on) {
    on = on ? 1 : 0;
    if (ui->persisted.sleep_partial != on) {
        ui->persisted.sleep_partial = on;
        persist_settings(ui);
        slog("sleep_partial -> %d", on);
    }
    ui->needs_redraw = 1;
}

static void set_night_pause(UIState *ui, int on) {
    on = on ? 1 : 0;
    if (ui->persisted.night_pause != on) {
        ui->persisted.night_pause = on;
        persist_settings(ui);
        slog("night_pause -> %d", on);
    }
    ui->needs_redraw = 1;
}

static void set_full_style(UIState *ui, RefreshStyle style) {
    if (ui->persisted.full_style != (int)style) {
        ui->persisted.full_style = (int)style;
        persist_settings(ui);
        slog("full_style -> %d", (int)style);
    }
    ui->needs_redraw = 1;
}

static void set_partial_style(UIState *ui, RefreshStyle style) {
    if (ui->persisted.partial_style != (int)style) {
        ui->persisted.partial_style = (int)style;
        persist_settings(ui);
        slog("partial_style -> %d", (int)style);
    }
    ui->needs_redraw = 1;
}

static void set_init_wash_settle(UIState *ui, int delta) {
    int v = ui->persisted.init_wash_settle_ms + delta * WASH_SETTLE_STEP_MS;
    if (v < WASH_SETTLE_MIN_MS) v = WASH_SETTLE_MIN_MS;
    if (v > WASH_SETTLE_MAX_MS) v = WASH_SETTLE_MAX_MS;
    if (ui->persisted.init_wash_settle_ms != v) {
        ui->persisted.init_wash_settle_ms = v;
        persist_settings(ui);
        slog("init_wash_settle_ms -> %d", v);
    }
    ui->needs_redraw = 1;
}

static void set_sleep_entry_wash(UIState *ui, int v) {
    if (v < SLEEP_ENTRY_NONE) v = SLEEP_ENTRY_NONE;
    if (v > SLEEP_ENTRY_WASH) v = SLEEP_ENTRY_WASH;
    if ((int)ui->persisted.sleep_entry_wash != v) {
        ui->persisted.sleep_entry_wash = v;
        persist_settings(ui);
        slog("sleep_entry_wash -> %d", v);
    }
    ui->needs_redraw = 1;
}

static void set_wake_flash_min(UIState *ui, int delta) {
    int v = ui->persisted.wake_flash_min + delta;
    if (v < WAKE_FLASH_MIN_MIN) v = WAKE_FLASH_MIN_MIN;
    if (v > WAKE_FLASH_MAX_MIN) v = WAKE_FLASH_MAX_MIN;
    if (ui->persisted.wake_flash_min != v) {
        ui->persisted.wake_flash_min = v;
        persist_settings(ui);
        slog("wake_flash_min -> %d", v);
    }
    ui->needs_redraw = 1;
}

static void set_wake_wash_min(UIState *ui, int delta) {
    int v = ui->persisted.wake_wash_min + delta;
    if (v < WAKE_WASH_MIN_MIN) v = WAKE_WASH_MIN_MIN;
    if (v > WAKE_WASH_MAX_MIN) v = WAKE_WASH_MAX_MIN;
    if (ui->persisted.wake_wash_min != v) {
        ui->persisted.wake_wash_min = v;
        persist_settings(ui);
        slog("wake_wash_min -> %d", v);
    }
    ui->needs_redraw = 1;
}

static void set_sleep_min(UIState *ui, int delta) {
    int v = ui->persisted.sleep_min + delta * 5;
    if (v < 5)  v = 5;
    if (v > 60) v = 60;
    if (ui->persisted.sleep_min != v) {
        ui->persisted.sleep_min    = v;
        ui->settings_ui.rearm_timers = 1;
        persist_settings(ui);
        slog("sleep_min -> %d", v);
    }
    ui->needs_redraw = 1;
}

static void set_active_min(UIState *ui, int delta) {
    int v = ui->persisted.active_min + delta * 5;
    if (v < 5)  v = 5;
    if (v > 60) v = 60;
    if (ui->persisted.active_min != v) {
        ui->persisted.active_min   = v;
        ui->settings_ui.rearm_timers = 1;
        persist_settings(ui);
        slog("active_min -> %d", v);
    }
    ui->needs_redraw = 1;
}

static void set_sleep_upd(UIState *ui, int delta) {
    /* Find current index in g_sleepupd_steps. */
    int idx = 0;
    for (int i = 0; i < 6; i++) {
        if (g_sleepupd_steps[i] == ui->persisted.sleep_upd_min) { idx = i; break; }
    }
    idx += delta;
    if (idx < 0) idx = 0;
    if (idx > 5) idx = 5;
    int v = g_sleepupd_steps[idx];
    if (ui->persisted.sleep_upd_min != v) {
        ui->persisted.sleep_upd_min = v;
        /* No rearm_timers: sleep_upd_min is kiosk-only (read fresh in
         * powersave.c); the interactive refresh/idle timers don't use it. */
        persist_settings(ui);
        slog("sleep_upd_min -> %d", v);
    }
    ui->needs_redraw = 1;
}

/* ----- Block B: hook functions, page tables, geometry implementations -- */

/* ---- General page hooks ---- */
static const char *lbl_sec_language(const UIState *ui)  { return tr((Lang)ui->persisted.lang, S_SEC_LANGUAGE); }
static const char *lbl_sec_temp(const UIState *ui)      { return tr((Lang)ui->persisted.lang, S_SEC_TEMP); }

static const char *lbl_lang_entry(const UIState *ui, int idx) { (void)ui; return g_lang_names[idx]; }
static int  chk_lang(const UIState *ui, int idx) { return ui->persisted.lang == idx; }
static void sel_lang(UIState *ui, int idx) { set_language(ui, idx); }

static const char *dyn_lbl_units(const UIState *ui) {
    return (ui->persisted.units == UNIT_IMPERIAL) ? "\xc2\xb0""C & m/s" : "\xc2\xb0""F & mph";
}
static void press_units(UIState *ui) {
    set_units(ui, ui->persisted.units == UNIT_IMPERIAL ? UNIT_METRIC : UNIT_IMPERIAL);
}

static const char *lbl_bold_cur(const UIState *ui)  { return tr((Lang)ui->persisted.lang, S_TEMP_BOLD_CUR); }
static int  chk_bold_cur(const UIState *ui, int idx)  { (void)idx; return ui->persisted.bold_cur_temp; }
static void sel_bold_cur(UIState *ui, int idx)  { (void)idx; set_bold_cur_temp(ui, !ui->persisted.bold_cur_temp); }

static const char *lbl_bold_max(const UIState *ui)  { return tr((Lang)ui->persisted.lang, S_TEMP_BOLD_MAX); }
static int  chk_bold_max(const UIState *ui, int idx)  { (void)idx; return ui->persisted.bold_max_temp; }
static void sel_bold_max(UIState *ui, int idx)  { (void)idx; set_bold_max_temp(ui, !ui->persisted.bold_max_temp); }

static const char *lbl_small_min(const UIState *ui) { return tr((Lang)ui->persisted.lang, S_TEMP_SMALL_MIN); }
static int  chk_small_min(const UIState *ui, int idx) { (void)idx; return ui->persisted.small_min_temp; }
static void sel_small_min(UIState *ui, int idx) { (void)idx; set_small_min_temp(ui, !ui->persisted.small_min_temp); }

/* ---- Advanced page hooks ---- */
/* Scroll mode lives on the Advanced tab — its heading and entries always
 * render in English (see lbl_sec_full_style et al. below). */
static const char *lbl_sec_scroll(const UIState *ui)   { (void)ui; return tr(LANG_EN, S_SEC_SCROLL); }
static const char *lbl_sec_timers(const UIState *ui)   { return tr((Lang)ui->persisted.lang, S_SEC_TIMERS); }

static const char *lbl_scroll_entry(const UIState *ui, int idx) {
    (void)ui;
    if (idx == 0) return tr(LANG_EN, S_SCROLL_FLOAT);
    if (idx == 1) return tr(LANG_EN, S_SCROLL_A2);
    return tr(LANG_EN, S_SCROLL_REAGL);
}
static int  chk_scroll(const UIState *ui, int idx) { return (int)ui->persisted.snap == idx; }
static void sel_scroll(UIState *ui, int idx)       { set_scroll_mode(ui, idx); }

static const char *lbl_timer_sleep(const UIState *ui)  { return tr((Lang)ui->persisted.lang, S_TIMER_SLEEP); }
static void fmt_timer_sleep(const UIState *ui, char *buf, int n)
    { fmt_duration((Lang)ui->persisted.lang, buf, n, ui->persisted.sleep_min); }
static void step_timer_sleep(UIState *ui, int delta)  { set_sleep_min(ui, delta); }

static const char *lbl_timer_active(const UIState *ui) { return tr((Lang)ui->persisted.lang, S_TIMER_ACTIVE); }
static void fmt_timer_active(const UIState *ui, char *buf, int n)
    { fmt_duration((Lang)ui->persisted.lang, buf, n, ui->persisted.active_min); }
static void step_timer_active(UIState *ui, int delta) { set_active_min(ui, delta); }

static const char *lbl_timer_upd(const UIState *ui)    { return tr((Lang)ui->persisted.lang, S_TIMER_SLEEP_UPD); }
static void fmt_timer_upd(const UIState *ui, char *buf, int n)
    { fmt_duration((Lang)ui->persisted.lang, buf, n, ui->persisted.sleep_upd_min); }
static void step_timer_upd(UIState *ui, int delta)    { set_sleep_upd(ui, delta); }

static const char *lbl_night_pause(const UIState *ui)  { return tr((Lang)ui->persisted.lang, S_NIGHT_PAUSE); }
static int  chk_night_pause(const UIState *ui, int idx) { (void)idx; return ui->persisted.night_pause; }
static void sel_night_pause(UIState *ui, int idx) { (void)idx; set_night_pause(ui, !ui->persisted.night_pause); }

static void press_demo(UIState *ui)    { ui->demo.enter = 1; }

/* ---- Advanced page hooks ----
 * Every heading and row label on the Advanced tab is always English,
 * regardless of the UI language — General/Timers stay translated. */
static const char *lbl_sec_full_style(const UIState *ui) { (void)ui; return tr(LANG_EN, S_SEC_FULL_STYLE); }
static const char *lbl_sec_refresh(const UIState *ui)    { (void)ui; return tr(LANG_EN, S_SEC_REFRESH); }
static const char *lbl_sec_wash(const UIState *ui)       { (void)ui; return tr(LANG_EN, S_SEC_WASH); }
static const char *lbl_sec_refresh_group(const UIState *ui) { (void)ui; return tr(LANG_EN, S_SEC_REFRESH_GROUP); }

static const char *lbl_style_entry(const UIState *ui, int idx) {
    (void)ui;
    return idx == 0 ? tr(LANG_EN, S_REFRESH_REAGL) : tr(LANG_EN, S_STYLE_GC16);
}
static int  chk_full_style(const UIState *ui, int idx)    { return (int)ui->persisted.full_style == idx; }
static void sel_full_style_item(UIState *ui, int idx)     { set_full_style(ui, (RefreshStyle)idx); }
static int  chk_partial_style(const UIState *ui, int idx) { return (int)ui->persisted.partial_style == idx; }
static void sel_partial_style_item(UIState *ui, int idx)  { set_partial_style(ui, (RefreshStyle)idx); }

/* Sleep-entry refresh tri-state: None / GC16 flash / Init wash. */
static const char *lbl_entry_wash(const UIState *ui, int idx) {
    (void)ui;
    if (idx == 0) return tr(LANG_EN, S_WASH_NONE);
    if (idx == 1) return tr(LANG_EN, S_GC16_FLASH);
    return tr(LANG_EN, S_WASH_INIT);
}
static int  chk_entry_wash(const UIState *ui, int idx) { return (int)ui->persisted.sleep_entry_wash == idx; }
static void sel_entry_wash(UIState *ui, int idx) { set_sleep_entry_wash(ui, idx); }

static const char *lbl_gc16_flash(const UIState *ui) { (void)ui; return tr(LANG_EN, S_GC16_FLASH); }
static int  chk_gc16_flash(const UIState *ui, int idx) { (void)idx; return ui->persisted.gc16_flash; }
static void sel_gc16_flash(UIState *ui, int idx) { (void)idx; set_gc16_flash(ui, !ui->persisted.gc16_flash); }

static const char *lbl_wash_delay(const UIState *ui)   { (void)ui; return tr(LANG_EN, S_WASH_DELAY); }
static void fmt_wash_delay(const UIState *ui, char *buf, int n)
    { fmt_settle(buf, n, ui->persisted.init_wash_settle_ms); }
static void step_wash_delay(UIState *ui, int delta) { set_init_wash_settle(ui, delta); }

static const char *lbl_sec_wake_stale(const UIState *ui)   { (void)ui; return tr(LANG_EN, S_SEC_WAKE_STALE); }
static const char *lbl_wake_flash_after(const UIState *ui) { (void)ui; return tr(LANG_EN, S_WAKE_FLASH_AFTER); }
static const char *lbl_wake_wash_after(const UIState *ui)  { (void)ui; return tr(LANG_EN, S_WAKE_WASH_AFTER); }
static void fmt_wake_flash_min(const UIState *ui, char *buf, int n)
    { snprintf(buf, n, "%d %s", ui->persisted.wake_flash_min, tr(LANG_EN, S_UNIT_MIN)); }
static void fmt_wake_wash_min(const UIState *ui, char *buf, int n)
    { snprintf(buf, n, "%d %s", ui->persisted.wake_wash_min, tr(LANG_EN, S_UNIT_MIN)); }
static void step_wake_flash_min(UIState *ui, int delta) { set_wake_flash_min(ui, delta * WAKE_FLASH_STEP_MIN); }
static void step_wake_wash_min(UIState *ui, int delta)  { set_wake_wash_min(ui, delta * WAKE_WASH_STEP_MIN); }

/* Advanced tab's two-column headers (Wake/Sleep). */
static const char *lbl_col_wake(const UIState *ui)  { (void)ui; return tr(LANG_EN, S_COL_WAKE); }
static const char *lbl_col_sleep(const UIState *ui) { (void)ui; return tr(LANG_EN, S_COL_SLEEP); }

/* Sleep-column rows: sleep_gc16_flash governs the recurring kiosk tick-wakes /
 * in-kiosk partials (NOT entry, which the Sleep-wash tri-state owns), so it gets
 * its own "Tick flash" label to distinguish it from the entry GC16-flash option;
 * sleep_partial reuses the existing "Partial updates" section-header text
 * (S_SEC_REFRESH) since it's the same Wake-side concept ("only redraw what
 * changed") opted into for sleep/kiosk ticks. */
static const char *lbl_sleep_tick_flash(const UIState *ui) { (void)ui; return tr(LANG_EN, S_SLEEP_TICK_FLASH); }
static int  chk_sleep_gc16_flash(const UIState *ui, int idx) { (void)idx; return ui->persisted.sleep_gc16_flash; }
static void sel_sleep_gc16_flash(UIState *ui, int idx) { (void)idx; set_sleep_gc16_flash(ui, !ui->persisted.sleep_gc16_flash); }

static const char *lbl_sleep_partial(const UIState *ui) { (void)ui; return tr(LANG_EN, S_SEC_REFRESH); }
static int  chk_sleep_partial(const UIState *ui, int idx) { (void)idx; return ui->persisted.sleep_partial; }
static void sel_sleep_partial(UIState *ui, int idx) { (void)idx; set_sleep_partial(ui, !ui->persisted.sleep_partial); }

/* ---- Page tables ---- */

static const SettingsItem PAGE_STANDARD[] = {
    { .kind=ITEM_SEC,       .label=lbl_sec_language },
    { .kind=ITEM_ROW_GROUP, .count=LANG_COUNT, .scope=ROW_SCOPE_WHOLE_BODY,
      .label_for=lbl_lang_entry, .is_checked=chk_lang, .on_select=sel_lang },
    { .kind=ITEM_SEC,       .label=lbl_sec_temp },
    { .kind=ITEM_BUTTON,    .scope=ROW_SCOPE_OWN_RECT,
      .dyn_label=dyn_lbl_units, .on_press=press_units },
    { .kind=ITEM_ROW,       .label=lbl_bold_cur,  .is_checked=chk_bold_cur,  .on_select=sel_bold_cur  },
    { .kind=ITEM_ROW,       .label=lbl_bold_max,  .is_checked=chk_bold_max,  .on_select=sel_bold_max  },
    { .kind=ITEM_ROW,       .label=lbl_small_min, .is_checked=chk_small_min, .on_select=sel_small_min },
};

static const SettingsItem PAGE_TIMERS[] = {
    { .kind=ITEM_SEC,       .label=lbl_sec_timers },
    { .kind=ITEM_STEPPER,   .scope=ROW_SCOPE_OWN_RECT, .label=lbl_timer_sleep,  .fmt_value=fmt_timer_sleep,  .on_step=step_timer_sleep  },
    { .kind=ITEM_STEPPER,   .scope=ROW_SCOPE_OWN_RECT, .label=lbl_timer_active, .fmt_value=fmt_timer_active, .on_step=step_timer_active },
    { .kind=ITEM_STEPPER,   .scope=ROW_SCOPE_OWN_RECT, .label=lbl_timer_upd,    .fmt_value=fmt_timer_upd,    .on_step=step_timer_upd    },
    { .kind=ITEM_ROW,       .label=lbl_night_pause, .is_checked=chk_night_pause, .on_select=sel_night_pause },
};

/* Advanced tab: two independently-laid-out columns (see
 * settings_page_is_dual). Wake (col=0) holds the settings that affect the
 * screen while awake — full-screen/partial refresh-style pickers, the
 * awake-only GC16 flash toggle, and scroll mode (moved in from Timers, kept
 * last since it's the odd one out among the refresh-style settings). Sleep
 * (col=1) holds the settings that affect the screen while asleep/kiosk — a
 * Refresh group (the two sleep-side refresh toggles) above the INIT-wash
 * settings (moved in from the old single column). */
static const SettingsItem PAGE_ADVANCED[] = {
    /* Wake column. gc16_flash sits right under the column title, ahead of
     * both style-picker groups — it governs GC16 flashing for refreshes
     * resolved from EITHER picker (see resolve_refresh_style()/RSTYLE_GC16
     * in ui_refresh.c), so it must not read as scoped to either one. */
    { .kind=ITEM_SEC,       .col=0, .label=lbl_col_wake, .col_title=true },
    { .kind=ITEM_ROW,       .col=0, .label=lbl_gc16_flash,
      .is_checked=chk_gc16_flash, .on_select=sel_gc16_flash },
    { .kind=ITEM_SEC,       .col=0, .label=lbl_sec_full_style },
    { .kind=ITEM_ROW_GROUP, .col=0, .count=2, .label_for=lbl_style_entry,
      .is_checked=chk_full_style, .on_select=sel_full_style_item },
    { .kind=ITEM_SEC,       .col=0, .label=lbl_sec_refresh },
    { .kind=ITEM_ROW_GROUP, .col=0, .count=2, .label_for=lbl_style_entry,
      .is_checked=chk_partial_style, .on_select=sel_partial_style_item },
    { .kind=ITEM_SEC,       .col=0, .label=lbl_sec_scroll },
    { .kind=ITEM_ROW_GROUP, .col=0, .count=3, .label_for=lbl_scroll_entry,
      .is_checked=chk_scroll, .on_select=sel_scroll },

    /* Sleep column */
    { .kind=ITEM_SEC,       .col=1, .label=lbl_col_sleep, .col_title=true },
    { .kind=ITEM_SEC,       .col=1, .label=lbl_sec_refresh_group },
    { .kind=ITEM_ROW,       .col=1, .label=lbl_sleep_tick_flash,
      .is_checked=chk_sleep_gc16_flash, .on_select=sel_sleep_gc16_flash },
    { .kind=ITEM_ROW,       .col=1, .label=lbl_sleep_partial,
      .is_checked=chk_sleep_partial, .on_select=sel_sleep_partial },
    { .kind=ITEM_SEC,       .col=1, .label=lbl_sec_wash },
    { .kind=ITEM_ROW_GROUP, .col=1, .count=3, .label_for=lbl_entry_wash,
      .is_checked=chk_entry_wash, .on_select=sel_entry_wash },
    { .kind=ITEM_STEPPER,   .col=1, .scope=ROW_SCOPE_OWN_RECT, .label=lbl_wash_delay,
      .fmt_value=fmt_wash_delay, .on_step=step_wash_delay },
    { .kind=ITEM_SEC,       .col=1, .label=lbl_sec_wake_stale },
    { .kind=ITEM_STEPPER,   .col=1, .scope=ROW_SCOPE_OWN_RECT, .label=lbl_wake_flash_after,
      .fmt_value=fmt_wake_flash_min, .on_step=step_wake_flash_min },
    { .kind=ITEM_STEPPER,   .col=1, .scope=ROW_SCOPE_OWN_RECT, .label=lbl_wake_wash_after,
      .fmt_value=fmt_wake_wash_min, .on_step=step_wake_wash_min },
};

/* ---- Geometry implementations (reference the page tables above) ---- */

static const SettingsItem *settings_page_items(const UIState *ui, int *n_out) {
    switch (ui->settings_ui.page) {
    case 0: *n_out = (int)(sizeof(PAGE_STANDARD) / sizeof(*PAGE_STANDARD)); return PAGE_STANDARD;
    case 1: *n_out = (int)(sizeof(PAGE_TIMERS)   / sizeof(*PAGE_TIMERS));   return PAGE_TIMERS;
    case 2: *n_out = (int)(sizeof(PAGE_ADVANCED) / sizeof(*PAGE_ADVANCED)); return PAGE_ADVANCED;
    default: *n_out = 0; return PAGE_STANDARD;
    }
}

static int settings_sec_total(const SettingsItem *items, int n, int col) {
    int c = 0;
    for (int i = 0; i < n; i++)
        if (items[i].kind == ITEM_SEC && items[i].col == col) c++;
    return c;
}

static int settings_row_total(const SettingsItem *items, int n, int col) {
    int c = 0;
    for (int i = 0; i < n; i++) {
        if (items[i].col != col) continue;
        switch (items[i].kind) {
        case ITEM_ROW: case ITEM_STEPPER: case ITEM_BUTTON: c++;              break;
        case ITEM_ROW_GROUP:                                c += items[i].count; break;
        default: break;
        }
    }
    return c;
}

static int settings_row_h(int cont_h, int sec_h, int n_sec, int n_row) {
    int v = n_row > 0 ? (cont_h - n_sec * sec_h) / n_row : 1;
    return v < 1 ? 1 : v;
}

/* The one row height every settings page uses, derived from General's
 * (single-column) row/section counts. Every page — including the
 * dual-column Advanced page — renders at this density; pages with fewer
 * rows than General simply leave blank space below their last row instead
 * of stretching to fill the container. */
static int settings_uniform_row_h(int cont_h, int sec_h) {
    int n_sec = settings_sec_total(PAGE_STANDARD,
        (int)(sizeof(PAGE_STANDARD) / sizeof(*PAGE_STANDARD)), 0);
    int n_row = settings_row_total(PAGE_STANDARD,
        (int)(sizeof(PAGE_STANDARD) / sizeof(*PAGE_STANDARD)), 0);
    return settings_row_h(cont_h, sec_h, n_sec, n_row);
}

/* Expand items[] into a flat array of laid-out bands — one per visible row or
 * section header, with ITEM_ROW_GROUP already unpacked into sub_idx entries.
 * This is the single source of y-positions for both draw and hit-test.
 *
 * Tracks two independent y-cursors (y[0], y[1]) — one per column — but a
 * single row_h shared by both, since every page now renders at the same
 * row density. Each item is placed at y[item.col] using row_h/sec_h for its
 * height, then only that column's cursor advances. On single-column pages
 * every item has col 0 and y[1] is simply unused. */
static int settings_build_rows(const SettingsItem *items, int n_items,
                                int cont_y, int sec_h, int row_h,
                                SettingsRow *out, int cap) {
    int n = 0;
    int y[2] = {cont_y, cont_y};
    int rh = row_h;
    for (int i = 0; i < n_items; i++) {
        int col = items[i].col;
        switch (items[i].kind) {
        case ITEM_SEC:
            if (n < cap)
                out[n++] = (SettingsRow){y[col], sec_h, i, 0, ITEM_SEC, col};
            y[col] += sec_h;
            break;
        case ITEM_ROW:
            if (n < cap)
                out[n++] = (SettingsRow){y[col], rh, i, 0, ITEM_ROW, col};
            y[col] += rh;
            break;
        case ITEM_STEPPER:
            if (n < cap)
                out[n++] = (SettingsRow){y[col], rh, i, 0, ITEM_STEPPER, col};
            y[col] += rh;
            break;
        case ITEM_BUTTON:
            if (n < cap)
                out[n++] = (SettingsRow){y[col], rh, i, 0, ITEM_BUTTON, col};
            y[col] += rh;
            break;
        case ITEM_ROW_GROUP:
            for (int j = 0; j < items[i].count; j++) {
                if (n < cap)
                    out[n++] = (SettingsRow){y[col], rh, i, j, ITEM_ROW_GROUP, col};
                y[col] += rh;
            }
            break;
        }
    }
    return n;
}

void settings_handle_touch(UIState *ui, const TouchEvent *ev) {
    if (hit_top_left_button(ui, ev->x, ev->y)) {
        ui_set_screen(ui, SCREEN_MAIN);
        ui->needs_redraw  = 1;
        ui->refresh.drawn_main.ok = 0;
        ui->touch.swallow = 1;
    } else {
        int sec_h  = sc(UI_REF_SETTINGS_SEC_H);
        int tab_h  = sec_h;
        int cont_y = ui->header_h + tab_h;
        int cont_h = (ui->screen_h - ui->header_h) - tab_h;
        bool dual  = settings_page_is_dual(ui->settings_ui.page);
        int nt_items;
        const SettingsItem *t_items = settings_page_items(ui, &nt_items);
        int row_h = settings_uniform_row_h(cont_h, sec_h);

        /* Tab bar tap */
        if (ev->y < cont_y) {
            int tw3 = ui->screen_w / 3;
            int new_page = (ev->x < tw3) ? 0 : (ev->x < 2 * tw3) ? 1 : 2;
            if (new_page != ui->settings_ui.page) {
                ui->settings_ui.page       = new_page;
                ui->settings_ui.tab_switch = 1;
                ui->needs_redraw        = 1;
            }
        } else {
            int touched_col = (dual && ev->x >= ui->screen_w / 2) ? 1 : 0;
            int lcw = dual ? ui->screen_w / 2 : ui->screen_w;
            SettingsRow rows[48];
            int nrows = settings_build_rows(t_items, nt_items, cont_y, sec_h, row_h,
                                            rows, 48);
            bool demo_hit = false;
            if (dual) {
                int max_y = settings_dual_max_y(rows, nrows, cont_y);
                Rect btn = demo_button_rect(ui, max_y, row_h,
                                            tr(LANG_EN, S_DEMO_RUN));
                if (ev->x >= btn.x && ev->x < btn.x + btn.w &&
                    ev->y >= btn.y && ev->y < btn.y + btn.h) {
                    demo_hit = true;
                    press_demo(ui);
                    ui->needs_redraw = 1;
                }
            }
            for (int i = 0; !demo_hit && i < nrows; i++) {
                if (rows[i].kind == ITEM_SEC) continue;
                if (rows[i].col != touched_col) continue;
                if (ev->y < rows[i].y || ev->y >= rows[i].y + rows[i].h) continue;
                const SettingsItem *item = &t_items[rows[i].item_idx];
                switch (rows[i].kind) {
                case ITEM_ROW:
                case ITEM_ROW_GROUP:
                    if (item->scope == ROW_SCOPE_WHOLE_BODY) {
                        ui->settings_ui.dirty_item = -1;
                    } else {
                        ui->settings_ui.dirty_item     = rows[i].item_idx;
                        ui->settings_ui.dirty_sub      = rows[i].sub_idx;
                        ui->settings_ui.dirty_prev_sub = -1;
                        /* ROW_GROUP reselection also unchecks the previously
                         * checked sub-row elsewhere in the group — flag it
                         * too, so draw_settings_screen erases it. */
                        if (rows[i].kind == ITEM_ROW_GROUP && item->is_checked) {
                            for (int k = 0; k < nrows; k++) {
                                if (rows[k].item_idx == rows[i].item_idx &&
                                    rows[k].sub_idx  != rows[i].sub_idx &&
                                    item->is_checked(ui, rows[k].sub_idx)) {
                                    ui->settings_ui.dirty_prev_sub = rows[k].sub_idx;
                                    break;
                                }
                            }
                        }
                    }
                    if (item->on_select) item->on_select(ui, rows[i].sub_idx);
                    break;
                case ITEM_STEPPER: {
                    int lx = (dual && rows[i].col == 1) ? ui->screen_w / 2 : 0;
                    RowGeom rg = compute_row_geom(ui, lx, rows[i].y, lcw, rows[i].h);
                    StepperHit sh = compute_stepper_hit(ui, &rg);
                    int d = (ev->x >= sh.rx0 && ev->x < sh.rx1) ?  1 :
                            (ev->x >= sh.lx0 && ev->x < sh.lx1) ? -1 : 0;
                    if (d != 0 && item->on_step) {
                        ui->settings_ui.dirty_item = (item->scope == ROW_SCOPE_OWN_RECT)
                            ? rows[i].item_idx : -1;
                        item->on_step(ui, d);
                    }
                    break;
                }
                case ITEM_BUTTON:
                    ui->settings_ui.dirty_item = (item->scope == ROW_SCOPE_OWN_RECT)
                        ? rows[i].item_idx : -1;
                    if (item->on_press) item->on_press(ui);
                    break;
                default: break;
                }
                break; /* first hit wins */
            }
        }
    }
    ui->touch.active = 0;
}
