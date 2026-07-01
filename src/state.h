#ifndef STATE_H
#define STATE_H

#include <time.h>

#define WEATHER_MAX_HOURS 240   /* 10 days * 24 hours */
#define WEATHER_PLACE_LEN 64

/* Unit system a WeatherState's values are stored in; formatters read this, not
 * the user's preference, so a metric-only source still renders correctly. */
typedef enum {
    UNIT_METRIC   = 0,  /* temp in °C, wind in m/s */
    UNIT_IMPERIAL = 1   /* temp in °F, wind in mph */
} UnitSystem;

typedef struct {
    time_t valid_time;     /* unix timestamp from validTime */
    double temp;            /* air temperature, in WeatherState.units */
    double wind;            /* wind speed, in WeatherState.units */
    double wind_gust;       /* wind gust speed, in WeatherState.units */
    int    wind_dir;        /* wind direction, 0-360° (0/360 = N) */
    int    symbol;          /* wsymb2 / symbol_code, 1-27 */
    int    has_temp;
    int    has_wind;
    int    has_gust;
    int    has_dir;
    int    has_symbol;
} WeatherHour;

typedef struct {
    char        place[WEATHER_PLACE_LEN];  /* display name, e.g. "Stockholm" */
    WeatherHour hours[WEATHER_MAX_HOURS];
    int         n_hours;
    time_t      fetched_at;
    UnitSystem  units;      /* unit system temp/wind values are expressed in */
} WeatherState;

#endif /* STATE_H */
