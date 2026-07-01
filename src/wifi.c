#include "wifi.h"
#include "fb.h"   /* g_debug */
#include "sysutil.h"
#include "wpa_ctrl.h"

#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>

extern bool g_debug;

#define wlog(...) sysutil_log("WIFI ", false, __VA_ARGS__)

/* ---- Interface detection ------------------------------------------ */

static char g_wifi_if[16] = "wlan0";

static int is_wireless(const char *ifname) {
    char p[64];
    struct stat st;
    snprintf(p, sizeof p, "/sys/class/net/%.15s/wireless", ifname);
    if (stat(p, &st) == 0) return 1;
    snprintf(p, sizeof p, "/sys/class/net/%.15s/phy80211", ifname);
    if (lstat(p, &st) == 0) return 1;
    return 0;
}

static int is_virtual_if(const char *n) {
    return n[0] == '.' || !strcmp(n, "lo") ||
           !strncmp(n, "usb",  3) || !strncmp(n, "rndis", 5) ||
           !strncmp(n, "tunl", 4) || !strncmp(n, "sit",   3) ||
           !strncmp(n, "gre",  3) || !strncmp(n, "ip6",   3) ||
           !strncmp(n, "dummy",5) || !strncmp(n, "bond",  4) ||
           !strncmp(n, "ifb",  3) || !strncmp(n, "teql",  4);
}

static int wireless_iface_present(void) {
    DIR *d = opendir("/sys/class/net");
    if (!d) return 0;
    struct dirent *e;
    int found = 0;
    while ((e = readdir(d))) {
        if (!is_virtual_if(e->d_name) && is_wireless(e->d_name)) {
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
}

void wifi_detect(void) {
    DIR *d = opendir("/sys/class/net");
    if (!d) {
        { time_t _t = time(NULL); struct tm _tm; localtime_r(&_t, &_tm);
          fprintf(stderr, "%02d:%02d:%02d DIAG: WIFI opendir /sys/class/net errno=%d - using %s\n",
                  _tm.tm_hour, _tm.tm_min, _tm.tm_sec, errno, g_wifi_if); }
        return;
    }
    char fallback[16] = "";
    struct dirent *e;
    int found = 0;
    while ((e = readdir(d))) {
        const char *n = e->d_name;
        if (is_virtual_if(n)) continue;
        if (is_wireless(n)) {
            snprintf(g_wifi_if, sizeof g_wifi_if, "%.15s", n);
            found = 1;
            break;
        }
        if (!fallback[0])
            snprintf(fallback, sizeof fallback, "%.15s", n);
    }
    closedir(d);
    if (!found && fallback[0])
        snprintf(g_wifi_if, sizeof g_wifi_if, "%s", fallback);
    wlog("interface = %s%s", g_wifi_if, found ? "" : " (fallback)");
}

/* ---- Shell helpers ------------------------------------------------- */

static int runf(int crit, const char *label, const char *fmt, ...) {
    char cmd[1024];
    va_list ap; va_start(ap, fmt); vsnprintf(cmd, sizeof cmd, fmt, ap); va_end(ap);
    int rc = sysutil_runf(cmd);
    if (g_debug)
        wlog("cmd %s exit=%d", label, rc);
    else if (crit && rc != 0)
        sysutil_log("WIFI ", true, "cmd %s exit=%d", label, rc);
    return rc;
}

/* ---- MediaTek WiFi power control ----------------------------------- */

/* Gate: on MTK Kobo, writing /dev/wmtWifi before Nickel has run its first-boot
 * WiFi init (which creates /proc/driver/wmt_dbg) panics the kernel
 * (KOReader issue #13990). Use wmt_dbg's presence as the safety check. */
static int wmt_initialized(void) {
    return access("/proc/driver/wmt_dbg", F_OK) == 0;
}

#define WIFI_IFACE_POLL_RETRIES  30   /* x500 ms = ~15 s */

static void wifi_module_on(void) {
    if (wireless_iface_present()) {
        /* Already up; just re-enable RF if it was powered off by wifi_down(). */
        if (wmt_initialized() && access("/dev/wmtWifi", W_OK) == 0)
            runf(0, "wmtWifi-on", "echo 1 > /dev/wmtWifi 2>/dev/null");
        return;
    }
    /* Never touch /dev/wmtWifi (or insmod) before Nickel inits wmt — panics the kernel. */
    if (!wmt_initialized()) {
        wlog("wmt not initialised by Nickel (no /proc/driver/wmt_dbg) — "
             "cannot enable WiFi safely; enable WiFi once in Nickel after boot");
        return;
    }
    if (access("/dev/wmtWifi", W_OK) != 0) {
        wlog("/dev/wmtWifi not writable — cannot power on WiFi");
        return;
    }
    /* Matches KOReader's MTK power-on sequence. */
    wlog("powering on MTK WiFi (wmt_dbg + wmtWifi)");
    runf(0, "wmt-poweron",
         "echo 0xDB9DB9 > /proc/driver/wmt_dbg 2>/dev/null;"
         "echo '7 9 0'   > /proc/driver/wmt_dbg 2>/dev/null;"
         "sleep 1;"
         "echo 1 > /dev/wmtWifi 2>/dev/null");
    for (int i = 0; i < WIFI_IFACE_POLL_RETRIES; i++) {
        if (wireless_iface_present()) {
            wlog("WiFi interface appeared after %d polls", i + 1);
            return;
        }
        usleep(500000);
    }
    wlog("WiFi interface did not appear after power-on");
}

/* ---- wpa_supplicant state reader ----------------------------------- */

#define WPA_DOWN       0
#define WPA_SCANNING   1
#define WPA_CONNECTING 2
#define WPA_COMPLETED  3

#define WIFI_WPA_POLL_RETRIES    30   /* x1 s   = 30 s  */
#define WIFI_SCAN_STALL_S         8   /* give up if stuck SCANNING/DOWN this long */

/* Lazily opened, reused across polls; reopened on failure. */
static WpaCtrl *g_wpa;

static WpaCtrl *wpa_ctrl(void) {
    if (!g_wpa) g_wpa = wpa_ctrl_connect(g_wifi_if);
    return g_wpa;
}

static void wpa_ctrl_reset(void) {
    wpa_ctrl_disconnect(g_wpa);
    g_wpa = NULL;
}

static int wpa_get_state(void) {
    WpaCtrl *c = wpa_ctrl();
    if (!c) return WPA_DOWN;

    char buf[4096];
    if (wpa_ctrl_command(c, "STATUS", buf, sizeof buf) != 0) {
        wpa_ctrl_reset();
        return WPA_DOWN;
    }

    int result = WPA_DOWN;
    char *line = buf, *nl;
    while (line && *line) {
        nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (strncmp(line, "wpa_state=", 10) == 0) {
            const char *s = line + 10;
            if (strcmp(s, "COMPLETED") == 0)
                result = WPA_COMPLETED;
            else if (strncmp(s, "ASSOCIAT",    8) == 0 ||
                     strncmp(s, "AUTHENTICAT", 11) == 0 ||
                     strncmp(s, "4WAY",         4) == 0 ||
                     strncmp(s, "GROUP",        5) == 0)
                result = WPA_CONNECTING;
            else
                result = WPA_SCANNING;  /* SCANNING / DISCONNECTED / INACTIVE */
            wlog("status wpa_state=%s", s);
            break;
        }
        line = nl ? nl + 1 : NULL;
    }
    return result;
}

/* Return 1 if the interface already has an IPv4 address. */
static int has_ip(void) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return 0;

    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    ifr.ifr_addr.sa_family = AF_INET;
    snprintf(ifr.ifr_name, IFNAMSIZ, "%.15s", g_wifi_if);

    int found = 0;
    if (ioctl(s, SIOCGIFADDR, &ifr) == 0) {
        struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
        found = (sin->sin_addr.s_addr != 0);  /* 0.0.0.0 counts as "no IP" */
    }
    close(s);
    return found;
}

/* True if the conf defines at least one network={...} block, i.e. our own
 * wpa_supplicant can drive an association (creds may instead live only in
 * connman's store, in which case we must not kill connman's supplicant). */
static int wpa_conf_has_networks(void) {
    FILE *f = fopen("/etc/wpa_supplicant/wpa_supplicant.conf", "r");
    if (!f) return 0;
    char line[256];
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "network=", 8) == 0) { found = 1; break; }
    }
    fclose(f);
    return found;
}

/* ---- Public API ---------------------------------------------------- */

int wifi_is_associated(void) {
    return has_ip();
}

void wifi_down(void) {
    wpa_ctrl_reset();   /* never hold a socket across teardown/restart */
    runf(0, "killall-udhcpc", "killall udhcpc 2>/dev/null");
    runf(0, "killall-wpa",    "killall wpa_supplicant 2>/dev/null");
    /* ifconfig down alone doesn't clear the address, so flush first to avoid
     * has_ip() returning a stale true on the next bring-up. */
    runf(0, "ipflush", "ip addr flush dev %.15s 2>/dev/null ||"
                       " ifconfig %.15s 0.0.0.0 2>/dev/null", g_wifi_if, g_wifi_if);
    runf(0, "ifdown",  "ifconfig %.15s down 2>/dev/null", g_wifi_if);
    /* Power off RF, leave module loaded; gated per wmt_initialized() above. */
    if (wmt_initialized() && access("/dev/wmtWifi", W_OK) == 0)
        runf(0, "wmtWifi-off", "echo 0 > /dev/wmtWifi 2>/dev/null");
}

void wifi_module_off(void) {
    /* Never unload the module (Nickel owns it); just power down the RF. */
    wifi_down();
}

static int wifi_connect_impl(WifiStageFn on_stage, void *ctx, int wpa_retries) {
    /* Fast-path: already connected via Nickel/connman — use it, don't touch it. */
    if (has_ip()) {
        wlog("already connected (has IP)");
        if (on_stage) on_stage(ctx, WIFI_STAGE_CONNECTED);
        return WIFI_STAGE_CONNECTED;
    }

    /* No live connection: bring the interface up ourselves. If wmt isn't
     * initialised, wifi_module_on() is a no-op and we fall back to cached data. */
    if (on_stage) on_stage(ctx, WIFI_STAGE_MODULE);
    wifi_module_on();
    wifi_detect();
    if (!wireless_iface_present()) {
        if (!wmt_initialized()) {
            wlog("WiFi off and not initialised by Nickel this boot — "
                 "showing cached data (enable WiFi once in Nickel to refresh)");
            if (on_stage) on_stage(ctx, WIFI_STAGE_NEEDS_NICKEL);
            return WIFI_STAGE_NEEDS_NICKEL;
        }
        wlog("no wireless interface after power-on");
        if (on_stage) on_stage(ctx, WIFI_STAGE_FAILED);
        return WIFI_STAGE_FAILED;
    }
    runf(1, "ifup", "ifconfig %.15s up 2>/dev/null", g_wifi_if);

    /* connmand is SIGSTOPped by the launcher by this point, so its leftover
     * wpa_supplicant can never associate. If our conf has networks, kill it
     * and start our own; otherwise leave it and rely on the stall bail-out below. */
    int own_supplicant = wpa_conf_has_networks();
    wlog("wpa.conf networks=%s wpa_state=%d",
         own_supplicant ? "yes" : "no", wpa_get_state());
    if (own_supplicant) {
        runf(0, "killall-wpa", "killall wpa_supplicant 2>/dev/null");
        runf(1, "wpa_supplicant",
             "wpa_supplicant -B -i %.15s"
             " -c /etc/wpa_supplicant/wpa_supplicant.conf 2>/dev/null",
             g_wifi_if);
        usleep(500000);
        wpa_ctrl_reset();   /* drop any socket to the now-killed supplicant */
        WpaCtrl *c = wpa_ctrl();
        if (c) {
            char buf[64];
            wpa_ctrl_command(c, "SCAN", buf, sizeof buf); /* best-effort */
        }
    }

    /* Stage 2/3: poll wpa_state (SCANNING vs CONNECTING). Bail out early if
     * stuck in SCANNING/DOWN for WIFI_SCAN_STALL_S with no progress, so we
     * don't burn the full timeout when there's no usable network/creds. */
    if (on_stage) on_stage(ctx, WIFI_STAGE_SCANNING);
    int last_emit = WIFI_STAGE_SCANNING;
    int wstate = WPA_DOWN;
    int saw_progress = 0;
    int stuck_for = 0;
    for (int i = 0; i < wpa_retries; i++) {
        wstate = wpa_get_state();
        if (wstate == WPA_COMPLETED) break;
        if (wstate == WPA_CONNECTING) saw_progress = 1;
        if (!saw_progress && (wstate == WPA_SCANNING || wstate == WPA_DOWN)) {
            if (++stuck_for >= WIFI_SCAN_STALL_S) {
                wlog("no association progress after %ds (state=%d) — giving up",
                     stuck_for, wstate);
                break;
            }
        } else {
            stuck_for = 0;
        }
        int cur = (wstate == WPA_CONNECTING) ? WIFI_STAGE_CONNECTING
                                             : WIFI_STAGE_SCANNING;
        if (cur != last_emit) {
            if (on_stage) on_stage(ctx, cur);
            last_emit = cur;
        }
        sleep(1);
    }
    if (wstate != WPA_COMPLETED) {
        wlog("wpa_supplicant failed to associate");
        if (on_stage) on_stage(ctx, WIFI_STAGE_FAILED);
        return WIFI_STAGE_FAILED;
    }

    /* Stage 4: get a DHCP lease. Always run after a fresh WPA association. */
    if (on_stage) on_stage(ctx, WIFI_STAGE_DHCP);
    int rc = runf(1, "udhcpc",
                  "udhcpc -i %.15s"
                  " -s /etc/udhcpc.d/default.script -t15 -T10 -q 2>/dev/null",
                  g_wifi_if);
    if (rc != 0 && !has_ip()) {
        wlog("udhcpc failed");
        if (on_stage) on_stage(ctx, WIFI_STAGE_FAILED);
        return WIFI_STAGE_FAILED;
    }

    wlog("connected");
    if (on_stage) on_stage(ctx, WIFI_STAGE_CONNECTED);
    return WIFI_STAGE_CONNECTED;
}

int wifi_connect_staged(WifiStageFn on_stage, void *ctx) {
    return wifi_connect_impl(on_stage, ctx, WIFI_WPA_POLL_RETRIES);
}

void wifi_up(void) {
    wifi_connect_staged(NULL, NULL);
}

int wifi_up_bounded(int wpa_retries, WifiStageFn on_stage, void *ctx) {
    if (wpa_retries < 1) wpa_retries = 1;
    return wifi_connect_impl(on_stage, ctx, wpa_retries);
}
