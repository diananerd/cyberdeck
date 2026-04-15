/*
 * CyberDeck — Task Manager app
 *
 * Displays all registered FreeRTOS tasks from os_task_list():
 *   - Task name (primary)
 *   - Stack high-water mark (words free), priority, core (secondary)
 *
 * Auto-refreshes every 2 s while in foreground.
 * Accessible via:
 *   - Launcher card ("Ps" icon, name "Processes")
 *   - Navbar □ button (EVT_NAV_PROCESSES → app_manager_launch)
 *
 * D1: on_create returns state*; on_destroy cleans up timer + mem.
 */

#include "app_taskman.h"
#include "app_registry.h"
#include "ui_activity.h"
#include "ui_theme.h"
#include "ui_statusbar.h"
#include "ui_common.h"
#include "os_task.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "taskman";

#define REFRESH_MS  2000

typedef struct {
    lv_obj_t   *list;
    lv_timer_t *timer;
} taskman_state_t;

/* ---- Helpers ---- */

static void populate_list(taskman_state_t *s)
{
    static os_process_info_t tasks[OS_MAX_TASKS];
    uint8_t count = os_task_list(tasks, OS_MAX_TASKS);

    lv_obj_clean(s->list);

    if (count == 0) {
        lv_obj_t *lbl = lv_label_create(s->list);
        lv_label_set_text(lbl, "NO TASKS");
        ui_theme_style_label_dim(lbl, &CYBERDECK_FONT_SM);
        return;
    }

    for (uint8_t i = 0; i < count; i++) {
        char secondary[40];
        snprintf(secondary, sizeof(secondary), "STK: %lu W   PRIO: %u   CORE: %u",
                 (unsigned long)tasks[i].stack_high_water,
                 (unsigned)tasks[i].priority,
                 (unsigned)tasks[i].core);
        ui_common_list_add_two_line(s->list, tasks[i].name, secondary,
                                    i, NULL, NULL);
    }
}

static void refresh_cb(lv_timer_t *timer)
{
    taskman_state_t *s = (taskman_state_t *)timer->user_data;
    if (!s || !s->list) return;
    populate_list(s);
}

/* ---- Activity callbacks (D1) ---- */

static void *taskman_on_create(lv_obj_t *screen, const view_args_t *args)
{
    (void)args;
    ui_statusbar_set_title("PROCESSES");

    taskman_state_t *s = lv_mem_alloc(sizeof(taskman_state_t));
    if (!s) return NULL;
    s->list  = NULL;
    s->timer = NULL;

    lv_obj_t *content = ui_common_content_area(screen);
    s->list = ui_common_list(content);
    populate_list(s);

    s->timer = lv_timer_create(refresh_cb, REFRESH_MS, s);

    ESP_LOGI(TAG, "Task manager created");
    return s;
}

static void taskman_on_resume(lv_obj_t *screen, void *state)
{
    (void)screen;
    (void)state;
    ui_statusbar_set_title("PROCESSES");
}

static void taskman_on_destroy(lv_obj_t *screen, void *state)
{
    (void)screen;
    taskman_state_t *s = (taskman_state_t *)state;
    if (!s) return;
    if (s->timer) {
        lv_timer_del(s->timer);
        s->timer = NULL;
    }
    lv_mem_free(s);
}

/* ---- Registration ---- */

esp_err_t app_taskman_register(void)
{
    static const app_manifest_t manifest = {
        .id          = APP_ID_TASKMAN,
        .name        = "Processes",
        .icon        = "Ps",
        .type        = APP_TYPE_BUILTIN,
        .permissions = 0,
        .storage_dir = NULL,
    };
    static const activity_cbs_t cbs = {
        .on_create  = taskman_on_create,
        .on_resume  = taskman_on_resume,
        .on_pause   = NULL,
        .on_destroy = taskman_on_destroy,
    };
    os_app_register(&manifest, NULL, &cbs);
    ESP_LOGI(TAG, "Task Manager registered");
    return ESP_OK;
}
