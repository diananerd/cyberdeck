/*
 * CyberDeck — Settings > About
 * Device identity, firmware details, and OTA update trigger.
 */

#include "app_settings.h"
#include "ui_activity.h"
#include "ui_theme.h"
#include "ui_statusbar.h"
#include "ui_common.h"
#include "ui_effect.h"
#include "svc_settings.h"
#include "svc_ota.h"
#include "os_settings.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include <stdio.h>

static const char *TAG = "settings_about";

static void ota_btn_cb(lv_event_t *e)
{
    (void)e;
    const char *ota_url = os_settings_get()->ota_url;  /* E3: read from cache */
    if (ota_url[0] == '\0') {
        ui_effect_toast("No OTA URL configured", 2000);
        return;
    }
    ui_effect_toast("Starting OTA update...", 2000);
    svc_ota_start(ota_url);
    ESP_LOGI(TAG, "OTA triggered: %s", ota_url);
}

/* D1: returns NULL (no state needed) */
static void *about_on_create(lv_obj_t *screen, const view_args_t *args, void *app_data)
{
    (void)args;
    (void)app_data;
    ui_statusbar_set_title("SETTINGS");

    lv_obj_t *content = ui_common_content_area(screen);

    const esp_app_desc_t *desc = esp_app_get_description();

    char ver_str[32];
    snprintf(ver_str, sizeof(ver_str), "%s", desc ? desc->version : "0.1.0");
    ui_common_data_row(content, "FIRMWARE:", ver_str);

    char idf_str[24];
    snprintf(idf_str, sizeof(idf_str), "%s", IDF_VER);
    ui_common_data_row(content, "IDF VERSION:", idf_str);

    char app_str[32];
    snprintf(app_str, sizeof(app_str), "%s", desc ? desc->project_name : "cyberdeck");
    ui_common_data_row(content, "APPLICATION:", app_str);

    char build_str[36];
    snprintf(build_str, sizeof(build_str), "%.16s %.8s",
             desc ? desc->date : "?", desc ? desc->time : "");
    ui_common_data_row(content, "BUILD DATE:", build_str);

    uint32_t boot_count = os_settings_get()->boot_count;  /* E3: read from cache */
    char boot_str[16];
    snprintf(boot_str, sizeof(boot_str), "%lu", (unsigned long)boot_count);
    ui_common_data_row(content, "BOOT COUNT:", boot_str);

    esp_chip_info_t chip = {0};
    esp_chip_info(&chip);
    char chip_str[32];
    snprintf(chip_str, sizeof(chip_str), "ESP32-S3  rev %d  %d cores",
             chip.revision, chip.cores);
    ui_common_data_row(content, "CHIP:", chip_str);

    ui_common_data_row(content, "FLASH:", CONFIG_ESPTOOLPY_FLASHSIZE);

    ui_common_section_gap(content);

    const char *ota_url = os_settings_get()->ota_url;          /* E3: read from cache */
    const char *ota_display = (ota_url[0] != '\0') ? ota_url : "(not configured)";

    lv_obj_t *ota_key = lv_label_create(content);
    lv_label_set_text(ota_key, "OTA ENDPOINT:");
    ui_theme_style_label_dim(ota_key, &CYBERDECK_FONT_SM);

    lv_obj_t *ota_lbl = lv_label_create(content);
    lv_label_set_text(ota_lbl, ota_display);
    lv_label_set_long_mode(ota_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ota_lbl, LV_PCT(100));
    ui_theme_style_label(ota_lbl, &CYBERDECK_FONT_MD);

    ui_common_spacer(content);

    lv_obj_t *btn_row = ui_common_action_row(content);
    lv_obj_t *ota_btn = ui_common_btn(btn_row, "Check for Update");
    ui_common_btn_style_primary(ota_btn);
    lv_obj_add_event_cb(ota_btn, ota_btn_cb, LV_EVENT_CLICKED, NULL);

    return NULL;
}

const view_cbs_t settings_about_cbs = {
    .on_create  = about_on_create,
    .on_resume  = NULL,
    .on_pause   = NULL,
    .on_destroy = NULL,
};
