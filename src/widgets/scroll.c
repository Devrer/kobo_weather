#include "scroll.h"
#include <string.h>

void scroll_init(ScrollState *s) {
    memset(s, 0, sizeof(*s));
}

void scroll_start(ScrollState *s, int gesture_y) {
    s->start_y   = gesture_y;
    s->start_off = s->y;
    s->active    = 1;
}

int scroll_drag(ScrollState *s, int touch_y) {
    int new_y = s->start_off + (s->start_y - touch_y);
    if (new_y < 0)      new_y = 0;
    if (new_y > s->max) new_y = s->max;
    int changed = (new_y != s->y);
    s->y = new_y;
    return changed;
}

void scroll_release(ScrollState *s) {
    s->active = 0;
}
