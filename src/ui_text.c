#include "ui_internal.h"
#include <string.h>

/* Return the pixel rect of the most recently rendered FBInk OT text. */
Rect fbink_last_rect(void) {
    FBInkRect lr = fbink_get_last_rect(false);
    return (Rect){ lr.left, lr.top, lr.width, lr.height };
}

/* Bounding-box union; a zero-area rect (w==0) is treated as empty. */
Rect rect_union(Rect a, Rect b) {
    if (a.w == 0) return b;
    if (b.w == 0) return a;
    int x1 = a.x < b.x ? a.x : b.x;
    int y1 = a.y < b.y ? a.y : b.y;
    int x2 = (a.x + a.w) > (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    int y2 = (a.y + a.h) > (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);
    return (Rect){ x1, y1, x2 - x1, y2 - y1 };
}

void draw_text(int size_pt, int x, int y_top, int max_w, const char *s) {
    if (!g_disp.font_loaded) return;
    /* Skip off-screen rows; FBInk logs noisily for out-of-range margins. */
    if (y_top < 0 || y_top >= g_disp.fb.h) return;
    FBInkOTConfig ot;
    memset(&ot, 0, sizeof(ot));
    ot.size_pt        = (unsigned short)size_pt;
    ot.margins.top    = (short)y_top;
    ot.margins.left   = (short)x;
    ot.margins.right  = (short)(g_disp.fb.w - x - max_w);
    if (ot.margins.right < 0) ot.margins.right = 0;
    FBInkConfig c = g_disp.cfg;
    c.no_refresh = true;
    c.is_bgless  = true;   /* draw glyphs only — never paint over separators */
    fbink_print_ot(g_disp.fbink_fd, s, &ot, &c, NULL);
}

/* Like draw_text(), but also bounds the bottom edge so wrapped text can't
 * bleed past a fixed-height band. */
void draw_text_clipped(int size_pt, int x, int y_top, int max_w, int max_h,
                        const char *s) {
    if (!g_disp.font_loaded) return;
    if (y_top < 0 || y_top >= g_disp.fb.h) return;
    FBInkOTConfig ot;
    memset(&ot, 0, sizeof(ot));
    ot.size_pt        = (unsigned short)size_pt;
    ot.margins.top    = (short)y_top;
    ot.margins.left   = (short)x;
    ot.margins.right  = (short)(g_disp.fb.w - x - max_w);
    if (ot.margins.right < 0) ot.margins.right = 0;
    ot.margins.bottom = (short)(g_disp.fb.h - y_top - max_h);
    if (ot.margins.bottom < 0) ot.margins.bottom = 0;
    FBInkConfig c = g_disp.cfg;
    c.no_refresh = true;
    c.is_bgless  = true;
    fbink_print_ot(g_disp.fbink_fd, s, &ot, &c, NULL);
}

/* Right-align `s` so its right edge lands at right_x; min_x clamps the start
 * so long text can't spill over neighbouring content. */
void draw_text_right_styled(int size_pt, int right_x, int y_top, int min_x,
                            const char *s, FONT_STYLE_T style) {
    if (!g_disp.font_loaded || !s || !s[0]) return;
    if (y_top < 0 || y_top >= g_disp.fb.h) return;

    FBInkConfig c = g_disp.cfg;
    c.no_refresh = true;
    c.is_bgless  = true;

    FBInkOTConfig ot;
    memset(&ot, 0, sizeof(ot));
    ot.size_pt       = (unsigned short)size_pt;
    ot.style         = style;
    ot.margins.top   = (short)y_top;
    ot.margins.left  = (short)min_x;
    ot.margins.right = (short)(g_disp.fb.w - right_x);
    if (ot.margins.right < 0) ot.margins.right = 0;

    /* Measure only — no rendering. */
    ot.compute_only  = true;
    ot.no_truncation = true;
    FBInkOTFit fit;
    memset(&fit, 0, sizeof(fit));
    fbink_print_ot(g_disp.fbink_fd, s, &ot, &c, &fit);
    ot.compute_only  = false;
    ot.no_truncation = false;
    int tw = (int)fit.bbox.width;
    if (tw <= 0) return;

    /* Single real draw, right-aligned (clamped at min_x). */
    int startx = right_x - tw;
    if (startx < min_x) startx = min_x;
    ot.margins.left = (short)startx;
    fbink_print_ot(g_disp.fbink_fd, s, &ot, &c, NULL);
}

void draw_text_right(int size_pt, int right_x, int y_top, int min_x,
                     const char *s) {
    draw_text_right_styled(size_pt, right_x, y_top, min_x, s, FNT_REGULAR);
}

/* Width in px that `s` would occupy at `size_pt` (no drawing). Returns 0 for
 * an empty string or before the font is loaded. */
int measure_text_w_styled(int size_pt, const char *s, FONT_STYLE_T style) {
    if (!g_disp.font_loaded || !s || !s[0]) return 0;
    FBInkConfig c = g_disp.cfg;
    c.no_refresh = true;
    c.is_bgless  = true;
    FBInkOTConfig ot;
    memset(&ot, 0, sizeof(ot));
    ot.size_pt       = (unsigned short)size_pt;
    ot.style         = style;
    ot.compute_only  = true;
    ot.no_truncation = true;
    FBInkOTFit fit;
    memset(&fit, 0, sizeof(fit));
    fbink_print_ot(g_disp.fbink_fd, s, &ot, &c, &fit);
    return (int)fit.bbox.width;
}

/* Low-temperature point size, shrunk when the "smaller min" toggle is on. */
int min_temp_pt(const UIState *ui) {
    return ui->persisted.small_min_temp ? UI_FONT_TEMP_MIN_SMALL_PT : UI_FONT_TEMP_LO_PT;
}

/* Resolve a bold toggle to an actual font style, falling back to regular
 * when no real bold face was registered. */
FONT_STYLE_T temp_style(int on) {
    return (on && g_disp.bold_loaded) ? FNT_BOLD : FNT_REGULAR;
}

/* Horizontally centre `s` within [x_left, x_left+box_w] at y_top.
 * FBInk's OTConfig.is_centered is a no-op in this build, so we centre manually. */
void draw_text_centered_styled(int size_pt, int x_left, int y_top, int box_w,
                               const char *s, FONT_STYLE_T style) {
    if (!g_disp.font_loaded || !s || !s[0]) return;
    if (y_top < 0 || y_top >= g_disp.fb.h) return;

    FBInkConfig c = g_disp.cfg;
    c.no_refresh = true;
    c.is_bgless  = true;   /* draw glyphs only — never paint over separators */

    FBInkOTConfig ot;
    memset(&ot, 0, sizeof(ot));
    ot.size_pt       = (unsigned short)size_pt;
    ot.style         = style;
    ot.margins.top   = (short)y_top;
    ot.margins.left  = (short)x_left;
    ot.margins.right = (short)(g_disp.fb.w - x_left - box_w);
    if (ot.margins.right < 0) ot.margins.right = 0;

    /* Measure only — no rendering. */
    ot.compute_only  = true;
    ot.no_truncation = true;
    FBInkOTFit fit;
    memset(&fit, 0, sizeof(fit));
    fbink_print_ot(g_disp.fbink_fd, s, &ot, &c, &fit);
    ot.compute_only  = false;
    ot.no_truncation = false;
    int tw = (int)fit.bbox.width;
    if (tw <= 0 || tw >= box_w) {
        /* Unmeasurable or wider than the box: draw left-aligned as a fallback. */
        fbink_print_ot(g_disp.fbink_fd, s, &ot, &c, NULL);
        return;
    }

    /* Single real draw, centred. */
    int startx = x_left + (box_w - tw) / 2;
    ot.margins.left  = (short)startx;
    ot.margins.right = (short)(g_disp.fb.w - startx - tw);
    if (ot.margins.right < 0) ot.margins.right = 0;
    fbink_print_ot(g_disp.fbink_fd, s, &ot, &c, NULL);
}

void draw_text_centered(int size_pt, int x_left, int y_top, int box_w,
                        const char *s) {
    draw_text_centered_styled(size_pt, x_left, y_top, box_w, s, FNT_REGULAR);
}

/* Renders `s` inside a fixed-height two-line slot, centred horizontally.
 * Single-line text is vertically centred; longer text is word-wrapped onto
 * two lines (slot_h == 2*em_h + line_gap). Slot's bottom edge never moves.
 * Returns the union bounding Rect of what was drawn. */
Rect draw_text_centered_wrapped(int size_pt, Rect slot, int line_gap,
                                const char *s, FONT_STYLE_T style) {
    if (!g_disp.font_loaded || !s || !s[0]) return (Rect){0,0,0,0};
    int em_h = band(size_pt)->em_h;

    int tw = measure_text_w_styled(size_pt, s, style);
    if (tw > 0 && tw < slot.w) {
        int y = slot.y + (slot.h - em_h) / 2;
        draw_text_centered_styled(size_pt, slot.x, y, slot.w, s, style);
        return fbink_last_rect();
    }

    /* Greedy word-wrap on ASCII spaces; UTF-8 safe. */
    char line1[256] = "";
    char line2[256] = "";
    int on_line2 = 0;
    const char *p = s;
    while (*p) {
        while (*p == ' ') p++;
        const char *start = p;
        while (*p && *p != ' ') p++;
        size_t wl = (size_t)(p - start);
        if (wl == 0) break;
        if (wl >= sizeof(line1)) wl = sizeof(line1) - 1;
        char word[256];
        memcpy(word, start, wl);
        word[wl] = '\0';

        if (!on_line2) {
            size_t l1 = strlen(line1);
            if (line1[0]) {
                /* Tentatively append, measure, undo if too wide. */
                if (l1 + 1 + wl + 1 <= sizeof line1) {
                    line1[l1] = ' ';
                    memcpy(line1 + l1 + 1, word, wl + 1);
                    int line_w = measure_text_w_styled(size_pt, line1, style);
                    if (line_w > 0 && line_w >= slot.w) {
                        line1[l1] = '\0';  /* undo */
                        on_line2 = 1;
                    } else {
                        continue;
                    }
                } else {
                    on_line2 = 1;
                }
            } else {
                memcpy(line1, word, wl + 1);
                continue;
            }
        }
        /* Append word to line2. */
        size_t l2 = strlen(line2);
        if (line2[0]) {
            if (l2 + 1 + wl + 1 <= sizeof line2) {
                line2[l2] = ' ';
                memcpy(line2 + l2 + 1, word, wl + 1);
            }
        } else {
            memcpy(line2, word, wl + 1);
        }
    }
    if (!line1[0]) { memcpy(line1, line2, strlen(line2) + 1); line2[0] = '\0'; }

    draw_text_centered_styled(size_pt, slot.x, slot.y, slot.w, line1, style);
    Rect total = fbink_last_rect();
    if (line2[0]) {
        draw_text_centered_styled(size_pt, slot.x, slot.y + em_h + line_gap, slot.w, line2, style);
        total = rect_union(total, fbink_last_rect());
    }
    return total;
}
