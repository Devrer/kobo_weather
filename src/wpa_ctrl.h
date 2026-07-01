#ifndef WPA_CTRL_H
#define WPA_CTRL_H

#include <stddef.h>

typedef struct WpaCtrl WpaCtrl;

/* Connect to wpa_supplicant's control socket for `ifname`; NULL if absent
 * (caller falls back as "wpa down"). */
WpaCtrl *wpa_ctrl_connect(const char *ifname);

void wpa_ctrl_disconnect(WpaCtrl *c);

/* Send `cmd`, copy the NUL-terminated reply into reply[0..reply_len).
 * Returns 0 on success, -1 on timeout/socket error. */
int wpa_ctrl_command(WpaCtrl *c, const char *cmd, char *reply, size_t reply_len);

#endif
