/*
 * CyberDeck — Task Manager app (Fase 6)
 *
 * Vista unificada del estado de proceso de cada app:
 *
 *  OPEN APPS — apps en la activity stack (depth 1+, sin launcher).
 *              Por app_id muestra: nombre, pantallas en stack, heap ~estimado,
 *              y tareas FreeRTOS propias (owner == app_id, is_killable).
 *              Botón KILL → confirm → ui_activity_close_app(app_id).
 *              El close hook en app_manager se encarga de matar las tasks y
 *              llamar app_ops.on_terminate — todo el ciclo de vida OS completo.
 *
 *  SYSTEM TASKS — tasks FreeRTOS con owner == OS_OWNER_SYSTEM (!is_killable).
 *                 Solo informativas — no tienen botón KILL.
 *
 * Principio: la UI aquí es solo un cliente de dos fuentes de datos:
 *   - ui_activity_list()  → estado de pantallas LVGL por app
 *   - os_task_list()      → estado de tasks FreeRTOS por app
 * La lógica de cierre vive en el OS (app_manager close hook), no aquí.
 *
 * Auto-refresh cada 2 s. Timer pausado mientras hay un confirm dialog abierto.
 * Acceso: navbar □ button (EVT_NAV_PROCESSES → app_manager_launch).
 */

#include "app_taskman.h"
#include "app_registry.h"
#include "ui_activity.h"
#include "ui_theme.h"
#include "ui_statusbar.h"
#include "ui_common.h"
#include "ui_effect.h"
#include "os_task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "taskman";

#define REFRESH_MS    2000
#define ACTION_BTN_W  72
#define ACTION_BTN_H  34
#define KILL_COLOR    0xFF3333

/* ---- State ---- */

typedef struct {
    lv_obj_t   *list;
    lv_timer_t *timer;
    bool        confirm_open;
} taskman_state_t;

/* Snapshots — safe because timer is paused while confirm_open=true */
static activity_info_t   s_apps[ACTIVITY_STACK_MAX];
static os_process_info_t s_tasks[OS_MAX_TASKS];

typedef struct {
    taskman_state_t *s;
    app_id_t         app_id;
    char             label[OS_TASK_NAME_LEN];
} app_ctx_t;

static app_ctx_t s_app_ctxs[ACTIVITY_STACK_MAX];

/* Forward declaration */
static void taskman_populate(taskman_state_t *s);

/* ---- Confirm callback for KILL APP ---- */

static void kill_app_confirm_cb(bool confirmed, void *user_data)
{
    app_ctx_t *ctx = (app_ctx_t *)user_data;
    if (!ctx) return;

    if (confirmed) {
        ESP_LOGI(TAG, "Killing app id=%u", (unsigned)ctx->app_id);
        /* ui_activity_close_app schedules an async LVGL close; the registered
         * close hook in app_manager will then call os_task_destroy_all_for_app
         * and app_ops.on_terminate — completing the full OS-level termination. */
        ui_activity_close_app(ctx->app_id);
        /* Do not repopulate: if TaskMan itself is above the closed app it will
         * also be destroyed by close_app_async. */
        return;
    }

    /* Cancelled: resume timer */
    if (ctx->s) {
        ctx->s->confirm_open = false;
        if (ctx->s->timer) lv_timer_resume(ctx->s->timer);
    }
}

/* ---- Button callback ---- */

static void kill_app_btn_cb(lv_event_t *e)
{
    app_ctx_t *ctx = (app_ctx_t *)lv_event_get_user_data(e);
    if (!ctx || !ctx->s) return;

    ctx->s->confirm_open = true;
    if (ctx->s->timer) lv_timer_pause(ctx->s->timer);

    char msg[96];
    snprintf(msg, sizeof(msg),
             "Terminate \"%s\"?\n"
             "Screen, state and all\n"
             "background tasks will\n"
             "be freed.", ctx->label);
    ui_effect_confirm("KILL APP", msg, kill_app_confirm_cb, ctx);
}

/* ---- Row / widget helpers ---- */

static lv_obj_t *make_row(lv_obj_t *list, const cyberdeck_theme_t *t)
{
    lv_obj_t *row = lv_obj_create(list);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, t->primary_dim, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_pad_ver(row, 10, 0);
    lv_obj_set_style_pad_hor(row, 8, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return row;
}

static lv_obj_t *make_info_col(lv_obj_t *row)
{
    lv_obj_t *col = lv_obj_create(row);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_pad_all(col, 0, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 2, 0);
    return col;
}

static void make_kill_btn(lv_obj_t *row, const cyberdeck_theme_t *t,
                          lv_event_cb_t cb, void *ctx)
{
    lv_obj_t *btn = lv_obj_create(row);
    lv_obj_set_size(btn, ACTION_BTN_W, ACTION_BTN_H);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(KILL_COLOR), 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(btn, lv_color_hex(KILL_COLOR), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, "KILL");
    lv_obj_set_style_text_color(lbl, lv_color_hex(KILL_COLOR), 0);
    lv_obj_set_style_text_color(lbl, t->bg_dark, LV_STATE_PRESSED);
    lv_obj_set_style_text_font(lbl, &CYBERDECK_FONT_SM, 0);

    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, ctx);
}

static void make_section_label(lv_obj_t *list, const char *text,
                               const cyberdeck_theme_t *t)
{
    lv_obj_t *lbl = lv_label_create(list);
    lv_label_set_text(lbl, text);
    ui_theme_style_label_dim(lbl, &CYBERDECK_FONT_SM);
    lv_obj_set_style_pad_top(lbl, 12, 0);
    lv_obj_set_style_pad_bottom(lbl, 4, 0);
    (void)t;
}

/* ---- Main populate ---- */

static void taskman_populate(taskman_state_t *s)
{
    const cyberdeck_theme_t *t = ui_theme_get();
    lv_obj_clean(s->list);

    /* Snapshot both data sources */
    uint8_t app_count  = ui_activity_list(s_apps,  ACTIVITY_STACK_MAX);
    uint8_t task_count = os_task_list(s_tasks, OS_MAX_TASKS);

    /* ---- Header: free heap ---- */
    {
        size_t free_int  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        char mem_buf[56];
        snprintf(mem_buf, sizeof(mem_buf), "FREE: %uK INT  %uK PSRAM",
                 (unsigned)(free_int  / 1024),
                 (unsigned)(free_psram / 1024));
        lv_obj_t *mem_lbl = lv_label_create(s->list);
        lv_label_set_text(mem_lbl, mem_buf);
        ui_theme_style_label_dim(mem_lbl, &CYBERDECK_FONT_SM);
        lv_obj_set_style_pad_bottom(mem_lbl, 8, 0);
    }

    /* ================================================================
     * SECTION 1: OPEN APPS
     * Per app_id: screens in stack + background tasks owned by that app.
     * KILL button triggers full OS-level termination via close hook.
     * ================================================================ */
    make_section_label(s->list, "OPEN APPS", t);

    /* Build unique user-app list (skip launcher at stack_idx==0) */
    app_id_t shown_ids[ACTIVITY_STACK_MAX];
    uint8_t  shown_count = 0;
    for (uint8_t i = 0; i < app_count; i++) {
        if (s_apps[i].stack_idx == 0) continue;
        app_id_t aid = s_apps[i].app_id;
        bool dup = false;
        for (uint8_t j = 0; j < shown_count; j++) if (shown_ids[j] == aid) { dup = true; break; }
        if (!dup && shown_count < ACTIVITY_STACK_MAX) shown_ids[shown_count++] = aid;
    }

    if (shown_count == 0) {
        lv_obj_t *empty = lv_label_create(s->list);
        lv_label_set_text(empty, "No apps open");
        ui_theme_style_label_dim(empty, &CYBERDECK_FONT_SM);
    } else {
        for (uint8_t ai = 0; ai < shown_count; ai++) {
            app_id_t aid = shown_ids[ai];

            const char *app_name = "APP";
            const app_entry_t *entry = app_registry_get_raw(aid);
            if (entry && entry->name) app_name = entry->name;

            /* Aggregate screens + heap across all sub-screens of this app */
            uint8_t screen_count = 0;
            size_t  total_heap   = 0;
            for (uint8_t k = 0; k < app_count; k++) {
                if (s_apps[k].app_id == aid && s_apps[k].stack_idx > 0) {
                    screen_count++;
                    total_heap += s_apps[k].heap_used;
                }
            }

            /* Count FreeRTOS tasks owned by this app */
            uint8_t bg_tasks = 0;
            for (uint8_t ti = 0; ti < task_count; ti++) {
                if (s_tasks[ti].owner == aid && s_tasks[ti].is_killable) bg_tasks++;
            }

            lv_obj_t *row = make_row(s->list, t);
            lv_obj_t *col = make_info_col(row);

            /* App name */
            lv_obj_t *name_lbl = lv_label_create(col);
            lv_label_set_text(name_lbl, app_name);
            lv_obj_set_style_text_color(name_lbl, t->primary, 0);
            lv_obj_set_style_text_font(name_lbl, &CYBERDECK_FONT_MD, 0);

            /* Resource line: screens + heap + bg tasks */
            char res_buf[72];
            int  pos = snprintf(res_buf, sizeof(res_buf),
                                "%u screen%s  ~%uK",
                                screen_count, screen_count != 1 ? "s" : "",
                                (unsigned)(total_heap / 1024));
            if (bg_tasks > 0 && pos > 0 && (size_t)pos < sizeof(res_buf)) {
                snprintf(res_buf + pos, sizeof(res_buf) - (size_t)pos,
                         "  +  %u task%s", bg_tasks, bg_tasks != 1 ? "s" : "");
            }
            lv_obj_t *res_lbl = lv_label_create(col);
            lv_label_set_text(res_lbl, res_buf);
            ui_theme_style_label_dim(res_lbl, &CYBERDECK_FONT_SM);

            /* Context for confirm dialog */
            s_app_ctxs[ai].s      = s;
            s_app_ctxs[ai].app_id = aid;
            strncpy(s_app_ctxs[ai].label, app_name, sizeof(s_app_ctxs[ai].label) - 1);
            s_app_ctxs[ai].label[sizeof(s_app_ctxs[ai].label) - 1] = '\0';

            make_kill_btn(row, t, kill_app_btn_cb, &s_app_ctxs[ai]);
        }
    }

    /* ================================================================
     * SECTION 2: SYSTEM TASKS
     * Tasks owned by OS_OWNER_SYSTEM — informational only, no KILL.
     * ================================================================ */
    ui_common_section_gap(s->list);
    make_section_label(s->list, "SYSTEM TASKS", t);

    uint8_t sys_count = 0;
    for (uint8_t i = 0; i < task_count; i++) {
        if (s_tasks[i].is_killable) continue;  /* app-owned — shown above */
        sys_count++;

        lv_obj_t *row = make_row(s->list, t);
        lv_obj_t *col = make_info_col(row);

        lv_obj_t *name_lbl = lv_label_create(col);
        lv_label_set_text(name_lbl, s_tasks[i].name);
        lv_obj_set_style_text_color(name_lbl, t->text_dim, 0);
        lv_obj_set_style_text_font(name_lbl, &CYBERDECK_FONT_MD, 0);

        char stats[56];
        snprintf(stats, sizeof(stats), "STK: %luW  PRIO: %u  CORE: %u",
                 (unsigned long)s_tasks[i].stack_high_water,
                 (unsigned)s_tasks[i].priority,
                 (unsigned)s_tasks[i].core);
        lv_obj_t *stats_lbl = lv_label_create(col);
        lv_label_set_text(stats_lbl, stats);
        ui_theme_style_label_dim(stats_lbl, &CYBERDECK_FONT_SM);

        lv_obj_t *sys_tag = lv_label_create(row);
        lv_label_set_text(sys_tag, "[SYS]");
        ui_theme_style_label_dim(sys_tag, &CYBERDECK_FONT_SM);
    }

    if (sys_count == 0) {
        lv_obj_t *empty = lv_label_create(s->list);
        lv_label_set_text(empty, "No system tasks");
        ui_theme_style_label_dim(empty, &CYBERDECK_FONT_SM);
    }
}

/* ---- Timer callback ---- */

static void refresh_cb(lv_timer_t *timer)
{
    taskman_state_t *s = (taskman_state_t *)timer->user_data;
    if (!s || !s->list || s->confirm_open) return;
    taskman_populate(s);
}

/* ---- Activity callbacks (D1) ---- */

static void *taskman_on_create(lv_obj_t *screen, const view_args_t *args)
{
    (void)args;
    ui_statusbar_set_title("PROCESSES");

    taskman_state_t *s = lv_mem_alloc(sizeof(taskman_state_t));
    if (!s) return NULL;
    s->list         = NULL;
    s->timer        = NULL;
    s->confirm_open = false;

    lv_obj_t *content = ui_common_content_area(screen);
    s->list = ui_common_list(content);
    taskman_populate(s);

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
        .permissions = APP_PERM_HIDE_LAUNCHER,
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
