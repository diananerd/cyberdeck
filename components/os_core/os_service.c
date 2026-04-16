/*
 * CyberDeck — OS Core: Service Registry
 */

#include "os_service.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "os_service";

static os_service_entry_t s_services[OS_MAX_SERVICES];
static SemaphoreHandle_t  s_mutex;

/* ---- Internal ---- */

static os_service_entry_t *find_locked(const char *name)
{
    for (int i = 0; i < OS_MAX_SERVICES; i++) {
        if (s_services[i].name[0] != '\0' &&
            strncmp(s_services[i].name, name, OS_SERVICE_NAME_LEN) == 0)
            return &s_services[i];
    }
    return NULL;
}

static os_service_entry_t *find_free_locked(void)
{
    for (int i = 0; i < OS_MAX_SERVICES; i++) {
        if (s_services[i].name[0] == '\0') return &s_services[i];
    }
    return NULL;
}

/* ---- Public API ---- */

void os_service_init(void)
{
    memset(s_services, 0, sizeof(s_services));
    s_mutex = xSemaphoreCreateMutex();
    configASSERT(s_mutex);
    ESP_LOGI(TAG, "Service registry initialized (%d slots)", OS_MAX_SERVICES);
}

void os_service_register(const char *name)
{
    if (!name || name[0] == '\0') return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (find_locked(name)) {
        /* Idempotente — ya existe */
        xSemaphoreGive(s_mutex);
        return;
    }

    os_service_entry_t *slot = find_free_locked();
    if (!slot) {
        ESP_LOGE(TAG, "Service registry full — cannot register '%s'", name);
        xSemaphoreGive(s_mutex);
        return;
    }

    strncpy(slot->name, name, OS_SERVICE_NAME_LEN - 1);
    slot->name[OS_SERVICE_NAME_LEN - 1] = '\0';
    slot->state          = SVC_STATE_INIT;
    slot->status_text[0] = '\0';

    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "Service registered: '%s'", name);
}

void os_service_update(const char *name, svc_state_t state, const char *status_text)
{
    if (!name || name[0] == '\0') return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    os_service_entry_t *svc = find_locked(name);
    if (!svc) {
        ESP_LOGW(TAG, "os_service_update: '%s' not registered", name);
        xSemaphoreGive(s_mutex);
        return;
    }

    svc->state = state;
    if (status_text) {
        strncpy(svc->status_text, status_text, OS_SERVICE_STATUS_LEN - 1);
        svc->status_text[OS_SERVICE_STATUS_LEN - 1] = '\0';
    } else {
        svc->status_text[0] = '\0';
    }

    xSemaphoreGive(s_mutex);
}

uint8_t os_service_list(os_service_entry_t *buf, uint8_t max)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint8_t count = 0;
    for (int i = 0; i < OS_MAX_SERVICES && count < max; i++) {
        if (s_services[i].name[0] != '\0')
            buf[count++] = s_services[i];
    }
    xSemaphoreGive(s_mutex);
    return count;
}
