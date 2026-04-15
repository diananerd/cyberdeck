#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Display parameters */
#define UI_ENGINE_H_RES             (800)
#define UI_ENGINE_V_RES             (480)
#define UI_ENGINE_TICK_PERIOD_MS    (CONFIG_EXAMPLE_LVGL_PORT_TICK)

/* Task parameters */
#define UI_ENGINE_TASK_MAX_DELAY_MS (CONFIG_EXAMPLE_LVGL_PORT_TASK_MAX_DELAY_MS)
#define UI_ENGINE_TASK_MIN_DELAY_MS (CONFIG_EXAMPLE_LVGL_PORT_TASK_MIN_DELAY_MS)
#define UI_ENGINE_TASK_STACK_SIZE   (CONFIG_EXAMPLE_LVGL_PORT_TASK_STACK_SIZE_KB * 1024)
#define UI_ENGINE_TASK_PRIORITY     (CONFIG_EXAMPLE_LVGL_PORT_TASK_PRIORITY)
#define UI_ENGINE_TASK_CORE         (CONFIG_EXAMPLE_LVGL_PORT_TASK_CORE)

/* Anti-tearing configuration */
#define UI_ENGINE_AVOID_TEAR_ENABLE  (CONFIG_EXAMPLE_LVGL_PORT_AVOID_TEAR_ENABLE)

#if UI_ENGINE_AVOID_TEAR_ENABLE
#define UI_ENGINE_AVOID_TEAR_MODE    (CONFIG_EXAMPLE_LVGL_PORT_AVOID_TEAR_MODE)
#define UI_ENGINE_ROTATION_DEGREE    (CONFIG_EXAMPLE_LVGL_PORT_ROTATION_DEGREE)

#if UI_ENGINE_AVOID_TEAR_MODE == 1
#define UI_ENGINE_LCD_RGB_BUFFER_NUMS   (2)
#define UI_ENGINE_FULL_REFRESH          (1)
#define UI_ENGINE_DIRECT_MODE           (0)
#elif UI_ENGINE_AVOID_TEAR_MODE == 2
#define UI_ENGINE_LCD_RGB_BUFFER_NUMS   (3)
#define UI_ENGINE_FULL_REFRESH          (1)
#define UI_ENGINE_DIRECT_MODE           (0)
#elif UI_ENGINE_AVOID_TEAR_MODE == 3
#define UI_ENGINE_LCD_RGB_BUFFER_NUMS   (2)
#define UI_ENGINE_FULL_REFRESH          (0)
#define UI_ENGINE_DIRECT_MODE           (1)
#endif

/* Rotation requires 3 buffers */
#if UI_ENGINE_ROTATION_DEGREE != 0
#undef UI_ENGINE_LCD_RGB_BUFFER_NUMS
#define UI_ENGINE_LCD_RGB_BUFFER_NUMS   (3)
#endif

#else /* !UI_ENGINE_AVOID_TEAR_ENABLE */
#define UI_ENGINE_LCD_RGB_BUFFER_NUMS   (1)
#define UI_ENGINE_FULL_REFRESH          (0)
#define UI_ENGINE_DIRECT_MODE           (0)
#define UI_ENGINE_ROTATION_DEGREE       (0)
#endif

/**
 * @brief Initialize UI engine (LVGL + display + touch)
 */
esp_err_t ui_engine_init(esp_lcd_panel_handle_t lcd_handle, esp_lcd_touch_handle_t tp_handle);

/**
 * @brief Set display rotation at runtime (software rotation).
 *        Must be called with the LVGL mutex held.
 * @param rotation 0 = landscape (800x480), 1 = portrait (480x800)
 */
void ui_engine_set_rotation(uint8_t rotation);

/**
 * @brief Take LVGL mutex. Must be called before any LVGL API usage from non-LVGL tasks.
 * @param timeout_ms Timeout in ms. -1 = block indefinitely.
 * @return true if mutex was taken
 */
bool ui_lock(int timeout_ms);

/**
 * @brief Release LVGL mutex
 */
void ui_unlock(void);

/**
 * @brief Notify LVGL task on VSYNC. Called from ISR.
 * @return true if higher priority task was woken
 */
bool ui_engine_notify_vsync(void);

#ifdef __cplusplus
}
#endif
