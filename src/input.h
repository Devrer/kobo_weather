#ifndef INPUT_H
#define INPUT_H

#include <stdint.h>

typedef enum {
    EV_TOUCH_DOWN,   /* finger placed */
    EV_TOUCH_MOVE,   /* finger moved */
    EV_TOUCH_UP,     /* finger lifted */
} TouchEventType;

typedef struct {
    TouchEventType type;
    int x;
    int y;
} TouchEvent;

/* Open the first available touchscreen device.
 * Returns fd >= 0 on success, -1 on error. */
int  input_open(void);
void input_close(int fd);

/* Injects a synthetic KEY_POWER press+release on wake, giving Nickel a second
 * toggle cycle so the real wakeup press (buffered in its grabbed fd) leaves it
 * awake instead of re-sleeping. Safe to call if the device can't be opened. */
void power_key_inject_cancel(void);

/* Opens the KEY_POWER evdev node read-only, ungrabbed (grabbing would break
 * Nickel's own sleep toggle). Keep the fd open across suspend/resume so a
 * wake press isn't missed. Returns fd >= 0, or -1 if not found / open failed. */
int power_key_open(void);

/* Drain all pending events on a fd from power_key_open(). Returns 1 if a
 * KEY_POWER press was seen since the last drain, else 0. Safe with fd < 0. */
int power_key_drain_pressed(int fd);

/* Tell the input layer the framebuffer dimensions so it can auto-detect
 * the rotation needed to map touch panel coordinates to screen coordinates.
 * Call this once after ui_init() when screen dimensions are known.
 * Override with env var KOBO_WEATHER_TOUCH_ROTATION=0|1|2|3. */
void input_set_screen(int fb_w, int fb_h);

/* Non-blocking read. Returns 0 if an event was returned, 1 if no event, -1 on error. */
int  input_read_nonblock(int fd, TouchEvent *ev);

/* Milliseconds since arbitrary epoch (CLOCK_MONOTONIC). */
uint64_t input_now_ms(void);

#endif /* INPUT_H */
