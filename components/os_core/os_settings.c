/*
 * CyberDeck — OS Settings: typed in-memory cache over NVS.
 *
 * All reads go through the struct (no NVS per-read roundtrip).
 * All writes go through the typed setters (cache + NVS + event).
 */

#include "os_settings.h"
#include "svc_settings.h"
#include "svc_event.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "os_settings";

static cyberdeck_settings_t s_cfg;

/* ================================================================
 * Internal load
 * ================================================================ */

void os_settings_reload(void)
{
    svc_settings_get_brightness(&s_cfg.brightness);
    svc_settings_get_theme(&s_cfg.theme);
    svc_settings_get_screen_timeout(&s_cfg.screen_timeout);
    svc_settings_get_rotation(&s_cfg.rotation);
    svc_settings_get_volume(&s_cfg.volume);
    svc_settings_get_bt_paired(s_cfg.bt_paired, sizeof(s_cfg.bt_paired));
    svc_settings_get_pin_enabled(&s_cfg.pin_enabled);
    svc_settings_get_pin_hash(&s_cfg.pin_hash);
    svc_settings_get_tz_offset(&s_cfg.tz_offset);
    svc_settings_get_boot_count(&s_cfg.boot_count);
    svc_settings_get_ota_url(s_cfg.ota_url, sizeof(s_cfg.ota_url));
    svc_settings_wifi_get_auto_idx(&s_cfg.wifi_auto_idx);
    svc_settings_get_bsky_handle(s_cfg.bsky_handle, sizeof(s_cfg.bsky_handle));
    svc_settings_get_bsky_did(s_cfg.bsky_did, sizeof(s_cfg.bsky_did));
    svc_settings_get_bsky_app_pw(s_cfg.bsky_app_pw, sizeof(s_cfg.bsky_app_pw));
    ESP_LOGD(TAG, "Settings cache reloaded");
}

/* ================================================================
 * Public API
 * ================================================================ */

esp_err_t os_settings_init(void)
{
    os_settings_reload();
    ESP_LOGI(TAG, "Settings cache ready (brightness=%u theme=%u rotation=%u)",
             s_cfg.brightness, s_cfg.theme, s_cfg.rotation);
    return ESP_OK;
}

const cyberdeck_settings_t *os_settings_get(void)
{
    return &s_cfg;
}

/* ================================================================
 * Typed setters — update cache + NVS + post event
 * ================================================================ */

static void post_changed(void)
{
    svc_event_post(EVT_SETTINGS_CHANGED, NULL, 0);
}

esp_err_t os_settings_set_brightness(uint8_t v)
{
    esp_err_t ret = svc_settings_set_brightness(v);
    if (ret == ESP_OK) { s_cfg.brightness = v; post_changed(); }
    return ret;
}

esp_err_t os_settings_set_theme(uint8_t v)
{
    esp_err_t ret = svc_settings_set_theme(v);
    if (ret == ESP_OK) { s_cfg.theme = v; post_changed(); }
    return ret;
}

esp_err_t os_settings_set_screen_timeout(uint16_t v)
{
    esp_err_t ret = svc_settings_set_screen_timeout(v);
    if (ret == ESP_OK) { s_cfg.screen_timeout = v; post_changed(); }
    return ret;
}

esp_err_t os_settings_set_rotation(uint8_t v)
{
    esp_err_t ret = svc_settings_set_rotation(v);
    if (ret == ESP_OK) { s_cfg.rotation = v; post_changed(); }
    return ret;
}

esp_err_t os_settings_set_volume(uint8_t v)
{
    esp_err_t ret = svc_settings_set_volume(v);
    if (ret == ESP_OK) { s_cfg.volume = v; post_changed(); }
    return ret;
}

esp_err_t os_settings_set_bt_paired(const char *addr)
{
    if (!addr) return ESP_ERR_INVALID_ARG;
    esp_err_t ret = svc_settings_set_bt_paired(addr);
    if (ret == ESP_OK) {
        strncpy(s_cfg.bt_paired, addr, sizeof(s_cfg.bt_paired) - 1);
        s_cfg.bt_paired[sizeof(s_cfg.bt_paired) - 1] = '\0';
        post_changed();
    }
    return ret;
}

esp_err_t os_settings_set_pin_enabled(bool v)
{
    esp_err_t ret = svc_settings_set_pin_enabled(v);
    if (ret == ESP_OK) { s_cfg.pin_enabled = v; post_changed(); }
    return ret;
}

esp_err_t os_settings_set_pin_hash(uint32_t v)
{
    esp_err_t ret = svc_settings_set_pin_hash(v);
    if (ret == ESP_OK) { s_cfg.pin_hash = v; post_changed(); }
    return ret;
}

esp_err_t os_settings_set_tz_offset(int8_t v)
{
    esp_err_t ret = svc_settings_set_tz_offset(v);
    if (ret == ESP_OK) { s_cfg.tz_offset = v; post_changed(); }
    return ret;
}

esp_err_t os_settings_set_ota_url(const char *url)
{
    if (!url) return ESP_ERR_INVALID_ARG;
    esp_err_t ret = svc_settings_set_ota_url(url);
    if (ret == ESP_OK) {
        strncpy(s_cfg.ota_url, url, sizeof(s_cfg.ota_url) - 1);
        s_cfg.ota_url[sizeof(s_cfg.ota_url) - 1] = '\0';
        post_changed();
    }
    return ret;
}

esp_err_t os_settings_set_wifi_auto_idx(uint8_t v)
{
    esp_err_t ret = svc_settings_wifi_set_auto_idx(v);
    if (ret == ESP_OK) { s_cfg.wifi_auto_idx = v; post_changed(); }
    return ret;
}

esp_err_t os_settings_set_bsky_handle(const char *h)
{
    if (!h) return ESP_ERR_INVALID_ARG;
    esp_err_t ret = svc_settings_set_bsky_handle(h);
    if (ret == ESP_OK) {
        strncpy(s_cfg.bsky_handle, h, sizeof(s_cfg.bsky_handle) - 1);
        s_cfg.bsky_handle[sizeof(s_cfg.bsky_handle) - 1] = '\0';
        post_changed();
    }
    return ret;
}

esp_err_t os_settings_set_bsky_did(const char *did)
{
    if (!did) return ESP_ERR_INVALID_ARG;
    esp_err_t ret = svc_settings_set_bsky_did(did);
    if (ret == ESP_OK) {
        strncpy(s_cfg.bsky_did, did, sizeof(s_cfg.bsky_did) - 1);
        s_cfg.bsky_did[sizeof(s_cfg.bsky_did) - 1] = '\0';
        post_changed();
    }
    return ret;
}

esp_err_t os_settings_set_bsky_app_pw(const char *pw)
{
    if (!pw) return ESP_ERR_INVALID_ARG;
    esp_err_t ret = svc_settings_set_bsky_app_pw(pw);
    if (ret == ESP_OK) {
        strncpy(s_cfg.bsky_app_pw, pw, sizeof(s_cfg.bsky_app_pw) - 1);
        s_cfg.bsky_app_pw[sizeof(s_cfg.bsky_app_pw) - 1] = '\0';
        post_changed();
    }
    return ret;
}
