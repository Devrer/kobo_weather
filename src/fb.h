#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct {
    int       fd;
    uint8_t  *mem;
    size_t    mem_size;
    int       w, h;
    int       bpp;
    int       stride;
    int       is_rgb565;
    int       clip_y_min; /* rows above this y are skipped (0 = no clip) */
    uint8_t  *snapshot;   /* Nickel's screen at fb_open, restored on exit to avoid a white flash */
} FB;

/* Bounding box for a screen element / refresh region. */
typedef struct { int x, y, w, h; } Rect;

/* FBInk's full-screen refresh convention. */
#define RECT_FULLSCREEN ((Rect){0, 0, 0, 0})

/* Quadrant for fb_draw_corner_arc: dx/dy are each +-1, e.g. {-1,-1}=top-left. */
typedef struct { int dx, dy; } Quadrant;

int  fb_open(FB *fb);
void fb_close(FB *fb);

/* Restores the snapshot captured at fb_open. Returns 1 if copied, 0 otherwise. */
int  fb_restore_snapshot(FB *fb);

void fb_set_pixel(FB *fb, int x, int y, uint8_t gray); /* 0=black 255=white */
void fb_fill_rect(FB *fb, Rect r, uint8_t gray);

/* Dumps the framebuffer to an 8-bit grayscale BMP at `path`. 0 on success, -1 on failure. */
int fb_dump_bmp(const FB *fb, const char *path);
void fb_draw_rect_outline(FB *fb, Rect r, uint8_t gray, int thick);

void fb_draw_corner_arc(FB *fb, int cx, int cy, int r, int thick,
                        uint8_t gray, Quadrant q);
void fb_draw_rounded_rect_outline(FB *fb, int x, int y, int w, int h,
                                  int r, int thick, uint8_t gray);
void fb_draw_circle(FB *fb, int cx, int cy, int r, uint8_t gray);
void fb_draw_circle_outline(FB *fb, int cx, int cy, int r, uint8_t gray, int thick);

/* Paced refresh: each call waits for the previous EPDC update to finish
 * before submitting the next, so the queue can't back up (max 1 frame in flight). */
void fb_paced_init(int fbink_fd);
void fb_paced_refresh(Rect r, int wfm, bool flashing);
void fb_paced_wait(void);

/* Set from --debug CLI flag; enables per-refresh logging. */
extern bool g_debug;

/* Refresh style for content updates. Values 0-1 are persisted in UISettings
 * (config.h) — don't reorder without updating settings_load and ui.c's Display tab. */
typedef enum {
    RSTYLE_REAGL = 0,   /* single REAGL pass, no flash */
    RSTYLE_GC16  = 1,   /* single GC16 pass, flash iff gc16_flash setting is on */
    RSTYLE_GC16_NOFLASH, /* single GC16 pass, never flashes (runtime-only) */
    RSTYLE_GC16_FLASH,  /* single GC16 pass, always flashes (runtime-only) */
} RefreshStyle;
