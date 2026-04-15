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
 *        Looks up the app in the registry and pushes its activity.
 *        Must be called with the LVGL mutex held.
 */
void ui_intent_navigate(const intent_t *intent);

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
