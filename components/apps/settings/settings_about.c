/*
 * S3 Cyber-Deck — Settings > About
 * Shows firmware version, board info, boot count, OTA trigger.
 */

#include "app_settings.h"
#include "ui_activity.h"
#include "ui_theme.h"
#include "ui_statusbar.h"
#include "ui_common.h"
#include "ui_effect.h"
#include "svc_settings.h"
#include "svc_ota.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include <stdio.h>

static const char *TAG = "settings_about";

static void ota_btn_cb(lv_event_t *e)
{
    (void)e;
    char ota_url[128] = {0};
    svc_settings_get_ota_url(ota_url, sizeof(ota_url));
    if (ota_url[0] == '\0') {
        ui_effect_toast("No OTA URL configured", 2000);
        return;
    }
    ui_effect_toast("Starting OTA update...", 2000);
    svc_ota_start(ota_url);
    ESP_LOGI(TAG, "OTA triggered: %s", ota_url);
}

static void about_on_create(lv_obj_t *screen, void *intent_data)
{
    (void)intent_data;
    ui_statusbar_set_title("SETTINGS");

    lv_obj_t *content = ui_common_content_area(screen);

    /* Firmware version */
    const esp_app_desc_t *desc = esp_app_get_description();
    char ver_str[64];
    snprintf(ver_str, sizeof(ver_str), "Firmware: %s", desc ? desc->version : "0.1.0");
    lv_obj_t *ver_lbl = lv_label_create(content);
    lv_label_set_text(ver_lbl, ver_str);
    ui_theme_style_label(ver_lbl, &CYBERDECK_FONT_MD);

    /* IDF version */
    char idf_str[64];
    snprintf(idf_str, sizeof(idf_str), "IDF: %s", IDF_VER);
    lv_obj_t *idf_lbl = lv_label_create(content);
    lv_label_set_text(idf_lbl, idf_str);
    ui_theme_style_label_dim(idf_lbl, &CYBERDECK_FONT_SM);

    /* App name */
    char app_str[64];
    snprintf(app_str, sizeof(app_str), "App: %s", desc ? desc->project_name : "cyberdeck");
    lv_obj_t *app_lbl = lv_label_create(content);
    lv_label_set_text(app_lbl, app_str);
    ui_theme_style_label_dim(app_lbl, &CYBERDECK_FONT_SM);

    /* Boot count */
    uint32_t boot_count = 0;
    svc_settings_get_boot_count(&boot_count);
    char boot_str[32];
    snprintf(boot_str, sizeof(boot_str), "Boot count: %lu", (unsigned long)boot_count);
    lv_obj_t *boot_lbl = lv_label_create(content);
    lv_label_set_text(boot_lbl, boot_str);
    ui_theme_style_label_dim(boot_lbl, &CYBERDECK_FONT_SM);

    ui_common_divider(content);

    /* OTA URL */
    char ota_url[128] = {0};
    svc_settings_get_ota_url(ota_url, sizeof(ota_url));
    char ota_str[160];
    snprintf(ota_str, sizeof(ota_str), "OTA: %s",
             (ota_url[0] != '\0') ? ota_url : "(not set)");
    lv_obj_t *ota_lbl = lv_label_create(content);
    lv_label_set_text(ota_lbl, ota_str);
    lv_label_set_long_mode(ota_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ota_lbl, LV_PCT(100));
    ui_theme_style_label_dim(ota_lbl, &CYBERDECK_FONT_SM);

    /* Trigger OTA button */
    lv_obj_t *ota_btn = ui_common_btn_full(content, "Check for OTA Update");
    lv_obj_add_event_cb(ota_btn, ota_btn_cb, LV_EVENT_CLICKED, NULL);
}

const activity_cbs_t settings_about_cbs = {
    .on_create  = about_on_create,
    .on_resume  = NULL,
    .on_pause   = NULL,
    .on_destroy = NULL,
};
