#include "deck_sdi_registry.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "deck_sdi";

static deck_sdi_driver_t s_drivers[DECK_SDI_DRIVER_MAX];
static size_t            s_count = 0;
static bool              s_initialized = false;

void deck_sdi_registry_init(void)
{
    memset(s_drivers, 0, sizeof(s_drivers));
    s_count = 0;
    s_initialized = true;
    ESP_LOGI(TAG, "registry initialized (cap=%d)", (int)DECK_SDI_DRIVER_MAX);
}

deck_sdi_err_t deck_sdi_register(const deck_sdi_driver_t *driver)
{
    if (!s_initialized)             return DECK_SDI_ERR_INVALID_ARG;
    if (!driver)                    return DECK_SDI_ERR_INVALID_ARG;
    if (!driver->name)              return DECK_SDI_ERR_INVALID_ARG;
    if (driver->id >= DECK_SDI_DRIVER_MAX) return DECK_SDI_ERR_INVALID_ARG;
    if (s_drivers[driver->id].name != NULL) return DECK_SDI_ERR_ALREADY_EXISTS;

    s_drivers[driver->id] = *driver;
    s_count++;
    ESP_LOGI(TAG, "registered %s v%s (id=%d)",
             driver->name,
             driver->version ? driver->version : "?",
             (int)driver->id);
    return DECK_SDI_OK;
}

const deck_sdi_driver_t *deck_sdi_lookup(deck_sdi_driver_id_t id)
{
    if (id >= DECK_SDI_DRIVER_MAX) return NULL;
    return s_drivers[id].name ? &s_drivers[id] : NULL;
}

const deck_sdi_driver_t *deck_sdi_lookup_by_name(const char *name)
{
    if (!name) return NULL;
    for (size_t i = 0; i < DECK_SDI_DRIVER_MAX; i++) {
        if (s_drivers[i].name && strcmp(s_drivers[i].name, name) == 0)
            return &s_drivers[i];
    }
    return NULL;
}

size_t deck_sdi_count(void) { return s_count; }

void deck_sdi_list(deck_sdi_list_cb_t cb, void *user)
{
    if (!cb) return;
    for (size_t i = 0; i < DECK_SDI_DRIVER_MAX; i++) {
        if (s_drivers[i].name) cb(&s_drivers[i], user);
    }
}

static void log_driver(const deck_sdi_driver_t *d, void *user)
{
    (void)user;
    ESP_LOGI(TAG, "  · %-16s v%-8s id=%d",
             d->name, d->version ? d->version : "?", (int)d->id);
}

void deck_sdi_log_registered(void)
{
    ESP_LOGI(TAG, "%u drivers registered:", (unsigned)s_count);
    deck_sdi_list(log_driver, NULL);
}
