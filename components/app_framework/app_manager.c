/*
 * CyberDeck — App Manager
 * Bridges ui_intent -> app_registry, and provides navigation helpers
 * for use from non-LVGL task contexts.
 */

#include "app_manager.h"
#include "app_registry.h"
#include "ui_activity.h"
#include "ui_intent.h"
#include "ui_engine.h"
#include "ui_effect.h"
#include "os_task.h"
#include "esp_log.h"

static const char *TAG = "app_manager";

static const activity_cbs_t *s_lockscreen_cbs = NULL;
static bool                  s_locked          = false;

/* ---------- H2: OS-level close hook — called by activity system after on_destroy ----------
 *
 * This is the single integration point between the UI lifecycle (activity stack)
 * and the OS process lifecycle (FreeRTOS tasks, app_ops.on_terminate).
 *
 * Contract:
 *  - Called ONCE per unique app_id that was permanently closed (not on rotation).
 *  - Called from the LVGL task (inside close_app_async / pop_to_home), after
 *    the LVGL screen and state* have already been freed by on_destroy.
 *  - Safe to call vTaskDelete here (not from within the deleted task itself).
 */
static void on_app_closed(app_id_t app_id)
{
    /* 1. Call app-level terminate callback (if registered). */
    const app_entry_t *entry = app_registry_get_raw(app_id);
    if (entry && entry->ops.on_terminate) {
        entry->ops.on_terminate(app_id);
    }

    /* 2. Kill all FreeRTOS tasks owned by this app.
     *    on_destroy should have already stopped lv_timers and unregistered event
     *    handlers; os_task_destroy_all_for_app handles the rest. */
    os_task_destroy_all_for_app(app_id);

    ESP_LOGI("app_manager", "App %d fully terminated (tasks killed, ops.on_terminate called)",
             (int)app_id);
}

/* ---------- navigate_fn registered with ui_intent ---------- */

static bool manager_navigate_fn(const intent_t *intent)
{
    /* Singleton check: if the app is already in the stack, raise it to the
     * front instead of creating a duplicate instance. This prevents multiple
     * copies of Settings, Task Manager, etc. accumulating memory and state. */
    if (ui_activity_raise(intent->app_id)) {
        ESP_LOGI(TAG, "App %d already open — raised to front", (int)intent->app_id);
        return true;
    }

    const app_entry_t *app = app_registry_get(intent->app_id);
    if (!app) {
        ESP_LOGW(TAG, "App %d not registered — showing toast", (int)intent->app_id);
        ui_effect_toast("Coming soon...", 1500);
        return false;
    }

    /* Wrap legacy intent data into view_args_t (not owned: caller manages lifetime) */
    view_args_t args = {
        .data  = intent->data,
        .size  = intent->data_size,
        .owned = false,
    };
    return ui_activity_push(intent->app_id, intent->screen_id,
                            &app->cbs, intent->data ? &args : NULL);
}

/* ---------- Public API ---------- */

esp_err_t app_manager_init(void)
{
    ui_intent_set_navigate_fn(manager_navigate_fn);
    ui_activity_set_close_hook(on_app_closed);  /* H2: wire lifecycle → process cleanup */
    ESP_LOGI(TAG, "Initialized");
    return ESP_OK;
}

void app_manager_launch(app_id_t app_id, void *data, size_t data_size)
{
    if (ui_lock(500)) {
        intent_t intent = {
            .app_id    = app_id,
            .screen_id = 0,
            .data      = data,
            .data_size = data_size,
        };
        ui_intent_navigate(&intent);
        ui_unlock();
    }
}

void app_manager_go_back(void)
{
    if (s_locked) return;
    if (ui_lock(500)) {
        ui_activity_pop();
        ui_unlock();
    }
}

void app_manager_go_home(void)
{
    if (s_locked) return;
    if (ui_lock(500)) {
        ui_activity_suspend_to_home();  /* apps stay in background; CLOSE to free memory */
        ui_unlock();
    }
}

void app_manager_lock(void)
{
    if (!s_lockscreen_cbs) {
        ESP_LOGW(TAG, "No lockscreen registered");
        return;
    }
    s_locked = true;
    ui_activity_set_nav_lock(true);
    if (ui_lock(500)) {
        ui_activity_push(APP_ID_LAUNCHER, 1, s_lockscreen_cbs, NULL);
        ui_unlock();
    }
}

void app_manager_clear_lock(void)
{
    s_locked = false;
    ui_activity_set_nav_lock(false);
}

void app_manager_set_lock(void)
{
    s_locked = true;
    ui_activity_set_nav_lock(true);
}

void app_manager_set_lockscreen_cbs(const activity_cbs_t *cbs)
{
    s_lockscreen_cbs = cbs;
}
