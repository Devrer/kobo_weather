#include "ui_internal.h"
#include <string.h>

static void draw_x_symbol(int cx, int cy, int half, int thick, uint8_t gray) {
    for (int i = -half; i <= half; i++) {
        for (int t = -thick / 2; t <= thick / 2; t++) {
            fb_set_pixel(&g_disp.fb, cx + i + t, cy + i, gray);
            fb_set_pixel(&g_disp.fb, cx - i + t, cy + i, gray);
        }
    }
}

static void draw_close_button(UIState *ui) {
    int bx = ui->screen_w - ui->header_h;
    draw_x_symbol(bx + ui->header_h / 2, ui->header_h / 2,
                  ui->x_half, ui->x_half / 3 + 1, UI_FG);
}

int hit_close_button(UIState *ui, int tx, int ty) {
    return tx >= ui->screen_w - ui->header_h && tx < ui->screen_w &&
           ty >= 0 && ty < ui->header_h;
}

/* Power-save button: crescent moon left of the close (X) button. */
static void draw_powersave_button(UIState *ui) {
    int bx = ui->screen_w - 2 * ui->header_h;   /* left edge of this square */
    int cx  = bx + ui->header_h / 2 - sc(UI_REF_HEADER_ICON_GAP);
    int cy  = ui->header_h / 2;
    int r   = ui->x_half;
    int off = r * 3 / 4;                          /* offset of cut circle, carves the crescent */
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            int in_main = dx * dx + dy * dy <= r * r;
            int cdx = dx - off;
            int in_cut  = cdx * cdx + dy * dy <= r * r;
            if (in_main && !in_cut)
                fb_set_pixel(&g_disp.fb, cx + dx, cy + dy, UI_FG);
        }
    }
}

int hit_powersave_button(UIState *ui, int tx, int ty) {
    return tx >= ui->screen_w - 2 * ui->header_h &&
           tx <  ui->screen_w - ui->header_h &&
           ty >= 0 && ty < ui->header_h;
}

static void draw_back_button(UIState *ui) {
    int cx    = ui->header_h / 2;
    int cy    = ui->header_h / 2;
    int half  = ui->x_half;
    int thick = ui->x_half / 3 + 1;
    for (int i = 0; i <= half; i++) {
        for (int t = -thick / 2; t <= thick / 2; t++) {
            fb_set_pixel(&g_disp.fb, cx - half + i + t, cy - i, UI_FG);
            fb_set_pixel(&g_disp.fb, cx - half + i + t, cy + i, UI_FG);
        }
    }
}

/* Back (‹) and settings (≡) icons share the top-left header square, so they
 * share one hit-test; the active screen decides the action. */
int hit_top_left_button(UIState *ui, int tx, int ty) {
    return tx >= 0 && tx < ui->header_h && ty >= 0 && ty < ui->header_h;
}

/* Settings menu icon (≡): three horizontal bars in the top-left header square. */
static void draw_settings_button(UIState *ui) {
    int cx    = ui->header_h / 2;
    int cy    = ui->header_h / 2;
    int half  = ui->x_half;
    int thick = ui->x_half / 3 + 1;
    int gap   = ui->x_half / 2 + 2;
    for (int b = -1; b <= 1; b++)
        fb_fill_rect(&g_disp.fb, (Rect){cx - half, cy + b * gap - thick / 2, half * 2, thick}, UI_FG);
}

void draw_header(UIState *ui, const char *title, int show_back, int show_settings,
                 int show_power) {
    fb_fill_rect(&g_disp.fb, (Rect){0, 0, ui->screen_w, ui->header_h}, UI_BG);
    /* Separator only on sub-screens (those showing the back arrow). */
    if (show_back)
        fb_fill_rect(&g_disp.fb, (Rect){0, ui->header_h - UI_REF_SEP_THICK,
                     ui->screen_w, UI_REF_SEP_THICK}, UI_FG);

    if (g_disp.font_loaded && title[0] != '\0') {
        int left  = (show_back || show_settings) ? ui->header_h + ui->pad : ui->pad;
        int right = (show_power ? 2 * ui->header_h : ui->header_h) + ui->pad;

        FBInkOTConfig ot;
        memset(&ot, 0, sizeof(ot));
        ot.size_pt       = UI_FONT_HEADER_PT;
        ot.margins.top   = (short)center_y(0, ui->header_h, UI_FONT_HEADER_PT);
        ot.margins.left  = (short)left;
        ot.margins.right = (short)right;
        FBInkConfig c = g_disp.cfg;
        c.no_refresh    = true;
        c.is_bgless     = true;
        fbink_print_ot(g_disp.fbink_fd, title, &ot, &c, NULL);
    }

    if (show_back)     draw_back_button(ui);
    if (show_settings) draw_settings_button(ui);
    if (show_power)    draw_powersave_button(ui);
    draw_close_button(ui);
}
