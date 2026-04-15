/*
 * S3 Cyber-Deck — Intent-based navigation
 * Bridges the intent system to the activity stack.
 * Navigation is resolved via a callback registered by app_manager_init().
 */

#include "ui_intent.h"
#include "ui_activity.h"
#include "esp_log.h"

static const char *TAG = "intent";

static ui_intent_navigate_fn_t s_navigate_fn = NULL;

void ui_intent_set_navigate_fn(ui_intent_navigate_fn_t fn)
{
    s_navigate_fn = fn;
}

void ui_intent_navigate(const intent_t *intent)
{
    if (!intent) {
        ESP_LOGE(TAG, "NULL intent");
        return;
    }
    if (!s_navigate_fn) {
        ESP_LOGW(TAG, "Navigate fn not set — call app_manager_init() first");
        return;
    }
    if (!s_navigate_fn(intent)) {
        ESP_LOGW(TAG, "Navigate failed for app=%d scr=%d",
                 intent->app_id, intent->screen_id);
    }
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
