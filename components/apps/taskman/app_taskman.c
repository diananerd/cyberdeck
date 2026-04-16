/*
 * CyberDeck — Task Manager: Overview Screen (K1)
 *
 * Event-driven overview that consumes svc_monitor snapshots.
 * No polling timer — updates fire on EVT_MONITOR_UPDATED (2 s default).
 *
 * Layout:
 *   MEMORY
 *     INT   [bar] XX%  XXK / XXK
 *     PSRAM [bar] XX%  X.XM / 4.0M
 *   RUNNING APPS
 *     [NAME]  STATE  Nv ~XK  Nt  [KILL]
 *     tap row → push detail (K2)
 *   SERVICES
 *     ● svc_wifi   192.168.1.5
 *     ○ svc_ota    idle
 *
 * Long press on MEMORY section → push sysview (K3, dev mode only).
 * Access: navbar □ button (EVT_NAV_PROCESSES → push on top of current stack).
 */

#include "app_taskman.h"
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
#include "os_process.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "taskman";

#define KILL_BTN_W   72
#define KILL_BTN_H   34
#define KILL_COLOR   0xFF3333
#define MEM_TAG_W    52    /* fixed width of "INT  " / "PSRAM" labels */
#define MEM_VAL_W    150   /* fixed width of "XX%  XXK / XXK" labels */

/* =========================================================================
 * Types
 * =========================================================================
 */

typedef struct {
    /* Memory section — static refs, updated in place (no rebuild) */
    lv_obj_t *bar_int;
    lv_obj_t *bar_psram;
    lv_obj_t *lbl_int;    /* "XX%  XXK / XXK" */
    lv_obj_t *lbl_psram;
    /* Dynamic sections — lv_obj_clean + repopulate on each update */
    lv_obj_t *apps_container;
    lv_obj_t *svcs_container;
    /* Suppress updates while confirm dialog is open */
    bool      confirm_open;
} taskman_overview_t;

typedef struct {
    taskman_overview_t *state;  /* back-ref; NULL when view is destroyed */
    mon_app_entry_t     entry;  /* snapshot copy for this row */
} app_row_ctx_t;

/* =========================================================================
 * Module-level state (static globals — safe because of confirm_open guard)
 * =========================================================================
 */

static taskman_overview_t *g_overview = NULL;
static app_row_ctx_t       s_app_ctxs[MON_MAX_APPS];

/* =========================================================================
 * Forward declarations
 * =========================================================================
 */

static void overview_monitor_cb(void *arg, esp_event_base_t base,
                                 int32_t id, void *data);
#if CONFIG_CYBERDECK_MONITOR_DEV_MODE
static void mem_longpress_cb(lv_event_t *e);
#endif

/* =========================================================================
 * Widget helpers
 * =========================================================================
 */

static lv_obj_t *make_row(lv_obj_t *parent, const cyberdeck_theme_t *t)
{
    lv_obj_t *row = lv_obj_create(parent);
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
    lv_obj_set_size(btn, KILL_BTN_W, KILL_BTN_H);
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

static void make_section_label(lv_obj_t *parent, const char *text)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    ui_theme_style_label_dim(lbl, &CYBERDECK_FONT_SM);
    lv_obj_set_style_pad_top(lbl, 12, 0);
    lv_obj_set_style_pad_bottom(lbl, 4, 0);
}

/* Create one memory bar row inside parent. Outputs bar + value label refs. */
static void make_mem_bar_row(lv_obj_t *parent, const char *tag_str,
                              lv_obj_t **bar_out, lv_obj_t **lbl_out,
                              const cyberdeck_theme_t *t)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* Fixed-width tag label */
    lv_obj_t *tag = lv_label_create(row);
    lv_label_set_text(tag, tag_str);
    ui_theme_style_label_dim(tag, &CYBERDECK_FONT_SM);
    lv_obj_set_width(tag, MEM_TAG_W);

    /* Flex-grow bar */
    lv_obj_t *bar = lv_bar_create(row);
    lv_obj_set_flex_grow(bar, 1);
    lv_obj_set_height(bar, 8);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, t->primary_dim, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, t->primary, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 0, LV_PART_INDICATOR);

    /* Fixed-width value label */
    lv_obj_t *val = lv_label_create(row);
    lv_label_set_text(val, "");
    ui_theme_style_label_dim(val, &CYBERDECK_FONT_SM);
    lv_obj_set_width(val, MEM_VAL_W);

    *bar_out = bar;
    *lbl_out = val;
}

static void update_mem_bar(lv_obj_t *bar, lv_obj_t *lbl,
                            size_t free_bytes, size_t total_bytes)
{
    int pct = (total_bytes > 0u) ?
              (int)(((total_bytes - free_bytes) * 100u) / total_bytes) : 0;
    lv_bar_set_value(bar, pct, LV_ANIM_OFF);

    char free_str[16], total_str[16], buf[40];
    fmt_bytes(free_str, sizeof(free_str), free_bytes);
    fmt_bytes(total_str, sizeof(total_str), total_bytes);
    snprintf(buf, sizeof(buf), "%d%%  %s / %s", pct, free_str, total_str);
    lv_label_set_text(lbl, buf);
}

/* =========================================================================
 * Kill confirm (overview)
 * =========================================================================
 */

static void kill_confirm_cb(bool confirmed, void *user_data)
{
    app_row_ctx_t *ctx = (app_row_ctx_t *)user_data;
    if (!ctx) return;

    if (confirmed) {
        ESP_LOGI(TAG, "KILL confirmed for app_id=%u", (unsigned)ctx->entry.app_id);
        ui_activity_close_app(ctx->entry.app_id);
        /* TaskMan itself may be closed as a side-effect — don't touch state */
        return;
    }

    /* Cancelled: resume updates */
    if (ctx->state) {
        ctx->state->confirm_open = false;
    }
}

static void kill_btn_cb(lv_event_t *e)
{
    app_row_ctx_t *ctx = (app_row_ctx_t *)lv_event_get_user_data(e);
    if (!ctx || !ctx->state) return;

    ctx->state->confirm_open = true;

    char msg[96];
    snprintf(msg, sizeof(msg),
             "Terminate \"%s\"?\n"
             "Screen, state and all\n"
             "background tasks will\n"
             "be freed.", ctx->entry.name);
    ui_effect_confirm("KILL APP", msg, kill_confirm_cb, ctx);
}

/* =========================================================================
 * Row tap → push detail
 * =========================================================================
 */

static void row_tap_cb(lv_event_t *e)
{
    app_row_ctx_t *ctx = (app_row_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;

    mon_app_entry_t *arg = malloc(sizeof(mon_app_entry_t));
    if (!arg) return;
    *arg = ctx->entry;

    view_args_t args = { .data = arg, .size = sizeof(*arg), .owned = true };
    ui_activity_push(APP_ID_TASKMAN, TASKMAN_SCR_DETAIL, &taskman_detail_cbs, &args);
}

/* =========================================================================
 * Populate helpers
 * =========================================================================
 */

static void populate_memory(taskman_overview_t *s, const sys_snapshot_t *snap)
{
    update_mem_bar(s->bar_int,   s->lbl_int,
                   snap->heap_internal_free, snap->heap_internal_total);
    update_mem_bar(s->bar_psram, s->lbl_psram,
                   snap->heap_psram_free,    snap->heap_psram_total);
}

static void populate_apps(taskman_overview_t *s, const sys_snapshot_t *snap)
{
    const cyberdeck_theme_t *t = ui_theme_get();
    lv_obj_clean(s->apps_container);

    if (snap->app_count == 0) {
        lv_obj_t *empty = lv_label_create(s->apps_container);
        lv_label_set_text(empty, "No apps running");
        ui_theme_style_label_dim(empty, &CYBERDECK_FONT_SM);
        return;
    }

    uint8_t ctx_idx = 0;
    for (uint8_t i = 0; i < snap->app_count; i++) {
        if (ctx_idx >= MON_MAX_APPS) break;
        const mon_app_entry_t *app = &snap->apps[i];
        bool is_self = (app->app_id == APP_ID_TASKMAN);

        /* Store snapshot copy in static context slot */
        s_app_ctxs[ctx_idx].state = s;
        s_app_ctxs[ctx_idx].entry = *app;

        lv_obj_t *row = make_row(s->apps_container, t);

        /* Make tappable (push detail) for non-self apps */
        if (!is_self) {
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICK_FOCUSABLE);
            lv_obj_set_style_bg_color(row, t->primary, LV_STATE_PRESSED);
            lv_obj_set_style_bg_opa(row, LV_OPA_20, LV_STATE_PRESSED);
            lv_obj_add_event_cb(row, row_tap_cb, LV_EVENT_CLICKED,
                                &s_app_ctxs[ctx_idx]);
        }

        lv_obj_t *col = make_info_col(row);

        /* App name */
        lv_obj_t *name_lbl = lv_label_create(col);
        lv_label_set_text(name_lbl, app->name[0] ? app->name : "APP");
        lv_obj_set_style_text_color(name_lbl, t->primary, 0);
        lv_obj_set_style_text_font(name_lbl, &CYBERDECK_FONT_MD, 0);

        /* State + resource line */
        char heap_str[12];
        fmt_bytes(heap_str, sizeof(heap_str), app->heap_delta);
        char res[56];
        snprintf(res, sizeof(res), "%s  %uv  ~%s  %ut",
                 proc_state_str(app->state),
                 (unsigned)app->view_count,
                 heap_str,
                 (unsigned)app->bg_task_count);
        lv_obj_t *res_lbl = lv_label_create(col);
        lv_label_set_text(res_lbl, res);
        ui_theme_style_label_dim(res_lbl, &CYBERDECK_FONT_SM);

        /* KILL button only for non-self apps */
        if (!is_self) {
            make_kill_btn(row, t, kill_btn_cb, &s_app_ctxs[ctx_idx]);
        }

        ctx_idx++;
    }
}

static void populate_services(taskman_overview_t *s, const sys_snapshot_t *snap)
{
    const cyberdeck_theme_t *t = ui_theme_get();
    lv_obj_clean(s->svcs_container);

    if (snap->service_count == 0) {
        lv_obj_t *empty = lv_label_create(s->svcs_container);
        lv_label_set_text(empty, "No services registered");
        ui_theme_style_label_dim(empty, &CYBERDECK_FONT_SM);
        return;
    }

    for (uint8_t i = 0; i < snap->service_count; i++) {
        const mon_service_entry_t *svc = &snap->services[i];

        lv_obj_t *row = lv_obj_create(s->svcs_container);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_pad_ver(row, 4, 0);
        lv_obj_set_style_pad_column(row, 8, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        /* Colored bullet indicator */
        lv_obj_t *dot = lv_label_create(row);
        lv_label_set_text(dot, LV_SYMBOL_BULLET);
        lv_color_t dot_color;
        switch (svc->state) {
            case SVC_STATE_RUNNING: dot_color = t->primary;           break;
            case SVC_STATE_ERROR:   dot_color = lv_color_hex(0xFF3333); break;
            default:                dot_color = t->primary_dim;       break;
        }
        lv_obj_set_style_text_color(dot, dot_color, 0);
        lv_obj_set_style_text_font(dot, &CYBERDECK_FONT_SM, 0);

        /* Service name (fixed width so status text aligns) */
        lv_obj_t *name_lbl = lv_label_create(row);
        lv_label_set_text(name_lbl, svc->name);
        ui_theme_style_label_dim(name_lbl, &CYBERDECK_FONT_SM);
        lv_obj_set_width(name_lbl, 160);

        /* Status text (if any) */
        if (svc->status_text[0] != '\0') {
            lv_obj_t *status = lv_label_create(row);
            lv_label_set_text(status, svc->status_text);
            lv_obj_set_style_text_color(status, t->text_dim, 0);
            lv_obj_set_style_text_font(status, &CYBERDECK_FONT_SM, 0);
        }
    }
}

/* Full update from latest snapshot */
static void update_overview(taskman_overview_t *s)
{
    if (!s || s->confirm_open) return;
    const sys_snapshot_t *snap = svc_monitor_get_snapshot();
    populate_memory(s, snap);
    populate_apps(s, snap);
    populate_services(s, snap);
}

/* =========================================================================
 * Event handler — runs on ESP event-loop task
 * =========================================================================
 */

static void overview_monitor_cb(void *arg, esp_event_base_t base,
                                 int32_t id, void *data)
{
    (void)arg; (void)base; (void)id; (void)data;
    if (!g_overview) return;
    if (ui_lock(200)) {
        if (g_overview)
            update_overview(g_overview);
        ui_unlock();
    }
}

/* =========================================================================
 * Long press on memory area → push sysview (K3)
 * Only compiled when dev mode is enabled to avoid -Wunused-function.
 * =========================================================================
 */

#if CONFIG_CYBERDECK_MONITOR_DEV_MODE
static void mem_longpress_cb(lv_event_t *e)
{
    (void)e;
    view_args_t args = { .data = NULL, .size = 0, .owned = false };
    ui_activity_push(APP_ID_TASKMAN, TASKMAN_SCR_SYSVIEW, &taskman_sysview_cbs, &args);
}
#endif /* CONFIG_CYBERDECK_MONITOR_DEV_MODE */

/* =========================================================================
 * Activity lifecycle callbacks
 * =========================================================================
 */

static void *overview_on_create(lv_obj_t *screen, const view_args_t *args, void *app_data)
{
    (void)args; (void)app_data;
    ui_statusbar_set_title("PROCESSES");

    taskman_overview_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    const cyberdeck_theme_t *t = ui_theme_get();
    lv_obj_t *content = ui_common_content_area(screen);

    /* ---- MEMORY section ---- */
    make_section_label(content, "MEMORY");

    /* Container for bar rows */
    lv_obj_t *mem_box = lv_obj_create(content);
    lv_obj_set_width(mem_box, LV_PCT(100));
    lv_obj_set_height(mem_box, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(mem_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mem_box, 0, 0);
    lv_obj_set_style_pad_all(mem_box, 0, 0);
    lv_obj_set_style_pad_row(mem_box, 6, 0);
    lv_obj_clear_flag(mem_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(mem_box, LV_FLEX_FLOW_COLUMN);

    make_mem_bar_row(mem_box, "INT  ", &s->bar_int,   &s->lbl_int,   t);
    make_mem_bar_row(mem_box, "PSRAM", &s->bar_psram, &s->lbl_psram, t);

    /* Long press → sysview (dev mode only) */
#if CONFIG_CYBERDECK_MONITOR_DEV_MODE
    lv_obj_add_flag(mem_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(mem_box, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_event_cb(mem_box, mem_longpress_cb, LV_EVENT_LONG_PRESSED, s);
#endif

    /* ---- RUNNING APPS section ---- */
    ui_common_section_gap(content);
    make_section_label(content, "RUNNING APPS");

    s->apps_container = lv_obj_create(content);
    lv_obj_set_width(s->apps_container, LV_PCT(100));
    lv_obj_set_height(s->apps_container, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s->apps_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s->apps_container, 0, 0);
    lv_obj_set_style_pad_all(s->apps_container, 0, 0);
    lv_obj_clear_flag(s->apps_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s->apps_container, LV_FLEX_FLOW_COLUMN);

    /* ---- SERVICES section ---- */
    ui_common_section_gap(content);
    make_section_label(content, "SERVICES");

    s->svcs_container = lv_obj_create(content);
    lv_obj_set_width(s->svcs_container, LV_PCT(100));
    lv_obj_set_height(s->svcs_container, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s->svcs_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s->svcs_container, 0, 0);
    lv_obj_set_style_pad_all(s->svcs_container, 0, 0);
    lv_obj_clear_flag(s->svcs_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s->svcs_container, LV_FLEX_FLOW_COLUMN);

    /* Initial populate from current snapshot */
    const sys_snapshot_t *snap = svc_monitor_get_snapshot();
    populate_memory(s, snap);
    populate_apps(s, snap);
    populate_services(s, snap);

    /* Subscribe — set g_overview BEFORE registering to avoid race */
    g_overview = s;
    svc_event_register(EVT_MONITOR_UPDATED, overview_monitor_cb, NULL);
    svc_monitor_force_refresh();

    ESP_LOGI(TAG, "Overview created");
    return s;
}

static void overview_on_resume(lv_obj_t *screen, void *view_state, void *app_data)
{
    (void)screen; (void)app_data;
    ui_statusbar_set_title("PROCESSES");
    taskman_overview_t *s = (taskman_overview_t *)view_state;
    if (s) svc_monitor_force_refresh();
}

static void overview_on_destroy(lv_obj_t *screen, void *view_state, void *app_data)
{
    (void)screen; (void)app_data;

    /* NULL global FIRST — prevents handler from accessing freed state */
    g_overview = NULL;
    for (uint8_t i = 0; i < MON_MAX_APPS; i++) {
        s_app_ctxs[i].state = NULL;
    }
    svc_event_unregister(EVT_MONITOR_UPDATED, overview_monitor_cb);

    free(view_state);
    ESP_LOGI(TAG, "Overview destroyed");
}

static const view_cbs_t s_overview_cbs = {
    .on_create  = overview_on_create,
    .on_resume  = overview_on_resume,
    .on_pause   = NULL,
    .on_destroy = overview_on_destroy,
};

/* =========================================================================
 * Registration
 * =========================================================================
 */

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
    os_app_register(&manifest, NULL, &s_overview_cbs);
    ESP_LOGI(TAG, "Task Manager registered");
    return ESP_OK;
}
