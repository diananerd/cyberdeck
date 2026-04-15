/*
 * S3 Cyber-Deck — WiFi STA service
 * Manages WiFi connection, scan, auto-reconnect.
 */

#include "svc_wifi.h"
#include "svc_event.h"
#include "svc_settings.h"
#include "ui_statusbar.h"
#include "ui_engine.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include <string.h>

static const char *TAG = "svc_wifi";

static bool s_initialized = false;
static bool s_connected = false;
static char s_ssid[33] = {0};
static int8_t s_rssi = 0;
static esp_netif_t *s_netif = NULL;

/* Retry logic */
#define WIFI_MAX_RETRY  5
static int s_retry_count = 0;

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "STA started");
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            s_connected = false;
            s_rssi = 0;
            svc_event_post(EVT_WIFI_DISCONNECTED, NULL, 0);
            if (ui_lock(100)) {
                ui_statusbar_set_wifi(false, 0);
                ui_unlock();
            }
            if (s_retry_count < WIFI_MAX_RETRY) {
                s_retry_count++;
                ESP_LOGI(TAG, "Reconnecting (attempt %d/%d)", s_retry_count, WIFI_MAX_RETRY);
                esp_wifi_connect();
            } else {
                ESP_LOGW(TAG, "Max retries reached, stopping reconnect");
            }
            break;
        case WIFI_EVENT_SCAN_DONE:
            svc_event_post(EVT_WIFI_SCAN_DONE, NULL, 0);
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_connected = true;
        s_retry_count = 0;

        /* Get RSSI */
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            s_rssi = ap.rssi;
            memcpy(s_ssid, ap.ssid, sizeof(s_ssid));
        }

        svc_event_post(EVT_WIFI_CONNECTED, NULL, 0);
        if (ui_lock(100)) {
            ui_statusbar_set_wifi(true, s_rssi);
            ui_unlock();
        }
    }
}

esp_err_t svc_wifi_init(void)
{
    if (s_initialized) return ESP_OK;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_initialized = true;
    ESP_LOGI(TAG, "WiFi STA initialized");
    return ESP_OK;
}

esp_err_t svc_wifi_connect(const char *ssid, const char *pass)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    if (pass && pass[0]) {
        strncpy((char *)wifi_cfg.sta.password, pass, sizeof(wifi_cfg.sta.password) - 1);
    }
    wifi_cfg.sta.threshold.authmode = pass && pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    s_retry_count = 0;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_LOGI(TAG, "Connecting to: %s", ssid);
    return esp_wifi_connect();
}

esp_err_t svc_wifi_disconnect(void)
{
    s_retry_count = WIFI_MAX_RETRY; /* prevent auto-reconnect */
    return esp_wifi_disconnect();
}

esp_err_t svc_wifi_auto_connect(void)
{
    uint8_t idx = 0;
    svc_settings_wifi_get_auto_idx(&idx);

    char ssid[33] = {0};
    char pass[65] = {0};
    esp_err_t ret = svc_settings_wifi_get(idx, ssid, sizeof(ssid),
                                           pass, sizeof(pass));
    if (ret != ESP_OK || ssid[0] == '\0') {
        ESP_LOGW(TAG, "No saved WiFi credentials at index %u", idx);
        return ESP_ERR_NOT_FOUND;
    }

    return svc_wifi_connect(ssid, pass);
}

esp_err_t svc_wifi_start_scan(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    wifi_scan_config_t scan_cfg = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    return esp_wifi_scan_start(&scan_cfg, false);
}

esp_err_t svc_wifi_get_scan_results(wifi_ap_record_t *results, uint16_t *count)
{
    return esp_wifi_scan_get_ap_records(count, results);
}

bool svc_wifi_is_connected(void)     { return s_connected; }
int8_t svc_wifi_get_rssi(void)       { return s_rssi; }

esp_err_t svc_wifi_get_ssid(char *buf, size_t len)
{
    if (!buf || len == 0) return ESP_ERR_INVALID_ARG;
    strncpy(buf, s_ssid, len - 1);
    buf[len - 1] = '\0';
    return ESP_OK;
}
