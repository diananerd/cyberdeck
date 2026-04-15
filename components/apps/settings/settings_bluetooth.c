/*
 * S3 Cyber-Deck — Settings > Bluetooth
 * BT module status and paired device info.
 * Actual A2DP functionality deferred to Phase 6.
 */

#include "app_settings.h"
#include "ui_activity.h"
#include "ui_theme.h"
#include "ui_statusbar.h"
#include "ui_common.h"
#include "ui_effect.h"
#include "app_state.h"
#include "svc_settings.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "settings_bt";

static void bt_on_create(lv_obj_t *screen, void *intent_data)
{
    (void)intent_data;
    ui_statusbar_set_title("SETTINGS");

    lv_obj_t *content = ui_common_content_area(screen);
    const cyberdeck_state_t *state = app_state_get();

    /* Module status */
    lv_obj_t *mod_lbl = lv_label_create(content);
    lv_label_set_text(mod_lbl, state->bt_module_present
                       ? "BT Module: Detected"
                       : "BT Module: Not detected");
    ui_theme_style_label(mod_lbl, &CYBERDECK_FONT_MD);

    if (state->bt_module_present) {
        /* Connection status */
        lv_obj_t *conn_lbl = lv_label_create(content);
        lv_label_set_text(conn_lbl, state->bt_connected
                           ? "Status: Connected"
                           : "Status: Disconnected");
        ui_theme_style_label_dim(conn_lbl, &CYBERDECK_FONT_SM);

        if (state->bt_connected && state->bt_device_name[0] != '\0') {
            char dev_str[64];
            snprintf(dev_str, sizeof(dev_str), "Device: %s", state->bt_device_name);
            lv_obj_t *dev_lbl = lv_label_create(content);
            lv_label_set_text(dev_lbl, dev_str);
            ui_theme_style_label_dim(dev_lbl, &CYBERDECK_FONT_SM);
        }

        ui_common_divider(content);

        /* Paired address from NVS */
        char paired[32] = {0};
        svc_settings_get_bt_paired(paired, sizeof(paired));
        if (paired[0] != '\0') {
            char pa_str[64];
            snprintf(pa_str, sizeof(pa_str), "Paired: %s", paired);
            lv_obj_t *pa_lbl = lv_label_create(content);
            lv_label_set_text(pa_lbl, pa_str);
            ui_theme_style_label_dim(pa_lbl, &CYBERDECK_FONT_SM);
        } else {
            lv_obj_t *pa_lbl = lv_label_create(content);
            lv_label_set_text(pa_lbl, "No paired device");
            ui_theme_style_label_dim(pa_lbl, &CYBERDECK_FONT_SM);
        }
    } else {
        ui_common_divider(content);

        lv_obj_t *hint = lv_label_create(content);
        lv_label_set_text(hint,
            "Connect a Bluetooth Classic module\n"
            "(BM62, RN52, etc.) to UART1\n"
            "GPIO 15 (TX) / GPIO 16 (RX)\n"
            "at 115200 baud.");
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(hint, LV_PCT(100));
        ui_theme_style_label_dim(hint, &CYBERDECK_FONT_SM);
    }

    ESP_LOGI(TAG, "BT settings shown (module=%s)",
             state->bt_module_present ? "yes" : "no");
}

const activity_cbs_t settings_bluetooth_cbs = {
    .on_create  = bt_on_create,
    .on_resume  = NULL,
    .on_pause   = NULL,
    .on_destroy = NULL,
};
