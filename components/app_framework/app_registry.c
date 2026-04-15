/*
 * CyberDeck — App Registry
 *
 * C2: dynamic list of app_entry_t* with malloc/realloc — extends at runtime.
 * C3: built-ins registered via os_app_register; legacy app_registry_register
 *     remains as a wrapper.
 */

#include "app_registry.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"

static const char *TAG = "app_registry";

/* ---- Dynamic list ---- */

static app_entry_t **s_apps     = NULL;
static uint16_t      s_app_count    = 0;
static uint16_t      s_app_capacity = 0;

#define REGISTRY_GROW_BY  8  /* slots added on each realloc */

/* Find entry by id; returns NULL if not present. */
static app_entry_t *find_entry(app_id_t id)
{
    for (uint16_t i = 0; i < s_app_count; i++) {
        if (s_apps[i] && s_apps[i]->app_id == id) return s_apps[i];
    }
    return NULL;
}

/* Allocate a new slot; grow the array if needed. Returns NULL on OOM. */
static app_entry_t *alloc_entry(void)
{
    if (s_app_count >= s_app_capacity) {
        uint16_t new_cap = (uint16_t)(s_app_capacity + REGISTRY_GROW_BY);
        app_entry_t **tmp = realloc(s_apps, new_cap * sizeof(app_entry_t *));
        if (!tmp) {
            ESP_LOGE(TAG, "OOM growing registry to %d slots", new_cap);
            return NULL;
        }
        s_apps = tmp;
        /* Zero new slots */
        for (uint16_t i = s_app_capacity; i < new_cap; i++) s_apps[i] = NULL;
        s_app_capacity = new_cap;
    }
    app_entry_t *entry = calloc(1, sizeof(app_entry_t));
    if (!entry) {
        ESP_LOGE(TAG, "OOM allocating app_entry_t");
        return NULL;
    }
    s_apps[s_app_count++] = entry;
    return entry;
}

/* ---- Public API ---- */

void app_registry_init(void)
{
    /* Free any prior state (if called more than once) */
    for (uint16_t i = 0; i < s_app_count; i++) {
        free(s_apps[i]);
        s_apps[i] = NULL;
    }
    free(s_apps);
    s_apps        = NULL;
    s_app_count   = 0;
    s_app_capacity = 0;
    ESP_LOGI(TAG, "Initialized (dynamic list)");
}

void os_app_register(const app_manifest_t *manifest,
                     const app_ops_t       *ops,
                     const activity_cbs_t  *cbs)
{
    if (!manifest) return;

    /* Update existing entry if already registered */
    app_entry_t *entry = find_entry(manifest->id);
    if (!entry) {
        entry = alloc_entry();
        if (!entry) return;
    }

    entry->manifest   = *manifest;
    if (ops) entry->ops = *ops;
    if (cbs) entry->cbs = *cbs;
    entry->available  = true;
    /* Legacy alias fields */
    entry->app_id = manifest->id;
    entry->name   = manifest->name;
    entry->icon   = manifest->icon;

    ESP_LOGI(TAG, "Registered app %u: %s", (unsigned)manifest->id,
             manifest->name ? manifest->name : "?");
}

void os_app_enumerate(void (*cb)(const app_entry_t *entry, void *ctx), void *ctx)
{
    if (!cb) return;
    for (uint16_t i = 0; i < s_app_count; i++) {
        if (s_apps[i]) cb(s_apps[i], ctx);
    }
}

/* ---- Legacy API (backward compat) ---- */

void app_registry_register(const app_entry_t *entry)
{
    if (!entry) return;

    app_manifest_t m = {
        .id          = entry->app_id,
        .name        = entry->name,
        .icon        = entry->icon,
        .type        = APP_TYPE_BUILTIN,
        .permissions = 0,
        .storage_dir = NULL,
    };
    os_app_register(&m, &entry->ops, &entry->cbs);
}

const app_entry_t *app_registry_get(app_id_t app_id)
{
    app_entry_t *e = find_entry(app_id);
    if (!e || !e->cbs.on_create) return NULL;  /* stub / not implemented */
    return e;
}

const app_entry_t *app_registry_get_raw(app_id_t app_id)
{
    app_entry_t *e = find_entry(app_id);
    if (!e || !e->name) return NULL;  /* completely empty slot */
    return e;
}
