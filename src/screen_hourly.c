#include "ui_internal.h"
#include "i18n.h"
#include <stdio.h>
#include <time.h>

static void draw_hourly_row(UIState *ui, int row_idx, int y_top) {
    if (row_idx < 0 || row_idx >= ui->hourly_count) return;
    int idx = ui->hourly_idx[row_idx];
    WeatherHour *h = &ui->state->hours[idx];

    if (y_top + ui->hourly_row_h <= ui->header_h) return;
    if (y_top >= ui->screen_h) return;
    /* At rest, skip a row only partially visible at the bottom edge. */
    if (!ui->scroll.active && y_top + ui->hourly_row_h > ui->screen_h) return;

    /* Row background — clip_y_min protects header from overpaint */
    fb_fill_rect(&g_disp.fb, (Rect){0, y_top, ui->screen_w, ui->hourly_row_h}, UI_BG);

    /* Bottom separator */
    fb_fill_rect(&g_disp.fb, (Rect){0, y_top + ui->hourly_row_h - UI_REF_SEP_THICK,
                 ui->screen_w, UI_REF_SEP_THICK}, UI_SEP);

    /* FBInk text bypasses our clip — if the row top is behind the header,
     * skip text and icon to avoid painting into the header band. */
    if (y_top < ui->header_h) return;

    /* Time on left — hour-only ("13" not "13:00"). */
    struct tm lt;
    localtime_r(&h->valid_time, &lt);
    char buf[64];
    snprintf(buf, sizeof(buf), "%02d", lt.tm_hour);
    int text_y = center_y(y_top, ui->hourly_row_h, UI_FONT_HOURLY_PT);
    draw_text(UI_FONT_HOURLY_PT, ui->pad, text_y, sc(UI_REF_HOURLY_TIME_W), buf);

    /* Small icon — row_h > 0 centres the icon on the true decoded PNG height. */
    if (h->has_symbol) {
        int icon_x = ui->pad + sc(UI_REF_HOURLY_ICON_X);
        draw_icon(h->symbol, h->valid_time, icon_x, y_top, ui->hourly_row_h, ICON_SMALL);
    }

    int temp_x = ui->pad + sc(UI_REF_HOURLY_ICON_X) + ui->small_icon + ui->pad * 2;
    int wind_x = temp_x + sc(UI_REF_HOURLY_WIND_DX);
    int dir_x  = wind_x + sc(UI_REF_HOURLY_DIR_DX);   /* speed gets its own wide box so "m/s" never wraps */

    /* Temperature — box runs up to the wind column so "°C"/"°F" isn't truncated. */
    if (h->has_temp) snprintf(buf, sizeof(buf), "%.1f%s", h->temp, temp_unit_suffix(ui->state->units));
    else             snprintf(buf, sizeof(buf), "---%s", temp_unit_suffix(ui->state->units));
    draw_text(UI_FONT_HOURLY_TEMP_PT, temp_x, text_y, wind_x - temp_x - ui->pad, buf);

    /* Wind — speed with gusts in parens, then direction in a smaller font. */
    if (h->has_wind) {
        if (h->has_gust)
            snprintf(buf, sizeof(buf), "%.1f(%.0f)%s", h->wind, h->wind_gust,
                     wind_unit_str(ui->state->units));
        else
            snprintf(buf, sizeof(buf), "%.1f%s", h->wind, wind_unit_str(ui->state->units));
        draw_text(UI_FONT_HOURLY_WIND_PT, wind_x, text_y, dir_x - wind_x - ui->pad, buf);
        if (h->has_dir) {
            snprintf(buf, sizeof(buf), "%s", tr_compass((Lang)ui->persisted.lang, h->wind_dir));
            draw_text(UI_FONT_HOURLY_DIR_PT, dir_x, text_y + 5,
                      ui->screen_w - dir_x - ui->pad, buf);
        }
    } else {
        snprintf(buf, sizeof(buf), "%s", tr((Lang)ui->persisted.lang, S_WIND_NA));
        draw_text(UI_FONT_HOURLY_WIND_PT, wind_x, text_y,
                  ui->screen_w - wind_x - ui->pad, buf);
    }
}

void draw_hourly_screen(UIState *ui) {
    int  scrolling      = ui->scroll.active;
    bool screen_change  = !ui->refresh.drawn_hourly;
    bool reagl_no_flash = (ui->persisted.snap == SCROLL_SNAP_REAGL);
    RefreshStyle rcommit_style = resolve_refresh_style(&(RefreshCtx){
        .screen_change = screen_change,
        .snap_reagl    = reagl_no_flash,
        .full_style    = ui->persisted.full_style,
        .partial_style = ui->persisted.partial_style,
    });

    /* List area below the header; scroll refresh/flash are bounded to it. */
    int list_y = ui->header_h;
    int list_h = ui->screen_h - ui->header_h;

    WakeOverride wo = {0};
    if (scrolling) {
        /* Repaint only the list — leaves the header untouched. */
        fb_fill_rect(&g_disp.fb, (Rect){0, list_y, ui->screen_w, list_h}, UI_BG);
    } else {
        fb_fill_rect(&g_disp.fb, (Rect){0, 0, ui->screen_w, ui->screen_h}, UI_BG);

        /* A pending kiosk wake override can upgrade this entry to a flash
         * or INIT-wash; must run while the panel is still pre-commit. */
        if (screen_change && !ui->refresh.hourly_scroll_settle) {
            wo = ui_consume_wake_override(ui);
            if (wo.init_wash) ui_run_init_wash(ui);
        }

        char title[64];
        format_long_date((Lang)ui->persisted.lang, ui->selected_day, title, sizeof(title));
        draw_header(ui, title, 1, 0, 0);
    }

    int render_sy = ui->scroll.y;

    /* Set clip so rows don't paint into the header. */
    g_disp.fb.clip_y_min = ui->header_h;

    for (int i = 0; i < ui->hourly_count; i++) {
        int y_top = ui->header_h + i * ui->hourly_row_h - render_sy;
        draw_hourly_row(ui, i, y_top);
    }

    g_disp.fb.clip_y_min = 0;

    if (scrolling) {
        /* A2 for floating/snap-A2, REAGL for snap-REAGL; chosen by snap_scroll only. */
        int wfm = (ui->persisted.snap == SCROLL_SNAP_REAGL) ? WFM_REAGL : WFM_A2;
        fb_paced_refresh((Rect){0, list_y, ui->screen_w, list_h}, wfm, false);
    } else if (screen_change) {
        if (ui->refresh.hourly_scroll_settle) {
            /* Scroll-stop settle: flashing GC16 clears A2/REAGL ghosts, bounded
             * to the list area (header is stable mid-scroll, must not flash). */
            fb_paced_refresh((Rect){0, list_y, ui->screen_w, list_h}, WFM_GC16, true);
            ui->refresh.hourly_scroll_settle = 0;
        } else {
            /* Entry transition, resolved by rcommit_style unless a wake override applies. */
            ui_region_commit(ui, RECT_FULLSCREEN, wo.override ? wo.style : rcommit_style);
        }
        ui->refresh.drawn_hourly = 1;
    } else {
        /* Data-only redraw (idle / background-fetch). */
        ui_region_commit(ui, RECT_FULLSCREEN, rcommit_style);
        ui->refresh.drawn_hourly = 1;
    }
}
