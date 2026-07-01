#include "ui_internal.h"

/* Settings "Demo" preview: mirrors the headerless sleep screen layout while
 * main.c cycles ui->demo.symbol through every Wsymb2 code once per second. */
void draw_demo_screen(UIState *ui) {
    fb_fill_rect(&g_disp.fb, (Rect){0, 0, ui->screen_w, ui->screen_h}, UI_BG);
    draw_current_weather(ui);
    draw_forecast(ui);
    draw_subtitle(ui);
    if (!ui->refresh.drawn_demo) {
        ui_region_commit(ui, RECT_FULLSCREEN, ui->persisted.full_style);  /* entry: REAGL or GC16 per full_style */
        ui->refresh.drawn_demo = 1;
    } else {
        ui_region_commit(ui, RECT_FULLSCREEN, ui->persisted.partial_style);
    }
}
