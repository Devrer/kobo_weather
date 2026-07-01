#include "config.h"
#include "fb.h"
#include "i18n.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    if (*s == '\0') return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
    return s;
}

/* Parse one "key = value" line in place. Returns 1 and sets *key / *val on a
 * valid assignment; 0 for blank lines, comments, or lines without '='. */
static int parse_kv_line(char *line, char **key, char **val) {
    char *p = trim(line);
    if (*p == '#' || *p == '\0') return 0;
    char *eq = strchr(p, '=');
    if (!eq) return 0;
    *eq  = '\0';
    *key = trim(p);
    *val = trim(eq + 1);
    return 1;
}

static void apply_defaults(Config *cfg) {
    cfg->loc.lat       = 0.0;
    cfg->loc.lon       = 0.0;
    cfg->loc.place[0]  = '\0';
    cfg->loc.precision = GEO_PRECISION_NONE;
    cfg->font_path[0]  = '\0';
    cfg->api_url[0]    = '\0';
    cfg->auto_location = 1;  /* detect from IP; set to 0 only when LAT/LON are explicit */
    cfg->provider      = PROVIDER_OPEN_METEO;  /* overridden by --smhi in app_main */
}

void config_build_url(Config *cfg) {
    if (cfg->api_url[0] != '\0') return;
    /* SMHI snow1g v1 — pmp3g v2 decommissioned 2026-03-31. */
    snprintf(cfg->api_url, sizeof(cfg->api_url),
        "https://opendata-download-metfcst.smhi.se/api/category/snow1g/version/1"
        "/geotype/point/lon/%.4f/lat/%.4f/data.json",
        cfg->loc.lon, cfg->loc.lat);
}

int config_load(const char *path, Config *cfg) {
    apply_defaults(cfg);

    FILE *f = fopen(path, "r");
    if (!f) {
        /* No config file is OK — defaults are usable. */
        config_build_url(cfg);
        return 0;
    }

    char line[512];
    char *key, *val;
    /* A lone LAT or LON must not disable auto-location; only a complete pair
     * or an explicit API_URL/AUTO_LOCATION=0 does. Decided after the loop. */
    int seen_lat = 0, seen_lon = 0, seen_api_url = 0, seen_auto = 0;
    while (fgets(line, sizeof(line), f)) {
        if (!parse_kv_line(line, &key, &val)) continue;

        if (strcmp(key, "PLACE") == 0) {
            snprintf(cfg->loc.place, sizeof(cfg->loc.place), "%s", val);
        } else if (strcmp(key, "LAT") == 0) {
            char *ep; double d = strtod(val, &ep);
            if (ep != val && d >= -90.0 && d <= 90.0) {
                cfg->loc.lat = d;
                seen_lat = 1;
            }
        } else if (strcmp(key, "LON") == 0) {
            char *ep; double d = strtod(val, &ep);
            if (ep != val && d >= -180.0 && d <= 180.0) {
                cfg->loc.lon = d;
                seen_lon = 1;
            }
        } else if (strcmp(key, "FONT_PATH") == 0) {
            snprintf(cfg->font_path, sizeof(cfg->font_path), "%s", val);
        } else if (strcmp(key, "API_URL") == 0) {
            snprintf(cfg->api_url, sizeof(cfg->api_url), "%s", val);
            seen_api_url = 1;
        } else if (strcmp(key, "AUTO_LOCATION") == 0) {
            cfg->auto_location = (strcmp(val, "1") == 0 || strcmp(val, "yes") == 0) ? 1 : 0;
            seen_auto = 1;
        }
    }

    fclose(f);

    /* Explicit AUTO_LOCATION= overrides inference; otherwise a full coord pair
     * or an explicit API_URL means the location is pinned manually. */
    if (!seen_auto && ((seen_lat && seen_lon) || seen_api_url))
        cfg->auto_location = 0;

    config_build_url(cfg);
    return 0;
}

const int g_sleepupd_steps[6] = {15, 30, 60, 180, 360, 720};

static int snap_to_sleepupd(int v) {
    int best = g_sleepupd_steps[0];
    int dist = abs(v - best);
    for (int i = 1; i < 6; i++) {
        int d = abs(v - g_sleepupd_steps[i]);
        if (d < dist) { dist = d; best = g_sleepupd_steps[i]; }
    }
    return best;
}

void settings_load(const char *path, UISettings *s) {
    s->snap          = 1;   /* default: Snap (A2) */
    s->lang          = 0;
    s->sleep_min     = TIMER_SLEEP_DEFAULT_MIN;
    s->active_min    = TIMER_ACTIVE_DEFAULT_MIN;
    s->sleep_upd_min = TIMER_SLEEPUPD_DEFAULT_MIN;
    s->bold_cur_temp  = 0;
    s->bold_max_temp  = 0;
    s->small_min_temp = 1;
    s->units          = 0;
    s->night_pause    = 1;  /* default: pause fetches 00:00–06:00 */
    s->full_style     = RSTYLE_REAGL;
    s->partial_style  = RSTYLE_REAGL;
    s->init_wash_settle_ms = WASH_SETTLE_DEFAULT_MS;
    s->sleep_entry_wash    = SLEEP_ENTRY_NONE;  /* default: kiosk entry follows partial_style (06:00 wash still INITs) */
    s->gc16_flash          = 0;  /* default: no forced flash on GC16 refreshes */
    s->sleep_gc16_flash    = 0;  /* default: no forced flash on kiosk tick-wakes */
    s->sleep_partial       = 1;  /* default: tick-wake does a partial update */
    s->wake_flash_min      = WAKE_FLASH_DEFAULT_MIN;
    s->wake_wash_min       = WAKE_WASH_DEFAULT_MIN;

    /* If the new SLEEP_ENTRY_WASH key is absent, migrate from the legacy
     * INIT_WASH_ENABLED / SLEEP_GC16_FLASH booleans (see after the loop). */
    int legacy_init_wash   = 0;
    int seen_entry_wash    = 0;

    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256], *key, *val;
    while (fgets(line, sizeof(line), f)) {
        if (!parse_kv_line(line, &key, &val)) continue;
        if (strcmp(key, "SCROLL_MODE") == 0) {
            if (strcmp(val, "snap_reagl") == 0) s->snap = 2;
            else if (strcmp(val, "snap") == 0)  s->snap = 1;
            else                                s->snap = 0;
        } else if (strcmp(key, "LANGUAGE") == 0) {
            char *ep; long v = strtol(val, &ep, 10);
            if (ep != val && v >= 0 && v < LANG_COUNT) s->lang = (int)v;
        } else if (strcmp(key, "UNITS") == 0) {
            s->units = (strcmp(val, "imperial") == 0) ? 1 : 0;
        } else if (strcmp(key, "SLEEP_MIN") == 0) {
            char *ep; long v = strtol(val, &ep, 10);
            if (ep != val) {
                if (v < 5) v = 5;
                if (v > 60) v = 60;
                s->sleep_min = (int)((v / 5) * 5);
            }
        } else if (strcmp(key, "ACTIVE_MIN") == 0) {
            char *ep; long v = strtol(val, &ep, 10);
            if (ep != val) {
                if (v < 5) v = 5;
                if (v > 60) v = 60;
                s->active_min = (int)((v / 5) * 5);
            }
        } else if (strcmp(key, "SLEEP_UPDATE_MIN") == 0) {
            char *ep; long v = strtol(val, &ep, 10);
            if (ep != val) s->sleep_upd_min = snap_to_sleepupd((int)v);
        } else if (strcmp(key, "TEMP_BOLD_CUR") == 0) {
            s->bold_cur_temp = (strcmp(val, "1") == 0) ? 1 : 0;
        } else if (strcmp(key, "TEMP_BOLD_MAX") == 0) {
            s->bold_max_temp = (strcmp(val, "1") == 0) ? 1 : 0;
        } else if (strcmp(key, "TEMP_SMALL_MIN") == 0) {
            s->small_min_temp = (strcmp(val, "1") == 0) ? 1 : 0;
        } else if (strcmp(key, "NIGHT_PAUSE") == 0) {
            s->night_pause = (strcmp(val, "0") == 0) ? 0 : 1;
        } else if (strcmp(key, "FULL_STYLE") == 0) {
            char *ep; long v = strtol(val, &ep, 10);
            if (ep != val) s->full_style = (v == 0) ? RSTYLE_REAGL : RSTYLE_GC16;
        } else if (strcmp(key, "PARTIAL_STYLE") == 0) {
            char *ep; long v = strtol(val, &ep, 10);
            if (ep != val) s->partial_style = (v == 0) ? RSTYLE_REAGL : RSTYLE_GC16;
        } else if (strcmp(key, "WASH_SETTLE_MS") == 0) {
            char *ep; long v = strtol(val, &ep, 10);
            if (ep != val) {
                if (v < WASH_SETTLE_MIN_MS) v = WASH_SETTLE_MIN_MS;
                if (v > WASH_SETTLE_MAX_MS) v = WASH_SETTLE_MAX_MS;
                s->init_wash_settle_ms = (int)v;
            }
        } else if (strcmp(key, "SLEEP_ENTRY_WASH") == 0) {
            char *ep; long v = strtol(val, &ep, 10);
            if (ep != val) {
                if (v < SLEEP_ENTRY_NONE) v = SLEEP_ENTRY_NONE;
                if (v > SLEEP_ENTRY_WASH) v = SLEEP_ENTRY_WASH;
                s->sleep_entry_wash = (int)v;
                seen_entry_wash = 1;
            }
        } else if (strcmp(key, "INIT_WASH_ENABLED") == 0) {
            legacy_init_wash = (strcmp(val, "0") == 0) ? 0 : 1;   /* legacy; see migration */
        } else if (strcmp(key, "GC16_FLASH") == 0) {
            s->gc16_flash = (strcmp(val, "1") == 0) ? 1 : 0;
        } else if (strcmp(key, "SLEEP_GC16_FLASH") == 0) {
            s->sleep_gc16_flash = (strcmp(val, "1") == 0) ? 1 : 0;
        } else if (strcmp(key, "SLEEP_PARTIAL") == 0) {
            s->sleep_partial = (strcmp(val, "1") == 0) ? 1 : 0;
        } else if (strcmp(key, "WAKE_FLASH_MIN") == 0) {
            char *ep; long v = strtol(val, &ep, 10);
            if (ep != val) {
                if (v < WAKE_FLASH_MIN_MIN) v = WAKE_FLASH_MIN_MIN;
                if (v > WAKE_FLASH_MAX_MIN) v = WAKE_FLASH_MAX_MIN;
                s->wake_flash_min = (int)((v / WAKE_FLASH_STEP_MIN) * WAKE_FLASH_STEP_MIN);
            }
        } else if (strcmp(key, "WAKE_WASH_MIN") == 0) {
            char *ep; long v = strtol(val, &ep, 10);
            if (ep != val) {
                if (v < WAKE_WASH_MIN_MIN) v = WAKE_WASH_MIN_MIN;
                if (v > WAKE_WASH_MAX_MIN) v = WAKE_WASH_MAX_MIN;
                s->wake_wash_min = (int)((v / WAKE_WASH_STEP_MIN) * WAKE_WASH_STEP_MIN);
            }
        } else if (strcmp(key, "REFRESH_WFM") == 0) {
            /* Migration from the pre-RefreshStyle config (single GC16/REAGL
             * choice). settings_save() rewrites the whole file with FULL_STYLE/
             * PARTIAL_STYLE, so this key never appears alongside them. */
            RefreshStyle v = (strcmp(val, "reagl") == 0) ? RSTYLE_REAGL : RSTYLE_GC16;
            s->full_style    = v;
            s->partial_style = v;
        }
    }
    fclose(f);

    /* No new key → derive from the old booleans (INIT wash won over flash). */
    if (!seen_entry_wash)
        s->sleep_entry_wash = legacy_init_wash   ? SLEEP_ENTRY_WASH  :
                              s->sleep_gc16_flash ? SLEEP_ENTRY_FLASH :
                                                    SLEEP_ENTRY_NONE;
}

void settings_save(const char *path, const UISettings *s) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    const char *mode = (s->snap == 2) ? "snap_reagl" : (s->snap == 1) ? "snap" : "float";
    fprintf(f, "SCROLL_MODE=%s\n", mode);
    fprintf(f, "LANGUAGE=%d\n", s->lang);
    fprintf(f, "UNITS=%s\n", (s->units == 1) ? "imperial" : "metric");
    fprintf(f, "SLEEP_MIN=%d\n", s->sleep_min);
    fprintf(f, "ACTIVE_MIN=%d\n", s->active_min);
    fprintf(f, "SLEEP_UPDATE_MIN=%d\n", s->sleep_upd_min);
    fprintf(f, "TEMP_BOLD_CUR=%d\n", s->bold_cur_temp);
    fprintf(f, "TEMP_BOLD_MAX=%d\n", s->bold_max_temp);
    fprintf(f, "TEMP_SMALL_MIN=%d\n", s->small_min_temp);
    fprintf(f, "NIGHT_PAUSE=%d\n", s->night_pause);
    fprintf(f, "FULL_STYLE=%d\n", s->full_style);
    fprintf(f, "PARTIAL_STYLE=%d\n", s->partial_style);
    fprintf(f, "WASH_SETTLE_MS=%d\n", s->init_wash_settle_ms);
    fprintf(f, "SLEEP_ENTRY_WASH=%d\n", s->sleep_entry_wash);
    fprintf(f, "GC16_FLASH=%d\n", s->gc16_flash);
    fprintf(f, "SLEEP_GC16_FLASH=%d\n", s->sleep_gc16_flash);
    fprintf(f, "SLEEP_PARTIAL=%d\n", s->sleep_partial);
    fprintf(f, "WAKE_FLASH_MIN=%d\n", s->wake_flash_min);
    fprintf(f, "WAKE_WASH_MIN=%d\n", s->wake_wash_min);
    fclose(f);
}
