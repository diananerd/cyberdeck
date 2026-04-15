/*
 * S3 Cyber-Deck — Settings > Bluetooth
 * BT module status and paired device info.
 * Actual A2DP functionality deferred to Phase 6.
 *
 * Layout: data breakdown (module, status, device, paired addr).
 * Wiring instructions shown when module is absent.
 */

#include "app_settings.h"
#include "ui_activity.h"
#include "ui_theme.h"
#include "ui_statusbar.h"
#include "ui_common.h"
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

    /* ---- Module status ---- */
    ui_common_data_row(content, "BT MODULE:",
                       state->bt_module_present ? "DETECTED" : "NOT DETECTED");

    if (state->bt_module_present) {
        /* Connection status */
        ui_common_data_row(content, "STATUS:",
                           state->bt_connected ? "CONNECTED" : "DISCONNECTED");

        /* Connected device name */
        if (state->bt_connected && state->bt_device_name[0] != '\0') {
            ui_common_data_row(content, "DEVICE:", state->bt_device_name);
        }

        /* Paired address from NVS */
        char paired[32] = {0};
        svc_settings_get_bt_paired(paired, sizeof(paired));
        ui_common_data_row(content, "PAIRED ADDR:",
                           paired[0] != '\0' ? paired : "(none)");

    } else {
        /* Section gap: module status → wiring instructions */
        ui_common_section_gap(content);

        /* Wiring instructions */
        lv_obj_t *hint_key = lv_label_create(content);
        lv_label_set_text(hint_key, "UART1 WIRING:");
        ui_theme_style_label_dim(hint_key, &CYBERDECK_FONT_SM);

        const char *wiring[] = {
            "TX → GPIO 15",
            "RX → GPIO 16",
            "BAUD: 115200",
            "Modules: BM62, RN52, etc.",
        };
        for (int i = 0; i < 4; i++) {
            lv_obj_t *lbl = lv_label_create(content);
            lv_label_set_text(lbl, wiring[i]);
            ui_theme_style_label(lbl, &CYBERDECK_FONT_MD);
        }
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
