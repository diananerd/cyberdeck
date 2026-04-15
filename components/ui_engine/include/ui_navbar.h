/*
 * CyberDeck — Navigation bar
 * Landscape: right sidebar, UI_NAVBAR_THICK wide
 * Portrait:  bottom bar,   UI_NAVBAR_THICK tall
 *
 * Icons (outline geometric):
 *   ◀ triangle  → BACK
 *   ○ circle    → HOME
 *   □ square    → PROCESSES
 *
 * Lives on lv_layer_sys() — always above everything else.
 * Activity screens must pad accordingly (see ui_activity.c).
 */

#pragma once

#include "lvgl.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Navbar thickness — roughly 2× the statusbar height */
#define UI_NAVBAR_THICK  72

/**
 * @brief Create the navigation bar on lv_layer_sys().
 *        Must be called with the LVGL mutex held, after ui_theme_apply().
 */
esp_err_t ui_navbar_init(void);

/**
 * @brief Rebuild the navbar for the current display orientation.
 *        Call this when the display is rotated, with LVGL mutex held.
 */
void ui_navbar_adapt(void);

/**
 * @brief Force a full styling refresh (e.g. after theme change).
 *        Must be called with the LVGL mutex held.
 */
void ui_navbar_refresh_theme(void);

/**
 * @brief Show or hide the navigation bar (e.g. for full-screen activities).
 *        Must be called with the LVGL mutex held.
 */
void ui_navbar_set_visible(bool visible);

#ifdef __cplusplus
}
#endif
