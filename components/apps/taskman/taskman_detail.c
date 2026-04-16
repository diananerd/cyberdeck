/*
 * CyberDeck — Task Manager: App Detail Screen (K2)
 *
 * Pushed from the Overview when the user taps an app row.
 * Receives a heap-allocated mon_app_entry_t via view_args_t (owned=true).
 *
 * Shows:
 *   STATE, LAUNCHED, VIEWS, HEAP DELTA, BG TASKS, STORAGE
 *
 * Updates STATE/VIEWS/HEAP/TASKS/LAUNCHED in place on EVT_MONITOR_UPDATED.
 * Auto-pops if the target app is no longer in the process list.
 *
 * Buttons:
 *   [RAISE TO FRONT] — calls ui_activity_raise(app_id) via lv_async_call
 *   [KILL]           — confirm dialog → ui_activity_close_app(app_id)
 */

#include "taskman_internal.h"
#include "app_registry.h"
#include "ui_activity.h"
#include "ui_engine.h"
#include "ui_theme.h"
#include "ui_statusbar.h"
#include "ui_common.h"
#include "ui_effect.h"
#include "svc_monitor.h"
#include "svc_event.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "taskman_detail";

/* =========================================================================
 * State
 * =========================================================================
 */

typedef struct {
    app_id_t  app_id;
    char      name[OS_PROCESS_NAME_LEN];
    /* Dynamic labels — updated in place on EVT_MONITOR_UPDATED */
    lv_obj_t *lbl_state;
    lv_obj_t *lbl_uptime;
    lv_obj_t *lbl_views;
    lv_obj_t *lbl_heap;
    lv_obj_t *lbl_tasks;
} taskman_detail_t;

static taskman_detail_t *g_detail = NULL;

/* =========================================================================
 * Update helper — refresh dynamic labels from snapshot
 * =========================================================================
 */

static void update_detail(taskman_detail_t *s, const sys_snapshot_t *snap)
{
    /* Find this app in the snapshot */
    const mon_app_entry_t *found = NULL;
    for (uint8_t i = 0; i < snap->app_count; i++) {
        if (snap->apps[i].app_id == s->app_id) {
            found = &snap->apps[i];
            break;
        }
    }

    if (!found) {
        /* App was terminated — auto-pop this detail screen */
        ESP_LOGI(TAG, "App id=%u no longer running, popping detail", (unsigned)s->app_id);
        ui_activity_pop();
        return;
    }

    lv_label_set_text(s->lbl_state, proc_state_str(found->state));

    char uptime_str[24];
    fmt_uptime(uptime_str, sizeof(uptime_str), found->uptime_s);
    lv_label_set_text(s->lbl_uptime, uptime_str);

    char views_str[8];
    snprintf(views_str, sizeof(views_str), "%u", (unsigned)found->view_count);
    lv_label_set_text(s->lbl_views, views_str);

    char heap_str[16];
    fmt_bytes(heap_str, sizeof(heap_str), found->heap_delta);
    char heap_buf[20];
    snprintf(heap_buf, sizeof(heap_buf), "~%s", heap_str);
    lv_label_set_text(s->lbl_heap, heap_buf);

    char tasks_str[8];
    snprintf(tasks_str, sizeof(tasks_str), "%u", (unsigned)found->bg_task_count);
    lv_label_set_text(s->lbl_tasks, tasks_str);
}

/* =========================================================================
 * Event handler
 * =========================================================================
 */

static void detail_monitor_cb(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg; (void)base; (void)id; (void)data;
    if (!g_detail) return;
    if (ui_lock(200)) {
        if (g_detail)
            update_detail(g_detail, svc_monitor_get_snapshot());
        ui_unlock();
    }
}

/* =========================================================================
 * RAISE TO FRONT
 * =========================================================================
 */

static void async_raise_app(void *arg)
{
    app_id_t target = (app_id_t)(uintptr_t)arg;
    if (!ui_activity_raise(target)) {
        /* App not in stack — just pop TaskMan overlay */
        ui_activity_pop_to_home();
    }
}

static void raise_btn_cb(lv_event_t *e)
{
    taskman_detail_t *s = (taskman_detail_t *)lv_event_get_user_data(e);
    if (!s) return;
    lv_async_call(async_raise_app, (void *)(uintptr_t)s->app_id);
}

/* =========================================================================
 * KILL
 * =========================================================================
 */

static app_id_t s_kill_target;

static void detail_kill_confirm_cb(bool confirmed, void *user_data)
{
    (void)user_data;
    if (confirmed) {
        ESP_LOGI(TAG, "Detail KILL confirmed for app_id=%u", (unsigned)s_kill_target);
        ui_activity_close_app(s_kill_target);
    }
}

static void detail_kill_btn_cb(lv_event_t *e)
{
    taskman_detail_t *s = (taskman_detail_t *)lv_event_get_user_data(e);
    if (!s) return;

    s_kill_target = s->app_id;
    char msg[96];
    snprintf(msg, sizeof(msg),
             "Terminate \"%s\"?\n"
             "Screen, state and all\n"
             "background tasks will\n"
             "be freed.", s->name);
    ui_effect_confirm("KILL APP", msg, detail_kill_confirm_cb, NULL);
}

/* =========================================================================
 * Lifecycle callbacks
 * =========================================================================
 */

static void *detail_on_create(lv_obj_t *screen, const view_args_t *args, void *app_data)
{
    (void)app_data;

    /* Extract app entry from args (owned=true → OS frees args->data after us) */
    const mon_app_entry_t *entry = (args && args->data) ?
                                   (const mon_app_entry_t *)args->data : NULL;

    taskman_detail_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    if (entry) {
        s->app_id = entry->app_id;
        strncpy(s->name, entry->name, sizeof(s->name) - 1);
    }

    ui_statusbar_set_title("PROCESSES");

    const cyberdeck_theme_t *t = ui_theme_get();
    lv_obj_t *content = ui_common_content_area(screen);

    /* App name as section header */
    lv_obj_t *title = lv_label_create(content);
    lv_label_set_text(title, s->name[0] ? s->name : "APP");
    ui_theme_style_label(title, &CYBERDECK_FONT_LG);
    lv_obj_set_style_pad_bottom(title, 8, 0);

    /* Data rows */
    s->lbl_state  = ui_common_data_row(content, "STATE:",       "");
    s->lbl_uptime = ui_common_data_row(content, "LAUNCHED:",    "");
    s->lbl_views  = ui_common_data_row(content, "VIEWS:",       "");
    s->lbl_heap   = ui_common_data_row(content, "HEAP DELTA:",  "");
    s->lbl_tasks  = ui_common_data_row(content, "BG TASKS:",    "");

    /* Storage path (static — from manifest) */
    const char *storage_path = "none";
    char storage_buf[72];
    const app_entry_t *reg_entry = app_registry_get_raw(s->app_id);
    if (reg_entry && reg_entry->manifest.storage_dir) {
        snprintf(storage_buf, sizeof(storage_buf),
                 "/sdcard/apps/%s/", reg_entry->manifest.storage_dir);
        storage_path = storage_buf;
    }
    ui_common_data_row(content, "STORAGE:", storage_path);

    /* Populate dynamic labels from current snapshot */
    const sys_snapshot_t *snap = svc_monitor_get_snapshot();
    const mon_app_entry_t *found = NULL;
    for (uint8_t i = 0; i < snap->app_count; i++) {
        if (snap->apps[i].app_id == s->app_id) {
            found = &snap->apps[i];
            break;
        }
    }
    if (found) {
        update_detail(s, snap);
    } else if (entry) {
        /* Snapshot not yet refreshed — use data from args */
        lv_label_set_text(s->lbl_state, proc_state_str(entry->state));
        char uptime_str[24];
        fmt_uptime(uptime_str, sizeof(uptime_str), entry->uptime_s);
        lv_label_set_text(s->lbl_uptime, uptime_str);
        char views_str[8];
        snprintf(views_str, sizeof(views_str), "%u", (unsigned)entry->view_count);
        lv_label_set_text(s->lbl_views, views_str);
        char heap_str[16];
        fmt_bytes(heap_str, sizeof(heap_str), entry->heap_delta);
        char heap_buf[20];
        snprintf(heap_buf, sizeof(heap_buf), "~%s", heap_str);
        lv_label_set_text(s->lbl_heap, heap_buf);
        char tasks_str[8];
        snprintf(tasks_str, sizeof(tasks_str), "%u", (unsigned)entry->bg_task_count);
        lv_label_set_text(s->lbl_tasks, tasks_str);
    }

    /* Spacer + action buttons */
    ui_common_spacer(content);

    lv_obj_t *btn_row = ui_common_action_row(content);

    lv_obj_t *raise_btn = ui_common_btn(btn_row, "RAISE TO FRONT");
    lv_obj_add_event_cb(raise_btn, raise_btn_cb, LV_EVENT_CLICKED, s);

    lv_obj_t *kill_btn = ui_common_btn(btn_row, "KILL");
    lv_obj_set_style_border_color(kill_btn, lv_color_hex(0xFF3333), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(kill_btn, 0),
                                lv_color_hex(0xFF3333), 0);
    lv_obj_set_style_bg_color(kill_btn, lv_color_hex(0xFF3333), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(kill_btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_event_cb(kill_btn, detail_kill_btn_cb, LV_EVENT_CLICKED, s);

    /* Subscribe */
    g_detail = s;
    svc_event_register(EVT_MONITOR_UPDATED, detail_monitor_cb, NULL);

    ESP_LOGI(TAG, "Detail created for app_id=%u (%s)",
             (unsigned)s->app_id, s->name);

    (void)t;
    return s;
}

static void detail_on_resume(lv_obj_t *screen, void *view_state, void *app_data)
{
    (void)screen; (void)app_data;
    ui_statusbar_set_title("PROCESSES");
    taskman_detail_t *s = (taskman_detail_t *)view_state;
    if (s) {
        update_detail(s, svc_monitor_get_snapshot());
        svc_monitor_force_refresh();
    }
}

static void detail_on_destroy(lv_obj_t *screen, void *view_state, void *app_data)
{
    (void)screen; (void)app_data;

    /* NULL global FIRST */
    g_detail = NULL;
    svc_event_unregister(EVT_MONITOR_UPDATED, detail_monitor_cb);

    free(view_state);
    ESP_LOGI(TAG, "Detail destroyed");
}

const view_cbs_t taskman_detail_cbs = {
    .on_create  = detail_on_create,
    .on_resume  = detail_on_resume,
    .on_pause   = NULL,
    .on_destroy = detail_on_destroy,
};
