#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Gesture types detected by the touch edge detector */
typedef enum {
    HAL_GESTURE_HOME = 0,   /* Swipe down from top edge */
    HAL_GESTURE_BACK,       /* Swipe right from left edge */
} hal_gesture_type_t;

/**
 * @brief Gesture callback type.
 *        Called from the LVGL task context (Core 1).
 */
typedef void (*hal_gesture_cb_t)(hal_gesture_type_t gesture);

/**
 * @brief Initialize touch gesture detection.
 *        Registers LVGL input event callbacks for edge gestures.
 *        Must be called after ui_engine_init(), with LVGL mutex held.
 *
 * @param cb  Callback invoked when a gesture is detected (or NULL)
 */
esp_err_t hal_gesture_init(hal_gesture_cb_t cb);

/**
 * @brief Recreate gesture strips after display rotation.
 *        Deletes old strips and creates new ones with current display dimensions.
 *        Must be called with LVGL mutex held.
 */
esp_err_t hal_gesture_recreate(void);

#ifdef __cplusplus
}
#endif
