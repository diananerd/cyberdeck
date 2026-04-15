#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the OTA service.
 */
esp_err_t svc_ota_init(void);

/**
 * @brief Start an OTA update from the configured URL.
 *        Runs in its own task. Posts EVT_OTA_* events.
 * @param url  HTTPS URL of the firmware binary (NULL to use stored URL)
 */
esp_err_t svc_ota_start(const char *url);

/**
 * @brief Check if an OTA update is currently in progress.
 */
bool svc_ota_in_progress(void);

#ifdef __cplusplus
}
#endif
