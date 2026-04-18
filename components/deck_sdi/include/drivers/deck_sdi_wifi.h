#pragma once

/* network.wifi — WiFi station capability.
 *
 * DL2 driver. Wraps the underlying ESP-IDF wifi stack behind a
 * platform-neutral vtable so the runtime never touches esp_wifi_*
 * directly. State changes are observable via status() / get_ip().
 *
 * Threading: connect() blocks the caller until either CONNECTED is
 * reached or timeout_ms elapses. scan() blocks until results are
 * delivered (synchronous wrapper around esp_wifi_scan_start).
 *
 * See deck-lang/05-deck-os-api.md and deck-lang/13-deck-cyberdeck-platform.md.
 */

#include "deck_sdi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DECK_SDI_WIFI_SSID_MAX     32
#define DECK_SDI_WIFI_PASS_MAX     64
#define DECK_SDI_WIFI_IP_MAX       16  /* "255.255.255.255\0" */

typedef enum {
    DECK_SDI_WIFI_DISCONNECTED = 0,
    DECK_SDI_WIFI_SCANNING,
    DECK_SDI_WIFI_CONNECTING,
    DECK_SDI_WIFI_CONNECTED,
    DECK_SDI_WIFI_FAILED,
} deck_sdi_wifi_state_t;

typedef enum {
    DECK_SDI_WIFI_AUTH_OPEN = 0,
    DECK_SDI_WIFI_AUTH_WEP,
    DECK_SDI_WIFI_AUTH_WPA,
    DECK_SDI_WIFI_AUTH_WPA2,
    DECK_SDI_WIFI_AUTH_WPA_WPA2,
    DECK_SDI_WIFI_AUTH_WPA3,
    DECK_SDI_WIFI_AUTH_OTHER,
} deck_sdi_wifi_auth_t;

typedef struct {
    char                  ssid[DECK_SDI_WIFI_SSID_MAX + 1];
    int8_t                rssi;
    uint8_t               channel;
    deck_sdi_wifi_auth_t  auth;
} deck_sdi_wifi_ap_t;

typedef bool (*deck_sdi_wifi_scan_cb_t)(const deck_sdi_wifi_ap_t *ap, void *user);

typedef struct {
    /* One-shot init. Must be safe to call once per boot. */
    deck_sdi_err_t (*init)(void *ctx);

    /* Synchronous scan. cb invoked once per AP. cb returns false to stop. */
    deck_sdi_err_t (*scan)(void *ctx,
                           deck_sdi_wifi_scan_cb_t cb, void *user);

    /* Connect with timeout. Blocks until CONNECTED or timeout. */
    deck_sdi_err_t (*connect)(void *ctx, const char *ssid,
                              const char *password, uint32_t timeout_ms);

    /* Disconnect from current AP. Idempotent. */
    deck_sdi_err_t (*disconnect)(void *ctx);

    /* Current state snapshot. */
    deck_sdi_wifi_state_t (*status)(void *ctx);

    /* Copy current IPv4 (e.g. "192.168.1.42") into out_buf.
     * Empty string when not connected. */
    deck_sdi_err_t (*get_ip)(void *ctx, char *out_buf, size_t out_size);

    /* Last RSSI of associated AP. 0 when not connected. */
    int8_t (*rssi)(void *ctx);
} deck_sdi_wifi_vtable_t;

/* ESP-IDF (esp_wifi) implementation. */
deck_sdi_err_t deck_sdi_wifi_register_esp32(void);

/* High-level wrappers. */
deck_sdi_err_t        deck_sdi_wifi_init(void);
deck_sdi_err_t        deck_sdi_wifi_scan(deck_sdi_wifi_scan_cb_t cb, void *user);
deck_sdi_err_t        deck_sdi_wifi_connect(const char *ssid, const char *password,
                                             uint32_t timeout_ms);
deck_sdi_err_t        deck_sdi_wifi_disconnect(void);
deck_sdi_wifi_state_t deck_sdi_wifi_status(void);
deck_sdi_err_t        deck_sdi_wifi_get_ip(char *out_buf, size_t out_size);
int8_t                deck_sdi_wifi_rssi(void);

/* Selftest: init() + state == DISCONNECTED. Does NOT scan or connect
 * (that requires unknowable AP availability). */
deck_sdi_err_t deck_sdi_wifi_selftest(void);

#ifdef __cplusplus
}
#endif
