#ifndef POWERSAVE_H
#define POWERSAVE_H

#include "ui.h"
#include "config.h"

/* Low-power kiosk mode: headerless frame, no WiFi/touch, suspend-to-RAM with an
 * RTC wake alarm; wakes periodically to refetch/redraw. Blocks until the power
 * button exits (or g_running clears). *input_fd is closed on entry, reopened on
 * exit. Returns 0. */
int power_save_loop(UIState *ui, Config *cfg, int *input_fd,
                    const char *cache_path);

/* Set KIOSK_FLAG to exempt kiosk from the launcher fail-safe. Call at the very
 * start of power-save entry, before the blocking fetch join, so the whole entry
 * sequence is covered. Cleared by power_save_loop() on exit. */
void power_save_mark_kiosk(void);

#endif /* POWERSAVE_H */
