/*
 * S3 Cyber-Deck — Touch edge gesture detection
 * Replaces physical buttons since GPIO 0 is owned by LCD.
 *
 * Gestures:
 *   - Swipe down from top edge (start y < 20px): HOME
 *   - Swipe right from left edge (start x < 15px): BACK
 *   - Long press in status bar area (y < 20px, 1.5s): LOCK
 */

#include "hal_gesture.h"
#include "ui_statusbar.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include <stdlib.h>

static const char *TAG = "hal_gesture";

/* Edge zones */
#define EDGE_TOP_PX         UI_STATUSBAR_HEIGHT
#define EDGE_LEFT_PX        15
#define SWIPE_MIN_DELTA     40
#define LONG_PRESS_MS       1500

/* State */
static lv_coord_t s_press_x = 0;
static lv_coord_t s_press_y = 0;
static int64_t    s_press_time = 0;
static bool       s_tracking = false;
static hal_gesture_cb_t s_callback = NULL;

static void gesture_input_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;

    lv_point_t point;
    lv_indev_get_point(indev, &point);

    if (code == LV_EVENT_PRESSED) {
        s_press_x = point.x;
        s_press_y = point.y;
        s_press_time = esp_timer_get_time();
        s_tracking = true;
    }
    else if (code == LV_EVENT_RELEASED && s_tracking) {
        s_tracking = false;
        int64_t duration_ms = (esp_timer_get_time() - s_press_time) / 1000;
        lv_coord_t dx = point.x - s_press_x;
        lv_coord_t dy = point.y - s_press_y;

        /* Long press in status bar area -> LOCK */
        if (s_press_y < EDGE_TOP_PX && duration_ms >= LONG_PRESS_MS &&
            abs(dx) < SWIPE_MIN_DELTA && abs(dy) < SWIPE_MIN_DELTA) {
            ESP_LOGI(TAG, "Gesture: LOCK");
            if (s_callback) s_callback(HAL_GESTURE_LOCK);
            return;
        }

        /* Swipe down from top edge -> HOME */
        if (s_press_y < EDGE_TOP_PX && dy > SWIPE_MIN_DELTA) {
            ESP_LOGI(TAG, "Gesture: HOME");
            if (s_callback) s_callback(HAL_GESTURE_HOME);
            return;
        }

        /* Swipe right from left edge -> BACK */
        if (s_press_x < EDGE_LEFT_PX && dx > SWIPE_MIN_DELTA) {
            ESP_LOGI(TAG, "Gesture: BACK");
            if (s_callback) s_callback(HAL_GESTURE_BACK);
            return;
        }
    }
}

esp_err_t hal_gesture_init(hal_gesture_cb_t cb)
{
    s_callback = cb;

    lv_obj_add_event_cb(lv_layer_top(), gesture_input_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(lv_layer_top(), gesture_input_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_flag(lv_layer_top(), LV_OBJ_FLAG_CLICKABLE);

    ESP_LOGI(TAG, "Gesture detection initialized");
    return ESP_OK;
}
