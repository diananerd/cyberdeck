/*
 * S3 Cyber-Deck — Battery monitoring service
 * Lee el ADC cada 30s via os_poller (sin task dedicada).
 * Llamar svc_battery_start() y luego os_poller_start() en app_main.
 */

#include "svc_battery.h"
#include "svc_event.h"
#include "hal_battery.h"
#include "ui_statusbar.h"
#include "ui_engine.h"
#include "os_poller.h"
#include "esp_log.h"

static const char *TAG = "svc_battery";

#define BATTERY_POLL_MS  30000

static uint8_t s_last_pct = 0;

static void poll_battery(void *arg)
{
    (void)arg;
    uint8_t pct = 0;
    if (hal_battery_read_pct(&pct) == ESP_OK) {
        s_last_pct = pct;
        svc_event_post(EVT_BATTERY_UPDATED, &pct, sizeof(pct));
        if (ui_lock(100)) {
            ui_statusbar_set_battery(pct, false);
            ui_unlock();
        }
    }
}

esp_err_t svc_battery_start(void)
{
    esp_err_t ret = os_poller_register("battery", poll_battery, NULL,
                                       BATTERY_POLL_MS, OS_OWNER_SYSTEM);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register battery poller: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Battery poller registered (every %d ms)", BATTERY_POLL_MS);
    return ESP_OK;
}

uint8_t svc_battery_get_pct(void)
{
    return s_last_pct;
}
