/*
 * S3 Cyber-Deck — OTA update service
 * HTTPS OTA via esp_https_ota, runs in a dedicated task.
 */

#include "svc_ota.h"
#include "svc_event.h"
#include "svc_settings.h"
#include "os_task.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "svc_ota";

#define OTA_TASK_STACK  (8 * 1024)

static bool s_in_progress = false;
static char s_ota_url[256] = {0};

static void ota_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "OTA update starting: %s", s_ota_url);

    svc_event_post(EVT_OTA_STARTED, NULL, 0);

    esp_http_client_config_t http_cfg = {
        .url = s_ota_url,
        .timeout_ms = 30000,
    };

    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_https_ota_handle_t ota_handle = NULL;
    esp_err_t ret = esp_https_ota_begin(&ota_cfg, &ota_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(ret));
        svc_event_post(EVT_OTA_ERROR, NULL, 0);
        s_in_progress = false;
        vTaskDelete(NULL);
        return;
    }

    int total = esp_https_ota_get_image_size(ota_handle);

    while (1) {
        ret = esp_https_ota_perform(ota_handle);
        if (ret != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;

        /* Post progress */
        if (total > 0) {
            int received = esp_https_ota_get_image_len_read(ota_handle);
            uint8_t pct = (uint8_t)((received * 100) / total);
            svc_event_post(EVT_OTA_PROGRESS, &pct, sizeof(pct));
        }
    }

    if (ret == ESP_OK) {
        ret = esp_https_ota_finish(ota_handle);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "OTA update complete, restarting...");
            svc_event_post(EVT_OTA_COMPLETE, NULL, 0);
            vTaskDelay(pdMS_TO_TICKS(2000)); /* let UI show success */
            esp_restart();
        }
    }

    ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(ret));
    esp_https_ota_abort(ota_handle);
    svc_event_post(EVT_OTA_ERROR, NULL, 0);
    s_in_progress = false;
    vTaskDelete(NULL);
}

esp_err_t svc_ota_init(void)
{
    ESP_LOGI(TAG, "OTA service initialized");
    return ESP_OK;
}

esp_err_t svc_ota_start(const char *url)
{
    if (s_in_progress) {
        ESP_LOGW(TAG, "OTA already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    if (url && url[0]) {
        strncpy(s_ota_url, url, sizeof(s_ota_url) - 1);
    } else {
        esp_err_t ret = svc_settings_get_ota_url(s_ota_url, sizeof(s_ota_url));
        if (ret != ESP_OK || s_ota_url[0] == '\0') {
            ESP_LOGE(TAG, "No OTA URL configured");
            return ESP_ERR_NOT_FOUND;
        }
    }

    s_in_progress = true;

    os_task_config_t cfg = {
        .name       = "ota_task",
        .fn         = ota_task,
        .arg        = NULL,
        .stack_size = OTA_TASK_STACK,
        .priority   = OS_PRIO_HIGH,
        .core       = OS_CORE_BG,
        .owner      = OS_OWNER_SYSTEM,
        .stack_in_psram = false,
    };
    esp_err_t ret = os_task_create(&cfg, NULL);
    if (ret != ESP_OK) {
        s_in_progress = false;
        ESP_LOGE(TAG, "Failed to create OTA task: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

bool svc_ota_in_progress(void)
{
    return s_in_progress;
}
