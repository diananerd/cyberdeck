/*
 * S3 Cyber-Deck — Battery monitoring service
 * FreeRTOS task on Core 0, reads ADC every 30s.
 */

#include "svc_battery.h"
#include "svc_event.h"
#include "hal_battery.h"
#include "ui_statusbar.h"
#include "ui_engine.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "svc_battery";

#define BATTERY_POLL_MS     30000
#define BATTERY_TASK_STACK  2048

static uint8_t s_last_pct = 0;

static void battery_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Battery monitor task started");

    while (1) {
        uint8_t pct = 0;
        if (hal_battery_read_pct(&pct) == ESP_OK) {
            s_last_pct = pct;
            svc_event_post(EVT_BATTERY_UPDATED, &pct, sizeof(pct));

            if (ui_lock(100)) {
                ui_statusbar_set_battery(pct, false);
                ui_unlock();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(BATTERY_POLL_MS));
    }
}

esp_err_t svc_battery_start(void)
{
    BaseType_t ret = xTaskCreatePinnedToCore(
        battery_task, "battery_mon", BATTERY_TASK_STACK,
        NULL, 1, NULL, 0);

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create battery task");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Battery monitor started");
    return ESP_OK;
}

uint8_t svc_battery_get_pct(void)
{
    return s_last_pct;
}
