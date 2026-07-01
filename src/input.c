#include "input.h"
#include "fb.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <time.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/input.h>

/* ABS_MT_SLOT added in Linux 2.6.36; older toolchain headers may lack it. */
#ifndef ABS_MT_SLOT
#define ABS_MT_SLOT 0x2f
#endif

/* Power button key code; guard in case the toolchain header is ancient. */
#ifndef KEY_POWER
#define KEY_POWER 116
#endif

#define BITS_PER_LONG  (8 * (int)sizeof(unsigned long))
#define NBITS(x)       (((x) + BITS_PER_LONG - 1) / BITS_PER_LONG)
#define TEST_BIT(arr, n) (((arr)[(n) / BITS_PER_LONG] >> ((n) % BITS_PER_LONG)) & 1)

/* Touch-to-screen coordinate transform. rotation: 0=none, 1=swap+mirror_y
 * (Clara BW default), 2=180 deg, 3=swap+mirror_x. */
typedef struct {
    int max_x, max_y;   /* touch panel native ranges (from EVIOCGABS) */
    int fb_w,  fb_h;    /* framebuffer dimensions (set by input_set_screen) */
    int rotation;
    int have_screen;    /* 1 once input_set_screen() has been called */
} TouchTransform;

/* Sane defaults for Kobo Clara BW (landscape panel 1448×1072, portrait fb 1072×1448) */
static TouchTransform g_tt = { 1448, 1072, 1072, 1448, 1, 0 };

/* MT protocol B state */
typedef struct {
    int tracking_id;
    int x;
    int y;
    int active;
} MTSlot;

#define MAX_SLOTS 10

typedef struct {
    MTSlot slots[MAX_SLOTS];
    int    cur_slot;
    /* pending event to emit after SYN_REPORT */
    int    pending;
    TouchEvent pending_ev;
} MTState;

static MTState g_mt;

/* Stateful DOWN/MOVE/UP emitter — declared here so do_read() can access it */
typedef struct { int touching; } SimpleMT;
static SimpleMT g_simple;

static int find_touch_device(void) {
    /* Try well-known Kobo paths first */
    static const char *known[] = {
        "/dev/input/event1",
        "/dev/input/event0",
        "/dev/input/event2",
        NULL
    };
    for (int i = 0; known[i]; i++) {
        int fd = open(known[i], O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        /* Check it has ABS_MT_POSITION_X capability */
        unsigned long evbit[2]  = {0, 0};
        unsigned long absbit[4] = {0, 0, 0, 0};
        ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), evbit);
        if (evbit[0] & (1u << EV_ABS)) {
            ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbit)), absbit);
            if (absbit[1] & (1u << (ABS_MT_POSITION_X - 32))) {
                /* looks like a touchscreen — query axis ranges for coordinate mapping */
                struct input_absinfo ai;
                if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &ai) == 0 && ai.maximum > 0)
                    g_tt.max_x = ai.maximum;
                if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &ai) == 0 && ai.maximum > 0)
                    g_tt.max_y = ai.maximum;
                return fd;
            }
        }
        close(fd);
    }
    /* Fallback: scan /dev/input/ */
    DIR *d = opendir("/dev/input");
    if (!d) return -1;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, "event", 5) != 0) continue;
        char path[280];
        snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        unsigned long evbit[2]  = {0, 0};
        unsigned long absbit[4] = {0, 0, 0, 0};
        ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), evbit);
        if (evbit[0] & (1u << EV_ABS)) {
            ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbit)), absbit);
            if (absbit[1] & (1u << (ABS_MT_POSITION_X - 32))) {
                struct input_absinfo ai;
                if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &ai) == 0 && ai.maximum > 0)
                    g_tt.max_x = ai.maximum;
                if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &ai) == 0 && ai.maximum > 0)
                    g_tt.max_y = ai.maximum;
                closedir(d);
                return fd;
            }
        }
        close(fd);
    }
    closedir(d);
    return -1;
}

int input_open(void) {
    memset(&g_mt, 0, sizeof(g_mt));
    for (int i = 0; i < MAX_SLOTS; i++) g_mt.slots[i].tracking_id = -1;
    g_simple.touching = 0;
    int fd = find_touch_device();
    if (fd >= 0) {
        /* Grab exclusively so the suspended Nickel doesn't also see our touches. */
        if (ioctl(fd, EVIOCGRAB, 1) < 0)
            fprintf(stderr, "EVIOCGRAB failed: errno=%d (continuing ungrabbed)\n", errno);
    }
    return fd;
}

void input_close(int fd) {
    if (fd >= 0) {
        ioctl(fd, EVIOCGRAB, 0);  /* release so Nickel regains input on exit */
        close(fd);
    }
    memset(&g_mt, 0, sizeof(g_mt));
    for (int i = 0; i < MAX_SLOTS; i++) g_mt.slots[i].tracking_id = -1;
    g_simple.touching = 0;
}

/* Find the evdev node for the power key and return its path in buf.
 * Returns 1 on success, 0 if not found. */
static int find_power_key_path(char *buf, size_t bufsz) {
    DIR *d = opendir("/dev/input");
    if (!d) return 0;
    struct dirent *ent;
    int found = 0;
    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, "event", 5) != 0) continue;
        snprintf(buf, bufsz, "/dev/input/%s", ent->d_name);
        int f = open(buf, O_RDONLY | O_NONBLOCK);
        if (f < 0) continue;
        unsigned long evbit[NBITS(EV_MAX)]  = {0};
        unsigned long keybit[NBITS(KEY_MAX)] = {0};
        if (ioctl(f, EVIOCGBIT(0, sizeof(evbit)), evbit) >= 0
            && TEST_BIT(evbit, EV_KEY)
            && ioctl(f, EVIOCGBIT(EV_KEY, sizeof(keybit)), keybit) >= 0
            && TEST_BIT(keybit, KEY_POWER)) {
            close(f);
            found = 1;
            break;
        }
        close(f);
    }
    closedir(d);
    return found;
}

void power_key_inject_cancel(void) {
    char path[280];
    if (!find_power_key_path(path, sizeof(path))) {
        fprintf(stderr, "power_key_inject_cancel: no KEY_POWER device found\n");
        return;
    }
    /* O_WRONLY doesn't need EVIOCGRAB; kernel delivers to the grabbing fd (Nickel's). */
    int fd = open(path, O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "power_key_inject_cancel: open %s failed errno=%d\n",
                path, errno);
        return;
    }
    struct input_event ev[4];
    memset(ev, 0, sizeof(ev));
    ev[0].type = EV_KEY; ev[0].code = KEY_POWER; ev[0].value = 1;
    ev[1].type = EV_SYN; ev[1].code = SYN_REPORT; ev[1].value = 0;
    ev[2].type = EV_KEY; ev[2].code = KEY_POWER; ev[2].value = 0;
    ev[3].type = EV_SYN; ev[3].code = SYN_REPORT; ev[3].value = 0;
    ssize_t n = write(fd, ev, sizeof(ev));
    if (n == (ssize_t)sizeof(ev))
        fprintf(stderr, "power_key_inject_cancel: injected cancel on %s\n", path);
    else
        fprintf(stderr, "power_key_inject_cancel: write failed errno=%d on %s\n",
                errno, path);
    close(fd);
}

int power_key_open(void) {
    char path[280];
    if (!find_power_key_path(path, sizeof(path))) {
        fprintf(stderr, "power_key_open: no KEY_POWER device found\n");
        return -1;
    }
    /* Read-only, no EVIOCGRAB: grabbing would break Nickel's own sleep toggle. */
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        fprintf(stderr, "power_key_open: open %s failed errno=%d\n", path, errno);
    return fd;
}

int power_key_drain_pressed(int fd) {
    if (fd < 0) return 0;
    struct input_event ie;
    int pressed = 0;
    int guard   = 0;
    while (read(fd, &ie, sizeof ie) == (ssize_t)sizeof ie) {
        if (ie.type == EV_KEY && ie.code == KEY_POWER && ie.value != 0)
            pressed = 1;
        if (++guard > 4096) break;   /* never spin forever on an event flood */
    }
    return pressed;
}

void input_set_screen(int fb_w, int fb_h) {
    g_tt.fb_w = fb_w;
    g_tt.fb_h = fb_h;
    g_tt.have_screen = 1;

    /* Manual override via environment variable */
    const char *env = getenv("KOBO_WEATHER_TOUCH_ROTATION");
    if (env && env[0] >= '0' && env[0] <= '3') {
        g_tt.rotation = env[0] - '0';
        if (g_debug)
            fprintf(stderr, "Touch: max=(%d,%d) fb=(%d,%d) rotation=%d (manual)\n",
                    g_tt.max_x, g_tt.max_y, fb_w, fb_h, g_tt.rotation);
        return;
    }

    /* Autodetect: if panel fits portrait orientation, no rotation needed */
    int portrait = (g_tt.max_x < g_tt.max_y);
    int fb_portrait = (fb_w < fb_h);
    if (portrait == fb_portrait) {
        g_tt.rotation = 0;
    } else {
        /* Landscape panel driving a portrait display — the normal Kobo case */
        g_tt.rotation = 1;
    }
    if (g_debug)
        fprintf(stderr, "Touch: max=(%d,%d) fb=(%d,%d) rotation=%d (auto)\n",
                g_tt.max_x, g_tt.max_y, fb_w, fb_h, g_tt.rotation);
}

static void apply_transform(TouchEvent *ev) {
    if (!g_tt.have_screen || g_tt.max_x <= 0 || g_tt.max_y <= 0
        || g_tt.fb_w <= 0 || g_tt.fb_h <= 0) return;
    int x = ev->x, y = ev->y;
    switch (g_tt.rotation) {
    case 0:
        ev->x = (int)((long)x * g_tt.fb_w / g_tt.max_x);
        ev->y = (int)((long)y * g_tt.fb_h / g_tt.max_y);
        break;
    case 1: /* swap axes, mirror Y */
        ev->x = (int)((long)(g_tt.max_y - y) * g_tt.fb_w / g_tt.max_y);
        ev->y = (int)((long)x               * g_tt.fb_h / g_tt.max_x);
        break;
    case 2: /* 180 deg */
        ev->x = (int)((long)(g_tt.max_x - x) * g_tt.fb_w / g_tt.max_x);
        ev->y = (int)((long)(g_tt.max_y - y) * g_tt.fb_h / g_tt.max_y);
        break;
    case 3: /* swap axes, mirror X */
        ev->x = (int)((long)y               * g_tt.fb_w / g_tt.max_y);
        ev->y = (int)((long)(g_tt.max_x - x) * g_tt.fb_h / g_tt.max_x);
        break;
    }
    /* Clamp: a raw value at max would map to fb_w/fb_h, one past the last pixel. */
    if (ev->x < 0)             ev->x = 0;
    else if (ev->x >= g_tt.fb_w) ev->x = g_tt.fb_w - 1;
    if (ev->y < 0)             ev->y = 0;
    else if (ev->y >= g_tt.fb_h) ev->y = g_tt.fb_h - 1;
}

uint64_t input_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
}

/* Returns 0 if an event is ready, 1 if EAGAIN (no data), -1 on error. */
static int do_read(int fd, TouchEvent *ev) {
    struct input_event ie;

    while (1) {
        /* Return any pending event from previous SYN_REPORT */
        if (g_mt.pending) {
            *ev = g_mt.pending_ev;
            g_mt.pending = 0;
            return 0;
        }

        ssize_t n = read(fd, &ie, sizeof(ie));
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 1;
        if (n != (ssize_t)sizeof(ie)) return -1;

        if (ie.type == EV_ABS) {
            if (ie.code == ABS_MT_SLOT) {
                if (ie.value >= 0 && ie.value < MAX_SLOTS)
                    g_mt.cur_slot = ie.value;
            } else if (ie.code == ABS_MT_TRACKING_ID) {
                if (g_mt.cur_slot < MAX_SLOTS) {
                    MTSlot *sl = &g_mt.slots[g_mt.cur_slot];
                    if (ie.value == -1) {
                        /* finger lifted via MT protocol */
                        if (sl->active) {
                            g_mt.pending_ev.type = EV_TOUCH_UP;
                            g_mt.pending_ev.x    = sl->x;
                            g_mt.pending_ev.y    = sl->y;
                            g_mt.pending = 1;
                        }
                        sl->active      = 0;
                        sl->tracking_id = -1;
                    } else {
                        /* Only synthesize UP if not already active — some panels
                         * re-assert TRACKING_ID every report, which would chop drags. */
                        int was_active = sl->active;
                        sl->tracking_id = ie.value;
                        sl->active      = 1;
                        if (!was_active && g_simple.touching && !g_mt.pending) {
                            g_mt.pending_ev.type = EV_TOUCH_UP;
                            g_mt.pending_ev.x    = sl->x;
                            g_mt.pending_ev.y    = sl->y;
                            g_mt.pending = 1;
                        }
                    }
                }
            } else if (ie.code == ABS_MT_POSITION_X) {
                if (g_mt.cur_slot < MAX_SLOTS)
                    g_mt.slots[g_mt.cur_slot].x = ie.value;
            } else if (ie.code == ABS_MT_POSITION_Y) {
                if (g_mt.cur_slot < MAX_SLOTS)
                    g_mt.slots[g_mt.cur_slot].y = ie.value;
            }
        } else if (ie.type == EV_KEY && ie.code == BTN_TOUCH && ie.value == 0) {
            /* Some panels signal lift via BTN_TOUCH=0 instead of TRACKING_ID=-1. */
            int had_active = 0;
            for (int i = 0; i < MAX_SLOTS; i++) {
                if (g_mt.slots[i].active) {
                    had_active = 1;
                    if (!g_mt.pending) {
                        g_mt.pending_ev.type = EV_TOUCH_UP;
                        g_mt.pending_ev.x    = g_mt.slots[i].x;
                        g_mt.pending_ev.y    = g_mt.slots[i].y;
                        g_mt.pending = 1;
                    }
                    g_mt.slots[i].active      = 0;
                    g_mt.slots[i].tracking_id = -1;
                    break;
                }
            }
            /* Only clear touching here if no UP was queued — otherwise
             * translate_event would drop the pending UP (touching=0 check fails). */
            if (!had_active)
                g_simple.touching = 0;
        } else if (ie.type == EV_SYN && ie.code == SYN_REPORT) {
            /* Emit MOVE for any active slot (use slot 0 for single touch) */
            for (int i = 0; i < MAX_SLOTS; i++) {
                if (g_mt.slots[i].active && g_mt.slots[i].tracking_id >= 0) {
                    if (!g_mt.pending) {
                        g_mt.pending_ev.type = EV_TOUCH_MOVE;
                        g_mt.pending_ev.x    = g_mt.slots[i].x;
                        g_mt.pending_ev.y    = g_mt.slots[i].y;
                        g_mt.pending = 1;
                    }
                    break;
                }
            }
        }
    }
}

static int translate_event(TouchEvent *raw, TouchEvent *out) {
    if (raw->type == EV_TOUCH_UP) {
        if (g_simple.touching) {
            g_simple.touching = 0;
            *out = *raw;
            apply_transform(out);
            out->type = EV_TOUCH_UP;
            return 1;
        }
        return 0;
    }
    /* MOVE event */
    if (!g_simple.touching) {
        g_simple.touching = 1;
        *out = *raw;
        apply_transform(out);
        out->type = EV_TOUCH_DOWN;
        return 1;
    }
    *out = *raw;
    apply_transform(out);
    out->type = EV_TOUCH_MOVE;
    return 1;
}

int input_read_nonblock(int fd, TouchEvent *ev) {
    TouchEvent raw;
    int r = do_read(fd, &raw);
    if (r != 0) return r;
    if (translate_event(&raw, ev)) return 0;
    return 1;
}
