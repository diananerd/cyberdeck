/*
 * CyberDeck — Crash Log (F4)
 *
 * At boot, reads esp_reset_reason(). If it indicates a crash (panic,
 * watchdog, brownout) appends a line to /sdcard/.cyberdeck/crash.log.
 *
 * The log is append-only and plain text:
 *   [2026-04-15 12:34:56] CRASH reason=PANIC
 *
 * No log rotation: the file grows indefinitely. Callers may inspect or
 * clear it via the Settings > About screen in a future release.
 */

#include "os_crash.h"
#include "hal_sdcard.h"
#include "esp_system.h"
#include "esp_log.h"
#include <stdio.h>
#include <time.h>

static const char *TAG = "os_crash";

#define CRASH_LOG_PATH  HAL_SDCARD_MOUNT_POINT "/.cyberdeck/crash.log"

static const char *reason_to_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "POWER_ON";
    case ESP_RST_EXT:       return "EXT_RESET";
    case ESP_RST_SW:        return "SW_RESET";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WATCHDOG";
    case ESP_RST_TASK_WDT:  return "TASK_WATCHDOG";
    case ESP_RST_WDT:       return "WATCHDOG";
    case ESP_RST_DEEPSLEEP: return "DEEP_SLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "UNKNOWN";
    }
}

static bool is_crash_reason(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
    case ESP_RST_BROWNOUT:
        return true;
    default:
        return false;
    }
}

void os_crash_init(void)
{
    if (!hal_sdcard_is_mounted()) {
        ESP_LOGD(TAG, "SD not mounted — crash log skipped");
        return;
    }

    esp_reset_reason_t reason = esp_reset_reason();
    if (!is_crash_reason(reason)) return;

    FILE *f = fopen(CRASH_LOG_PATH, "a");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open crash log: %s", CRASH_LOG_PATH);
        return;
    }

    /* Use wall-clock time if RTC was synced; otherwise uptime-relative zero */
    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_info);

    fprintf(f, "[%s] CRASH reason=%s\n", ts, reason_to_str(reason));
    fclose(f);

    ESP_LOGW(TAG, "Previous boot crashed: %s", reason_to_str(reason));
}
