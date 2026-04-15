/*
 * S3 Cyber-Deck — OS Core: Task Factory
 *
 * Todas las tasks del sistema pasan por os_task_create() en lugar de llamar
 * xTaskCreate / xTaskCreatePinnedToCore directamente.
 *
 * Beneficios:
 *  - Registro centralizado: nombre, owner, handle.
 *  - os_task_destroy_all_for_app() limpia todas las tasks de una app.
 *  - stack_in_psram: alloca el stack en SPIRAM para tasks pesadas.
 */

#include "os_task.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include <string.h>

static const char *TAG = "os_task";

/* ---- Registro interno ---- */

typedef struct {
    TaskHandle_t handle;
    app_id_t     owner;
    char         name[OS_TASK_NAME_LEN];
    bool         used;
} task_entry_t;

static task_entry_t s_tasks[OS_MAX_TASKS];

static task_entry_t *find_free_slot(void)
{
    for (int i = 0; i < OS_MAX_TASKS; i++) {
        if (!s_tasks[i].used) return &s_tasks[i];
    }
    return NULL;
}

static task_entry_t *find_by_handle(TaskHandle_t h)
{
    for (int i = 0; i < OS_MAX_TASKS; i++) {
        if (s_tasks[i].used && s_tasks[i].handle == h) return &s_tasks[i];
    }
    return NULL;
}

/* ---- API pública ---- */

esp_err_t os_task_create(const os_task_config_t *cfg, TaskHandle_t *out_handle)
{
    if (!cfg || !cfg->fn) return ESP_ERR_INVALID_ARG;

    task_entry_t *slot = find_free_slot();
    if (!slot) {
        ESP_LOGE(TAG, "Task registry full (OS_MAX_TASKS=%d)", OS_MAX_TASKS);
        return ESP_ERR_NO_MEM;
    }

    BaseType_t core = (cfg->core < 0) ? tskNO_AFFINITY : (BaseType_t)cfg->core;
    BaseType_t ret;
    TaskHandle_t handle = NULL;

    if (cfg->stack_in_psram) {
        /* Stack en SPIRAM para tasks pesadas (downloader, OTA, SQLite) */
        StaticTask_t *tcb = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
        StackType_t  *stk = heap_caps_malloc(cfg->stack_size, MALLOC_CAP_SPIRAM);
        if (!tcb || !stk) {
            free(tcb);
            free(stk);
            ESP_LOGE(TAG, "Failed to alloc PSRAM stack for '%s'", cfg->name);
            return ESP_ERR_NO_MEM;
        }
        handle = xTaskCreateStaticPinnedToCore(
            cfg->fn, cfg->name, cfg->stack_size / sizeof(StackType_t),
            cfg->arg, cfg->priority, stk, tcb, core);
        ret = (handle != NULL) ? pdPASS : errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;
        /* Note: TCB and stack are intentionally leaked — tasks are permanent in this design. */
    } else {
        ret = xTaskCreatePinnedToCore(
            cfg->fn, cfg->name, cfg->stack_size,
            cfg->arg, cfg->priority, &handle, core);
    }

    if (ret != pdPASS || !handle) {
        ESP_LOGE(TAG, "Failed to create task '%s'", cfg->name);
        return ESP_FAIL;
    }

    slot->handle = handle;
    slot->owner  = cfg->owner;
    slot->used   = true;
    strncpy(slot->name, cfg->name, OS_TASK_NAME_LEN - 1);
    slot->name[OS_TASK_NAME_LEN - 1] = '\0';

    ESP_LOGI(TAG, "Task '%s' created (owner=%u, prio=%u, core=%d, psram=%d)",
             cfg->name, (unsigned)cfg->owner, cfg->priority, cfg->core,
             (int)cfg->stack_in_psram);

    if (out_handle) *out_handle = handle;
    return ESP_OK;
}

esp_err_t os_task_destroy(TaskHandle_t handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;

    task_entry_t *entry = find_by_handle(handle);
    if (entry) {
        ESP_LOGI(TAG, "Destroying task '%s'", entry->name);
        entry->used = false;
        entry->handle = NULL;
    }
    vTaskDelete(handle);
    return ESP_OK;
}

void os_task_destroy_all_for_app(app_id_t id)
{
    for (int i = 0; i < OS_MAX_TASKS; i++) {
        if (s_tasks[i].used && s_tasks[i].owner == id) {
            ESP_LOGI(TAG, "Killing task '%s' (owner app %u)", s_tasks[i].name, (unsigned)id);
            TaskHandle_t h = s_tasks[i].handle;
            s_tasks[i].used = false;
            s_tasks[i].handle = NULL;
            vTaskDelete(h);
        }
    }
}

uint8_t os_task_list(os_process_info_t *buf, uint8_t max)
{
    uint8_t count = 0;
    for (int i = 0; i < OS_MAX_TASKS && count < max; i++) {
        if (!s_tasks[i].used) continue;
        os_process_info_t *p = &buf[count++];
        strncpy(p->name, s_tasks[i].name, sizeof(p->name) - 1);
        p->name[sizeof(p->name) - 1] = '\0';
        p->handle          = s_tasks[i].handle;
        p->owner           = s_tasks[i].owner;
        p->stack_high_water = uxTaskGetStackHighWaterMark(s_tasks[i].handle);
        p->priority        = (uint8_t)uxTaskPriorityGet(s_tasks[i].handle);
        p->core            = (uint8_t)xTaskGetCoreID(s_tasks[i].handle);
    }
    return count;
}
