#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Show the on-screen keyboard for a textarea.
 *        Creates a terminal-styled keyboard at the bottom of the screen.
 *        Only one keyboard can be active at a time.
 *        Must be called with the LVGL mutex held.
 *
 * @param ta Target textarea to bind to
 */
void ui_keyboard_show(lv_obj_t *ta);

/**
 * @brief Hide the on-screen keyboard.
 */
void ui_keyboard_hide(void);

/**
 * @brief Check if the keyboard is currently visible.
 */
bool ui_keyboard_is_visible(void);

/**
 * @brief Refresh keyboard theme (call after theme change).
 */
void ui_keyboard_refresh_theme(void);

#ifdef __cplusplus
}
#endif
