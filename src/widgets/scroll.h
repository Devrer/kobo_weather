#pragma once

typedef struct {
    int y;          /* current offset (pixels) */
    int max;        /* max allowed y (set by caller) */
    int active;     /* 1 = finger committed to scroll */
    int start_y;    /* finger y at gesture start */
    int start_off;  /* scroll.y at gesture start */
} ScrollState;

void scroll_init(ScrollState *s);
void scroll_start(ScrollState *s, int gesture_y);
/* Update y from the current finger position. Returns 1 if y changed. */
int  scroll_drag(ScrollState *s, int touch_y);
void scroll_release(ScrollState *s);
