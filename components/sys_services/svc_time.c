/*
 * S3 Cyber-Deck — Time service
 * SNTP sync on WiFi connect, RTC bridge, periodic status bar update.
 */

#include "svc_time.h"
#include "svc_event.h"
#include "svc_settings.h"
#include "hal_rtc.h"
#include "ui_statusbar.h"
#include "ui_engine.h"
#include "os_task.h"
#include "esp_sntp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <time.h>
#include <sys/time.h>

static const char *TAG = "svc_time";

static bool s_synced = false;

#define TIME_UPDATE_MS  1000    /* Update status bar every second */
#define TIME_TASK_STACK 4096    /* localtime_r + TZ needs ~1KB stack */

static void sntp_sync_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "SNTP time synchronized");
    s_synced = true;

    /* Write synced time to RTC */
    hal_rtc_sync_from_system();

    svc_event_post(EVT_TIME_SYNCED, NULL, 0);
}

static void time_update_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Time update task started");

    while (1) {
        uint8_t h, m, s;
        svc_time_get_hms(&h, &m, &s);

        if (ui_lock(500)) {
            ui_statusbar_set_time(h, m, s);
            ui_unlock();
        }

        vTaskDelay(pdMS_TO_TICKS(TIME_UPDATE_MS));
    }
}

static void on_wifi_connected(void *handler_arg, esp_event_base_t base,
                              int32_t id, void *data)
{
    (void)handler_arg;
    (void)base;
    (void)id;
    (void)data;
    ESP_LOGI(TAG, "WiFi connected, triggering SNTP sync");
    svc_time_sync();
}

esp_err_t svc_time_init(void)
{
    /* Apply timezone offset from settings */
    int8_t tz_offset = 0;
    svc_settings_get_tz_offset(&tz_offset);
    char tz_str[16];
    /* POSIX TZ: sign is inverted (UTC-5 = "UTC5" is wrong, need "EST5" or "<-05>5")
     * Simplify: use "UTC<offset>" where offset sign is inverted */
    if (tz_offset >= 0) {
        snprintf(tz_str, sizeof(tz_str), "UTC-%d", tz_offset);
    } else {
        snprintf(tz_str, sizeof(tz_str), "UTC+%d", -tz_offset);
    }
    setenv("TZ", tz_str, 1);
    tzset();

    /* Try to read initial time from RTC */
    hal_rtc_sync_to_system();

    /* Listen for WiFi connect to trigger SNTP */
    svc_event_register(EVT_WIFI_CONNECTED, on_wifi_connected, NULL);

    /* Start status bar update task */
    os_task_config_t cfg = {
        .name       = "time_upd",
        .fn         = time_update_task,
        .arg        = NULL,
        .stack_size = TIME_TASK_STACK,
        .priority   = OS_PRIO_LOW,
        .core       = OS_CORE_BG,
        .owner      = OS_OWNER_SYSTEM,
    };
    esp_err_t ret = os_task_create(&cfg, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create time update task: %s", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "Time service initialized (TZ: %s)", tz_str);
    return ESP_OK;
}

esp_err_t svc_time_sync(void)
{
    if (esp_sntp_enabled()) {
        esp_sntp_stop();
    }

    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(sntp_sync_cb);
    esp_sntp_init();

    ESP_LOGI(TAG, "SNTP sync started");
    return ESP_OK;
}

bool svc_time_is_synced(void) { return s_synced; }

void svc_time_get_hms(uint8_t *hour, uint8_t *minute, uint8_t *second)
{
    time_t now;
    time(&now);
    struct tm t;
    localtime_r(&now, &t);
    if (hour)   *hour   = (uint8_t)t.tm_hour;
    if (minute) *minute = (uint8_t)t.tm_min;
    if (second) *second = (uint8_t)t.tm_sec;
}
