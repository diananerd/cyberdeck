/*
 * S3 Cyber-Deck — Settings > Storage
 * SD card status and space breakdown.
 *
 * Layout: data rows (status, mount point, total/used/free),
 * [Mount] / [Unmount] action button pinned to bottom right.
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
            ui_effect_toast("Mount failed - no card?", 2000);
        }
    }
}

static void storage_on_create(lv_obj_t *screen, void *intent_data)
{
    (void)intent_data;
    ui_statusbar_set_title("SETTINGS");

    lv_obj_t *content = ui_common_content_area(screen);

    bool mounted = hal_sdcard_is_mounted();

    /* ---- Status ---- */
    ui_common_data_row(content, "SD CARD STATUS:",
                       mounted ? "MOUNTED" : "NOT MOUNTED");

    if (mounted) {
        /* Mount point */
        ui_common_data_row(content, "MOUNT POINT:", HAL_SDCARD_MOUNT_POINT);

        /* Space breakdown */
        uint32_t total_kb = 0, used_kb = 0;
        if (hal_sdcard_get_space(&total_kb, &used_kb) == ESP_OK) {
            uint32_t free_kb  = total_kb - used_kb;

            char total_str[20], used_str[20], free_str[20];

            if (total_kb >= 1024) {
                snprintf(total_str, sizeof(total_str),
                         "%lu MB", (unsigned long)(total_kb / 1024));
            } else {
                snprintf(total_str, sizeof(total_str),
                         "%lu KB", (unsigned long)total_kb);
            }

            if (used_kb >= 1024) {
                snprintf(used_str, sizeof(used_str),
                         "%lu MB", (unsigned long)(used_kb / 1024));
            } else {
                snprintf(used_str, sizeof(used_str),
                         "%lu KB", (unsigned long)used_kb);
            }

            if (free_kb >= 1024) {
                snprintf(free_str, sizeof(free_str),
                         "%lu MB", (unsigned long)(free_kb / 1024));
            } else {
                snprintf(free_str, sizeof(free_str),
                         "%lu KB", (unsigned long)free_kb);
            }

            ui_common_data_row(content, "TOTAL:", total_str);
            ui_common_data_row(content, "USED:", used_str);
            ui_common_data_row(content, "FREE:", free_str);

            /* Usage percentage */
            if (total_kb > 0) {
                char pct_str[12];
                snprintf(pct_str, sizeof(pct_str), "%lu%%",
                         (unsigned long)((used_kb * 100UL) / total_kb));
                ui_common_data_row(content, "USAGE:", pct_str);
            }
        } else {
            lv_obj_t *err_lbl = lv_label_create(content);
            lv_label_set_text(err_lbl, "Could not read space info");
            ui_theme_style_label_dim(err_lbl, &CYBERDECK_FONT_SM);
        }
    }

    /* ---- Spacer + action button ---- */
    ui_common_spacer(content);

    lv_obj_t *btn_row = ui_common_action_row(content);
    lv_obj_t *toggle_btn = ui_common_btn(btn_row,
        mounted ? "Unmount" : "Mount SD Card");
    if (!mounted) ui_common_btn_style_primary(toggle_btn);
    lv_obj_add_event_cb(toggle_btn, mount_btn_cb, LV_EVENT_CLICKED, NULL);

    ESP_LOGI(TAG, "Storage screen created (mounted=%d)", mounted);
}

const activity_cbs_t settings_storage_cbs = {
    .on_create  = storage_on_create,
    .on_resume  = NULL,
    .on_pause   = NULL,
    .on_destroy = NULL,
};
