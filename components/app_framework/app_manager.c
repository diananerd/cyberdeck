/*
 * S3 Cyber-Deck — App Manager
 * Bridges ui_intent -> app_registry, and provides navigation helpers
 * for use from non-LVGL task contexts.
 */

#include "app_manager.h"
#include "app_registry.h"
#include "ui_activity.h"
#include "ui_intent.h"
#include "ui_engine.h"
#include "ui_effect.h"
#include "esp_log.h"

static const char *TAG = "app_manager";

static const activity_cbs_t *s_lockscreen_cbs = NULL;
static bool                  s_locked          = false;

/* ---------- navigate_fn registered with ui_intent ---------- */

static bool manager_navigate_fn(const intent_t *intent)
{
    const app_entry_t *app = app_registry_get(intent->app_id);
    if (!app) {
        ESP_LOGW(TAG, "App %d not registered — showing toast", intent->app_id);
        ui_effect_toast("Coming soon...", 1500);
        return false;
    }
    return ui_activity_push(intent->app_id, intent->screen_id,
                            &app->cbs, intent->data);
}

/* ---------- Public API ---------- */

esp_err_t app_manager_init(void)
{
    ui_intent_set_navigate_fn(manager_navigate_fn);
    ESP_LOGI(TAG, "Initialized");
    return ESP_OK;
}

void app_manager_launch(uint8_t app_id, void *data, size_t data_size)
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
    if (s_locked) return;   /* don't allow back-swipe to bypass lockscreen */
    if (ui_lock(500)) {
        ui_activity_pop();
        ui_unlock();
    }
}

void app_manager_go_home(void)
{
    if (s_locked) return;   /* don't allow home-swipe to bypass lockscreen */
    if (ui_lock(500)) {
        ui_activity_pop_to_home();
        ui_unlock();
    }
}

void app_manager_lock(void)
{
    if (!s_lockscreen_cbs) {
        ESP_LOGW(TAG, "No lockscreen registered");
        return;
    }
    /* Lock navigation BEFORE pushing so no race can bypass it */
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
