#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Navigation intent. Describes which screen to navigate to and optional data.
 */
typedef struct {
    uint8_t  app_id;        /* Target app */
    uint8_t  screen_id;     /* Screen within the app (0 = main) */
    void    *data;          /* Heap-allocated, ownership transfers to target on_create */
    size_t   data_size;     /* Size of data allocation */
} intent_t;

/**
 * @brief Navigate to a new activity described by the intent.
 *        Resolves app_id via the registered navigate_fn (set by app_manager_init).
 *        Must be called with the LVGL mutex held.
 */
void ui_intent_navigate(const intent_t *intent);

/**
 * @brief Callback type for navigation resolution.
 *        Set by app_manager to decouple ui_engine from app_framework.
 *        Called with the LVGL mutex already held.
 * @return true if navigation succeeded
 */
typedef bool (*ui_intent_navigate_fn_t)(const intent_t *intent);

/**
 * @brief Register the navigation resolution callback.
 *        Called once during app_manager_init().
 */
void ui_intent_set_navigate_fn(ui_intent_navigate_fn_t fn);

/**
 * @brief Go back one screen (pop top activity).
 */
void ui_intent_go_back(void);

/**
 * @brief Go home (pop all except launcher at bottom).
 */
void ui_intent_go_home(void);

#ifdef __cplusplus
}
#endif
