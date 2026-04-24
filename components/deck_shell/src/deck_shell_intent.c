/* deck_shell_intent — intent registry + navbar wiring. */

#include "deck_shell_intent.h"

#include "deck_bridge_ui.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "shell.intent";

#define INTENT_MAX_APPS  16

typedef struct {
    uint16_t                       app_id;
    deck_shell_intent_resolver_t   fn;
} intent_slot_t;

static intent_slot_t s_resolvers[INTENT_MAX_APPS];
static size_t        s_resolver_count = 0;
static bool          s_nav_locked     = false;

deck_err_t deck_shell_intent_register(uint16_t app_id,
                                       deck_shell_intent_resolver_t resolver)
{
    if (!resolver) return DECK_RT_INTERNAL;
    /* Replace existing? */
    for (size_t i = 0; i < s_resolver_count; i++) {
        if (s_resolvers[i].app_id == app_id) {
            s_resolvers[i].fn = resolver;
            return DECK_RT_OK;
        }
    }
    if (s_resolver_count >= INTENT_MAX_APPS) {
        ESP_LOGE(TAG, "registry full (cap=%d)", INTENT_MAX_APPS);
        return DECK_RT_NO_MEMORY;
    }
    s_resolvers[s_resolver_count++] = (intent_slot_t){
        .app_id = app_id,
        .fn     = resolver,
    };
    ESP_LOGI(TAG, "registered resolver for app_id=%u", (unsigned)app_id);
    return DECK_RT_OK;
}

deck_err_t deck_shell_intent_navigate(const deck_shell_intent_t *intent)
{
    if (!intent) return DECK_RT_INTERNAL;
    for (size_t i = 0; i < s_resolver_count; i++) {
        if (s_resolvers[i].app_id == intent->app_id) {
            return s_resolvers[i].fn(intent);
        }
    }
    ESP_LOGW(TAG, "no resolver for app_id=%u", (unsigned)intent->app_id);
    return DECK_LOAD_UNRESOLVED;
}

void deck_shell_navbar_back(void)
{
    if (s_nav_locked) {
        ESP_LOGI(TAG, "back ignored — nav locked");
        return;
    }
    /* Pop only if we have something popable (depth > 1 or > 0 if no
     * launcher yet). */
    if (deck_bridge_ui_activity_depth() > 1) {
        deck_bridge_ui_activity_pop();
    } else {
        ESP_LOGI(TAG, "back at root — no-op");
    }
}

void deck_shell_navbar_home(void)
{
    if (s_nav_locked) {
        ESP_LOGI(TAG, "home ignored — nav locked");
        return;
    }
    deck_bridge_ui_activity_pop_to_home();
}

void deck_shell_nav_lock(bool lock)
{
    s_nav_locked = lock;
    ESP_LOGI(TAG, "nav %s", lock ? "LOCKED" : "unlocked");
}

bool deck_shell_nav_is_locked(void) { return s_nav_locked; }
