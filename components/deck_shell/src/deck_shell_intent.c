/* deck_shell_intent — intent registry + navbar wiring. */

#include "deck_shell_intent.h"
#include "deck_shell_deck_apps.h"

#include "deck_bridge_ui.h"
#include "deck_interp.h"
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

/* §14.8 — async resolver: when @on back returns :confirm the runtime
 * shows a dialog and reports HANDLED/UNHANDLED through this callback
 * once the user picks. The shell acts on the outcome: HANDLED keeps the
 * current activity; UNHANDLED pops. */
static void back_resolved_cb(deck_back_result_t outcome)
{
    if (s_nav_locked) return;
    if (outcome == DECK_BACK_HANDLED) {
        ESP_LOGI(TAG, "back :confirm → handled (staying)");
        return;
    }
    if (deck_bridge_ui_activity_depth() > 1) {
        deck_bridge_ui_activity_pop();
    }
}

void deck_shell_navbar_back(void)
{
    if (s_nav_locked) {
        ESP_LOGI(TAG, "back ignored — nav locked");
        return;
    }

    /* If the top activity is a .deck app, let @on back steer the gesture
     * first. Ensure the runtime resolver is registered — it is a no-op
     * if the app's @on back doesn't return :confirm. */
    deck_runtime_set_back_resolved_handler(back_resolved_cb);

    deck_bridge_ui_activity_t *top = deck_bridge_ui_activity_current();
    deck_runtime_app_t *app = top ? deck_shell_deck_apps_handle(top->app_id) : NULL;
    if (app) {
        deck_back_result_t r = deck_runtime_app_back(app);
        if (r == DECK_BACK_HANDLED) {
            ESP_LOGI(TAG, "back consumed by @on back");
            return;
        }
        if (r == DECK_BACK_CONFIRMED) {
            /* Dialog is up; back_resolved_cb will pop (or not) later. */
            return;
        }
        /* UNHANDLED / ERROR → fall through to shell default. */
    }

    /* Default: pop only if we have something popable. */
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

void deck_shell_navbar_tasks(void)
{
    if (s_nav_locked) {
        ESP_LOGI(TAG, "tasks ignored — nav locked");
        return;
    }
    /* Resolve cyberdeck.taskman by id and route to it. The .deck file
     * registers itself in the apps slot table at boot, so the lookup
     * stays robust to app_id reshuffling. */
    uint32_t cnt = deck_shell_deck_apps_count();
    for (uint32_t i = 0; i < cnt; i++) {
        deck_shell_deck_app_info_t info = {0};
        deck_shell_deck_apps_info(i, &info);
        if (info.id && strcmp(info.id, "cyberdeck.taskman") == 0) {
            deck_shell_intent_t intent = {
                .app_id = info.app_id, .screen_id = 0, .data = NULL, .data_size = 0,
            };
            deck_shell_intent_navigate(&intent);
            return;
        }
    }
    ESP_LOGW(TAG, "tasks: cyberdeck.taskman not loaded");
}

void deck_shell_nav_lock(bool lock)
{
    s_nav_locked = lock;
    ESP_LOGI(TAG, "nav %s", lock ? "LOCKED" : "unlocked");
}

bool deck_shell_nav_is_locked(void) { return s_nav_locked; }
