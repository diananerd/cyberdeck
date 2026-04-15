/*
 * S3 Cyber-Deck — OS Core: Poller task
 *
 * Task única en Core 0 que itera la lista de pollers cada 100 ms y
 * llama a los que han alcanzado su intervalo.
 *
 * El poller task se crea con os_task_create (ownership = OS_OWNER_SYSTEM).
 */

#include "os_poller.h"
#include "os_task.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "os_poller";

#define POLLER_TICK_MS  100   /* resolución mínima de intervalos */

typedef struct {
    char         name[16];
    os_poll_fn_t fn;
    void        *arg;
    uint32_t     interval_ms;
    uint32_t     elapsed_ms;
    app_id_t     owner;
    bool         active;
} poller_entry_t;

static poller_entry_t s_pollers[OS_POLLER_MAX];
static uint8_t        s_count = 0;

esp_err_t os_poller_register(const char *name, os_poll_fn_t fn, void *arg,
                             uint32_t interval_ms, app_id_t owner)
{
    if (s_count >= OS_POLLER_MAX) {
        ESP_LOGE(TAG, "Poller table full (max=%d)", OS_POLLER_MAX);
        return ESP_ERR_NO_MEM;
    }
    if (!fn || interval_ms == 0) return ESP_ERR_INVALID_ARG;

    poller_entry_t *p = &s_pollers[s_count++];
    strncpy(p->name, name ? name : "unnamed", sizeof(p->name) - 1);
    p->name[sizeof(p->name) - 1] = '\0';
    p->fn          = fn;
    p->arg         = arg;
    p->interval_ms = interval_ms;
    p->elapsed_ms  = 0; /* fire after first full interval */
    p->owner       = owner;
    p->active      = true;

    ESP_LOGI(TAG, "Registered poller '%s' every %u ms", p->name, (unsigned)interval_ms);
    return ESP_OK;
}

static void poller_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Poller task started (%u registered)", (unsigned)s_count);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(POLLER_TICK_MS));

        for (int i = 0; i < s_count; i++) {
            poller_entry_t *p = &s_pollers[i];
            if (!p->active) continue;

            p->elapsed_ms += POLLER_TICK_MS;
            if (p->elapsed_ms >= p->interval_ms) {
                p->elapsed_ms = 0;
                p->fn(p->arg);
            }
        }
    }
}

esp_err_t os_poller_start(void)
{
    os_task_config_t cfg = {
        .name       = "os_poller",
        .fn         = poller_task,
        .arg        = NULL,
        .stack_size = 3072,
        .priority   = OS_PRIO_LOW,
        .core       = OS_CORE_BG,
        .owner      = OS_OWNER_SYSTEM,
    };
    esp_err_t ret = os_task_create(&cfg, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start poller task: %s", esp_err_to_name(ret));
    }
    return ret;
}

void os_poller_remove_all_for_app(app_id_t owner)
{
    for (int i = 0; i < s_count; i++) {
        if (s_pollers[i].owner == owner) {
            ESP_LOGI(TAG, "Removing poller '%s' (owner app %u)",
                     s_pollers[i].name, (unsigned)owner);
            s_pollers[i].active = false;
        }
    }
}
