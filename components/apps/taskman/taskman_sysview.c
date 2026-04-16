/*
 * CyberDeck — Task Manager: Sys View Screen (K3)
 *
 * Shows a table of FreeRTOS tasks from the monitor snapshot.
 * Accessible only via long press on the MEMORY section in the Overview.
 *
 * Requires CONFIG_CYBERDECK_MONITOR_DEV_MODE=y to show actual task data.
 * Without dev mode, task_count in the snapshot is always 0 and the screen
 * shows a "DEV MODE DISABLED" message.
 *
 * Event-driven: subscribes to EVT_MONITOR_UPDATED and rebuilds the list.
 */

#include "taskman_internal.h"
#include "ui_activity.h"
#include "ui_engine.h"
#include "ui_theme.h"
#include "ui_statusbar.h"
#include "ui_common.h"
#include "svc_monitor.h"
#include "svc_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "taskman_sysview";

/* =========================================================================
 * State
 * =========================================================================
 */

typedef struct {
    lv_obj_t *tasks_container;  /* cleaned + rebuilt on update */
} taskman_sysview_t;

static taskman_sysview_t *g_sysview = NULL;

/* =========================================================================
 * Helpers
 * =========================================================================
 */

static const char *rtos_state_str(uint8_t raw_state)
{
    switch ((eTaskState)raw_state) {
        case eRunning:   return "RUN";
        case eReady:     return "READY";
        case eBlocked:   return "BLOCK";
        case eSuspended: return "SUSP";
        case eDeleted:   return "DONE";
        default:         return "?";
    }
}

/* =========================================================================
 * Populate
 * =========================================================================
 */

static void populate_tasks(taskman_sysview_t *s, const sys_snapshot_t *snap)
{
    const cyberdeck_theme_t *t = ui_theme_get();
    lv_obj_clean(s->tasks_container);

    if (snap->task_count == 0) {
        lv_obj_t *msg = lv_label_create(s->tasks_container);
#if CONFIG_CYBERDECK_MONITOR_DEV_MODE
        lv_label_set_text(msg, "No tasks in snapshot");
#else
        lv_label_set_text(msg, "DEV MODE DISABLED\n"
                               "Enable CONFIG_CYBERDECK_MONITOR_DEV_MODE\n"
                               "to see FreeRTOS task details.");
#endif
        ui_theme_style_label_dim(msg, &CYBERDECK_FONT_SM);
        lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }

    /* Column header row */
    {
        lv_obj_t *hdr = lv_obj_create(s->tasks_container);
        lv_obj_set_width(hdr, LV_PCT(100));
        lv_obj_set_height(hdr, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(hdr, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(hdr, 1, 0);
        lv_obj_set_style_border_color(hdr, t->primary_dim, 0);
        lv_obj_set_style_border_side(hdr, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_radius(hdr, 0, 0);
        lv_obj_set_style_pad_ver(hdr, 4, 0);
        lv_obj_set_style_pad_hor(hdr, 0, 0);
        lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(hdr, 8, 0);

        const char *cols[] = { "NAME", "PRIO", "CORE", "STK W", "STATE" };
        int widths[] = { 160, 48, 48, 72, 72 };
        for (int c = 0; c < 5; c++) {
            lv_obj_t *lbl = lv_label_create(hdr);
            lv_label_set_text(lbl, cols[c]);
            ui_theme_style_label_dim(lbl, &CYBERDECK_FONT_SM);
            lv_obj_set_width(lbl, widths[c]);
        }
    }

    for (uint8_t i = 0; i < snap->task_count; i++) {
        const mon_task_entry_t *task = &snap->tasks[i];

        lv_obj_t *row = lv_obj_create(s->tasks_container);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_ver(row, 5, 0);
        lv_obj_set_style_pad_hor(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(row, 8, 0);

        /* Name */
        lv_obj_t *name_lbl = lv_label_create(row);
        lv_label_set_text(name_lbl, task->name);
        lv_obj_set_style_text_color(name_lbl, t->primary, 0);
        lv_obj_set_style_text_font(name_lbl, &CYBERDECK_FONT_SM, 0);
        lv_obj_set_width(name_lbl, 160);

        /* Priority */
        char prio_str[8];
        snprintf(prio_str, sizeof(prio_str), "%u", (unsigned)task->priority);
        lv_obj_t *prio_lbl = lv_label_create(row);
        lv_label_set_text(prio_lbl, prio_str);
        ui_theme_style_label_dim(prio_lbl, &CYBERDECK_FONT_SM);
        lv_obj_set_width(prio_lbl, 48);

        /* Core */
        char core_str[4];
        snprintf(core_str, sizeof(core_str), "%u", (unsigned)task->core);
        lv_obj_t *core_lbl = lv_label_create(row);
        lv_label_set_text(core_lbl, core_str);
        ui_theme_style_label_dim(core_lbl, &CYBERDECK_FONT_SM);
        lv_obj_set_width(core_lbl, 48);

        /* Stack HWM in words */
        char stk_str[12];
        snprintf(stk_str, sizeof(stk_str), "%luW",
                 (unsigned long)task->stack_hwm);
        lv_obj_t *stk_lbl = lv_label_create(row);
        lv_label_set_text(stk_lbl, stk_str);
        ui_theme_style_label_dim(stk_lbl, &CYBERDECK_FONT_SM);
        lv_obj_set_width(stk_lbl, 72);

        /* State */
        lv_obj_t *state_lbl = lv_label_create(row);
        lv_label_set_text(state_lbl, rtos_state_str(task->rtos_state));
        ui_theme_style_label_dim(state_lbl, &CYBERDECK_FONT_SM);
        lv_obj_set_width(state_lbl, 72);
    }
}

/* =========================================================================
 * Event handler
 * =========================================================================
 */

static void sysview_monitor_cb(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    (void)arg; (void)base; (void)id; (void)data;
    if (!g_sysview) return;
    if (ui_lock(200)) {
        if (g_sysview)
            populate_tasks(g_sysview, svc_monitor_get_snapshot());
        ui_unlock();
    }
}

/* =========================================================================
 * Lifecycle callbacks
 * =========================================================================
 */

static void *sysview_on_create(lv_obj_t *screen, const view_args_t *args, void *app_data)
{
    (void)args; (void)app_data;
    ui_statusbar_set_title("PROCESSES");

    taskman_sysview_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    const cyberdeck_theme_t *t = ui_theme_get();
    lv_obj_t *content = ui_common_content_area(screen);

    /* Header */
    lv_obj_t *title = lv_label_create(content);
    lv_label_set_text(title, "SYSTEM TASKS");
    ui_theme_style_label(title, &CYBERDECK_FONT_MD);
    lv_obj_set_style_pad_bottom(title, 8, 0);

    /* Uptime + task count line (static at creation, update via event) */
    const sys_snapshot_t *snap = svc_monitor_get_snapshot();
    char hdr_buf[48];
    snprintf(hdr_buf, sizeof(hdr_buf), "UPTIME: %lus   TASKS: %u",
             (unsigned long)snap->uptime_s,
             (unsigned)snap->task_count);
    lv_obj_t *hdr_lbl = lv_label_create(content);
    lv_label_set_text(hdr_lbl, hdr_buf);
    ui_theme_style_label_dim(hdr_lbl, &CYBERDECK_FONT_SM);

    /* Task list container */
    s->tasks_container = lv_obj_create(content);
    lv_obj_set_width(s->tasks_container, LV_PCT(100));
    lv_obj_set_height(s->tasks_container, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s->tasks_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s->tasks_container, 0, 0);
    lv_obj_set_style_pad_all(s->tasks_container, 0, 0);
    lv_obj_clear_flag(s->tasks_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s->tasks_container, LV_FLEX_FLOW_COLUMN);

    populate_tasks(s, snap);

    g_sysview = s;
    svc_event_register(EVT_MONITOR_UPDATED, sysview_monitor_cb, NULL);
    svc_monitor_force_refresh();

    ESP_LOGI(TAG, "Sysview created (%u tasks)", (unsigned)snap->task_count);
    (void)t;
    return s;
}

static void sysview_on_resume(lv_obj_t *screen, void *view_state, void *app_data)
{
    (void)screen; (void)app_data;
    ui_statusbar_set_title("PROCESSES");
    taskman_sysview_t *s = (taskman_sysview_t *)view_state;
    if (s) {
        populate_tasks(s, svc_monitor_get_snapshot());
        svc_monitor_force_refresh();
    }
}

static void sysview_on_destroy(lv_obj_t *screen, void *view_state, void *app_data)
{
    (void)screen; (void)app_data;
    g_sysview = NULL;
    svc_event_unregister(EVT_MONITOR_UPDATED, sysview_monitor_cb);
    free(view_state);
    ESP_LOGI(TAG, "Sysview destroyed");
}

const view_cbs_t taskman_sysview_cbs = {
    .on_create  = sysview_on_create,
    .on_resume  = sysview_on_resume,
    .on_pause   = NULL,
    .on_destroy = sysview_on_destroy,
};
