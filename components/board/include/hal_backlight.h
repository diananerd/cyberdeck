#pragma once

#include "esp_err.h"
#include <stdbool.h>

esp_err_t hal_backlight_on(void);
esp_err_t hal_backlight_off(void);
esp_err_t hal_backlight_set(bool on);
