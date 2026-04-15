/*
 * S3 Cyber-Deck — Intent-based navigation
 * Bridges the intent system to the activity stack.
 * App registry lookup will be added in Phase 4 when app_framework is created.
 */

#include "ui_intent.h"
#include "ui_activity.h"
#include "esp_log.h"

static const char *TAG = "intent";

void ui_intent_navigate(const intent_t *intent)
{
    if (!intent) {
        ESP_LOGE(TAG, "NULL intent");
        return;
    }

    /*
     * TODO (Phase 4): Look up app_id in app_registry to get activity_cbs_t.
     * For now, this is a stub that logs the intent. Apps will register their
     * callbacks via app_registry, and this function will call:
     *   const app_entry_t *app = app_registry_get(intent->app_id);
     *   ui_activity_push(intent->app_id, intent->screen_id, &app->cbs, intent->data);
     */
    ESP_LOGW(TAG, "Navigate app=%d scr=%d (registry not yet available)",
             intent->app_id, intent->screen_id);
}

void ui_intent_go_back(void)
{
    ESP_LOGD(TAG, "Go back");
    ui_activity_pop();
}

void ui_intent_go_home(void)
{
    ESP_LOGD(TAG, "Go home");
    ui_activity_pop_to_home();
}
