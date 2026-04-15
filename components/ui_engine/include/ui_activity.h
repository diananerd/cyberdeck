#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ACTIVITY_STACK_MAX  4

/**
 * @brief Activity lifecycle callbacks.
 * All callbacks receive the screen object and an opaque state pointer.
 */
typedef struct {
    void (*on_create)(lv_obj_t *screen, void *intent_data);
    void (*on_resume)(lv_obj_t *screen, void *state);
    void (*on_pause)(lv_obj_t *screen, void *state);
    void (*on_destroy)(lv_obj_t *screen, void *state);
} activity_cbs_t;

/**
 * @brief Activity instance on the stack.
 */
typedef struct {
    uint8_t         app_id;
    uint8_t         screen_id;
    lv_obj_t       *screen;
    void           *state;          /* App-managed opaque state, heap-allocated */
    activity_cbs_t  cbs;
} activity_t;

/**
 * @brief Initialize the activity system. Call once after ui_engine_init().
 */
void ui_activity_init(void);

/**
 * @brief Push a new activity onto the stack.
 *        If stack is full, entry [1] is destroyed and shifted.
 *        Must be called with the LVGL mutex held.
 *
 * @param app_id      App identifier
 * @param screen_id   Screen within the app
 * @param cbs         Lifecycle callbacks
 * @param intent_data Data passed to on_create (ownership transfers to the activity)
 * @return true if pushed successfully
 */
bool ui_activity_push(uint8_t app_id, uint8_t screen_id,
                      const activity_cbs_t *cbs, void *intent_data);

/**
 * @brief Pop the top activity (go back).
 *        Calls on_destroy on popped, on_resume on new top.
 *        Will not pop the last activity (launcher).
 * @return true if popped
 */
bool ui_activity_pop(void);

/**
 * @brief Pop all activities except the bottom one (go home).
 *        No-op while nav lock is active (see ui_activity_set_nav_lock).
 */
void ui_activity_pop_to_home(void);

/**
 * @brief Lock / unlock navigation gestures and the HOME button.
 *        While locked, pop_to_home() is a no-op so the lockscreen
 *        cannot be bypassed by a HOME swipe or navbar tap.
 *        Does NOT affect ui_activity_pop() so the PIN entry can still dismiss itself.
 */
void ui_activity_set_nav_lock(bool locked);

/**
 * @brief Get the current (top) activity.
 * @return Pointer to the top activity, or NULL if stack is empty.
 */
const activity_t *ui_activity_current(void);

/**
 * @brief Get the current stack depth.
 */
uint8_t ui_activity_depth(void);

/**
 * @brief Set the opaque state pointer for the current activity.
 *        Typically called from on_create.
 */
void ui_activity_set_state(void *state);

/**
 * @brief Recreate all activities in the stack in-place.
 *        For each entry: calls on_destroy (if set), clears screen children,
 *        re-applies base styling, then calls on_create again with NULL intent.
 *        Use this after a display rotation so all layouts adapt to the new dimensions.
 *        Must be called with the LVGL mutex held.
 */
void ui_activity_recreate_all(void);

#ifdef __cplusplus
}
#endif
