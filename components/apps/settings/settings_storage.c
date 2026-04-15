/*
 * CyberDeck — Settings > Storage
 *
 * Shows SD card status and space breakdown.
 * Subscribes to EVT_SDCARD_MOUNTED / EVT_SDCARD_UNMOUNTED so the UI
 * updates in real-time when the card is inserted or removed.
 *
 * B4: uses os_event_subscribe_ui — handler runs in LVGL task, no manual lock.
 *     Global pointer guard kept for async safety (format_done_async races).
 * D1: on_create returns state*.
 */

#include "app_settings.h"
#include "ui_activity.h"
#include "ui_engine.h"
#include "ui_theme.h"
#include "ui_statusbar.h"
#include "ui_common.h"
#include "ui_effect.h"
#include "hal_sdcard.h"
#include "app_state.h"
#include "os_event.h"
#include "os_task.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

static const char *TAG = "settings_storage";

/* ================================================================
 * Screen state
 * ================================================================ */

typedef struct {
    lv_obj_t   *content;     /* content area — cleaned + rebuilt on every refresh */
    event_sub_t sub_mounted;
    event_sub_t sub_unmounted;
} storage_scr_state_t;

/* Global pointer guard — needed by format_done_async to detect stale state */
static storage_scr_state_t *g_storage_scr_state = NULL;

/* ================================================================
 * Helpers
 * ================================================================ */

static void format_kb(char *buf, size_t n, uint32_t kb)
{
    if (kb >= 1024)
        snprintf(buf, n, "%lu MB", (unsigned long)(kb / 1024));
    else
        snprintf(buf, n, "%lu KB", (unsigned long)kb);
}

/* ================================================================
 * Content builder — called on create, resume, and SD events
 * ================================================================ */

static void mount_btn_cb(lv_event_t *e);
static void format_btn_cb(lv_event_t *e);

static void build_content(storage_scr_state_t *s)
{
    lv_obj_clean(s->content);

    bool mounted = hal_sdcard_is_mounted();

    lv_obj_t *status_val = ui_common_data_row(s->content, "STATUS:",
                                               mounted ? "MOUNTED" : "NOT MOUNTED");
    if (!mounted) {
        const cyberdeck_theme_t *t = ui_theme_get();
        lv_obj_set_style_text_color(status_val, t->text_dim, 0);
    }

    if (mounted) {
        ui_common_data_row(s->content, "MOUNT POINT:", HAL_SDCARD_MOUNT_POINT);

        const cyberdeck_state_t *st = app_state_get();
        uint32_t total_kb = st->sd_total_kb;
        uint32_t used_kb  = st->sd_used_kb;

        if (total_kb > 0) {
            uint32_t free_kb = total_kb - used_kb;
            char buf[20];

            format_kb(buf, sizeof(buf), total_kb);
            ui_common_data_row(s->content, "TOTAL:", buf);

            format_kb(buf, sizeof(buf), used_kb);
            ui_common_data_row(s->content, "USED:", buf);

            format_kb(buf, sizeof(buf), free_kb);
            ui_common_data_row(s->content, "FREE:", buf);

            char pct[12];
            snprintf(pct, sizeof(pct), "%lu%%",
                     (unsigned long)((used_kb * 100UL) / total_kb));
            ui_common_data_row(s->content, "USAGE:", pct);
        }
    } else {
        lv_obj_t *hint = lv_label_create(s->content);
        lv_label_set_text(hint, "Insert SD card to view details");
        ui_theme_style_label_dim(hint, &CYBERDECK_FONT_SM);
    }

    ui_common_spacer(s->content);

    lv_obj_t *btn_row = ui_common_action_row(s->content);

    if (mounted) {
        lv_obj_t *unmount_btn = ui_common_btn(btn_row, "UNMOUNT");
        lv_obj_add_event_cb(unmount_btn, mount_btn_cb, LV_EVENT_CLICKED, s);

        lv_obj_t *format_btn = ui_common_btn(btn_row, "FORMAT");
        ui_common_btn_style_primary(format_btn);
        lv_obj_add_event_cb(format_btn, format_btn_cb, LV_EVENT_CLICKED, s);
    } else {
        lv_obj_t *mount_btn = ui_common_btn(btn_row, "MOUNT SD CARD");
        ui_common_btn_style_primary(mount_btn);
        lv_obj_add_event_cb(mount_btn, mount_btn_cb, LV_EVENT_CLICKED, s);
    }
}

/* ================================================================
 * Mount / Unmount button
 * ================================================================ */

static void mount_btn_cb(lv_event_t *e)
{
    storage_scr_state_t *s = (storage_scr_state_t *)lv_event_get_user_data(e);
    if (!s) return;

    if (hal_sdcard_is_mounted()) {
        if (hal_sdcard_unmount() == ESP_OK) {
            ui_effect_toast("SD card unmounted", 1500);
        } else {
            ui_effect_toast("Unmount failed", 1500);
            return;
        }
    } else {
        if (hal_sdcard_mount() == ESP_OK) {
            ui_effect_toast("SD card mounted", 1500);
        } else {
            ui_effect_toast("Mount failed - no card?", 2000);
            return;
        }
    }

    build_content(s);
}

/* Context passed from do_format → format_task → format_done_async */
typedef struct {
    esp_err_t            result;
    storage_scr_state_t *scr_state;
} format_result_t;

/* Called on the LVGL task via lv_async_call */
static void format_done_async(void *param)
{
    format_result_t *fr = (format_result_t *)param;
    ui_effect_progress_hide();

    if (fr->result == ESP_OK) {
        ui_effect_toast("Format complete", 1500);
    } else {
        ui_effect_toast("Format failed", 2000);
    }

    /* Guard: screen may have been destroyed while format was running */
    if (fr->scr_state && g_storage_scr_state == fr->scr_state)
        build_content(fr->scr_state);

    lv_mem_free(fr);
}

static void format_task(void *arg)
{
    format_result_t *fr = (format_result_t *)arg;
    fr->result = hal_sdcard_format();
    lv_async_call(format_done_async, fr);
    vTaskDelete(NULL);
}

static void do_format(bool confirmed, void *ctx)
{
    if (!confirmed) return;

    format_result_t *fr = (format_result_t *)lv_mem_alloc(sizeof(format_result_t));
    if (!fr) return;
    fr->result    = ESP_FAIL;
    fr->scr_state = (storage_scr_state_t *)ctx;

    ui_effect_progress_show("FORMATTING SD CARD...", false, NULL, NULL);

    os_task_config_t cfg = {
        .name       = "sd_format",
        .fn         = format_task,
        .arg        = fr,
        .stack_size = 4096,
        .priority   = OS_PRIO_MEDIUM,
        .core       = OS_CORE_BG,
        .owner      = APP_ID_SETTINGS,
    };
    if (os_task_create(&cfg, NULL) != ESP_OK) {
        lv_mem_free(fr);
        ui_effect_progress_hide();
        ui_effect_toast("Format failed", 2000);
    }
}

static void format_btn_cb(lv_event_t *e)
{
    storage_scr_state_t *s = (storage_scr_state_t *)lv_event_get_user_data(e);
    ui_effect_confirm("FORMAT SD CARD",
                      "All data will be erased.\nThis cannot be undone.",
                      do_format, s);
}

/* ================================================================
 * SD card event handlers (B4: delivered in LVGL task by os_event_subscribe_ui)
 * ================================================================ */

static void sdcard_event_handler(void *arg, esp_event_base_t base,
                                  int32_t id, void *data)
{
    (void)base; (void)id; (void)data;
    /* Handler executes in LVGL task — no manual lock needed */
    storage_scr_state_t *s = (storage_scr_state_t *)arg;
    if (s && s == g_storage_scr_state)
        build_content(s);
}

/* ================================================================
 * Activity callbacks (D1)
 * ================================================================ */

static void *storage_on_create(lv_obj_t *screen, const view_args_t *args)
{
    (void)args;

    storage_scr_state_t *s =
        (storage_scr_state_t *)lv_mem_alloc(sizeof(storage_scr_state_t));
    if (!s) return NULL;
    s->content      = NULL;
    s->sub_mounted   = EVENT_SUB_INVALID;
    s->sub_unmounted = EVENT_SUB_INVALID;

    g_storage_scr_state = s;

    /* B4: os_event_subscribe_ui — delivers in LVGL task, no manual lock */
    s->sub_mounted   = os_event_subscribe_ui(APP_ID_SETTINGS, EVT_SDCARD_MOUNTED,
                                              sdcard_event_handler, s);
    s->sub_unmounted = os_event_subscribe_ui(APP_ID_SETTINGS, EVT_SDCARD_UNMOUNTED,
                                              sdcard_event_handler, s);

    ui_statusbar_set_title("SETTINGS");
    s->content = ui_common_content_area(screen);

    build_content(s);

    ESP_LOGI(TAG, "Storage screen created");
    return s;
}

static void storage_on_resume(lv_obj_t *screen, void *state)
{
    (void)screen;
    storage_scr_state_t *s = (storage_scr_state_t *)state;
    if (!s) return;
    build_content(s);
}

static void storage_on_destroy(lv_obj_t *screen, void *state)
{
    (void)screen;
    storage_scr_state_t *s = (storage_scr_state_t *)state;
    g_storage_scr_state = NULL;  /* clear guard before unsubscribe */
    os_event_unsubscribe(s->sub_mounted);
    os_event_unsubscribe(s->sub_unmounted);
    lv_mem_free(s);
    ESP_LOGI(TAG, "Storage screen destroyed");
}

const activity_cbs_t settings_storage_cbs = {
    .on_create  = storage_on_create,
    .on_resume  = storage_on_resume,
    .on_pause   = NULL,
    .on_destroy = storage_on_destroy,
};
