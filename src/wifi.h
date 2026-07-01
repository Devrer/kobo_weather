#ifndef WIFI_H
#define WIFI_H

enum {
    WIFI_STAGE_MODULE       = 1,
    WIFI_STAGE_SCANNING     = 2,
    WIFI_STAGE_CONNECTING   = 3,
    WIFI_STAGE_DHCP         = 4,
    WIFI_STAGE_CONNECTED    = 5,
    WIFI_STAGE_FAILED       = 6,
    /* WiFi off and Nickel hasn't initialised the MTK subsystem this boot;
     * user must toggle WiFi once in Nickel. Used instead of WIFI_STAGE_FAILED. */
    WIFI_STAGE_NEEDS_NICKEL = 7
};

/* Per-WiFi-stage callback; ctx is caller-supplied (e.g. UIState* or FetchManager*). */
typedef void (*WifiStageFn)(void *ctx, int stage);

/* Detect the WiFi interface name (called once at startup or kiosk entry). */
void wifi_detect(void);

/* Bring WiFi up (calls wifi_connect_staged with no callback). */
void wifi_up(void);

/* Like wifi_connect_staged but caps WPA-association polling at `wpa_retries`
 * seconds, so a scheduled kiosk refresh can't pin the thread. on_stage may be NULL. */
int wifi_up_bounded(int wpa_retries, WifiStageFn on_stage, void *ctx);

/* Tear WiFi down (ifconfig + kill wpa/dhcp). */
void wifi_down(void);

/* True if the interface currently has an IPv4 address, regardless of who raised it. */
int wifi_is_associated(void);

/* Tear WiFi down and power off the RF transceiver. Never unloads the kernel
 * module (Nickel/the system owns it; rmmod-ing it would be unsafe). */
void wifi_module_off(void);

/* Connect WiFi with per-stage callbacks.  on_stage(ctx, stage) is called as
 * each stage begins; pass NULL if not needed.  Returns WIFI_STAGE_CONNECTED on
 * success or WIFI_STAGE_FAILED on error. */
int wifi_connect_staged(WifiStageFn on_stage, void *ctx);

#endif /* WIFI_H */
