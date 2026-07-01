#include "ui_internal.h"
#include "sysutil.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <limits.h>

/* ---- Font band calibration ----------------------------------------- */
/* Per-pt visible ink offsets measured at startup by pixel-scanning fb->mem.
 * ink_top_off/ink_bottom_off: full "Åjg0" extent (accent to descender), used
 * by center_y(). cap_off/base_off: "H0" cap-height to baseline, used for
 * inter-line gaps in digit/cap text to avoid unused descender/accent slack. */

static FontBand  g_bands[FB_COUNT];
static const int k_band_pts[FB_COUNT] = {11,13,14,15,16,18,20,22,24,40,48};

const FontBand *band(int pt) {
    FontBandIdx i;
    switch (pt) {
    case 11: i = FB_11PT; break;
    case 13: i = FB_13PT; break;
    case 14: i = FB_14PT; break;
    case 15: i = FB_15PT; break;
    case 16: i = FB_16PT; break;
    case 18: i = FB_18PT; break;
    case 20: i = FB_20PT; break;
    case 22: i = FB_22PT; break;
    case 24: i = FB_24PT; break;
    case 40: i = FB_40PT; break;
    case 48: i = FB_48PT; break;
    default: i = FB_11PT; break;   /* fallback */
    }
    return &g_bands[i];
}

/* Returns margins.top argument to fbink_print_ot() that visually centres
 * text of `pt` points inside a row of height `row_h` starting at `row_y`. */
int center_y(int row_y, int row_h, int pt) {
    const FontBand *b = band(pt);
    return row_y + (row_h - (b->ink_top_off + b->ink_bottom_off)) / 2;
}

/* Render `s` at `pt` points and pixel-scan for first/last inked rows, as
 * offsets from the em-box top. Returns em-box height, or 0 if nothing rendered. */
static int scan_ink_band(int pt, const char *s, int scratch_y,
                         int *top, int *bottom) {
    /* Fill scratch strip with white so the background is known. */
    fb_fill_rect(&g_disp.fb, (Rect){0, scratch_y, g_disp.fb.w, 200}, UI_BG);

    FBInkConfig fc;
    memset(&fc, 0, sizeof(fc));
    fc.no_refresh = true;
    /* is_bgless=false: paints the full em-box against the pre-filled white. */

    FBInkOTConfig ot;
    memset(&ot, 0, sizeof(ot));
    ot.size_pt       = (unsigned short)pt;
    ot.margins.top   = (short)scratch_y;
    ot.margins.left  = 0;
    ot.margins.right = 0;
    fbink_print_ot(g_disp.fbink_fd, s, &ot, &fc, NULL);

    FBInkRect r = fbink_get_last_rect(false);
    if (r.width == 0 || r.height == 0) { *top = 0; *bottom = 0; return 0; }

    int top_off    = (int)r.height;  /* sentinel: no ink yet */
    int bottom_off = 0;
    int bytes_pp   = (g_disp.fb.bpp >= 16) ? (g_disp.fb.bpp / 8) : 1;

    for (int row = 0; row < (int)r.height; row++) {
        int abs_y = r.top + row;
        if (abs_y < 0 || abs_y >= g_disp.fb.h) continue;
        for (int col = 0; col < (int)r.width; col++) {
            int abs_x = r.left + col;
            if (abs_x < 0 || abs_x >= g_disp.fb.w) continue;

            int off = abs_y * g_disp.fb.stride + abs_x * bytes_pp;
            uint8_t gray;
            if (g_disp.fb.is_rgb565) {
                uint16_t px;
                memcpy(&px, g_disp.fb.mem + off, 2);
                /* White = 0xFFFF in RGB565; any darker pixel is ink. */
                gray = (px == 0xFFFFu) ? 0xFF : 0x00;
            } else if (g_disp.fb.bpp == 32) {
                uint32_t px;
                memcpy(&px, g_disp.fb.mem + off, 4);
                gray = (uint8_t)((px >> 16) & 0xFF);
            } else {
                gray = g_disp.fb.mem[off];
            }

            if (gray < 0xE0) {
                if (row < top_off)    top_off    = row;
                if (row + 1 > bottom_off) bottom_off = row + 1;
            }
        }
    }

    *top    = (top_off < (int)r.height) ? top_off    : 0;
    *bottom = (bottom_off > 0)          ? bottom_off : (int)r.height;
    return (int)r.height;
}

void calibrate_fonts(UIState *ui) {
    /* Probe with "Åjg0" (accent->descender band, for center_y) and "H0"
     * (cap-height->baseline, for inter-line gaps), then erase the scratch area. */
    int scratch_y = ui->screen_h - 200;
    if (scratch_y < 0) scratch_y = 0;

    for (int i = 0; i < FB_COUNT; i++) {
        int it, ib, ct, cb;
        int em = scan_ink_band(k_band_pts[i], "Åjg0", scratch_y, &it, &ib);
        scan_ink_band(k_band_pts[i], "H0", scratch_y, &ct, &cb);

        g_bands[i].em_h           = em;
        g_bands[i].ink_top_off    = it;
        g_bands[i].ink_bottom_off = ib;
        g_bands[i].cap_off        = ct;
        g_bands[i].base_off       = cb;

        if (g_debug)
            fprintf(stderr, "Font band %2dpt: em_h=%d ink=[%d,%d) cap=[%d,%d)\n",
                    k_band_pts[i], em, it, ib, ct, cb);
    }

    /* Erase scratch area (final entry may have left pixels). */
    fb_fill_rect(&g_disp.fb, (Rect){0, scratch_y, g_disp.fb.w, 200}, UI_BG);
}

/* ---- Persisted font-band calibration ------------------------------ */
/* Caches calibrate_fonts() results to disk (startup-latency optimisation
 * only). Keyed on font path + mtime/size + fb bpp; any mismatch or read
 * error falls back to a fresh calibration. */
/* font_bands.cache lives beside the binary (sysutil_exe_dir()); built lazily. */
static const char *font_bands_cache_path(void) {
    static char p[PATH_MAX];
    if (!p[0]) sysutil_path(p, sizeof p, "font_bands.cache");
    return p;
}
#define FONT_BANDS_MAGIC 0x46424E44u  /* "FBND" */
#define FONT_BANDS_VER   1u

typedef struct {
    uint32_t  magic;       /* FONT_BANDS_MAGIC */
    uint32_t  version;     /* FONT_BANDS_VER */
    uint32_t  count;       /* FB_COUNT at write time */
    uint32_t  bpp;         /* g_disp.fb.bpp */
    long long font_mtime;  /* font file st_mtime */
    long long font_size;   /* font file st_size */
    uint32_t  path_len;    /* bytes of font path following the header */
} BandCacheHdr;

/* Returns 1 and fills g_bands if a valid cache matches the current font. */
int load_band_cache(const char *font_path) {
    if (!font_path || !font_path[0]) return 0;
    struct stat st;
    if (stat(font_path, &st) != 0) return 0;
    FILE *f = fopen(font_bands_cache_path(), "rb");
    if (!f) return 0;
    int          ok = 0;
    BandCacheHdr h;
    char         pbuf[512];
    FontBand     tmp[FB_COUNT];
    if (fread(&h, sizeof h, 1, f) == 1 &&
        h.magic      == FONT_BANDS_MAGIC &&
        h.version    == FONT_BANDS_VER &&
        h.count      == (uint32_t)FB_COUNT &&
        h.bpp        == (uint32_t)g_disp.fb.bpp &&
        h.font_mtime == (long long)st.st_mtime &&
        h.font_size  == (long long)st.st_size &&
        h.path_len   == (uint32_t)strlen(font_path) &&
        h.path_len   <  sizeof pbuf &&
        fread(pbuf, 1, h.path_len, f) == h.path_len &&
        memcmp(pbuf, font_path, h.path_len) == 0 &&
        fread(tmp, sizeof(FontBand), FB_COUNT, f) == (size_t)FB_COUNT) {
        memcpy(g_bands, tmp, sizeof g_bands);
        ok = 1;
    }
    fclose(f);
    return ok;
}

void save_band_cache(const char *font_path) {
    if (!font_path || !font_path[0]) return;
    struct stat st;
    if (stat(font_path, &st) != 0) return;
    FILE *f = fopen(font_bands_cache_path(), "wb");
    if (!f) return;
    BandCacheHdr h;
    memset(&h, 0, sizeof h);
    h.magic      = FONT_BANDS_MAGIC;
    h.version    = FONT_BANDS_VER;
    h.count      = (uint32_t)FB_COUNT;
    h.bpp        = (uint32_t)g_disp.fb.bpp;
    h.font_mtime = (long long)st.st_mtime;
    h.font_size  = (long long)st.st_size;
    h.path_len   = (uint32_t)strlen(font_path);
    int ok = (fwrite(&h, sizeof h, 1, f) == 1) &&
             (fwrite(font_path, 1, h.path_len, f) == h.path_len) &&
             (fwrite(g_bands, sizeof(FontBand), FB_COUNT, f) == (size_t)FB_COUNT);
    if (fclose(f) != 0) ok = 0;
    if (!ok) remove(font_bands_cache_path());   /* drop a partial/corrupt cache */
}
