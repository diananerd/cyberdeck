/*
 * CyberDeck — OS Navigation API (D4)
 *
 * Public navigation API for use by apps and settings sub-screens.
 * Wraps ui_activity_push/pop so app code never touches ui_engine directly.
 */

#pragma once

#include "os_core.h"
#include "ui_activity.h"  /* view_cbs_t, view_args_t */
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Push a new view (sub-screen) onto the activity stack.
 *        Must be called with the LVGL mutex held.
 *
 * @param app_id    Owning app.
 * @param screen_id Screen ID within the app.
 * @param cbs       Lifecycle callbacks (must have on_create set).
 * @param args      Optional view arguments; may be NULL.
 * @return ESP_OK on success, ESP_ERR_NO_MEM if stack full.
 */
esp_err_t os_view_push(app_id_t app_id, uint8_t screen_id,
                       const view_cbs_t  *cbs,
                       const view_args_t *args);

/**
 * @brief Pop the current view (go back one level).
 *        Must be called with the LVGL mutex held.
 */
void os_view_pop(void);

/**
 * @brief Pop all views until the root of the current app (screen_id == 0).
 *        Must be called with the LVGL mutex held.
 */
void os_view_pop_to_root(void);

/**
 * @brief Pop all views and return to the launcher (home screen).
 *        May be blocked by nav_lock (lockscreen).
 *        Must be called with the LVGL mutex held.
 */
void os_view_home(void);

#ifdef __cplusplus
}
#endif
