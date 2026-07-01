#ifndef I18N_H
#define I18N_H

typedef enum {
    LANG_EN = 0,
    LANG_SV = 1,
    LANG_ES = 2,
    LANG_PT = 3,
    LANG_DE = 4,
    LANG_FR = 5,
    LANG_IT = 6,
    LANG_COUNT = 7
} Lang;

typedef enum {
    /* ===== Home / data (home, hourly, demo screens) ===== */
    /* Day names (index = tm_wday: 0=Sun..6=Sat) — order load-bearing */
    S_DAY_SUN,
    S_DAY_MON,
    S_DAY_TUE,
    S_DAY_WED,
    S_DAY_THU,
    S_DAY_FRI,
    S_DAY_SAT,
    /* Relative day labels */
    S_TODAY,
    S_TOMORROW,
    /* Month abbreviations (0=Jan..11=Dec) */
    S_MON_JAN,
    S_MON_FEB,
    S_MON_MAR,
    S_MON_APR,
    S_MON_MAY,
    S_MON_JUN,
    S_MON_JUL,
    S_MON_AUG,
    S_MON_SEP,
    S_MON_OCT,
    S_MON_NOV,
    S_MON_DEC,
    /* Compass sectors (0..7, see tr_compass) */
    S_CMP_N,
    S_CMP_NE,
    S_CMP_E,
    S_CMP_SE,
    S_CMP_S,
    S_CMP_SW,
    S_CMP_W,
    S_CMP_NW,
    /* Weather condition codes 1-27 (index = symbol-1) + unknown */
    S_WX_1,
    S_WX_2,
    S_WX_3,
    S_WX_4,
    S_WX_5,
    S_WX_6,
    S_WX_7,
    S_WX_8,
    S_WX_9,
    S_WX_10,
    S_WX_11,
    S_WX_12,
    S_WX_13,
    S_WX_14,
    S_WX_15,
    S_WX_16,
    S_WX_17,
    S_WX_18,
    S_WX_19,
    S_WX_20,
    S_WX_21,
    S_WX_22,
    S_WX_23,
    S_WX_24,
    S_WX_25,
    S_WX_26,
    S_WX_27,
    S_WX_UNKNOWN,
    /* Wind label */
    S_WIND_NA,
    /* Screen titles */
    S_SETTINGS_TITLE,
    /* ===== Status / network (subtitle) ===== */
    S_UPDATED,
    S_UPDATING,
    S_WIFI_MODULE,
    S_WIFI_SCANNING,
    S_WIFI_CONNECTING,
    S_WIFI_DHCP,
    S_WIFI_CONNECTED,
    S_WIFI_FAILED,
    S_WIFI_NEEDS_NICKEL,
    /* Currently unused (former full-screen messages) */
    S_ERROR_FETCH,
    /* ===== Settings menu ===== */
    /* General tab */
    S_TAB_GENERAL,
    S_SEC_SCROLL,
    S_SCROLL_FLOAT,
    S_SCROLL_A2,
    S_SCROLL_REAGL,
    S_SEC_LANGUAGE,
    S_SEC_TEMP,
    S_TEMP_BOLD_CUR,
    S_TEMP_BOLD_MAX,
    S_TEMP_SMALL_MIN,
    S_NIGHT_PAUSE,
    /* Timers tab */
    S_TAB_TIMERS,
    S_SEC_TIMERS,
    S_TIMER_SLEEP,
    S_TIMER_ACTIVE,
    S_TIMER_SLEEP_UPD,
    S_UNIT_MIN,
    S_UNIT_HOUR,
    /* Advanced tab */
    S_TAB_ADVANCED,
    S_COL_WAKE,
    S_COL_SLEEP,
    S_SEC_REFRESH,
    S_REFRESH_REAGL,
    S_SEC_FULL_STYLE,
    S_STYLE_GC16,
    S_SEC_WASH,
    S_WASH_DELAY,
    S_GC16_FLASH,
    S_WASH_NONE,
    S_WASH_INIT,
    S_SLEEP_TICK_FLASH,
    S_SEC_REFRESH_GROUP,
    S_SEC_WAKE_STALE,
    S_WAKE_FLASH_AFTER,
    S_WAKE_WASH_AFTER,
    S_DEMO_RUN,
    STR_COUNT
} StrId;

extern const char * const g_strings[LANG_COUNT][STR_COUNT];

static inline const char *tr(Lang lang, StrId id) {
    if ((unsigned)lang >= LANG_COUNT || (unsigned)id >= STR_COUNT) return "";
    return g_strings[(int)lang][(int)id];
}

/* Translate internal wsymb2 symbol code (1–27) to condition string. */
const char *tr_weather(Lang lang, int symbol);

/* Translate wind degrees (0–360) to compass abbreviation string. */
const char *tr_compass(Lang lang, int degrees);

#endif /* I18N_H */
