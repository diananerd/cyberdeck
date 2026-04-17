#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"

esp_err_t hal_lcd_init(esp_lcd_panel_handle_t *panel_handle, esp_lcd_touch_handle_t *tp_handle);

/* VSYNC callback contract — returns true if a refresh was triggered. */
typedef bool (*hal_lcd_vsync_cb_t)(void);

/* Register (or clear, with NULL) the UI-layer vsync hook. Not required at DL1. */
void hal_lcd_set_vsync_cb(hal_lcd_vsync_cb_t cb);
