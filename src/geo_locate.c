#include "geo_locate.h"
#include "../http_client.h"
#include "../cJSON.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Plaintext HTTP only (no HTTPS on ip-api.com's free tier); discloses the
 * device's public IP and is unauthenticated. Opt out via AUTO_LOCATION=0. */
#define GEO_URL "http://ip-api.com/json"

int geo_locate(Location *loc) {
    loc->place[0]  = '\0';
    loc->precision = GEO_PRECISION_NONE;

    struct MemoryStruct chunk = {NULL, 0};
    if (https_get(GEO_URL, "KoboWeatherApp/2.0", &chunk) != HTTPC_OK ||
        !chunk.memory) {
        free(chunk.memory);
        return -1;
    }

    cJSON *root = cJSON_Parse(chunk.memory);
    free(chunk.memory);
    if (!root) return -1;

    int ok = 0;

    cJSON *status = cJSON_GetObjectItem(root, "status");
    if (cJSON_IsString(status) && strcmp(status->valuestring, "success") == 0) {
        cJSON *jlat = cJSON_GetObjectItem(root, "lat");
        cJSON *jlon = cJSON_GetObjectItem(root, "lon");
        if (cJSON_IsNumber(jlat) && cJSON_IsNumber(jlon)) {
            double la = jlat->valuedouble;
            double lo = jlon->valuedouble;
            if (la >= -90.0 && la <= 90.0 && lo >= -180.0 && lo <= 180.0) {
                loc->lat = la;
                loc->lon = lo;
                ok       = 1;
            }
        }
    }

    if (ok) {
        /* "city" is sometimes absent; fall back to a coarser label. */
        static const struct { const char *field; GeoPrecision prec; } fields[] = {
            { "city",       GEO_PRECISION_CITY    },
            { "regionName", GEO_PRECISION_REGION  },
            { "country",    GEO_PRECISION_COUNTRY },
        };
        for (size_t i = 0; i < sizeof fields / sizeof fields[0]; i++) {
            cJSON *f = cJSON_GetObjectItem(root, fields[i].field);
            if (cJSON_IsString(f) && f->valuestring[0]) {
                snprintf(loc->place, sizeof loc->place, "%s", f->valuestring);
                loc->precision = fields[i].prec;
                break;
            }
        }
    }

    cJSON_Delete(root);
    return ok ? 0 : -1;
}
