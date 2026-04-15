#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* NVS namespace */
#define SVC_SETTINGS_NAMESPACE  "cyberdeck"

/* Max stored WiFi networks */
#define SVC_SETTINGS_WIFI_MAX   5

/**
 * @brief Initialize NVS and load defaults.
 */
esp_err_t svc_settings_init(void);

/* ========== WiFi ========== */

esp_err_t svc_settings_wifi_get(uint8_t index, char *ssid, size_t ssid_len,
                                char *pass, size_t pass_len);
esp_err_t svc_settings_wifi_set(uint8_t index, const char *ssid, const char *pass);
esp_err_t svc_settings_wifi_get_auto_idx(uint8_t *idx);
esp_err_t svc_settings_wifi_set_auto_idx(uint8_t idx);

/* ========== Display ========== */

esp_err_t svc_settings_get_brightness(uint8_t *val);
esp_err_t svc_settings_set_brightness(uint8_t val);
esp_err_t svc_settings_get_theme(uint8_t *val);
esp_err_t svc_settings_set_theme(uint8_t val);
esp_err_t svc_settings_get_screen_timeout(uint16_t *val);
esp_err_t svc_settings_set_screen_timeout(uint16_t val);
esp_err_t svc_settings_get_rotation(uint8_t *val);   /* 0=horizontal, 1=vertical */
esp_err_t svc_settings_set_rotation(uint8_t val);

/* ========== Audio ========== */

esp_err_t svc_settings_get_volume(uint8_t *val);
esp_err_t svc_settings_set_volume(uint8_t val);
esp_err_t svc_settings_get_bt_paired(char *addr, size_t len);
esp_err_t svc_settings_set_bt_paired(const char *addr);

/* ========== Security ========== */

esp_err_t svc_settings_get_pin_enabled(bool *val);
esp_err_t svc_settings_set_pin_enabled(bool val);
esp_err_t svc_settings_get_pin_hash(uint32_t *val);
esp_err_t svc_settings_set_pin_hash(uint32_t val);

/* ========== Bluesky ========== */

esp_err_t svc_settings_get_bsky_handle(char *buf, size_t len);
esp_err_t svc_settings_set_bsky_handle(const char *handle);
esp_err_t svc_settings_get_bsky_app_pw(char *buf, size_t len);
esp_err_t svc_settings_set_bsky_app_pw(const char *pw);
esp_err_t svc_settings_get_bsky_did(char *buf, size_t len);
esp_err_t svc_settings_set_bsky_did(const char *did);

/* ========== System ========== */

esp_err_t svc_settings_get_ota_url(char *buf, size_t len);
esp_err_t svc_settings_set_ota_url(const char *url);
esp_err_t svc_settings_get_tz_offset(int8_t *val);
esp_err_t svc_settings_set_tz_offset(int8_t val);
esp_err_t svc_settings_get_boot_count(uint32_t *val);
esp_err_t svc_settings_inc_boot_count(void);

#ifdef __cplusplus
}
#endif
