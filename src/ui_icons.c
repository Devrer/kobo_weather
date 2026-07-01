#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../stb_image.h"
#pragma GCC diagnostic pop
#include "../weather_icons_clara.h"
#include "../weather_icons_libra.h"
#include "ui_internal.h"
#include <time.h>

/* Each icon pack provides parallel [WICON_COUNT] data/len tables; the active
 * pack is set once at startup in ui_init() based on screen width. */
static const unsigned char * const g_icons_small_clara[WICON_COUNT] = {
    small_CLEAR_NIGHT, small_CLOUDY, small_FOG,
    small_HEAVY_RAIN, small_LIGHT_RAIN, small_LIGHT_SNOW,
    small_PARTLY_CLOUDY, small_RAIN, small_SLEET,
    small_SNOW, small_SUNNY, small_THUNDERSTORM,
};
static const unsigned int g_icons_small_len_clara[WICON_COUNT] = {
    small_CLEAR_NIGHT_len, small_CLOUDY_len, small_FOG_len,
    small_HEAVY_RAIN_len, small_LIGHT_RAIN_len, small_LIGHT_SNOW_len,
    small_PARTLY_CLOUDY_len, small_RAIN_len, small_SLEET_len,
    small_SNOW_len, small_SUNNY_len, small_THUNDERSTORM_len,
};
static const unsigned char * const g_icons_medium_clara[WICON_COUNT] = {
    medium_CLEAR_NIGHT, medium_CLOUDY, medium_FOG,
    medium_HEAVY_RAIN, medium_LIGHT_RAIN, medium_LIGHT_SNOW,
    medium_PARTLY_CLOUDY, medium_RAIN, medium_SLEET,
    medium_SNOW, medium_SUNNY, medium_THUNDERSTORM,
};
static const unsigned int g_icons_medium_len_clara[WICON_COUNT] = {
    medium_CLEAR_NIGHT_len, medium_CLOUDY_len, medium_FOG_len,
    medium_HEAVY_RAIN_len, medium_LIGHT_RAIN_len, medium_LIGHT_SNOW_len,
    medium_PARTLY_CLOUDY_len, medium_RAIN_len, medium_SLEET_len,
    medium_SNOW_len, medium_SUNNY_len, medium_THUNDERSTORM_len,
};
static const unsigned char * const g_icons_large_clara[WICON_COUNT] = {
    large_CLEAR_NIGHT, large_CLOUDY, large_FOG,
    large_HEAVY_RAIN, large_LIGHT_RAIN, large_LIGHT_SNOW,
    large_PARTLY_CLOUDY, large_RAIN, large_SLEET,
    large_SNOW, large_SUNNY, large_THUNDERSTORM,
};
static const unsigned int g_icons_large_len_clara[WICON_COUNT] = {
    large_CLEAR_NIGHT_len, large_CLOUDY_len, large_FOG_len,
    large_HEAVY_RAIN_len, large_LIGHT_RAIN_len, large_LIGHT_SNOW_len,
    large_PARTLY_CLOUDY_len, large_RAIN_len, large_SLEET_len,
    large_SNOW_len, large_SUNNY_len, large_THUNDERSTORM_len,
};

static const unsigned char * const g_icons_small_libra[WICON_COUNT] = {
    libra_small_CLEAR_NIGHT, libra_small_CLOUDY, libra_small_FOG,
    libra_small_HEAVY_RAIN, libra_small_LIGHT_RAIN, libra_small_LIGHT_SNOW,
    libra_small_PARTLY_CLOUDY, libra_small_RAIN, libra_small_SLEET,
    libra_small_SNOW, libra_small_SUNNY, libra_small_THUNDERSTORM,
};
static const unsigned int g_icons_small_len_libra[WICON_COUNT] = {
    libra_small_CLEAR_NIGHT_len, libra_small_CLOUDY_len, libra_small_FOG_len,
    libra_small_HEAVY_RAIN_len, libra_small_LIGHT_RAIN_len, libra_small_LIGHT_SNOW_len,
    libra_small_PARTLY_CLOUDY_len, libra_small_RAIN_len, libra_small_SLEET_len,
    libra_small_SNOW_len, libra_small_SUNNY_len, libra_small_THUNDERSTORM_len,
};
static const unsigned char * const g_icons_medium_libra[WICON_COUNT] = {
    libra_medium_CLEAR_NIGHT, libra_medium_CLOUDY, libra_medium_FOG,
    libra_medium_HEAVY_RAIN, libra_medium_LIGHT_RAIN, libra_medium_LIGHT_SNOW,
    libra_medium_PARTLY_CLOUDY, libra_medium_RAIN, libra_medium_SLEET,
    libra_medium_SNOW, libra_medium_SUNNY, libra_medium_THUNDERSTORM,
};
static const unsigned int g_icons_medium_len_libra[WICON_COUNT] = {
    libra_medium_CLEAR_NIGHT_len, libra_medium_CLOUDY_len, libra_medium_FOG_len,
    libra_medium_HEAVY_RAIN_len, libra_medium_LIGHT_RAIN_len, libra_medium_LIGHT_SNOW_len,
    libra_medium_PARTLY_CLOUDY_len, libra_medium_RAIN_len, libra_medium_SLEET_len,
    libra_medium_SNOW_len, libra_medium_SUNNY_len, libra_medium_THUNDERSTORM_len,
};
static const unsigned char * const g_icons_large_libra[WICON_COUNT] = {
    libra_large_CLEAR_NIGHT, libra_large_CLOUDY, libra_large_FOG,
    libra_large_HEAVY_RAIN, libra_large_LIGHT_RAIN, libra_large_LIGHT_SNOW,
    libra_large_PARTLY_CLOUDY, libra_large_RAIN, libra_large_SLEET,
    libra_large_SNOW, libra_large_SUNNY, libra_large_THUNDERSTORM,
};
static const unsigned int g_icons_large_len_libra[WICON_COUNT] = {
    libra_large_CLEAR_NIGHT_len, libra_large_CLOUDY_len, libra_large_FOG_len,
    libra_large_HEAVY_RAIN_len, libra_large_LIGHT_RAIN_len, libra_large_LIGHT_SNOW_len,
    libra_large_PARTLY_CLOUDY_len, libra_large_RAIN_len, libra_large_SLEET_len,
    libra_large_SNOW_len, libra_large_SUNNY_len, libra_large_THUNDERSTORM_len,
};

static WeatherIconType wsymb_to_icon(int wsymb, time_t t) {
    int is_night = 0;
    if (t > 0) {
        struct tm lt;
        localtime_r(&t, &lt);
        is_night = (lt.tm_hour >= 20 || lt.tm_hour < 6);
    }
    switch (wsymb) {
    case 1: case 2:                                   return is_night ? WICON_CLEAR_NIGHT : WICON_SUNNY;
    case 3: case 4:                                   return WICON_PARTLY_CLOUDY;
    case 5: case 6:                                   return WICON_CLOUDY;
    case 7:                                           return WICON_FOG;
    case 8: case 18:                                  return WICON_LIGHT_RAIN;
    case 9: case 19:                                  return WICON_RAIN;
    case 10: case 20:                                 return WICON_HEAVY_RAIN;
    case 11: case 21:                                 return WICON_THUNDERSTORM;
    case 12: case 13: case 14: case 22: case 23: case 24: return WICON_SLEET;
    case 15: case 25:                                 return WICON_LIGHT_SNOW;
    case 16: case 17: case 26: case 27:               return WICON_SNOW;
    default:                                          return WICON_CLOUDY;
    }
}

/* Decoded-icon cache, keyed by (size, type); decoded once and kept until
 * ui_cleanup() since PNG decode is expensive on ARM and rows redraw often. */
static IconCacheEntry g_icon_cache[3][WICON_COUNT];   /* [IconSize][WeatherIconType] */

static const IconCacheEntry *icon_decode_cached(IconSize size, WeatherIconType wt) {
    IconCacheEntry *e = &g_icon_cache[size][wt];
    if (e->pixels) return e;

    bool libra = (g_disp.icon_pack == ICONPACK_LIBRA);
    const unsigned char *src;
    unsigned int len;
    switch (size) {
    case ICON_MEDIUM:
        src = libra ? g_icons_medium_libra[wt] : g_icons_medium_clara[wt];
        len = libra ? g_icons_medium_len_libra[wt] : g_icons_medium_len_clara[wt];
        break;
    case ICON_LARGE:
        src = libra ? g_icons_large_libra[wt] : g_icons_large_clara[wt];
        len = libra ? g_icons_large_len_libra[wt] : g_icons_large_len_clara[wt];
        break;
    default:
        src = libra ? g_icons_small_libra[wt] : g_icons_small_clara[wt];
        len = libra ? g_icons_small_len_libra[wt] : g_icons_small_len_clara[wt];
        break;
    }
    if (!src || len == 0) return NULL;

    int channels;
    e->pixels = stbi_load_from_memory(src, (int)len, &e->w, &e->h, &channels, 1);
    if (!e->pixels) return NULL;
    return e;
}

void icon_cache_free(void) {
    for (int s = 0; s < 3; s++)
        for (int t = 0; t < WICON_COUNT; t++) {
            if (g_icon_cache[s][t].pixels) {
                stbi_image_free(g_icon_cache[s][t].pixels);
                g_icon_cache[s][t].pixels = NULL;
            }
        }
}

/* draw_icon: render wsymb icon with top-left at (x, y).
 * If row_h > 0 the image is vertically centred within row_h. */
int draw_icon(int wsymb, time_t valid_time, int x, int y, int row_h, IconSize size) {
    WeatherIconType wt = wsymb_to_icon(wsymb, valid_time);
    const IconCacheEntry *e = icon_decode_cached(size, wt);
    if (!e) return -2;

    if (row_h > 0)
        y += (row_h - e->h) / 2;

    FBInkConfig img_cfg = {0};
    img_cfg.ignore_alpha = true;
    img_cfg.no_refresh   = true;
    return fbink_print_raw_data(g_disp.fbink_fd, e->pixels, e->w, e->h,
                                (size_t)e->w * e->h, (short)x, (short)y, &img_cfg);
}
