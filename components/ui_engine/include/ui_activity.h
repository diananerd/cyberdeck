/*
 * CyberDeck — Activity stack manager
 * Manages a stack of up to 8 activities with lifecycle callbacks.
 *
 * D1: on_create returns void* (the activity's own state).
 * D3: stack depth extended from 4 to 8; push fails instead of evicting.
 * D6: view_args_t replaces raw void* intent_data.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "lvgl.h"
#include "os_core.h"   /* app_id_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Stack size (D3) ---- */
#define ACTIVITY_STACK_MAX  8

/* ---- View arguments (D6) ---- */

/**
 * @brief Arguments passed from a caller to the next activity's on_create.
 *        When owned=true the OS calls free(data) after on_create returns,
 *        so on_create only needs to copy, not free.
 */
typedef struct {
    void   *data;   /**< Heap-allocated payload, may be NULL. */
    size_t  size;   /**< Size of data allocation in bytes. */
    bool    owned;  /**< If true, OS calls free(data) after on_create. */
} view_args_t;

/**
 * @brief Activity lifecycle callbacks (D1).
 *
 *  on_create  — allocate + build screen, return own state*.
 *  on_resume  — screen becomes top again (e.g. after pop of child).
 *  on_pause   — another activity pushed on top (rarely needed).
 *  on_destroy — free state; called synchronously before lv_obj_del.
 */
typedef struct {
    void *(*on_create )(lv_obj_t *screen, const view_args_t *args); /* returns state* */
    void  (*on_resume )(lv_obj_t *screen, void *state);
    void  (*on_pause  )(lv_obj_t *screen, void *state);
    void  (*on_destroy)(lv_obj_t *screen, void *state);  /* must free state */
} activity_cbs_t;

/**
 * @brief Activity instance on the stack.
 */
typedef struct {
    app_id_t        app_id;
    uint8_t         screen_id;
    lv_obj_t       *screen;
    void           *state;      /* returned by on_create, passed back to all other cbs */
    activity_cbs_t  cbs;
    size_t          heap_used;  /* bytes consumed by on_create (internal heap delta) */
} activity_t;

/**
 * @brief Initialize the activity system. Call once after ui_engine_init().
 */
void ui_activity_init(void);

/**
 * @brief Push a new activity onto the stack.
 *        Stack max is ACTIVITY_STACK_MAX (8). Returns false if full (D3).
 *        Must be called with the LVGL mutex held.
 *
 * @param app_id    App identifier.
 * @param screen_id Screen within the app (0 = main).
 * @param cbs       Lifecycle callbacks.
 * @param args      Arguments passed to on_create; may be NULL. If args->owned,
 *                  OS frees args->data after on_create returns.
 * @return true if pushed; false if stack full or cbs invalid.
 */
bool ui_activity_push(app_id_t app_id, uint8_t screen_id,
                      const activity_cbs_t *cbs, const view_args_t *args);

/**
 * @brief Pop the top activity (go back).
 *        Calls on_destroy on popped, on_resume on new top.
 *        Will not pop the last activity (launcher).
 * @return true if popped.
 */
bool ui_activity_pop(void);

/**
 * @brief Suspend all activities and show the launcher (go home).
 *        Apps remain in the stack with their state preserved.
 *        Use ui_activity_close_app() (Task Manager) to actually free memory.
 *        No-op while nav lock is active.
 */
void ui_activity_suspend_to_home(void);

/**
 * @brief Destroy all activities except the launcher (hard close all).
 *        Used internally and by Task Manager "close all".
 *        No-op while nav lock is active.
 */
void ui_activity_pop_to_home(void);

/**
 * @brief Lock / unlock navigation gestures and the HOME button.
 *        While locked, pop_to_home() is a no-op so the lockscreen
 *        cannot be bypassed. Does NOT affect ui_activity_pop().
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
 * @brief Snapshot of a single entry in the activity stack (for Task Manager).
 */
typedef struct {
    app_id_t app_id;
    uint8_t  screen_id;
    uint8_t  stack_idx;   /**< Index in the stack (0 = launcher). */
    size_t   heap_used;   /**< Internal heap consumed at on_create time (bytes). */
} activity_info_t;

/**
 * @brief Fill buf with snapshots of all active activities (bottom-to-top).
 * @return Number of entries written.
 */
uint8_t ui_activity_list(activity_info_t *buf, uint8_t max);

/**
 * @brief Raise an existing app to the top of the stack.
 *        If the app is already on top, does nothing.
 *        If the app is below the top, destroys all activities above it
 *        (calling on_destroy on each) and resumes the target app.
 * @return true if the app was found in the stack (and raised or already top).
 *         false if the app is not in the stack.
 */
bool ui_activity_raise(app_id_t app_id);

/**
 * @brief Force-close an app and everything stacked above it.
 *        Equivalent to popping the stack down to just below the target app.
 *        Safe to call from within the Task Manager (uses lv_async_call internally).
 * @param app_id  The app to close. Launcher (depth 0) cannot be closed.
 * @return true if the app was found and closed.
 */
bool ui_activity_close_app(app_id_t app_id);

/**
 * @brief Recreate all activities in-place (after display rotation).
 *        Calls on_destroy + on_create on every entry with NULL args.
 *        Must be called with the LVGL mutex held.
 */
void ui_activity_recreate_all(void);

/* ---- App close hook (H2) ---- */

/**
 * @brief Callback type invoked once per unique app_id when one or more
 *        of its activities are permanently destroyed.
 *
 *        Called AFTER on_destroy for all affected activities.
 *        NOT called during ui_activity_recreate_all() (rotation — app stays alive).
 *        Safe to call os_task_destroy_all_for_app() and app_ops.on_terminate() here.
 */
typedef void (*ui_activity_close_hook_fn)(app_id_t app_id);

/**
 * @brief Register the OS-level app-closed callback.
 *        There is only one hook slot — calling this again replaces the prior hook.
 *        Typically called from app_manager_init().
 */
void ui_activity_set_close_hook(ui_activity_close_hook_fn fn);

#ifdef __cplusplus
}
#endif
