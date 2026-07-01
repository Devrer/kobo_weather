#include "weather.h"
#include "../cJSON.h"
#include "../http_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include <limits.h>

/* ---- Last-error reporting ------------------------------------------- */

static char g_weather_last_err[256] = "";

const char *weather_last_error(void) {
    return g_weather_last_err[0] ? g_weather_last_err : "no error";
}

static void set_err(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_weather_last_err, sizeof g_weather_last_err, fmt, ap);
    va_end(ap);
}

/* ---- Time helpers --------------------------------------------------- */

/* Days since 1970-01-01 for proleptic-Gregorian y/m/d (Hinnant's days_from_civil). */
static long days_from_civil(long y, long m, long d) {
    y -= (m <= 2);
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;                                   /* [0, 399]    */
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;  /* [0, 365]    */
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;           /* [0, 146096] */
    return era * 146097 + doe - 719468;
}

/* Parse ISO 8601 timestamp ("2026-05-19T12:00:00Z") to time_t (UTC), 0 on failure.
 * Uses pure integer math instead of mktime/timegm: this runs on the fetch thread
 * and those touch libc's global timezone state, racing with the main thread. */
static time_t parse_iso8601_utc(const char *s) {
    if (!s) return 0;
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    int n = sscanf(s, "%d-%d-%dT%d:%d:%d",
                   &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                   &tm.tm_hour, &tm.tm_min, &tm.tm_sec);
    if (n < 5) return 0;  /* Open-Meteo omits seconds ("HH:MM") — allow 5 fields */
    /* tm_sec stays 0 (memset) when seconds are absent. */
    tm.tm_year -= 1900;
    tm.tm_mon  -= 1;
    long days = days_from_civil(tm.tm_year + 1900L, tm.tm_mon + 1L, tm.tm_mday);
    return (time_t)(days * 86400L + tm.tm_hour * 3600L
                    + tm.tm_min * 60L + tm.tm_sec);
}

/* ---- Parameter extraction ------------------------------------------- */

/* SMHI pmp3g format: timeSeries[i].parameters[].{name, values[0]} */
static int read_param(cJSON *params, const char *name, double *out) {
    int n = cJSON_GetArraySize(params);
    for (int i = 0; i < n; i++) {
        cJSON *p = cJSON_GetArrayItem(params, i);
        cJSON *nm = cJSON_GetObjectItem(p, "name");
        if (!cJSON_IsString(nm)) continue;
        if (strcmp(nm->valuestring, name) != 0) continue;
        cJSON *vals = cJSON_GetObjectItem(p, "values");
        cJSON *v0 = cJSON_GetArrayItem(vals, 0);
        if (cJSON_IsNumber(v0)) { *out = v0->valuedouble; return 1; }
    }
    return 0;
}

/* Legacy snow1g format: timeSeries[i].data.{air_temperature, wind_speed, ...} */
static int read_data_field(cJSON *data, const char *name, double *out) {
    cJSON *v = cJSON_GetObjectItem(data, name);
    if (cJSON_IsNumber(v)) { *out = v->valuedouble; return 1; }
    return 0;
}

static void parse_one(cJSON *entry, WeatherHour *h) {
    memset(h, 0, sizeof(*h));

    /* snow1g uses "time"; pmp3g v2 used "validTime". Try both. */
    cJSON *vt = cJSON_GetObjectItem(entry, "time");
    if (!cJSON_IsString(vt)) vt = cJSON_GetObjectItem(entry, "validTime");
    if (cJSON_IsString(vt)) h->valid_time = parse_iso8601_utc(vt->valuestring);

    cJSON *params = cJSON_GetObjectItem(entry, "parameters");
    cJSON *data   = cJSON_GetObjectItem(entry, "data");
    double val;

    if (cJSON_IsArray(params)) {
        if (read_param(params, "t",      &val)) { h->temp   = val;       h->has_temp   = 1; }
        if (read_param(params, "ws",     &val)) { h->wind  = val;       h->has_wind   = 1; }
        if (read_param(params, "gust",   &val)) { h->wind_gust = val;     h->has_gust   = 1; }
        if (read_param(params, "wd",     &val)) { h->wind_dir = (int)val;  h->has_dir    = 1; }
        if (read_param(params, "Wsymb2", &val) && (int)val >= 1 && (int)val <= 27) {
            h->symbol = (int)val; h->has_symbol = 1;
        }
    } else if (cJSON_IsObject(data)) {
        /* Legacy snow1g schema */
        if (read_data_field(data, "air_temperature", &val)) { h->temp   = val;      h->has_temp   = 1; }
        if (read_data_field(data, "wind_speed",      &val)) { h->wind  = val;      h->has_wind   = 1; }
        if (read_data_field(data, "wind_speed_of_gust", &val)) { h->wind_gust = val; h->has_gust = 1; }
        if (read_data_field(data, "wind_from_direction", &val)) { h->wind_dir = (int)val; h->has_dir = 1; }
        if (read_data_field(data, "symbol_code", &val) && (int)val >= 1 && (int)val <= 27) {
            h->symbol = (int)val; h->has_symbol = 1;
        }
    }
}

/* ---- Provider abstraction ------------------------------------------- */

typedef struct {
    void (*build_url)(const Config *cfg, UnitSystem units, char *buf, size_t len);
    int  (*parse)(cJSON *json, WeatherState *state);
    int   metric_only;  /* if set, state->units is forced to UNIT_METRIC */
} WeatherProvider;

static void build_url_smhi(const Config *cfg, UnitSystem units,
                            char *buf, size_t len) {
    (void)units;
    snprintf(buf, len, "%s", cfg->api_url);
}

static void build_url_open_meteo(const Config *cfg, UnitSystem units,
                                  char *buf, size_t len) {
    snprintf(buf, len,
        "https://api.open-meteo.com/v1/forecast"
        "?latitude=%.4f&longitude=%.4f"
        "&hourly=temperature_2m,weathercode,windspeed_10m,winddirection_10m,windgusts_10m"
        "&forecast_days=7&timeformat=iso8601%s",
        cfg->loc.lat, cfg->loc.lon,
        (units == UNIT_IMPERIAL)
            ? "&temperature_unit=fahrenheit&wind_speed_unit=mph"
            : "&wind_speed_unit=ms");
}

static int parse_smhi(cJSON *json, WeatherState *state) {
    cJSON *ts = cJSON_GetObjectItem(json, "timeSeries");
    if (!cJSON_IsArray(ts)) {
        set_err("no timeSeries array in response");
        return -4;
    }
    int n = cJSON_GetArraySize(ts);
    if (n > WEATHER_MAX_HOURS) n = WEATHER_MAX_HOURS;
    for (int i = 0; i < n; i++) {
        cJSON *e = cJSON_GetArrayItem(ts, i);
        if (!e) continue;
        parse_one(e, &state->hours[state->n_hours]);
        if (state->hours[state->n_hours].valid_time != 0)
            state->n_hours++;
    }
    if (state->n_hours == 0) {
        set_err("0 valid hours from %d timeSeries entries", n);
        return -5;
    }
    return 0;
}

/* Map WMO weather code to internal wsymb2-equivalent (1–27). */
static int wmo_to_symbol(int wmo) {
    switch (wmo) {
    case 0:        return 1;  /* clear sky */
    case 1:        return 2;  /* mainly clear */
    case 2:        return 3;  /* partly cloudy */
    case 3:        return 6;  /* overcast */
    case 45: case 48: return 7;   /* fog */
    case 51: case 56: return 18;  /* light drizzle */
    case 53: case 57: return 19;  /* moderate drizzle */
    case 55:       return 20; /* dense drizzle */
    case 61:       return 18; /* light rain */
    case 63:       return 19; /* moderate rain */
    case 65:       return 20; /* heavy rain */
    case 66: case 67: return 19;  /* freezing rain */
    case 71: case 77: return 25;  /* slight snow */
    case 73:       return 26; /* moderate snow */
    case 75:       return 27; /* heavy snow */
    case 80:       return 8;  /* slight rain showers */
    case 81:       return 9;  /* moderate rain showers */
    case 82:       return 10; /* violent rain showers */
    case 85:       return 15; /* slight snow showers */
    case 86:       return 16; /* moderate snow showers */
    case 95:       return 11; /* thunderstorm */
    case 96: case 99: return 11;  /* thunderstorm with hail */
    default:       return 6;  /* unmapped — overcast, matching wsymb_to_icon()'s default */
    }
}

static int parse_open_meteo(cJSON *json, WeatherState *state) {
    cJSON *hourly = cJSON_GetObjectItem(json, "hourly");
    if (!cJSON_IsObject(hourly)) {
        set_err("no 'hourly' object in response");
        return -4;
    }
    cJSON *j_time  = cJSON_GetObjectItem(hourly, "time");
    cJSON *j_temp  = cJSON_GetObjectItem(hourly, "temperature_2m");
    cJSON *j_wcode = cJSON_GetObjectItem(hourly, "weathercode");
    cJSON *j_wspd  = cJSON_GetObjectItem(hourly, "windspeed_10m");
    cJSON *j_wdir  = cJSON_GetObjectItem(hourly, "winddirection_10m");
    cJSON *j_gust  = cJSON_GetObjectItem(hourly, "windgusts_10m");
    if (!cJSON_IsArray(j_time)) {
        set_err("no 'time' array in hourly");
        return -5;
    }
    int n = cJSON_GetArraySize(j_time);
    if (n > WEATHER_MAX_HOURS) n = WEATHER_MAX_HOURS;
    for (int i = 0; i < n; i++) {
        cJSON *t = cJSON_GetArrayItem(j_time, i);
        if (!cJSON_IsString(t)) continue;
        time_t vt = parse_iso8601_utc(t->valuestring);
        if (!vt) continue;
        WeatherHour *h = &state->hours[state->n_hours];
        memset(h, 0, sizeof(*h));
        h->valid_time = vt;
        cJSON *v;
        v = cJSON_GetArrayItem(j_temp,  i);
        if (cJSON_IsNumber(v)) { h->temp = v->valuedouble; h->has_temp = 1; }
        v = cJSON_GetArrayItem(j_wspd,  i);
        if (cJSON_IsNumber(v)) { h->wind = v->valuedouble; h->has_wind = 1; }
        v = cJSON_GetArrayItem(j_gust,  i);
        if (cJSON_IsNumber(v)) { h->wind_gust = v->valuedouble; h->has_gust = 1; }
        v = cJSON_GetArrayItem(j_wdir,  i);
        if (cJSON_IsNumber(v)) { h->wind_dir = (int)v->valuedouble; h->has_dir = 1; }
        v = cJSON_GetArrayItem(j_wcode, i);
        if (cJSON_IsNumber(v)) {
            h->symbol = wmo_to_symbol((int)v->valuedouble);
            h->has_symbol = 1;
        }
        state->n_hours++;
    }
    if (state->n_hours == 0) {
        set_err("0 valid hours from %d time entries", n);
        return -6;
    }
    return 0;
}

static const WeatherProvider PROVIDER_TABLE_SMHI = {
    build_url_smhi, parse_smhi, 1
};

static const WeatherProvider PROVIDER_TABLE_OPEN_METEO = {
    build_url_open_meteo, parse_open_meteo, 0
};

static int weather_fetch_generic(const Config *cfg, WeatherState *state,
                                  UnitSystem units, const WeatherProvider *p) {
    g_weather_last_err[0] = '\0';
    snprintf(state->place, sizeof(state->place), "%s", cfg->loc.place);
    state->n_hours    = 0;
    state->fetched_at = 0;
    state->units      = p->metric_only ? UNIT_METRIC : units;

    char url[CONFIG_MAX_URL];
    p->build_url(cfg, units, url, sizeof(url));

    struct MemoryStruct chunk = { malloc(1), 0 };
    if (!chunk.memory) { set_err("malloc failed"); return -1; }
    chunk.memory[0] = '\0';

    int rc = https_get(url, "KoboWeatherApp/2.0", &chunk);
    if (rc != HTTPC_OK) {
        set_err("https %d: %s", rc, http_client_strerror());
        free(chunk.memory);
        return -2;
    }

    cJSON *json = cJSON_Parse(chunk.memory);
    free(chunk.memory);
    if (!json) {
        const char *ep = cJSON_GetErrorPtr();
        set_err("json parse @ %.40s", ep ? ep : "(unknown)");
        return -3;
    }

    rc = p->parse(json, state);
    cJSON_Delete(json);
    if (rc == 0)
        state->fetched_at = time(NULL);
    return rc;
}

/* ---- Public API ----------------------------------------------------- */

int weather_fetch_provider(const Config *cfg, WeatherState *state,
                           int provider, UnitSystem units) {
    const WeatherProvider *p = (provider == PROVIDER_OPEN_METEO)
        ? &PROVIDER_TABLE_OPEN_METEO : &PROVIDER_TABLE_SMHI;
    return weather_fetch_generic(cfg, state, units, p);
}

/* Local date helpers — local time so "today/tomorrow" matches the wall clock. */
static void local_ymd(time_t t, int *y, int *m, int *d) {
    struct tm lt;
    localtime_r(&t, &lt);
    *y = lt.tm_year + 1900;
    *m = lt.tm_mon + 1;
    *d = lt.tm_mday;
}

/* True if local-time `t` falls on calendar day y/m/d. Callers testing many
 * timestamps against one reference day decompose the reference once. */
static int is_on_ymd(time_t t, int y, int m, int d) {
    int ty, tm, td;
    local_ymd(t, &ty, &tm, &td);
    return ty == y && tm == m && td == d;
}

static time_t day_offset_ref(int day_offset) {
    /* Reference instant: local noon today + day_offset days. */
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    lt.tm_hour = 12;
    lt.tm_min  = 0;
    lt.tm_sec  = 0;
    lt.tm_mday += day_offset;
    return mktime(&lt);
}

int weather_pick_noon(const WeatherState *state, int day_offset) {
    time_t target = day_offset_ref(day_offset);
    int ry, rm, rd; local_ymd(target, &ry, &rm, &rd);
    int best = -1;
    long best_diff = LONG_MAX;
    for (int i = 0; i < state->n_hours; i++) {
        if (!is_on_ymd(state->hours[i].valid_time, ry, rm, rd)) continue;
        long d = (long)state->hours[i].valid_time - (long)target;
        if (d < 0) d = -d;
        if (d < best_diff) { best_diff = d; best = i; }
    }
    /* Fallback: if nothing matches the day (e.g. today already past noon),
     * pick the first entry whose date matches. */
    if (best < 0) {
        for (int i = 0; i < state->n_hours; i++) {
            if (is_on_ymd(state->hours[i].valid_time, ry, rm, rd)) { best = i; break; }
        }
    }
    return best;
}

int weather_hours_for_day(const WeatherState *state, int day_offset,
                          int out_indices[24]) {
    time_t ref = day_offset_ref(day_offset);
    int ry, rm, rd; local_ymd(ref, &ry, &rm, &rd);
    int count = 0;
    for (int i = 0; i < state->n_hours && count < 24; i++) {
        if (is_on_ymd(state->hours[i].valid_time, ry, rm, rd))
            out_indices[count++] = i;
    }
    return count;
}

/* Plausible base temperature (°C) and wind (m/s) per Wsymb2 category. */
static void demo_temp_wind(int symbol, double *out_temp, double *out_wind, int *out_dir) {
    double t, w;
    switch (symbol) {
    case 1: case 2:                            t = 21.0; w = 2.5; break; /* clear */
    case 3: case 4:                            t = 17.0; w = 3.0; break; /* partly cloudy */
    case 5: case 6:                            t = 13.0; w = 3.5; break; /* cloudy/overcast */
    case 7:                                    t =  9.0; w = 1.0; break; /* fog */
    case 8: case 18:                           t = 12.0; w = 4.0; break; /* light rain */
    case 9: case 19:                           t = 11.0; w = 5.0; break; /* rain */
    case 10: case 20:                          t = 10.0; w = 7.0; break; /* heavy rain */
    case 11: case 21:                          t = 19.0; w = 8.0; break; /* thunder */
    case 12: case 13: case 14:
    case 22: case 23: case 24:                 t =  1.5; w = 5.5; break; /* sleet */
    case 15: case 25:                          t = -2.0; w = 3.0; break; /* light snow */
    default:                                   t = -5.0; w = 6.0; break; /* snow: 16,17,26,27 */
    }
    *out_temp = t;
    *out_wind = w;
    *out_dir  = (symbol * 37) % 360;   /* deterministic but varied per symbol */
}

void weather_fill_demo(WeatherState *st, int symbol, UnitSystem units) {
    memset(st, 0, sizeof(*st));
    snprintf(st->place, sizeof st->place, "Demo");
    st->fetched_at = time(NULL);
    st->units      = units;  /* follow the user's preference (°C/m/s or °F/mph) */

    double base_temp, wind;
    int dir;
    demo_temp_wind(symbol, &base_temp, &wind, &dir);

    /* Three samples per day (morning/noon/evening) give a min/max spread per day. */
    static const int    offset_h[3] = { -5, 0, 6 };
    static const double delta_c[3]  = { -3.0, 2.0, 0.0 };

    int n = 0;
    for (int day = 0; day < 4; day++) {
        time_t noon = day_offset_ref(day);
        for (int j = 0; j < 3; j++) {
            WeatherHour *h = &st->hours[n++];
            h->valid_time = noon + offset_h[j] * 3600;
            /* Synthesize in metric, then convert for imperial. */
            double tc = base_temp + delta_c[j] + day * 0.5;   /* °C */
            double ws = wind + (j == 1 ? 0.5 : 0.0);           /* m/s */
            double gs = ws * 1.8;                              /* gusts, m/s */
            if (units == UNIT_IMPERIAL) {
                tc = tc * 9.0 / 5.0 + 32.0;   /* °F */
                ws = ws * 2.23694;            /* mph */
                gs = gs * 2.23694;            /* mph */
            }
            h->temp     = tc;
            h->wind    = ws;
            h->wind_gust  = gs;
            h->wind_dir   = dir;
            h->symbol     = symbol;
            h->has_temp = h->has_wind = h->has_gust = h->has_dir = h->has_symbol = 1;
        }
    }
    st->n_hours = n;
}
