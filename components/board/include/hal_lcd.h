#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"

esp_err_t hal_lcd_init(esp_lcd_panel_handle_t *panel_handle, esp_lcd_touch_handle_t *tp_handle);
