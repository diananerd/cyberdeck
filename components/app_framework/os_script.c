/*
 * CyberDeck — Script Runtime Registry (G4)
 *
 * Simple list of registered script runtimes.
 * No interpreter is bundled — this is the extension point.
 */

#include "os_script.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "os_script";

#define OS_SCRIPT_MAX_RUNTIMES 4

static const script_runtime_t *s_runtimes[OS_SCRIPT_MAX_RUNTIMES];
static int                     s_count = 0;

void os_script_register_runtime(const script_runtime_t *rt)
{
    if (!rt || !rt->name) return;

    /* Replace existing registration for the same name */
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_runtimes[i]->name, rt->name) == 0) {
            s_runtimes[i] = rt;
            ESP_LOGI(TAG, "Runtime updated: %s", rt->name);
            return;
        }
    }

    if (s_count >= OS_SCRIPT_MAX_RUNTIMES) {
        ESP_LOGE(TAG, "Runtime table full (max %d)", OS_SCRIPT_MAX_RUNTIMES);
        return;
    }

    s_runtimes[s_count++] = rt;
    ESP_LOGI(TAG, "Runtime registered: %s (ext=%s)", rt->name, rt->file_ext ? rt->file_ext : "?");
}

const script_runtime_t *os_script_get_runtime(const char *name)
{
    if (!name) return NULL;
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_runtimes[i]->name, name) == 0) {
            return s_runtimes[i];
        }
    }
    return NULL;
}

int os_script_runtime_count(void)
{
    return s_count;
}
