/*
 * CyberDeck — System Monitor Service
 */

#include "svc_monitor.h"
#include "svc_event.h"
#include "os_process.h"
#include "os_service.h"
#include "os_task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "svc_monitor";

/* ---- Double buffer ---- */

static sys_snapshot_t    s_bufs[2];
static volatile uint8_t  s_read_idx  = 0;
static volatile uint8_t  s_write_idx = 1;
static SemaphoreHandle_t s_swap_mutex;

/* ---- Monitor task ---- */

static uint32_t          s_refresh_interval_ms;
static TaskHandle_t      s_task_handle;
static volatile bool     s_force_refresh;

/* ---- Build snapshot ---- */

static void build_snapshot(sys_snapshot_t *snap)
{
    memset(snap, 0, sizeof(*snap));

    /* Memoria */
    snap->heap_internal_free  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    snap->heap_internal_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    snap->heap_psram_free     = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    snap->heap_psram_total    = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);

    /* Apps en ejecución */
    os_process_t proc_buf[MON_MAX_APPS];
    uint8_t proc_count = os_process_list(proc_buf, MON_MAX_APPS);
    snap->app_count = proc_count;

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

    for (uint8_t i = 0; i < proc_count; i++) {
        mon_app_entry_t *a = &snap->apps[i];
        const os_process_t *p = &proc_buf[i];

        a->app_id       = p->app_id;
        a->state        = p->state;
        a->view_count   = p->view_count;
        a->bg_task_count = p->task_count;
        a->uptime_s     = (now_ms - p->launched_ms) / 1000;

        if (p->name[0] != '\0') {
            strncpy(a->name, p->name, OS_PROCESS_NAME_LEN - 1);
            a->name[OS_PROCESS_NAME_LEN - 1] = '\0';
        } else {
            /* Fallback si el proceso no tiene nombre todavía */
            snprintf(a->name, OS_PROCESS_NAME_LEN, "APP_%u", (unsigned)p->app_id);
        }

        /* Heap delta: aproximado (heap puede haber fluctuado por otros actores) */
        size_t cur_free = snap->heap_internal_free;
        a->heap_delta = (p->heap_before > cur_free) ? (p->heap_before - cur_free) : 0;
    }

    /* Servicios */
    os_service_entry_t svc_buf[MON_MAX_SERVICES];
    uint8_t svc_count = os_service_list(svc_buf, MON_MAX_SERVICES);
    snap->service_count = svc_count;
    for (uint8_t i = 0; i < svc_count; i++)
        snap->services[i] = svc_buf[i];

    /* FreeRTOS tasks — solo en dev mode */
#if CONFIG_CYBERDECK_MONITOR_DEV_MODE
    os_task_info_t task_buf[OS_MAX_TASKS];
    uint8_t task_count = os_task_list(task_buf, OS_MAX_TASKS);
    snap->task_count = task_count;
    for (uint8_t i = 0; i < task_count; i++) {
        mon_task_entry_t *mt = &snap->tasks[i];
        const os_task_info_t *ot = &task_buf[i];
        strncpy(mt->name, ot->name, OS_TASK_NAME_LEN - 1);
        mt->name[OS_TASK_NAME_LEN - 1] = '\0';
        mt->owner      = ot->owner;
        mt->stack_hwm  = ot->stack_high_water;
        mt->priority   = ot->priority;
        mt->core       = ot->core;
        mt->rtos_state = (uint8_t)eTaskGetState(ot->handle);
    }
#else
    snap->task_count = 0;
#endif

    /* Metadatos */
    snap->uptime_s      = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    snap->snapshot_tick = (uint32_t)xTaskGetTickCount();
    snap->refresh_count++;
}

/* ---- Monitor task loop ---- */

static void monitor_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Monitor task started (interval=%lums)", (unsigned long)s_refresh_interval_ms);

    while (1) {
        /* Build into write buffer — no lock needed, only this task writes */
        build_snapshot(&s_bufs[s_write_idx]);

        /* Swap read/write indices atomically */
        xSemaphoreTake(s_swap_mutex, portMAX_DELAY);
        uint8_t tmp   = s_read_idx;
        s_read_idx    = s_write_idx;
        s_write_idx   = tmp;
        xSemaphoreGive(s_swap_mutex);

        svc_event_post(EVT_MONITOR_UPDATED, NULL, 0);

        s_force_refresh = false;

        /* Wait for next interval or force_refresh signal */
        uint32_t remaining = s_refresh_interval_ms;
        while (remaining > 0 && !s_force_refresh) {
            uint32_t slice = (remaining > 50) ? 50 : remaining;
            vTaskDelay(pdMS_TO_TICKS(slice));
            remaining -= slice;
        }
    }
}

/* ---- Public API ---- */

esp_err_t svc_monitor_init(uint32_t refresh_interval_ms)
{
    s_refresh_interval_ms = (refresh_interval_ms > 0) ? refresh_interval_ms : 2000;
    s_force_refresh = false;

    memset(s_bufs, 0, sizeof(s_bufs));

    s_swap_mutex = xSemaphoreCreateMutex();
    if (!s_swap_mutex) {
        ESP_LOGE(TAG, "Failed to create swap mutex");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ret = xTaskCreatePinnedToCore(
        monitor_task, "svc_monitor",
        4096,           /* stack: snapshot build + string ops */
        NULL,
        2,              /* low priority — no bloquea nada crítico */
        &s_task_handle,
        0               /* core 0 (BG) */
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create monitor task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Monitor initialized");
    return ESP_OK;
}

const sys_snapshot_t *svc_monitor_get_snapshot(void)
{
    xSemaphoreTake(s_swap_mutex, portMAX_DELAY);
    uint8_t idx = s_read_idx;
    xSemaphoreGive(s_swap_mutex);
    return &s_bufs[idx];
}

void svc_monitor_force_refresh(void)
{
    s_force_refresh = true;
}
