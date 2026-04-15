/*
 * CyberDeck — App Registry implementation
 */

#include "app_registry.h"
#include <string.h>
#include "esp_log.h"

static const char *TAG = "app_registry";

static app_entry_t s_entries[APP_ID_COUNT];

void app_registry_init(void)
{
    memset(s_entries, 0, sizeof(s_entries));
    ESP_LOGI(TAG, "Initialized");
}

void app_registry_register(const app_entry_t *entry)
{
    if (!entry || entry->app_id >= APP_ID_COUNT) {
        ESP_LOGE(TAG, "Invalid entry (app_id=%d)", entry ? entry->app_id : 0xFF);
        return;
    }
    s_entries[entry->app_id] = *entry;
    ESP_LOGI(TAG, "Registered app %d: %s", entry->app_id,
             entry->name ? entry->name : "?");
}

const app_entry_t *app_registry_get(uint8_t app_id)
{
    if (app_id >= APP_ID_COUNT) return NULL;
    if (!s_entries[app_id].cbs.on_create) return NULL;  /* not registered */
    return &s_entries[app_id];
}

const app_entry_t *app_registry_get_raw(uint8_t app_id)
{
    if (app_id >= APP_ID_COUNT) return NULL;
    if (!s_entries[app_id].name) return NULL;  /* completely empty slot */
    return &s_entries[app_id];
}
