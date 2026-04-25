#pragma once

#include "esp_err.h"
#include <stdbool.h>

esp_err_t hal_backlight_on(void);
esp_err_t hal_backlight_off(void);
esp_err_t hal_backlight_set(bool on);

/* J3 — set the backlight to a continuous level in [0.0, 1.0]. On the
 * Waveshare ESP32-S3-Touch-LCD-4.3 the BL pin is wired through the
 * CH422G I/O expander (EXIO2), so true PWM dimming is not available
 * in hardware. This call quantises to off (≤ 0.01) / on (> 0.01) and
 * is the API surface bridges should call so a future board revision
 * with a PWM-capable BL line can drop in fine dimming without
 * touching callers. Returns ESP_OK on success. */
esp_err_t hal_backlight_set_level(float level);

/* Read back the last level set via hal_backlight_set_level. */
float     hal_backlight_get_level(void);
