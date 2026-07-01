/* timeutil.h — clock-boundary helpers for weather refresh scheduling.
 * Shared by main.c's refresh timer and powersave.c's RTC alarm so both agree
 * on boundary instants. Boundaries are computed in local time (DST-aware via
 * tm_gmtoff) so long sleep intervals land on round local clock times. */
#ifndef TIMEUTIL_H
#define TIMEUTIL_H

#include <time.h>

/* Epoch of the next local-aligned boundary strictly after `now`, bucketed by
 * `step` seconds. DST-aware. */
static inline time_t next_local_boundary(time_t now, long step) {
    struct tm lt;
    localtime_r(&now, &lt);
    long off = lt.tm_gmtoff;              /* seconds east of UTC */
    return (((now + off) / step) + 1) * step - off;
}

/* True if `now` and `last` fall in different local-aligned buckets of size
 * `step` seconds, i.e. a boundary was crossed between them. */
static inline int crossed_boundary(time_t now, time_t last, long step) {
    struct tm lt;
    localtime_r(&now, &lt);
    long off = lt.tm_gmtoff;
    return ((now + off) / step) != ((last + off) / step);
}

/* Local time of the next 06:00 strictly after `now`. Used to schedule the
 * daily morning screen wash. */
static inline time_t next_06am(time_t now) {
    struct tm lt;
    localtime_r(&now, &lt);
    lt.tm_hour = 6; lt.tm_min = 0; lt.tm_sec = 0; lt.tm_isdst = -1;
    time_t t = mktime(&lt);
    if (t <= now) { lt.tm_mday++; t = mktime(&lt); }
    return t;
}

#endif /* TIMEUTIL_H */
