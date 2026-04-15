#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the time service.
 *        Sets up SNTP and starts periodic status bar updates.
 *        Listens for EVT_WIFI_CONNECTED to trigger SNTP sync.
 */
esp_err_t svc_time_init(void);

/**
 * @brief Trigger an SNTP sync now (requires WiFi).
 */
esp_err_t svc_time_sync(void);

/**
 * @brief Check if time has been synchronized via SNTP.
 */
bool svc_time_is_synced(void);

/**
 * @brief Get current hour, minute, and second (local time with TZ offset).
 */
void svc_time_get_hms(uint8_t *hour, uint8_t *minute, uint8_t *second);

#ifdef __cplusplus
}
#endif
