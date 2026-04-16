/*
 * CyberDeck — OS Core: Process Registry
 */

#include "os_process.h"
#include "svc_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "os_process";

/* ---- Registry ---- */

static os_process_t    s_procs[OS_MAX_PROCESSES];
static SemaphoreHandle_t s_mutex;

/* ---- Internal helpers ---- */

static os_process_t *find_slot_locked(app_id_t id)
{
    for (int i = 0; i < OS_MAX_PROCESSES; i++) {
        if (s_procs[i].state != PROC_STATE_STOPPED && s_procs[i].app_id == id)
            return &s_procs[i];
    }
    return NULL;
}

static os_process_t *find_free_locked(void)
{
    for (int i = 0; i < OS_MAX_PROCESSES; i++) {
        if (s_procs[i].state == PROC_STATE_STOPPED)
            return &s_procs[i];
    }
    return NULL;
}

/* ---- Public API ---- */

void os_process_init(void)
{
    memset(s_procs, 0, sizeof(s_procs));
    s_mutex = xSemaphoreCreateMutex();
    configASSERT(s_mutex);
    ESP_LOGI(TAG, "Process registry initialized (%d slots)", OS_MAX_PROCESSES);
}

esp_err_t os_process_start(app_id_t id, const char *name, void *app_data, size_t heap_before)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (find_slot_locked(id)) {
        ESP_LOGW(TAG, "os_process_start: app %u already running", (unsigned)id);
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    os_process_t *slot = find_free_locked();
    if (!slot) {
        ESP_LOGE(TAG, "Process registry full (OS_MAX_PROCESSES=%d)", OS_MAX_PROCESSES);
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }

    slot->app_id      = id;
    slot->state       = PROC_STATE_RUNNING;
    slot->app_data    = app_data;
    slot->heap_before = heap_before;
    slot->launched_ms = (uint32_t)(esp_timer_get_time() / 1000);
    slot->view_count  = 0;
    slot->task_count  = 0;

    if (name) {
        strncpy(slot->name, name, OS_PROCESS_NAME_LEN - 1);
        slot->name[OS_PROCESS_NAME_LEN - 1] = '\0';
    } else {
        slot->name[0] = '\0';
    }

    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Process started: app_id=%u", (unsigned)id);
    svc_event_post(EVT_APP_LAUNCHED, &id, sizeof(id));
    return ESP_OK;
}

void os_process_stop(app_id_t id)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    os_process_t *p = find_slot_locked(id);
    if (p) {
        p->state    = PROC_STATE_STOPPED;
        p->app_data = NULL;
    }
    xSemaphoreGive(s_mutex);

    if (p) {
        ESP_LOGI(TAG, "Process stopped: app_id=%u", (unsigned)id);
        svc_event_post(EVT_APP_TERMINATED, &id, sizeof(id));
    } else {
        ESP_LOGW(TAG, "os_process_stop: app %u not found", (unsigned)id);
    }
}

os_process_t *os_process_get(app_id_t id)
{
    /* Caller must not hold the result across context switches.
     * Safe for single-threaded LVGL reads where the process won't be stopped
     * concurrently without going through app_manager (which runs on LVGL task). */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    os_process_t *p = find_slot_locked(id);
    xSemaphoreGive(s_mutex);
    return p;
}

bool os_process_is_running(app_id_t id)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool running = (find_slot_locked(id) != NULL);
    xSemaphoreGive(s_mutex);
    return running;
}

void os_process_set_state(app_id_t id, proc_state_t state)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    os_process_t *p = find_slot_locked(id);
    if (p) p->state = state;
    xSemaphoreGive(s_mutex);
}

void os_process_update_counts(app_id_t id, uint8_t views, uint8_t tasks)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    os_process_t *p = find_slot_locked(id);
    if (p) {
        p->view_count = views;
        p->task_count = tasks;
    }
    xSemaphoreGive(s_mutex);
}

uint8_t os_process_list(os_process_t *buf, uint8_t max)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint8_t count = 0;
    for (int i = 0; i < OS_MAX_PROCESSES && count < max; i++) {
        if (s_procs[i].state != PROC_STATE_STOPPED)
            buf[count++] = s_procs[i];
    }
    xSemaphoreGive(s_mutex);
    return count;
}
