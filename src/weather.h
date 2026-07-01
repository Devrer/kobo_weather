#ifndef WEATHER_H
#define WEATHER_H

#include "state.h"
#include "config.h"

/* Fetch the forecast for `provider` (PROVIDER_*) into state in the given unit
 * system (ignored for SMHI, which is metric-only). Returns 0, negative on error. */
int weather_fetch_provider(const Config *cfg, WeatherState *state,
                           int provider, UnitSystem units);

/* Description of the last fetch failure; valid until the next fetch call. */
const char *weather_last_error(void);

/* Index into state->hours nearest local noon for day_offset (0=today), or -1. */
int weather_pick_noon(const WeatherState *state, int day_offset);

/* Fill out_indices with hour indices for the given day, ascending by valid_time.
 * Returns the count (0..24). */
int weather_hours_for_day(const WeatherState *state, int day_offset,
                          int out_indices[24]);

/* Fill st with synthetic "Demo" data: hourly entries around noon for today and
 * the next three days, all carrying symbol (1..27, Wsymb2) so the normal render
 * path shows one consistent symbol everywhere. */
void weather_fill_demo(WeatherState *st, int symbol, UnitSystem units);

#endif /* WEATHER_H */
