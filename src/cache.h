#ifndef CACHE_H
#define CACHE_H

#include "state.h"

/* Load cached WeatherState from path. Returns 0 on success, -1 on failure. */
int  cache_load(const char *path, WeatherState *state);

/* Save WeatherState to cache file. Silently ignores write errors. */
void cache_save(const char *path, const WeatherState *state);

#endif /* CACHE_H */
