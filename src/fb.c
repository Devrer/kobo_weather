#include "fb.h"
#include "sysutil.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>
#include <time.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <fbink.h>

int fb_open(FB *fb) {
    fb->snapshot = NULL;
    fb->fd = open("/dev/fb0", O_RDWR);
    if (fb->fd < 0) return -1;

    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &vinfo) < 0 ||
        ioctl(fb->fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        close(fb->fd);
        return -1;
    }

    fb->w      = (int)vinfo.xres;
    fb->h      = (int)vinfo.yres;
    fb->bpp    = (int)vinfo.bits_per_pixel;
    fb->stride = (int)finfo.line_length;
    fb->is_rgb565 = (fb->bpp == 16);
    if (fb->bpp != 8 && fb->bpp != 16 && fb->bpp != 32)
        fprintf(stderr, "Warning: unknown framebuffer bpp=%d, drawing may be garbled\n", fb->bpp);

    fb->mem_size = fb->stride * fb->h;
    fb->mem = mmap(NULL, fb->mem_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb->fd, 0);
    if (fb->mem == MAP_FAILED) { close(fb->fd); return -1; }

    /* Capture Nickel's screen before drawing over it, to restore on exit. */
    fb->snapshot = malloc(fb->mem_size);
    if (fb->snapshot) memcpy(fb->snapshot, fb->mem, fb->mem_size);
    return 0;
}

void fb_close(FB *fb) {
    if (fb->mem && fb->mem != MAP_FAILED) munmap(fb->mem, fb->mem_size);
    if (fb->fd >= 0) close(fb->fd);
    free(fb->snapshot);
    fb->snapshot = NULL;
}

int fb_restore_snapshot(FB *fb) {
    if (!fb->snapshot || !fb->mem || fb->mem == MAP_FAILED) return 0;
    memcpy(fb->mem, fb->snapshot, fb->mem_size);
    return 1;
}

void fb_set_pixel(FB *fb, int x, int y, uint8_t gray) {
    if (x < 0 || y < fb->clip_y_min || y < 0 || x >= fb->w || y >= fb->h) return;
    if (fb->is_rgb565) {
        uint16_t v = ((gray >> 3) << 11) | ((gray >> 2) << 5) | (gray >> 3);
        uint16_t *p = (uint16_t *)(fb->mem + y * fb->stride + x * 2);
        *p = v;
    } else if (fb->bpp == 32) {
        uint32_t *p = (uint32_t *)(fb->mem + y * fb->stride + x * 4);
        *p = 0xFF000000u | ((uint32_t)gray << 16) | ((uint32_t)gray << 8) | (uint32_t)gray;
    } else {
        fb->mem[y * fb->stride + x] = gray;
    }
}

void fb_fill_rect(FB *fb, Rect rect, uint8_t gray) {
    int x = rect.x, y = rect.y, w = rect.w, h = rect.h;
    if (x < 0)      { w += x; x = 0; }
    if (y < 0)      { h += y; y = 0; }
    if (y < fb->clip_y_min) { h -= (fb->clip_y_min - y); y = fb->clip_y_min; }
    if (w <= 0 || h <= 0 || x >= fb->w || y >= fb->h) return;
    if (x + w > fb->w) w = fb->w - x;
    if (y + h > fb->h) h = fb->h - y;

    if (fb->is_rgb565) {
        uint16_t v = ((gray >> 3) << 11) | ((gray >> 2) << 5) | (gray >> 3);
        uint16_t *first = (uint16_t *)(fb->mem + y * fb->stride + x * 2);
        for (int col = 0; col < w; col++) first[col] = v;
        for (int row = y + 1; row < y + h; row++)
            memcpy(fb->mem + row * fb->stride + x * 2, first, (size_t)w * 2);
    } else if (fb->bpp == 32) {
        uint32_t v = 0xFF000000u | ((uint32_t)gray << 16) | ((uint32_t)gray << 8) | (uint32_t)gray;
        uint32_t *first = (uint32_t *)(fb->mem + y * fb->stride + x * 4);
        for (int col = 0; col < w; col++) first[col] = v;
        for (int row = y + 1; row < y + h; row++)
            memcpy(fb->mem + row * fb->stride + x * 4, first, (size_t)w * 4);
    } else {
        for (int row = y; row < y + h; row++)
            memset(fb->mem + row * fb->stride + x, gray, (size_t)w);
    }
}

/* Gray value at (x,y); inverse of fb_set_pixel's per-bpp packing. */
static uint8_t fb_get_gray(const FB *fb, int x, int y) {
    if (fb->is_rgb565) {
        uint16_t v = *(const uint16_t *)(fb->mem + y * fb->stride + x * 2);
        return (uint8_t)(((v >> 5) & 0x3F) << 2);
    } else if (fb->bpp == 32) {
        return fb->mem[y * fb->stride + x * 4];
    } else {
        return fb->mem[y * fb->stride + x];
    }
}

static void bmp_put_u16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
static void bmp_put_u32(uint8_t *p, uint32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}

int fb_dump_bmp(const FB *fb, const char *path) {
    int row_size  = (fb->w + 3) & ~3;   /* BMP rows padded to a 4-byte boundary */
    long pix_off  = 14 + 40 + 256 * 4;
    long file_size = pix_off + (long)row_size * fb->h;

    FILE *f = fopen(path, "wb");
    if (!f) {
        sysutil_log("", true, "fb_dump_bmp: open %s failed", path);
        return -1;
    }

    uint8_t hdr[14 + 40] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    bmp_put_u32(hdr + 2,  (uint32_t)file_size);
    bmp_put_u32(hdr + 10, (uint32_t)pix_off);
    bmp_put_u32(hdr + 14, 40);             /* biSize */
    bmp_put_u32(hdr + 18, (uint32_t)fb->w);
    bmp_put_u32(hdr + 22, (uint32_t)fb->h); /* positive: bottom-up rows */
    bmp_put_u16(hdr + 26, 1);              /* biPlanes */
    bmp_put_u16(hdr + 28, 8);              /* biBitCount */
    bmp_put_u32(hdr + 34, (uint32_t)row_size * fb->h); /* biSizeImage */
    bmp_put_u32(hdr + 46, 256);            /* biClrUsed */

    uint8_t palette[256 * 4];
    for (int i = 0; i < 256; i++) {
        palette[i * 4 + 0] = (uint8_t)i;
        palette[i * 4 + 1] = (uint8_t)i;
        palette[i * 4 + 2] = (uint8_t)i;
        palette[i * 4 + 3] = 0;
    }

    int ok = fwrite(hdr, sizeof hdr, 1, f) == 1 &&
             fwrite(palette, sizeof palette, 1, f) == 1;

    uint8_t *row = ok ? calloc(1, (size_t)row_size) : NULL;
    if (ok && !row) ok = 0;
    for (int y = fb->h - 1; ok && y >= 0; y--) {   /* bottom-up */
        for (int x = 0; x < fb->w; x++)
            row[x] = fb_get_gray(fb, x, y);
        if (fwrite(row, (size_t)row_size, 1, f) != 1) ok = 0;
    }
    free(row);
    fclose(f);

    if (!ok) sysutil_log("", true, "fb_dump_bmp: write %s failed", path);
    return ok ? 0 : -1;
}

void fb_draw_rect_outline(FB *fb, Rect r, uint8_t gray, int thick) {
    fb_fill_rect(fb, (Rect){r.x,               r.y,               r.w,   thick}, gray);
    fb_fill_rect(fb, (Rect){r.x,               r.y + r.h - thick, r.w,   thick}, gray);
    fb_fill_rect(fb, (Rect){r.x,               r.y,               thick, r.h},   gray);
    fb_fill_rect(fb, (Rect){r.x + r.w - thick, r.y,               thick, r.h},   gray);
}

/* Row-based O(r) arc: one fb_fill_rect per row. */
void fb_draw_corner_arc(FB *fb, int cx, int cy, int r, int thick,
                        uint8_t gray, Quadrant q) {
    int ri = r - thick;
    if (ri < 0) ri = 0;
    for (int dy = 0; dy <= r; dy++) {
        int hw_o = (int)sqrtf((float)(r * r - dy * dy));
        int hw_i = (dy <= ri) ? (int)sqrtf((float)(ri * ri - dy * dy)) : 0;
        int seg  = hw_o - hw_i;
        if (seg <= 0) continue;
        int rx = (q.dx > 0) ? cx + hw_i : cx - hw_o;
        fb_fill_rect(fb, (Rect){rx, cy + dy * q.dy, seg, 1}, gray);
    }
}

void fb_draw_rounded_rect_outline(FB *fb, int x, int y, int w, int h,
                                  int r, int thick, uint8_t gray) {
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r < 1)     r = 1;
    fb_fill_rect(fb, (Rect){x + r,         y,              w - 2 * r, thick}, gray);
    fb_fill_rect(fb, (Rect){x + r,         y + h - thick,  w - 2 * r, thick}, gray);
    fb_fill_rect(fb, (Rect){x,             y + r,           thick, h - 2 * r}, gray);
    fb_fill_rect(fb, (Rect){x + w - thick, y + r,           thick, h - 2 * r}, gray);
    fb_draw_corner_arc(fb, x + r,         y + r,         r, thick, gray, (Quadrant){-1, -1});
    fb_draw_corner_arc(fb, x + w - r - 1, y + r,         r, thick, gray, (Quadrant){+1, -1});
    fb_draw_corner_arc(fb, x + r,         y + h - r - 1, r, thick, gray, (Quadrant){-1, +1});
    fb_draw_corner_arc(fb, x + w - r - 1, y + h - r - 1, r, thick, gray, (Quadrant){+1, +1});
}

/* Row-based circle fill: O(r) rows, each a memset via fb_fill_rect. */
void fb_draw_circle(FB *fb, int cx, int cy, int r, uint8_t gray) {
    for (int dy = -r; dy <= r; dy++) {
        int hw = (int)sqrtf((float)(r * r - dy * dy));
        fb_fill_rect(fb, (Rect){cx - hw, cy + dy, hw * 2 + 1, 1}, gray);
    }
}

static int  g_paced_fd      = -1;
static bool g_have_pending  = false;

void fb_paced_init(int fbink_fd) {
    g_paced_fd     = fbink_fd;
    g_have_pending = false;
}

void fb_paced_wait(void) {
    if (!g_have_pending || g_paced_fd < 0) return;
    /* Waits for the EPDC to finish rendering the most recent submitted update. */
    fbink_wait_for_complete(g_paced_fd, LAST_MARKER);
    g_have_pending = false;
}

void fb_paced_refresh(Rect rect, int wfm, bool flashing) {
    int x = rect.x, y = rect.y, w = rect.w, h = rect.h;
    if (g_paced_fd < 0) return;
    fb_paced_wait();

    FBInkConfig cfg = {0};
    /* __typeof__ avoids hard-coding FBInk's enum-typedef name (varies across versions). */
    cfg.wfm_mode    = (__typeof__(cfg.wfm_mode)) wfm;
    cfg.is_flashing = flashing;
    /* no_refresh stays false — this call IS the refresh */

    fbink_refresh(g_paced_fd,
                  (uint32_t) y, (uint32_t) x,
                  (uint32_t) w, (uint32_t) h,
                  &cfg);
    g_have_pending = true;

    if (g_debug) {
        /* (0,0,0,0) is the full-screen convention; anything else is a sub-rect. */
        int full = (w == 0 && h == 0);
        const char *kind = full ? (flashing ? "full screen wash" : "full update")
                                : (flashing ? "partial flash"    : "partial update");
        const char *name = wfm == WFM_A2     ? "A2"    :
                           wfm == WFM_DU     ? "DU"    :
                           wfm == WFM_GC16   ? "GC16"  :
                           wfm == WFM_REAGL  ? "REAGL" :
                           wfm == WFM_INIT   ? "INIT"  :
                           wfm == WFM_AUTO   ? "AUTO"  : "?";
        time_t _t = time(NULL); struct tm _tm; localtime_r(&_t, &_tm);
        fprintf(stderr, "%02d:%02d:%02d REFRESH %-16s wfm=%-4s rect=(%d,%d %dx%d)\n",
                _tm.tm_hour, _tm.tm_min, _tm.tm_sec, kind, name, x, y, w, h);
    }
}

/* Row-based circle outline: two fb_fill_rect spans per row. */
void fb_draw_circle_outline(FB *fb, int cx, int cy, int r, uint8_t gray, int thick) {
    int ri = r - thick;
    if (ri < 0) ri = 0;
    for (int dy = -r; dy <= r; dy++) {
        int hw_o = (int)sqrtf((float)(r * r - dy * dy));
        if (ri > 0 && dy >= -ri && dy <= ri) {
            int hw_i = (int)sqrtf((float)(ri * ri - dy * dy));
            int seg  = hw_o - hw_i;
            if (seg > 0) {
                fb_fill_rect(fb, (Rect){cx - hw_o,      cy + dy, seg, 1}, gray);
                fb_fill_rect(fb, (Rect){cx + hw_i + 1,  cy + dy, seg, 1}, gray);
            }
        } else {
            fb_fill_rect(fb, (Rect){cx - hw_o, cy + dy, hw_o * 2 + 1, 1}, gray);
        }
    }
}
