#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize ADC for battery monitoring on GPIO 6.
 */
esp_err_t hal_battery_init(void);

/**
 * @brief Read battery voltage and return percentage.
 * @param pct Output 0-100
 * @return ESP_OK on success
 */
esp_err_t hal_battery_read_pct(uint8_t *pct);

/**
 * @brief Read raw battery voltage in mV.
 * @param mv Output millivolts
 */
esp_err_t hal_battery_read_mv(uint32_t *mv);

#ifdef __cplusplus
}
#endif
