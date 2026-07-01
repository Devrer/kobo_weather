#ifndef FETCH_SERVICE_H
#define FETCH_SERVICE_H

#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>

#include "config.h"
#include "weather.h"
#include "wifi.h"

/* Fetch pipeline: bring WiFi up, optionally geolocate, then fetch from the
 * provider. Runs on a background thread (interactive) or synchronously
 * (no-cache startup, kiosk wake-tick). */

/* Outcome of a fetch attempt; distinct codes let callers render the right error screen. */
typedef enum {
    FETCH_OK = 0,
    FETCH_ERR_WIFI,     /* WiFi association / DHCP failed */
    FETCH_ERR_NICKEL,   /* WiFi can't be enabled this boot; user must use Nickel */
    FETCH_ERR_GEO,      /* geolocation failed */
    FETCH_ERR_FETCH,    /* provider fetch/parse failed (see FetchResult.err) */
} FetchStatus;

/* What to fetch and how to bring the network up. */
typedef struct {
    Config *cfg;             /* read-only to the worker; cfg->provider picks the source
                                (PROVIDER_*). A geolocated location is published in
                                FetchResult.resolved_loc, not written back here */
    int     units;           /* UnitSystem */
    bool    geolocate;       /* run geolocation first (gated on cfg->auto_location);
                                interactive/startup pass true, kiosk passes false to
                                reuse the coords already in cfg */
    int     wpa_retries;     /* >0: cap WPA association at N seconds (kiosk); 0: default */
    WifiStageFn on_stage;    /* per-WiFi-stage callback for blocking fetches (NULL = silent);
                                ignored by the threaded path, which signals status_fd instead */
    void   *on_stage_ctx;    /* passed through to on_stage */
} FetchRequest;

/* Result of a fetch. `state` is valid iff status == FETCH_OK. */
typedef struct {
    FetchStatus  status;
    int          wifi_stage;  /* last WIFI_STAGE_* observed */
    int          rc;          /* provider fetch return code (for diagnostics) */
    char         err[256];    /* set iff status == FETCH_ERR_FETCH; copied from
                                  weather_last_error() on the fetch's own thread */
    int          has_resolved_loc; /* 1 if geolocation ran; caller adopts resolved_loc
                                      into its Config (the worker never writes cfg) */
    Location     resolved_loc;
    WeatherState state;
} FetchResult;

/* Background (threaded) fetch manager. At most one fetch in flight at a time. */
typedef struct {
    pthread_t    thread;
    bool         in_progress;
    int          event_fd;    /* fires once when the worker completes */
    int          status_fd;   /* fires on each WiFi-stage change */
    _Atomic int  stage;       /* latest WIFI_STAGE_* (read after status_fd fires) */
    FetchRequest req;
    FetchResult  result;      /* worker-owned; read only after event_fd fires */
} FetchManager;

/* Create the manager's eventfds. Returns 0 on success, -1 on failure. */
int  fetch_manager_init(FetchManager *fm);

/* Start a background fetch. Copies *req into the manager. Returns 0 if the
 * worker thread was created (in_progress becomes true), or -1 on failure
 * (in_progress stays false; caller must not wait on it). */
int  fetch_manager_start(FetchManager *fm, const FetchRequest *req);

/* Join the in-flight worker and copy its result into *out. Drains event_fd and
 * clears in_progress. No-op (out->status untouched) if no fetch is in flight. */
void fetch_manager_complete(FetchManager *fm, FetchResult *out);

/* Latest WiFi stage observed by the in-flight worker (for the live subtitle). */
int  fetch_manager_stage(const FetchManager *fm);

/* Close the manager's eventfds. */
void fetch_manager_cleanup(FetchManager *fm);

/* Run a fetch synchronously on the calling thread (startup + kiosk). */
void fetch_blocking(const FetchRequest *req, FetchResult *out);

#endif /* FETCH_SERVICE_H */
