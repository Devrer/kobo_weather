#ifndef GEO_LOCATE_H
#define GEO_LOCATE_H

/* Precision of the human label in a Location; ordered for numeric comparison. */
typedef enum {
    GEO_PRECISION_NONE    = 0,
    GEO_PRECISION_COUNTRY = 1,
    GEO_PRECISION_REGION  = 2,
    GEO_PRECISION_CITY    = 3,
} GeoPrecision;

typedef struct {
    double       lat, lon;
    char         place[64];   /* best-available human label */
    GeoPrecision precision;   /* which field produced `place` */
} Location;

/* Looks up the current public IP's location via ip-api.com/json (city ->
 * regionName -> country fallback). Returns 0 on success (*loc filled,
 * coordinates always set), -1 on failure (*loc indeterminate). */
int geo_locate(Location *loc);

#endif /* GEO_LOCATE_H */
