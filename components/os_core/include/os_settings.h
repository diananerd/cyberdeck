/*
 * CyberDeck — OS Settings: typed in-memory cache over NVS.
 *
 * Wraps svc_settings with a single `cyberdeck_settings_t` struct loaded at
 * boot.  Reads are O(1) struct field accesses — no NVS roundtrip.
 * Setters update the cache AND commit to NVS AND post EVT_SETTINGS_CHANGED
 * in one call, replacing the old pattern:
 *
 *   svc_settings_set_brightness(v);
 *   svc_event_post(EVT_SETTINGS_CHANGED, NULL, 0);
 *
 * WiFi per-network credentials (indexed SSID/pass) are NOT cached here —
 * use svc_settings_wifi_get/set directly.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * All user-configurable settings in one struct.
 * Read via os_settings_get()->field.
 */
typedef struct {
    /* Display */
    uint8_t  brightness;       /* 0–100, default 80 */
    uint8_t  theme;            /* cyberdeck_theme_id_t: 0=green, 1=amber, 2=neon */
    uint16_t screen_timeout;   /* seconds, 0=never, default 120 */
    uint8_t  rotation;         /* 0=landscape, 1=portrait */
    /* Audio */
    uint8_t  volume;           /* 0–100, default 50 */
    char     bt_paired[18];    /* "XX:XX:XX:XX:XX:XX\0" or empty */
    /* Security */
    bool     pin_enabled;
    uint32_t pin_hash;
    /* System */
    int8_t   tz_offset;        /* hours from UTC, default 0 */
    uint32_t boot_count;       /* incremented each boot */
    char     ota_url[128];
    /* WiFi (only the auto-connect index; full network records stay in NVS indexed) */
    uint8_t  wifi_auto_idx;
    /* Bluesky */
    char     bsky_handle[64];
    char     bsky_did[64];
    char     bsky_app_pw[64];  /* app-password — stored in NVS, not shown in UI */
} cyberdeck_settings_t;

/**
 * Load all settings from NVS into the in-memory cache.
 * Call once after svc_settings_init() during boot.
 */
esp_err_t os_settings_init(void);

/**
 * Return a const pointer to the cached settings struct.
 * Safe to call from any task — no mutex needed for reads.
 */
const cyberdeck_settings_t *os_settings_get(void);

/**
 * Re-read all settings from NVS into the cache.
 * Normally not needed (setters keep cache in sync), but useful if NVS
 * was modified outside of this module (e.g. factory reset).
 */
void os_settings_reload(void);

/* ---- Typed setters ----
 * Each setter: updates cache field + commits to NVS + posts EVT_SETTINGS_CHANGED.
 */

/* Display */
esp_err_t os_settings_set_brightness(uint8_t v);
esp_err_t os_settings_set_theme(uint8_t v);
esp_err_t os_settings_set_screen_timeout(uint16_t v);
esp_err_t os_settings_set_rotation(uint8_t v);

/* Audio */
esp_err_t os_settings_set_volume(uint8_t v);
esp_err_t os_settings_set_bt_paired(const char *addr);

/* Security */
esp_err_t os_settings_set_pin_enabled(bool v);
esp_err_t os_settings_set_pin_hash(uint32_t v);

/* System */
esp_err_t os_settings_set_tz_offset(int8_t v);
esp_err_t os_settings_set_ota_url(const char *url);

/* WiFi */
esp_err_t os_settings_set_wifi_auto_idx(uint8_t v);

/* Bluesky */
esp_err_t os_settings_set_bsky_handle(const char *h);
esp_err_t os_settings_set_bsky_did(const char *did);
esp_err_t os_settings_set_bsky_app_pw(const char *pw);

#ifdef __cplusplus
}
#endif
