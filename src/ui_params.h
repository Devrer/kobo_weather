/*
 * ui_params.h — Tuneable UI parameters.
 * All UI_REF_* are pixels at reference width UI_REF_W; scaled by
 * actual_screen_w / UI_REF_W at runtime.
 */
#ifndef UI_PARAMS_H
#define UI_PARAMS_H

#define UI_REF_W       1072   /* reference screen width (Kobo Clara BW) */

/* ---- Header ---- */
#define UI_REF_HEADER_H        80
#define UI_REF_PAD             16
#define UI_REF_X_HALF          28
#define UI_REF_HEADER_ICON_GAP 16  /* extra gap between the moon and close header icons */

/* ---- Current weather section ---- */
/* Icon-pack native pixel sizes, used directly as the on-screen icon size;
 * each crop note records the whitespace trimmed at the source size. */
#define UI_REF_BIG_ICON_CLARA       402   /* large icon height (450 - 16px top - 32px bottom crop) */
#define UI_REF_BIG_ICON_W_CLARA     398   /* large icon width  (450 - 25px left - 27px right crop) */
#define UI_REF_BIG_ICON_LIBRA       475   /* large icon height (531 - 19px top - 37px bottom crop) */
#define UI_REF_BIG_ICON_W_LIBRA     470   /* large icon width  (531 - 29px left - 32px right crop) */
#define UI_REF_CUR_ICON_TOP    0    /* large icon top below the header */
#define UI_REF_CUR_SIDE_INSET  40   /* symmetric inset: icon left, texts right */
#define UI_REF_CUR_COND_ICON_GAP       5  /* visible px: icon bottom → condition cap-top */
#define UI_REF_CUR_COND_SEP_GAP       10  /* visible px: condition slot bottom → separator */
#define UI_REF_CUR_COND_SIDE_MARGIN    30  /* px added each side of the condition box (0 = icon width) */
/* Extra px between the two wrapped condition lines (negative = tighter than
 * natural pitch). Shared by current and forecast columns. */
#define UI_REF_COND_LINE_GAP           -6
#define UI_REF_CUR_SEP_WIND_GAP    50  /* visible px: separator → wind baseline */
#define UI_REF_CUR_WIND_MAXMIN_GAP 40  /* visible px: wind cap → max/min baseline */
#define UI_REF_CUR_MAXMIN_TEMP_GAP 40  /* visible px: max/min cap → temp baseline */
#define UI_REF_MAIN_H_SEP_W   920   /* width of separator between current and forecast */

/* ---- Three-day forecast section ---- (gaps are visible px, cap-height→baseline) */
#define UI_REF_FORECAST_TOP_INSET  30  /* separator line → first day label */
#define UI_REF_MED_ICON_CLARA           223  /* medium icon height (250 - 9px top - 18px bottom crop) */
#define UI_REF_MED_ICON_W_CLARA         221  /* medium icon width  (250 - 14px left - 15px right crop) */
#define UI_REF_MED_ICON_LIBRA           263  /* medium icon height (295 - 11px top - 21px bottom crop) */
#define UI_REF_MED_ICON_W_LIBRA         261  /* medium icon width  (295 - 16px left - 18px right crop) */
#define UI_REF_DAY_ICON_GAP        10  /* label baseline → icon top */
#define UI_REF_DAY_ICON_COND_GAP   5   /* icon bottom → condition cap-top */
#define UI_REF_DAY_COND_TEMP_GAP   5   /* condition slot bottom → max-temp cap-top */
#define UI_REF_DAY_COND_SIDE_MARGIN 15  /* px added each side of a forecast condition box (0 = column width) */
#define UI_REF_DAY_MIN_WIND_GAP    40  /* min-temp baseline → wind cap-top */
#define UI_REF_DAY_MAXMIN_SEP_PAD   4  /* vertical gap above/below the max/min divider line */

/* ---- Hourly list (drill-down screen) ---- */
#define UI_REF_SMALL_ICON_CLARA     100   /* small icon, hourly list (uncropped) */
#define UI_REF_SMALL_ICON_LIBRA     118   /* small icon, hourly list (uncropped) */
#define UI_REF_HOURLY_ROW_H   134
#define UI_REF_HOURLY_TIME_W  180   /* width of the hour-label text box */
#define UI_REF_HOURLY_ICON_X  140   /* icon x-offset from the left pad */
#define UI_REF_HOURLY_WIND_DX 250   /* wind column offset from temp column */
#define UI_REF_HOURLY_DIR_DX  380   /* direction column offset from wind column */

/* ---- Settings screen ---- */
#define UI_REF_SETTINGS_SEC_H  70   /* settings section-header bar height */
#define UI_REF_SETTINGS_COL_GAP  8  /* gap between the two settings columns */
#define UI_REF_STEPPER_ARROW_W  48  /* width of a stepper < or > arrow hit zone */
#define UI_REF_STEPPER_ARROW_H  12  /* half-size of the arrow chevron glyph */
#define UI_REF_STEPPER_HIT_W    72  /* full tap-target width for each stepper arrow */

/* ---- Shared separators ----------------------------------------- */
#define UI_REF_SEP_THICK        2

/* ================================================================
 * COLOURS  (8-bit grayscale: 0x00=black, 0xFF=white)
 * ================================================================ */
#define UI_BG               0xFF
#define UI_FG               0x00
#define UI_FG_SOFT          0x55
#define UI_SEP              0x00

/* ================================================================
 * FONT SIZES (points; FBInk OpenType)
 * ================================================================ */

/* ---- Header & current weather ---------------------------------- */
#define UI_FONT_HEADER_PT       16
#define UI_FONT_TEMP_BIG_PT     40   /* current temperature, large */
#define UI_FONT_COND_PT         16   /* current condition text */
#define UI_FONT_WIND_PT         16   /* wind info under current temp */

/* ---- Three-day forecast ---------------------------------------- */
#define UI_FONT_DAY_PT        14   /* day label in forecast */
#define UI_FONT_DAY_COND_PT   11   /* condition text in day stack */
#define UI_FONT_DAY_WIND_PT   14   /* wind in day stack */

/* ---- High/low temperature (today's "min-max°C" line and day stack) --
 * High and low share the same default size; the "smaller min" setting
 * shrinks the low value to UI_FONT_TEMP_MIN_SMALL_PT instead. */
#define UI_FONT_TEMP_HI_PT    24   /* high temperature ("-max°C" / day-stack max) */
#define UI_FONT_TEMP_LO_PT    24   /* low temperature ("min" / day-stack min), default */
#define UI_FONT_TEMP_MIN_SMALL_PT 20 /* low temperature when "smaller min" toggle is on */

/* ---- Hourly list ----------------------------------------------- */
#define UI_FONT_HOURLY_PT     22   /* hour-of-day label in hourly list */
#define UI_FONT_HOURLY_TEMP_PT 18  /* temperature in hourly list */
#define UI_FONT_HOURLY_WIND_PT 16  /* wind speed in hourly list */
#define UI_FONT_HOURLY_DIR_PT  14  /* wind direction (smaller than speed) */

/* ---- Settings screen ------------------------------------------- */
#define UI_FONT_SETTINGS_SEC_PT  11   /* settings section-header text */
#define UI_FONT_SETTINGS_COL_TITLE_PT  13   /* dual-column Wake/Sleep titles, slightly larger */
#define UI_FONT_SETTINGS_ROW_PT  11   /* settings radio/stepper rows */
#define UI_FONT_SETTINGS_TAB_PT  13   /* tab labels (General/Timers/Advanced) */

/* ---- Subtitle & misc ------------------------------------------- */
#define UI_FONT_PLACE_PT      11   /* "Stockholm" or similar subtitle */
#define UI_REF_SUBTITLE_BOTTOM 72  /* distance from screen bottom to subtitle y */
#define UI_FONT_LOADING_PT    18

/* ================================================================
 * INTERACTION
 * ================================================================ */
#define UI_GESTURE_COMMIT_PX  12

#endif /* UI_PARAMS_H */
