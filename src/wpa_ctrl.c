#include "wpa_ctrl.h"
#include "sysutil.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

extern bool g_debug;

#define wlog(...) sysutil_log("WPA ", false, __VA_ARGS__)

struct WpaCtrl {
    int  fd;
    char local_path[108];
};

/* Find the ctrl_interface directory in wpa_supplicant.conf, handling both
 * "ctrl_interface=/path" and "ctrl_interface=DIR=/path GROUP=..." forms.
 * Falls back to the upstream default if the conf can't be read or has no
 * ctrl_interface line. */
static void resolve_ctrl_dir(char *out, size_t out_len) {
    snprintf(out, out_len, "/var/run/wpa_supplicant");

    FILE *f = fopen("/etc/wpa_supplicant/wpa_supplicant.conf", "r");
    if (!f) return;

    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "ctrl_interface=", 15) != 0) continue;
        p += 15;
        if (strncmp(p, "DIR=", 4) == 0) p += 4;
        char *end = p;
        while (*end && *end != ' ' && *end != '\t' &&
               *end != '\r' && *end != '\n') end++;
        *end = '\0';
        if (*p) snprintf(out, out_len, "%s", p);
        break;
    }
    fclose(f);
}

WpaCtrl *wpa_ctrl_connect(const char *ifname) {
    char ctrl_dir[200];
    resolve_ctrl_dir(ctrl_dir, sizeof ctrl_dir);

    struct sockaddr_un remote = { .sun_family = AF_UNIX };
    if (snprintf(remote.sun_path, sizeof remote.sun_path, "%s/%s",
                 ctrl_dir, ifname) >= (int)sizeof remote.sun_path)
        return NULL;

    WpaCtrl *c = calloc(1, sizeof *c);
    if (!c) return NULL;

    c->fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (c->fd < 0) { free(c); return NULL; }

    /* Unique per-pid path on tmpfs so the kernel can route replies back to us;
     * an unclean exit just leaves a harmless file that's gone on reboot. */
    snprintf(c->local_path, sizeof c->local_path,
             "/tmp/kobo_wpa_ctrl-%d", (int)getpid());
    struct sockaddr_un local = { .sun_family = AF_UNIX };
    snprintf(local.sun_path, sizeof local.sun_path, "%s", c->local_path);
    unlink(c->local_path);
    if (bind(c->fd, (struct sockaddr *)&local, sizeof local) < 0) {
        close(c->fd);
        free(c);
        return NULL;
    }

    if (connect(c->fd, (struct sockaddr *)&remote, sizeof remote) < 0) {
        wlog("connect %s failed errno=%d (%s)",
             remote.sun_path, errno, strerror(errno));
        close(c->fd);
        unlink(c->local_path);
        free(c);
        return NULL;
    }

    wlog("connected %s", remote.sun_path);
    return c;
}

void wpa_ctrl_disconnect(WpaCtrl *c) {
    if (!c) return;
    close(c->fd);
    unlink(c->local_path);
    free(c);
}

int wpa_ctrl_command(WpaCtrl *c, const char *cmd, char *reply, size_t reply_len) {
    if (!c || reply_len == 0) return -1;

    if (send(c->fd, cmd, strlen(cmd), 0) < 0) {
        wlog("send '%s' failed errno=%d (%s)", cmd, errno, strerror(errno));
        return -1;
    }

    struct pollfd pfd = { .fd = c->fd, .events = POLLIN };
    int pr = poll(&pfd, 1, 2000);
    if (pr == 0) {
        wlog("cmd '%s' timed out", cmd);
        return -1;
    }
    if (pr < 0) {
        wlog("poll failed errno=%d (%s)", errno, strerror(errno));
        return -1;
    }

    ssize_t n = recv(c->fd, reply, reply_len - 1, 0);
    if (n < 0) {
        wlog("recv '%s' failed errno=%d (%s)", cmd, errno, strerror(errno));
        return -1;
    }
    reply[n] = '\0';
    return 0;
}
