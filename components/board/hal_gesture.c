/*
 * CyberDeck — Touch edge gesture detection
 * Replaces physical buttons since GPIO 0 is owned by LCD.
 *
 * Gestures:
 *   - Swipe down from top edge (start y < EDGE_TOP_PX): HOME
 *   - Swipe right from left edge (start x < EDGE_LEFT_PX): BACK
 *
 * Implementation note:
 *   Two narrow transparent strips are placed on lv_layer_top() covering only
 *   the edge zones.  This avoids making the entire layer CLICKABLE, which would
 *   intercept ALL touch events on every screen before they reach UI widgets.
 */

#include "hal_gesture.h"
#include "ui_statusbar.h"
#include "ui_navbar.h"
#include "esp_log.h"
#include "lvgl.h"
#include <stdlib.h>

static const char *TAG = "hal_gesture";

/* Edge zones */
#define EDGE_TOP_PX         UI_STATUSBAR_HEIGHT
#define EDGE_LEFT_PX        15
#define SWIPE_MIN_DELTA     40

/* State */
static lv_coord_t s_press_x    = 0;
static lv_coord_t s_press_y    = 0;
static bool       s_tracking   = false;
static hal_gesture_cb_t s_callback = NULL;

/* Strip references for recreation after rotation */
static lv_obj_t *s_top_strip  = NULL;
static lv_obj_t *s_left_strip = NULL;

static void gesture_input_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;

    lv_point_t point;
    lv_indev_get_point(indev, &point);

    if (code == LV_EVENT_PRESSED) {
        s_press_x  = point.x;
        s_press_y  = point.y;
        s_tracking = true;
    }
    else if (code == LV_EVENT_RELEASED && s_tracking) {
        s_tracking = false;
        lv_coord_t dx = point.x - s_press_x;
        lv_coord_t dy = point.y - s_press_y;

        /* Swipe down from top edge → HOME */
        if (s_press_y < EDGE_TOP_PX && dy > SWIPE_MIN_DELTA) {
            ESP_LOGI(TAG, "Gesture: HOME");
            if (s_callback) s_callback(HAL_GESTURE_HOME);
            return;
        }

        /* Swipe right from left edge → BACK */
        if (s_press_x < EDGE_LEFT_PX && dx > SWIPE_MIN_DELTA) {
            ESP_LOGI(TAG, "Gesture: BACK");
            if (s_callback) s_callback(HAL_GESTURE_BACK);
            return;
        }
    }
}

/* Create a transparent non-scrollable strip on lv_layer_top() */
static lv_obj_t *make_edge_strip(lv_coord_t x, lv_coord_t y,
                                  lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *strip = lv_obj_create(lv_layer_top());
    lv_obj_set_pos(strip, x, y);
    lv_obj_set_size(strip, w, h);
    lv_obj_set_style_bg_opa(strip, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(strip, 0, 0);
    lv_obj_set_style_pad_all(strip, 0, 0);
    lv_obj_clear_flag(strip, LV_OBJ_FLAG_SCROLLABLE);
    /* CLICKABLE is set by default on lv_obj — we keep it so events reach here */
    lv_obj_add_event_cb(strip, gesture_input_cb, LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(strip, gesture_input_cb, LV_EVENT_RELEASED, NULL);
    return strip;
}

esp_err_t hal_gesture_init(hal_gesture_cb_t cb)
{
    s_callback = cb;

    lv_disp_t  *disp = lv_disp_get_default();
    lv_coord_t  w    = lv_disp_get_hor_res(disp);
    lv_coord_t  h    = lv_disp_get_ver_res(disp);

    /* Top strip — covers statusbar height, full width (HOME swipe) */
    s_top_strip = make_edge_strip(0, 0, w, EDGE_TOP_PX);

    /* Left strip — from below statusbar to (above bottom-navbar in portrait,
     * or full height in landscape since navbar is on the right side). */
    bool portrait = (w < h);
    lv_coord_t left_h = portrait ? (h - EDGE_TOP_PX - UI_NAVBAR_THICK)
                                  : (h - EDGE_TOP_PX);
    if (left_h < 0) left_h = h;
    s_left_strip = make_edge_strip(0, EDGE_TOP_PX, EDGE_LEFT_PX, left_h);

    ESP_LOGI(TAG, "Gesture detection initialized (%dx%d)", (int)w, (int)h);
    return ESP_OK;
}

esp_err_t hal_gesture_recreate(void)
{
    /* Delete old strips and recreate with current display dimensions.
     * Must be called with LVGL mutex held. */
    if (s_top_strip)  { lv_obj_del(s_top_strip);  s_top_strip  = NULL; }
    if (s_left_strip) { lv_obj_del(s_left_strip); s_left_strip = NULL; }

    lv_disp_t  *disp = lv_disp_get_default();
    lv_coord_t  w    = lv_disp_get_hor_res(disp);
    lv_coord_t  h    = lv_disp_get_ver_res(disp);

    s_top_strip  = make_edge_strip(0, 0, w, EDGE_TOP_PX);

    bool portrait = (w < h);
    lv_coord_t left_h = portrait ? (h - EDGE_TOP_PX - UI_NAVBAR_THICK)
                                  : (h - EDGE_TOP_PX);
    if (left_h < 0) left_h = h;
    s_left_strip = make_edge_strip(0, EDGE_TOP_PX, EDGE_LEFT_PX, left_h);

    ESP_LOGI(TAG, "Gesture strips recreated (%dx%d)", (int)w, (int)h);
    return ESP_OK;
}
