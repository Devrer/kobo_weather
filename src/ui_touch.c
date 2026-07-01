#include "ui_internal.h"
#include "weather.h"
#include "sysutil.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <limits.h>

/* Snap-scroll: move the list one row in `dir` (+1 = later rows).
 * Returns 1 if the position actually changed (not already at the boundary). */
static int snap_step(UIState *ui, int dir) {
    int rh = ui->hourly_row_h;
    if (rh <= 0) return 0;
    int ny = ((ui->scroll.y + dir * rh) / rh) * rh;
    if (ny < 0) ny = 0;
    if (ny > ui->scroll.max) ny = ui->scroll.max;
    if (ny == ui->scroll.y) return 0;
    ui->scroll.y     = ny;
    ui->needs_redraw = 1;
    return 1;
}

static void enter_hourly(UIState *ui, int day_offset) {
    ui->refresh.drawn_hourly         = 0;
    ui->refresh.hourly_scroll_settle = 0;
    ui->refresh.hourly_scrolled      = 0;
    ui->selected_day  = day_offset;
    ui->hourly_count  = weather_hours_for_day(ui->state, day_offset, ui->hourly_idx);
    int total_h       = ui->hourly_count * ui->hourly_row_h;
    int visible_h     = ui->screen_h - ui->header_h;
    /* Row-aligned page height so scroll.max is a whole number of rows: no
     * partial row peeks past the header or screen bottom at either end. */
    int page_h        = (visible_h / ui->hourly_row_h) * ui->hourly_row_h;
    ui->scroll.max    = total_h > page_h ? total_h - page_h : 0;
    ui->scroll.y      = 0;
    ui_set_screen(ui, SCREEN_HOURLY);
    ui->needs_redraw  = 1;
}

static void exit_hourly(UIState *ui) {
    ui_set_screen(ui, SCREEN_MAIN);
    ui->refresh.drawn_hourly       = 0;
    ui->needs_redraw       = 1;
    ui->refresh.drawn_main.ok      = 0;   /* was showing hourly layout; force full redraw */
    if (ui->refresh.hourly_scrolled) {
        ui->refresh.hourly_scrolled     = 0;
        ui->refresh.full_redraw_reason  = RR_HOURLY_EXIT_FLASH;
    }
}

/* Returns day index 0..2 if (tx, ty) lands on a forecast column, -1 otherwise. */
static int hit_forecast_column(UIState *ui, int tx, int ty) {
    int y0 = ui->home_top + ui->current_h;
    if (ty < y0 || ty >= y0 + ui->forecast_h) return -1;
    int col_w = ui->screen_w / 3;
    int col = tx / col_w;
    if (col < 0 || col > 2) return -1;
    return col;
}

int ui_handle_touch(UIState *ui, const TouchEvent *ev) {
    /* Quit only fires on UP, not the down-touch, so the still-down finger
     * isn't inherited by Nickel's search button in the same top-right corner. */
    if (ui->touch.quit_pending) {
        return ev->type == EV_TOUCH_UP ? 1 : 0;
    }
    /* Swallow all events (incl. bounce DOWN) until the finger lifts, so a tap
     * can't re-fire on a button sharing the same corner of the new screen. */
    if (ui->touch.swallow) {
        if (ev->type == EV_TOUCH_UP) ui->touch.swallow = 0;
        return 0;
    }
    if (ev->type == EV_TOUCH_DOWN) {
        ui->touch.active           = 1;
        ui->touch.start_x          = ev->x;
        ui->touch.start_y          = ev->y;
        ui->touch.committed_scroll = 0;
        ui->touch.snap_steps             = 0;

        /* Hidden --debug screenshot corner: bottom-right header_h square,
         * unused by any screen's real controls. */
        if (g_debug &&
            ev->x >= ui->screen_w - ui->header_h &&
            ev->y >= ui->screen_h - ui->header_h) {
            char path[PATH_MAX];
            time_t t = time(NULL);
            struct tm lt;
            localtime_r(&t, &lt);
            snprintf(path, sizeof path, "%s/screenshot_%02d%02d%02d.bmp",
                    sysutil_exe_dir(), lt.tm_hour, lt.tm_min, lt.tm_sec);
            ui_dump_screenshot(path);
            ui->touch.active = 0;
            return 0;
        }

        if (ui->screen == SCREEN_DEMO) {
            /* Any tap exits the demo. */
            ui->demo.quit    = 1;
            ui->touch.active = 0;
            return 0;
        }
        if (hit_close_button(ui, ev->x, ev->y)) {
            ui->touch.quit_pending = 1;
            return 0;
        }
        if (ui->screen == SCREEN_HOURLY && hit_top_left_button(ui, ev->x, ev->y)) {
            exit_hourly(ui);
            ui->touch.active  = 0;
            ui->touch.swallow = 1;
            return 0;
        }
        if (ui->screen == SCREEN_MAIN && hit_powersave_button(ui, ev->x, ev->y)) {
            ui->power.enter = 1;   /* main.c enters power_save_loop() */
            ui->touch.active = 0;
            return 0;
        }
        if (ui->screen == SCREEN_MAIN && hit_top_left_button(ui, ev->x, ev->y)) {
            ui_set_screen(ui, SCREEN_SETTINGS);
            ui->refresh.drawn_settings = 0;
            ui->needs_redraw  = 1;
            ui->touch.active  = 0;
            ui->touch.swallow = 1;
            return 0;
        }
        if (ui->screen == SCREEN_SETTINGS) {
            settings_handle_touch(ui, ev);
            return 0;
        }
        return 0;
    }

    if (ev->type == EV_TOUCH_MOVE) {
        if (!ui->touch.active) return 0;
        if (ui->screen != SCREEN_HOURLY) return 0;

        int dy = ev->y - ui->touch.start_y;
        int dx = ev->x - ui->touch.start_x;
        if (!ui->touch.committed_scroll) {
            if (abs(dy) >= UI_GESTURE_COMMIT_PX && abs(dy) > abs(dx)) {
                ui->touch.committed_scroll = 1;
                ui->touch.snap_steps             = 0;
                if (ui->persisted.snap) {
                    /* scroll.active flags the fast-refresh draw path. */
                    ui->scroll.active = 1;
                } else {
                    scroll_start(&ui->scroll, ui->touch.start_y);
                }
            }
        }
        if (ui->touch.committed_scroll) {
            if (ui->persisted.snap) {
                /* Step one row for each row-height the finger has travelled.
                 * Reversing the finger snaps back; clamped edges stop the counter. */
                int rh     = ui->hourly_row_h;
                int target = rh > 0 ? (ui->touch.start_y - ev->y) / rh : 0;
                while (ui->touch.snap_steps < target && snap_step(ui, +1)) ui->touch.snap_steps++;
                while (ui->touch.snap_steps > target && snap_step(ui, -1)) ui->touch.snap_steps--;
            } else {
                if (scroll_drag(&ui->scroll, ev->y)) ui->needs_redraw = 1;
            }
        }
        return 0;
    }

    if (ev->type == EV_TOUCH_UP) {
        int was_committed = ui->touch.committed_scroll;
        ui->touch.active           = 0;
        ui->touch.committed_scroll = 0;

        if (was_committed) {
            if (ui->persisted.snap) {
                /* Guarantee at least one step for a short committed swipe. */
                if (ui->touch.snap_steps == 0)
                    snap_step(ui, ev->y < ui->touch.start_y ? +1 : -1);
                ui->refresh.drawn_hourly         = 0;   /* post-scroll GC16 wash must still fire */
                ui->refresh.hourly_scroll_settle = 1;
                ui->refresh.hourly_scrolled      = 1;
                ui->scroll.active        = 0;   /* next draw → static GC16 wash */
                ui->needs_redraw         = 1;
            } else {
                scroll_release(&ui->scroll);
                ui->refresh.drawn_hourly         = 0;   /* post-scroll GC16 wash must still fire */
                ui->refresh.hourly_scroll_settle = 1;
                ui->refresh.hourly_scrolled      = 1;
                ui->needs_redraw         = 1;
            }
            ui->touch.snap_steps = 0;
            return 0;
        }

        /* Tap: dispatch by screen */
        if (ui->screen == SCREEN_MAIN) {
            if (ev->y >= ui->home_top &&
                ev->y < ui->home_top + ui->current_h) {
                enter_hourly(ui, 0);          /* current weather → today, hour by hour */
            } else {
                int col = hit_forecast_column(ui, ev->x, ev->y);
                if (col >= 0) enter_hourly(ui, col + 1);
            }
        }
        return 0;
    }

    return 0;
}
