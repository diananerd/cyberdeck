/*
 * S3 Cyber-Deck — Settings > Storage
 * SD card mount status and space info.
 */

#include "app_settings.h"
#include "ui_activity.h"
#include "ui_theme.h"
#include "ui_statusbar.h"
#include "ui_common.h"
#include "ui_effect.h"
#include "hal_sdcard.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "settings_storage";

static void mount_btn_cb(lv_event_t *e)
{
    (void)e;
    if (hal_sdcard_is_mounted()) {
        if (hal_sdcard_unmount() == ESP_OK) {
            ui_effect_toast("SD card unmounted", 1500);
        } else {
            ui_effect_toast("Unmount failed", 1500);
        }
    } else {
        if (hal_sdcard_mount() == ESP_OK) {
            ui_effect_toast("SD card mounted", 1500);
        } else {
            ui_effect_toast("Mount failed — no card?", 2000);
        }
    }
}

static void storage_on_create(lv_obj_t *screen, void *intent_data)
{
    (void)intent_data;
    ui_statusbar_set_title("SETTINGS");

    lv_obj_t *content = ui_common_content_area(screen);

    /* Mount status */
    bool mounted = hal_sdcard_is_mounted();
    lv_obj_t *status_lbl = lv_label_create(content);
    lv_label_set_text(status_lbl, mounted
                       ? "SD Card: Mounted"
                       : "SD Card: Not mounted");
    ui_theme_style_label(status_lbl, &CYBERDECK_FONT_MD);

    if (mounted) {
        /* Space info */
        uint32_t total_kb = 0, used_kb = 0;
        if (hal_sdcard_get_space(&total_kb, &used_kb) == ESP_OK) {
            uint32_t free_kb  = total_kb - used_kb;
            uint32_t total_mb = total_kb / 1024;
            uint32_t used_mb  = used_kb  / 1024;
            uint32_t free_mb  = free_kb  / 1024;

            char space_str[80];
            snprintf(space_str, sizeof(space_str),
                     "Total: %lu MB\nUsed:  %lu MB\nFree:  %lu MB",
                     (unsigned long)total_mb,
                     (unsigned long)used_mb,
                     (unsigned long)free_mb);
            lv_obj_t *space_lbl = lv_label_create(content);
            lv_label_set_text(space_lbl, space_str);
            lv_label_set_long_mode(space_lbl, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(space_lbl, LV_PCT(100));
            ui_theme_style_label_dim(space_lbl, &CYBERDECK_FONT_SM);
        } else {
            lv_obj_t *err_lbl = lv_label_create(content);
            lv_label_set_text(err_lbl, "Could not read space info");
            ui_theme_style_label_dim(err_lbl, &CYBERDECK_FONT_SM);
        }

        /* Mount point */
        lv_obj_t *mp_lbl = lv_label_create(content);
        lv_label_set_text(mp_lbl, "Mount point: " HAL_SDCARD_MOUNT_POINT);
        ui_theme_style_label_dim(mp_lbl, &CYBERDECK_FONT_SM);
    }

    ui_common_divider(content);

    lv_obj_t *toggle_btn = ui_common_btn_full(content,
        mounted ? "Unmount SD Card" : "Mount SD Card");
    lv_obj_add_event_cb(toggle_btn, mount_btn_cb, LV_EVENT_CLICKED, NULL);

    ESP_LOGI(TAG, "Storage screen created (mounted=%d)", mounted);
}

const activity_cbs_t settings_storage_cbs = {
    .on_create  = storage_on_create,
    .on_resume  = NULL,
    .on_pause   = NULL,
    .on_destroy = NULL,
};
