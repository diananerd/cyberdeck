/*
 * S3 Cyber-Deck — Settings > Storage
 *
 * Shows SD card status and space breakdown.
 * Subscribes to EVT_SDCARD_MOUNTED / EVT_SDCARD_UNMOUNTED so the UI
 * updates in real-time when the card is inserted or removed while the
 * screen is open — same pattern as settings_wifi.c.
 *
 * Thread-safety:
 *   SD events fire on the event-loop task → must lock LVGL.
 *   Button callbacks fire on the LVGL task → no lock needed.
 *   g_storage_scr_state is NULL while the screen is not active.
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
#include "svc_event.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "settings_storage";

/* ================================================================
 * Screen state
 * ================================================================ */

typedef struct {
    lv_obj_t *content;  /* content area — cleaned + rebuilt on every refresh */
} storage_scr_state_t;

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

static void mount_btn_cb(lv_event_t *e);   /* forward declaration */

static void build_content(storage_scr_state_t *s)
{
    lv_obj_clean(s->content);

    bool mounted = hal_sdcard_is_mounted();

    /* ---- Status row ---- */
    lv_obj_t *status_val = ui_common_data_row(s->content, "STATUS:",
                                               mounted ? "MOUNTED" : "NOT MOUNTED");
    if (!mounted) {
        const cyberdeck_theme_t *t = ui_theme_get();
        lv_obj_set_style_text_color(status_val, t->text_dim, 0);
    }

    if (mounted) {
        ui_common_data_row(s->content, "MOUNT POINT:", HAL_SDCARD_MOUNT_POINT);

        /* Space info is pre-fetched by sd_poll_task (Core 0) and cached in
         * app_state so we never call statvfs from the LVGL task. */
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

    /* ---- Spacer + action button ---- */
    ui_common_spacer(s->content);

    lv_obj_t *btn_row = ui_common_action_row(s->content);
    lv_obj_t *toggle_btn = ui_common_btn(btn_row,
        mounted ? "UNMOUNT" : "MOUNT SD CARD");
    if (!mounted) ui_common_btn_style_primary(toggle_btn);
    lv_obj_add_event_cb(toggle_btn, mount_btn_cb, LV_EVENT_CLICKED, s);
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

    /* Rebuild immediately — already on the LVGL task, no lock needed */
    build_content(s);
}

/* ================================================================
 * SD card event handlers (event-loop task)
 * ================================================================ */

static void sdcard_event_handler(void *arg, esp_event_base_t base,
                                  int32_t id, void *data)
{
    (void)arg; (void)base; (void)id; (void)data;

    if (!g_storage_scr_state) return;

    if (ui_lock(200)) {
        if (g_storage_scr_state)
            build_content(g_storage_scr_state);
        ui_unlock();
    }
}

/* ================================================================
 * Activity callbacks
 * ================================================================ */

static void storage_on_create(lv_obj_t *screen, void *intent_data)
{
    (void)intent_data;

    storage_scr_state_t *s =
        (storage_scr_state_t *)lv_mem_alloc(sizeof(storage_scr_state_t));
    if (!s) return;
    s->content = NULL;
    ui_activity_set_state(s);
    g_storage_scr_state = s;

    svc_event_register(EVT_SDCARD_MOUNTED,   sdcard_event_handler, NULL);
    svc_event_register(EVT_SDCARD_UNMOUNTED, sdcard_event_handler, NULL);

    ui_statusbar_set_title("SETTINGS");
    s->content = ui_common_content_area(screen);

    build_content(s);

    ESP_LOGI(TAG, "Storage screen created");
}

static void storage_on_resume(lv_obj_t *screen, void *state)
{
    (void)screen;
    storage_scr_state_t *s = (storage_scr_state_t *)state;
    if (!s) return;
    /* Refresh in case card state changed while the screen was paused */
    build_content(s);
}

static void storage_on_destroy(lv_obj_t *screen, void *state)
{
    (void)screen;
    g_storage_scr_state = NULL;
    svc_event_unregister(EVT_SDCARD_MOUNTED,   sdcard_event_handler);
    svc_event_unregister(EVT_SDCARD_UNMOUNTED, sdcard_event_handler);
    lv_mem_free(state);
    ESP_LOGI(TAG, "Storage screen destroyed");
}

const activity_cbs_t settings_storage_cbs = {
    .on_create  = storage_on_create,
    .on_resume  = storage_on_resume,
    .on_pause   = NULL,
    .on_destroy = storage_on_destroy,
};
