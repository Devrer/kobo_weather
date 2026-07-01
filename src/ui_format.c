#include "ui_internal.h"
#include "i18n.h"
#include "weather.h"
#include "wifi.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>

/* Degree-suffix / wind-unit string for a WeatherState's unit system (read
 * from the data, not user preference, so metric-only sources render right). */
const char *temp_unit_suffix(UnitSystem u) {
    return (u == UNIT_IMPERIAL) ? "\xc2\xb0\x46" : "\xc2\xb0\x43";
}
const char *wind_unit_str(UnitSystem u) {
    return (u == UNIT_IMPERIAL) ? "mph" : "m/s";
}

const char *day_label(Lang lang, int day_offset) {
    time_t t = time(NULL);
    struct tm lt;
    localtime_r(&t, &lt);
    lt.tm_mday += day_offset;
    mktime(&lt);
    if (day_offset == 0) return tr(lang, S_TODAY);
    if (day_offset == 1) return tr(lang, S_TOMORROW);
    return tr(lang, (StrId)(S_DAY_SUN + (lt.tm_wday % 7)));
}

void format_long_date(Lang lang, int day_offset, char *buf, size_t n) {
    time_t t = time(NULL);
    struct tm lt;
    localtime_r(&t, &lt);
    lt.tm_mday += day_offset;
    mktime(&lt);
    snprintf(buf, n, "%s %d %s", day_label(lang, day_offset), lt.tm_mday,
             tr(lang, (StrId)(S_MON_JAN + lt.tm_mon)));
}

/* Max age for find_now() to consider an hour "current"; beyond this the
 * cache is stale and callers fall back to "---" placeholders. */
#define FIND_NOW_MAX_AGE_S (90 * 60)

WeatherHour *find_now(const WeatherState *st) {
    if (st->n_hours <= 0) return NULL;
    time_t now = time(NULL);
    long best = 1L << 30;
    WeatherHour *cur = NULL;
    for (int i = 0; i < st->n_hours; i++) {
        long d = (long)st->hours[i].valid_time - (long)now;
        if (d < 0) d = -d;
        if (d < best) { best = d; cur = (WeatherHour *)&st->hours[i]; }
    }
    return (best <= FIND_NOW_MAX_AGE_S) ? cur : NULL;
}

void fmt_temp(char *buf, int n, const WeatherHour *h, UnitSystem units) {
    if (h && h->has_temp) snprintf(buf, n, "%.1f%s", h->temp, temp_unit_suffix(units));
    else                   snprintf(buf, n, "---%s", temp_unit_suffix(units));
}

void fmt_cond(Lang lang, char *buf, int n, const WeatherHour *h) {
    if (h && h->has_symbol) snprintf(buf, n, "%s", tr_weather(lang, h->symbol));
    else                    buf[0] = '\0';
}

/* Wind speed with gust in parentheses, e.g. "2.9(7)m/s", no direction. */
void fmt_day_wind(Lang lang, char *buf, int n, const WeatherHour *h, UnitSystem units) {
    if (h && h->has_wind) {
        const char *u = wind_unit_str(units);
        if (h->has_gust)
            snprintf(buf, n, "%.1f(%.0f)%s", h->wind, h->wind_gust, u);
        else
            snprintf(buf, n, "%.1f%s", h->wind, u);
    } else {
        snprintf(buf, n, "%s", tr(lang, S_WIND_NA));
    }
}

/* Compute daily high/low formatted strings for day_offset.
 * Writes "" into tmax/tmin buffers when no temperature data is available. */
void compute_day_maxmin(const WeatherState *st, int day_offset,
                        char *tmax, int tn, char *tmin, int mn) {
    int idx[24];
    int n = weather_hours_for_day(st, day_offset, idx);
    double hi = NAN, lo = NAN;
    for (int i = 0; i < n; i++) {
        const WeatherHour *h = &st->hours[idx[i]];
        if (!h->has_temp) continue;
        if (isnan(hi) || h->temp > hi) hi = h->temp;
        if (isnan(lo) || h->temp < lo) lo = h->temp;
    }
    const char *suf = temp_unit_suffix(st->units);
    if (!isnan(hi)) snprintf(tmax, tn, "%.0f%s", hi, suf); else tmax[0] = '\0';
    if (!isnan(lo)) snprintf(tmin, mn, "%.0f%s", lo, suf); else tmin[0] = '\0';
}

/* "low°C/high°C" plain-text equivalent of what draw_cur_maxmin() renders. */
void fmt_cur_maxmin(char *buf, int n, const WeatherState *st) {
    char hi[16], lo[16];
    compute_day_maxmin(st, 0, hi, sizeof hi, lo, sizeof lo);
    if (hi[0] && lo[0])
        snprintf(buf, n, "%s/%s", lo, hi);
    else
        buf[0] = '\0';
}

/* Map a WIFI_STAGE_* value to the matching i18n string. */
static const char *wifi_stage_str(Lang lang, int stage) {
    switch (stage) {
    case WIFI_STAGE_MODULE:     return tr(lang, S_WIFI_MODULE);
    case WIFI_STAGE_SCANNING:   return tr(lang, S_WIFI_SCANNING);
    case WIFI_STAGE_CONNECTING: return tr(lang, S_WIFI_CONNECTING);
    case WIFI_STAGE_DHCP:       return tr(lang, S_WIFI_DHCP);
    case WIFI_STAGE_CONNECTED:    return tr(lang, S_WIFI_CONNECTED);
    case WIFI_STAGE_FAILED:       return tr(lang, S_WIFI_FAILED);
    case WIFI_STAGE_NEEDS_NICKEL: return tr(lang, S_WIFI_NEEDS_NICKEL);
    default:                      return tr(lang, S_UPDATING);
    }
}

/* Build the subtitle string (same logic as draw_subtitle renders). */
void build_subtitle_str(char *buf, int n, const UIState *ui) {
    Lang lang = (Lang)ui->persisted.lang;
    if (ui->wifi_needs_nickel) {
        /* "Enable WiFi in Nickel" owns the whole subtitle line — no timestamp/place. */
        snprintf(buf, n, "%s", tr(lang, S_WIFI_NEEDS_NICKEL));
        return;
    }
    if (ui->fetch_in_progress) {
        /* Mid-fetch: "stage • place", same shape as the Updated line. No
         * "Updated HH:MM" prefix — stale mid-fetch, and a 3-part line overflows. */
        const char *stage_s = (ui->fetch_stage > 0 &&
                               ui->fetch_stage < WIFI_STAGE_CONNECTED)
                              ? wifi_stage_str(lang, ui->fetch_stage)
                              : tr(lang, S_UPDATING);
        if (ui->state->place[0])
            snprintf(buf, n, "%s  \xe2\x80\xa2  %s", stage_s, ui->state->place);
        else
            snprintf(buf, n, "%s", stage_s);
        return;
    }
    if (ui->state->fetched_at > 0) {
        struct tm lt;
        localtime_r(&ui->state->fetched_at, &lt);
        if (ui->state->place[0])
            snprintf(buf, n, "%s %02d:%02d  \xe2\x80\xa2  %s",
                     tr(lang, S_UPDATED), lt.tm_hour, lt.tm_min, ui->state->place);
        else
            snprintf(buf, n, "%s %02d:%02d",
                     tr(lang, S_UPDATED), lt.tm_hour, lt.tm_min);
    } else if (ui->state->place[0]) {
        /* Never fetched / last fetch failed: short note + place, not a blank line. */
        snprintf(buf, n, "%s  \xe2\x80\xa2  %s", tr(lang, S_ERROR_FETCH), ui->state->place);
    } else {
        snprintf(buf, n, "%s", tr(lang, S_ERROR_FETCH));
    }
}
