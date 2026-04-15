#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_STATUSBAR_HEIGHT  36

/**
 * @brief Create the status bar on lv_layer_top().
 *        Must be called with the LVGL mutex held, after ui_theme_apply().
 */
void ui_statusbar_init(void);

/**
 * @brief Update the time display. Call periodically or on time sync.
 * @param hour 0-23
 * @param minute 0-59
 * @param second 0-59
 */
void ui_statusbar_set_time(uint8_t hour, uint8_t minute, uint8_t second);

/**
 * @brief Update WiFi icon state.
 * @param connected true if connected
 * @param rssi signal strength (0-4 bars approximation)
 */
void ui_statusbar_set_wifi(bool connected, int8_t rssi);

/**
 * @brief Update battery display.
 * @param pct 0-100
 * @param charging true if charging
 */
void ui_statusbar_set_battery(uint8_t pct, bool charging);

/**
 * @brief Show/hide audio playing indicator.
 */
void ui_statusbar_set_audio(bool playing);

/**
 * @brief Show/hide SD card indicator.
 */
void ui_statusbar_set_sdcard(bool inserted);

/**
 * @brief Show/hide Bluetooth indicator.
 * @param connected true = primary color, false = dim
 */
void ui_statusbar_set_bluetooth(bool connected);

/**
 * @brief Set the title text (app name) in the center of the status bar.
 * @param title NULL or "" to clear
 */
void ui_statusbar_set_title(const char *title);

/**
 * @brief Force a full refresh of status bar styling (e.g. after theme change).
 */
void ui_statusbar_refresh_theme(void);

#ifdef __cplusplus
}
#endif
