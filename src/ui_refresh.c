#include "ui_internal.h"

/* Shared entry/in-place/forced-flash style decision for the three screens.
 * Priority: tab_switch > screen_change (snap_reagl ? REAGL : full_style) > partial_style. */
RefreshStyle resolve_refresh_style(const RefreshCtx *c) {
    if (c->tab_switch)    return RSTYLE_REAGL;
    if (c->screen_change) return c->snap_reagl ? RSTYLE_REAGL : c->full_style;
    return c->partial_style;
}

/* The single point where the gc16_flash setting meets a refresh. */
void ui_region_commit(const UIState *ui, Rect r, RefreshStyle style) {
    switch (style) {
    case RSTYLE_REAGL:        fb_paced_refresh(r, WFM_REAGL, false);          break;
    case RSTYLE_GC16:         fb_paced_refresh(r, WFM_GC16,  ui->persisted.gc16_flash); break;
    case RSTYLE_GC16_NOFLASH: fb_paced_refresh(r, WFM_GC16,  false);          break;
    case RSTYLE_GC16_FLASH:   fb_paced_refresh(r, WFM_GC16,  true);           break;
    }
}

/* In kiosk (asleep) context every partial commit forces a single GC16 sweep,
 * flashing iff sleep_gc16_flash is on. Awake returns the caller's style unchanged. */
RefreshStyle kiosk_resolve_style(const UIState *ui, RefreshStyle base) {
    if (!ui->power.kiosk) return base;
    return ui->persisted.sleep_gc16_flash ? RSTYLE_GC16_FLASH : RSTYLE_GC16_NOFLASH;
}

/* INIT-on-white clear + settle. Must run before new content is committed. */
void ui_run_init_wash(UIState *ui) {
    fb_paced_refresh(RECT_FULLSCREEN, WFM_INIT, true);
    fb_paced_wait();
    settle_sleep_ms(ui->persisted.init_wash_settle_ms);
}

/* Resolves a pending button-wake staleness override to a one-shot
 * {init_wash?, style}, consuming the pending state. WASH and FLASH are
 * mutually exclusive (guaranteed by classify_wake() in powersave.c). */
WakeOverride ui_consume_wake_override(UIState *ui) {
    WakeOverride wo = {0};
    switch (ui->power.wake_need) {
    case WAKE_REFRESH_WASH:  wo = (WakeOverride){ true, true,  RSTYLE_GC16_NOFLASH }; break;
    case WAKE_REFRESH_FLASH: wo = (WakeOverride){ true, false, RSTYLE_GC16_FLASH };   break;
    case WAKE_REFRESH_CLEAN: wo = (WakeOverride){ true, false, RSTYLE_GC16_NOFLASH }; break;
    case WAKE_REFRESH_NONE:  break;
    }
    ui->power.wake_need = WAKE_REFRESH_NONE;
    return wo;
}
