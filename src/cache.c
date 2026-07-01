#include "cache.h"
#include <stdio.h>

#define CACHE_MAGIC   0x57544843U  /* "WTHC" little-endian */
#define CACHE_VERSION 2

typedef struct {
    unsigned int magic;
    unsigned int version;
    int          n_hours;
    int          place_len;     /* bytes of place string written after header */
    long long    fetched_at;
    int          units;         /* UnitSystem the stored hours are expressed in */
} CacheHeader;

int cache_load(const char *path, WeatherState *state) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    CacheHeader hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1 ||
        hdr.magic   != CACHE_MAGIC          ||
        hdr.version != CACHE_VERSION) {
        fclose(f);
        return -1;
    }

    if (hdr.n_hours < 0 || hdr.n_hours > WEATHER_MAX_HOURS ||
        hdr.place_len < 0 || hdr.place_len >= (int)sizeof(state->place)) {
        fclose(f);
        return -1;
    }

    state->n_hours    = hdr.n_hours;
    state->fetched_at = (time_t)hdr.fetched_at;
    state->units      = (UnitSystem)hdr.units;

    if (hdr.place_len > 0) {
        if (fread(state->place, 1, (size_t)hdr.place_len, f) != (size_t)hdr.place_len) {
            fclose(f);
            return -1;
        }
    }
    state->place[hdr.place_len] = '\0';

    /* Raw WeatherHour blobs, no per-field revalidation — safe only because every
     * consumer range-checks (has_* flags, symbol/wind bounds). */
    if ((size_t)fread(state->hours, sizeof(WeatherHour), (size_t)hdr.n_hours, f)
            != (size_t)hdr.n_hours) {
        fclose(f);
        return -1;
    }

    fclose(f);
    return 0;
}

void cache_save(const char *path, const WeatherState *state) {
    char tmp_path[300];
    if (snprintf(tmp_path, sizeof tmp_path, "%s.tmp", path) >= (int)sizeof tmp_path)
        return;

    FILE *f = fopen(tmp_path, "wb");
    if (!f) return;

    int place_len = 0;
    while (place_len < (int)sizeof(state->place) && state->place[place_len] != '\0')
        place_len++;

    CacheHeader hdr = {
        .magic      = CACHE_MAGIC,
        .version    = CACHE_VERSION,
        .n_hours    = state->n_hours,
        .place_len  = place_len,
        .fetched_at = (long long)state->fetched_at,
        .units      = (int)state->units,
    };
    int ok = (fwrite(&hdr, sizeof(hdr), 1, f) == 1);
    if (ok && place_len > 0)
        ok = (fwrite(state->place, 1, (size_t)place_len, f) == (size_t)place_len);
    if (ok && state->n_hours > 0)
        ok = (fwrite(state->hours, sizeof(WeatherHour), (size_t)state->n_hours, f)
              == (size_t)state->n_hours);

    if (fclose(f) != 0) ok = 0;

    /* Partial write: drop the temp file, leave the previous cache untouched. */
    if (!ok) {
        remove(tmp_path);
        return;
    }
    /* rename() is atomic, so a crash here can't leave a half-written cache. */
    if (rename(tmp_path, path) != 0)
        remove(tmp_path);
}
