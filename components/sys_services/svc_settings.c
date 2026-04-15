/*
 * CyberDeck — NVS Settings service
 * Persistent key-value storage for all cyberdeck configuration.
 */

#include "svc_settings.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "svc_settings";

static nvs_handle_t s_nvs = 0;

esp_err_t svc_settings_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition issue, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ret = nvs_open(SVC_SETTINGS_NAMESPACE, NVS_READWRITE, &s_nvs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Increment boot count */
    svc_settings_inc_boot_count();

    ESP_LOGI(TAG, "Settings initialized");
    return ESP_OK;
}

/* ========== Helpers ========== */

static esp_err_t get_u8(const char *key, uint8_t *val, uint8_t def)
{
    esp_err_t ret = nvs_get_u8(s_nvs, key, val);
    if (ret == ESP_ERR_NVS_NOT_FOUND) { *val = def; return ESP_OK; }
    return ret;
}

static esp_err_t set_u8(const char *key, uint8_t val)
{
    esp_err_t ret = nvs_set_u8(s_nvs, key, val);
    if (ret == ESP_OK) ret = nvs_commit(s_nvs);
    return ret;
}

static esp_err_t get_u16(const char *key, uint16_t *val, uint16_t def)
{
    esp_err_t ret = nvs_get_u16(s_nvs, key, val);
    if (ret == ESP_ERR_NVS_NOT_FOUND) { *val = def; return ESP_OK; }
    return ret;
}

static esp_err_t set_u16(const char *key, uint16_t val)
{
    esp_err_t ret = nvs_set_u16(s_nvs, key, val);
    if (ret == ESP_OK) ret = nvs_commit(s_nvs);
    return ret;
}

static esp_err_t get_u32(const char *key, uint32_t *val, uint32_t def)
{
    esp_err_t ret = nvs_get_u32(s_nvs, key, val);
    if (ret == ESP_ERR_NVS_NOT_FOUND) { *val = def; return ESP_OK; }
    return ret;
}

static esp_err_t set_u32(const char *key, uint32_t val)
{
    esp_err_t ret = nvs_set_u32(s_nvs, key, val);
    if (ret == ESP_OK) ret = nvs_commit(s_nvs);
    return ret;
}

static esp_err_t get_str(const char *key, char *buf, size_t len)
{
    size_t required = len;
    esp_err_t ret = nvs_get_str(s_nvs, key, buf, &required);
    if (ret == ESP_ERR_NVS_NOT_FOUND) { buf[0] = '\0'; return ESP_OK; }
    return ret;
}

static esp_err_t set_str(const char *key, const char *val)
{
    esp_err_t ret = nvs_set_str(s_nvs, key, val);
    if (ret == ESP_OK) ret = nvs_commit(s_nvs);
    return ret;
}

static esp_err_t get_i8(const char *key, int8_t *val, int8_t def)
{
    esp_err_t ret = nvs_get_i8(s_nvs, key, val);
    if (ret == ESP_ERR_NVS_NOT_FOUND) { *val = def; return ESP_OK; }
    return ret;
}

static esp_err_t set_i8(const char *key, int8_t val)
{
    esp_err_t ret = nvs_set_i8(s_nvs, key, val);
    if (ret == ESP_OK) ret = nvs_commit(s_nvs);
    return ret;
}

/* ========== WiFi ========== */

esp_err_t svc_settings_wifi_get(uint8_t index, char *ssid, size_t ssid_len,
                                char *pass, size_t pass_len)
{
    if (index >= SVC_SETTINGS_WIFI_MAX) return ESP_ERR_INVALID_ARG;
    char key[16];
    snprintf(key, sizeof(key), "wifi_ssid_%u", index);
    esp_err_t ret = get_str(key, ssid, ssid_len);
    if (ret != ESP_OK) return ret;
    snprintf(key, sizeof(key), "wifi_pass_%u", index);
    return get_str(key, pass, pass_len);
}

esp_err_t svc_settings_wifi_set(uint8_t index, const char *ssid, const char *pass)
{
    if (index >= SVC_SETTINGS_WIFI_MAX) return ESP_ERR_INVALID_ARG;
    char key[16];
    snprintf(key, sizeof(key), "wifi_ssid_%u", index);
    esp_err_t ret = set_str(key, ssid);
    if (ret != ESP_OK) return ret;
    snprintf(key, sizeof(key), "wifi_pass_%u", index);
    return set_str(key, pass);
}

esp_err_t svc_settings_wifi_get_auto_idx(uint8_t *idx)
{
    return get_u8("wifi_auto_idx", idx, 0);
}

esp_err_t svc_settings_wifi_set_auto_idx(uint8_t idx)
{
    return set_u8("wifi_auto_idx", idx);
}

/* ========== Display ========== */

esp_err_t svc_settings_get_brightness(uint8_t *val)  { return get_u8("brightness", val, 80); }
esp_err_t svc_settings_set_brightness(uint8_t val)   { return set_u8("brightness", val); }
esp_err_t svc_settings_get_theme(uint8_t *val)       { return get_u8("theme", val, 0); }
esp_err_t svc_settings_set_theme(uint8_t val)        { return set_u8("theme", val); }
esp_err_t svc_settings_get_screen_timeout(uint16_t *val) { return get_u16("scr_timeout", val, 120); }
esp_err_t svc_settings_set_screen_timeout(uint16_t val)  { return set_u16("scr_timeout", val); }
esp_err_t svc_settings_get_rotation(uint8_t *val)        { return get_u8("rotation", val, 0); }
esp_err_t svc_settings_set_rotation(uint8_t val)         { return set_u8("rotation", val); }

/* ========== Audio ========== */

esp_err_t svc_settings_get_volume(uint8_t *val)      { return get_u8("volume", val, 50); }
esp_err_t svc_settings_set_volume(uint8_t val)       { return set_u8("volume", val); }
esp_err_t svc_settings_get_bt_paired(char *addr, size_t len) { return get_str("bt_paired", addr, len); }
esp_err_t svc_settings_set_bt_paired(const char *addr)      { return set_str("bt_paired", addr); }

/* ========== Security ========== */

esp_err_t svc_settings_get_pin_enabled(bool *val)
{
    uint8_t v;
    esp_err_t ret = get_u8("pin_enabled", &v, 0);
    *val = (v != 0);
    return ret;
}

esp_err_t svc_settings_set_pin_enabled(bool val)     { return set_u8("pin_enabled", val ? 1 : 0); }
esp_err_t svc_settings_get_pin_hash(uint32_t *val)   { return get_u32("pin_hash", val, 0); }
esp_err_t svc_settings_set_pin_hash(uint32_t val)    { return set_u32("pin_hash", val); }

/* ========== Bluesky ========== */

esp_err_t svc_settings_get_bsky_handle(char *buf, size_t len) { return get_str("bsky_handle", buf, len); }
esp_err_t svc_settings_set_bsky_handle(const char *h)         { return set_str("bsky_handle", h); }
esp_err_t svc_settings_get_bsky_app_pw(char *buf, size_t len) { return get_str("bsky_app_pw", buf, len); }
esp_err_t svc_settings_set_bsky_app_pw(const char *pw)        { return set_str("bsky_app_pw", pw); }
esp_err_t svc_settings_get_bsky_did(char *buf, size_t len)    { return get_str("bsky_did", buf, len); }
esp_err_t svc_settings_set_bsky_did(const char *did)          { return set_str("bsky_did", did); }

/* ========== System ========== */

esp_err_t svc_settings_get_ota_url(char *buf, size_t len)  { return get_str("ota_url", buf, len); }
esp_err_t svc_settings_set_ota_url(const char *url)        { return set_str("ota_url", url); }
esp_err_t svc_settings_get_tz_offset(int8_t *val)          { return get_i8("tz_offset", val, 0); }
esp_err_t svc_settings_set_tz_offset(int8_t val)           { return set_i8("tz_offset", val); }
esp_err_t svc_settings_get_boot_count(uint32_t *val)       { return get_u32("boot_count", val, 0); }

esp_err_t svc_settings_inc_boot_count(void)
{
    uint32_t count = 0;
    get_u32("boot_count", &count, 0);
    count++;
    ESP_LOGI(TAG, "Boot count: %lu", (unsigned long)count);
    return set_u32("boot_count", count);
}
