#include "deck_sdi_services.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "deck_sdi.svc";

#define DECK_SDI_SERVICES_CAP 16
#define DECK_SDI_SERVICES_ID_MAX 64

typedef struct {
    char  id[DECK_SDI_SERVICES_ID_MAX];
    void *provider;
} entry_t;

static entry_t s_entries[DECK_SDI_SERVICES_CAP];
static size_t  s_count = 0;
static bool    s_inited = false;

void deck_sdi_services_init(void)
{
    memset(s_entries, 0, sizeof(s_entries));
    s_count  = 0;
    s_inited = true;
}

deck_sdi_err_t deck_sdi_services_register(const char *service_id, void *provider)
{
    if (!s_inited) deck_sdi_services_init();
    if (!service_id || !*service_id) return DECK_SDI_ERR_INVALID_ARG;
    for (size_t i = 0; i < DECK_SDI_SERVICES_CAP; i++) {
        if (s_entries[i].id[0] && strcmp(s_entries[i].id, service_id) == 0)
            return DECK_SDI_ERR_ALREADY_EXISTS;
    }
    for (size_t i = 0; i < DECK_SDI_SERVICES_CAP; i++) {
        if (!s_entries[i].id[0]) {
            snprintf(s_entries[i].id, sizeof(s_entries[i].id), "%s", service_id);
            s_entries[i].provider = provider;
            s_count++;
            ESP_LOGI(TAG, "registered \"%s\" → %p", service_id, provider);
            return DECK_SDI_OK;
        }
    }
    return DECK_SDI_ERR_NO_MEMORY;
}

void deck_sdi_services_unregister(const char *service_id)
{
    if (!s_inited || !service_id) return;
    for (size_t i = 0; i < DECK_SDI_SERVICES_CAP; i++) {
        if (s_entries[i].id[0] && strcmp(s_entries[i].id, service_id) == 0) {
            s_entries[i].id[0]   = '\0';
            s_entries[i].provider = NULL;
            s_count--;
            return;
        }
    }
}

void *deck_sdi_services_lookup(const char *service_id)
{
    if (!s_inited || !service_id) return NULL;
    for (size_t i = 0; i < DECK_SDI_SERVICES_CAP; i++) {
        if (s_entries[i].id[0] && strcmp(s_entries[i].id, service_id) == 0)
            return s_entries[i].provider;
    }
    return NULL;
}

void deck_sdi_services_iter(deck_sdi_services_iter_cb cb, void *user)
{
    if (!s_inited || !cb) return;
    for (size_t i = 0; i < DECK_SDI_SERVICES_CAP; i++) {
        if (s_entries[i].id[0]) {
            if (!cb(s_entries[i].id, s_entries[i].provider, user)) return;
        }
    }
}

size_t deck_sdi_services_count(void)
{
    return s_count;
}
