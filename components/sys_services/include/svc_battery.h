#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the battery monitoring task.
 *        Reads ADC every 30s, posts EVT_BATTERY_UPDATED,
 *        and updates the status bar.
 *        Requires hal_battery_init() and svc_event_init() first.
 */
esp_err_t svc_battery_start(void);

/**
 * @brief Get the last known battery percentage (0-100).
 */
uint8_t svc_battery_get_pct(void);

#ifdef __cplusplus
}
#endif
