#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the PCF85063A RTC on the shared I2C bus.
 */
esp_err_t hal_rtc_init(void);

/**
 * @brief Read current time from RTC.
 * @param t  Output struct tm (year, mon, mday, hour, min, sec)
 */
esp_err_t hal_rtc_get_time(struct tm *t);

/**
 * @brief Write time to RTC.
 * @param t  Time to set
 */
esp_err_t hal_rtc_set_time(const struct tm *t);

/**
 * @brief Read RTC time and set the system clock (settimeofday).
 */
esp_err_t hal_rtc_sync_to_system(void);

/**
 * @brief Read system clock and write to RTC.
 */
esp_err_t hal_rtc_sync_from_system(void);

#ifdef __cplusplus
}
#endif
