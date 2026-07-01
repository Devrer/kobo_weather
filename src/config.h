#ifndef CONFIG_H
#define CONFIG_H

#include "geo_locate.h"   /* Location (coords + label + precision) */

#define CONFIG_MAX_STR    256
#define CONFIG_MAX_URL    512

typedef struct {
    Location loc;               /* resolved location: lat/lon + label + precision */
    char   font_path[CONFIG_MAX_STR];
    char   api_url[CONFIG_MAX_URL]; /* SMHI fetch URL; built from loc.lat/lon if
                                       empty. Open-Meteo builds its own URL and
                                       ignores this field. */
    int    auto_location;       /* 1 = detect coords from IP; 0 = use loc.lat/lon */
    int    provider;            /* PROVIDER_*; set once from --smhi, not from the file */
} Config;

/* Load config from file. Returns 0 on success, -1 on error.
 * Missing file is non-fatal: defaults are populated (Stockholm). */
int config_load(const char *path, Config *cfg);

/* Weather data provider constants. */
#define PROVIDER_SMHI        0
#define PROVIDER_OPEN_METEO  1

/* Compose the SMHI URL into cfg->api_url if not already set. Harmless when
 * Open-Meteo is selected, since that path ignores api_url. */
void config_build_url(Config *cfg);

/* Timer defaults and the discrete step set for sleep-update interval. */
#define TIMER_SLEEP_DEFAULT_MIN    5
#define TIMER_ACTIVE_DEFAULT_MIN   15
#define TIMER_SLEEPUPD_DEFAULT_MIN 60

extern const int g_sleepupd_steps[6];  /* {15,30,60,180,360,720} minutes */

/* INIT-wash settle delay (ms) for the Display-tab stepper. See settle_sleep_ms() in ui.c. */
#define WASH_SETTLE_DEFAULT_MS    0
#define WASH_SETTLE_MIN_MS        0
#define WASH_SETTLE_MAX_MS     5000
#define WASH_SETTLE_STEP_MS     100

/* Settle time (ms) from seizing the panel from Nickel to our first startup
 * flash, so a still-draining Nickel menu-close update lands first instead of
 * merging with (and shortening) our flash. See settle_since_panel_seized()
 * in ui.c. Remainder-based: startup work already done counts toward it. */
#define STARTUP_HANDOVER_SETTLE_MS 200

/* Button-wake staleness thresholds (minutes asleep) for the Advanced-tab steppers.
 * See classify_wake() in powersave.c; flash is short-stale, wash is long-stale. */
#define WAKE_FLASH_DEFAULT_MIN   10
#define WAKE_FLASH_MIN_MIN        5
#define WAKE_FLASH_MAX_MIN       25
#define WAKE_FLASH_STEP_MIN       5

#define WAKE_WASH_DEFAULT_MIN    60
#define WAKE_WASH_MIN_MIN        30
#define WAKE_WASH_MAX_MIN       180
#define WAKE_WASH_STEP_MIN       30

/* Kiosk-entry refresh: NONE follows the awake partial_style, FLASH a single
 * full-screen GC16 flash, WASH a full INIT-on-white clean. Owns entry alone;
 * independent of sleep_gc16_flash (which only drives the tick-wakes). */
typedef enum {
    SLEEP_ENTRY_NONE  = 0,
    SLEEP_ENTRY_FLASH = 1,
    SLEEP_ENTRY_WASH  = 2,
} SleepEntryWash;

/* Hourly-list scroll mode. Lives here (not ui.h) because the persisted field
 * below is typed with it, and ui.h includes config.h (not vice-versa). */
typedef enum {
    SCROLL_FLOAT      = 0,
    SCROLL_SNAP_A2    = 1,
    SCROLL_SNAP_REAGL = 2,
} ScrollMode;

/* Persisted UI settings. */
typedef struct {
    ScrollMode snap;    /* hourly-list scroll mode */
    int lang;           /* language index 0–6 */
    int sleep_min;      /* auto-sleep idle timeout: 5..60 step 5 */
    int active_min;     /* active refresh interval: 5..60 step 5 */
    int sleep_upd_min;  /* asleep refresh interval: one of g_sleepupd_steps */
    int bold_cur_temp;  /* bold the big current temperature: 0/1 */
    int bold_max_temp;  /* bold the high temperature everywhere: 0/1 */
    int small_min_temp; /* shrink the low temperature everywhere: 0/1 */
    int units;          /* unit system: 0=metric (°C, m/s), 1=imperial (°F, mph) */
    int night_pause;    /* suppress fetch 00:00–06:00 local time: 0=off, 1=on */
    int full_style;     /* RefreshStyle for full-screen entry transitions: 0=REAGL, 1=GC16-flash */
    int partial_style;  /* RefreshStyle for ordinary content updates: 0=REAGL, 1=GC16-flash */
    int init_wash_settle_ms; /* INIT-wash settle delay before follow-up REAGL render, ms */
    SleepEntryWash sleep_entry_wash; /* kiosk-entry refresh (NONE/FLASH/WASH); 06:00 wash always INITs */
    int gc16_flash;     /* force flash on all GC16 refreshes: 0/1 */
    int sleep_gc16_flash; /* force flash on GC16 for kiosk tick-wakes / in-kiosk partials (NOT entry): 0/1 */
    int sleep_partial;    /* let kiosk timer-wake ticks use partial update instead of full sweep: 0/1 */
    int wake_flash_min;   /* button-wake staleness threshold (min) for forced GC16 flash */
    int wake_wash_min;    /* button-wake staleness threshold (min) for forced INIT-wash+GC16 (wins over wake_flash_min) */
} UISettings;

void settings_load(const char *path, UISettings *s);
void settings_save(const char *path, const UISettings *s);

#endif /* CONFIG_H */
