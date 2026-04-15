#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Callback for confirm dialog result.
 * @param confirmed true if user tapped "OK", false if "Cancel"
 * @param ctx       User-provided context pointer
 */
typedef void (*ui_confirm_cb_t)(bool confirmed, void *ctx);

/**
 * @brief Show a toast message overlay.
 *        Appears at the bottom of the screen and auto-dismisses.
 *        Must be called with the LVGL mutex held.
 *
 * @param msg  Message text
 * @param ms   Display duration in milliseconds (0 = default 2000ms)
 */
void ui_effect_toast(const char *msg, uint16_t ms);

/**
 * @brief Show a confirm dialog with OK/Cancel buttons.
 *        Rendered as modal overlay on lv_layer_top().
 *
 * @param title  Dialog title
 * @param msg    Dialog message
 * @param cb     Result callback
 * @param ctx    Context passed to callback
 */
void ui_effect_confirm(const char *title, const char *msg,
                       ui_confirm_cb_t cb, void *ctx);

/**
 * @brief Show or hide a loading overlay with blinking cursor.
 * @param show true to show, false to hide
 */
void ui_effect_loading(bool show);

#ifdef __cplusplus
}
#endif
