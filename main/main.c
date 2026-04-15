/*
 * S3 Cyber-Deck — Main boot sequence
 * Phase 2: Theme + status bar + activity system demo
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lvgl.h"
#include "hal_ch422g.h"
#include "hal_backlight.h"
#include "hal_lcd.h"
#include "ui_engine.h"
#include "ui_theme.h"
#include "ui_statusbar.h"
#include "ui_activity.h"
#include "ui_common.h"
#include "ui_effect.h"
#include "driver/usb_serial_jtag.h"

static const char *TAG = "cyberdeck";

/* ---------- Placeholder launcher activity ---------- */

static void launcher_on_create(lv_obj_t *screen, void *intent_data)
{
    (void)intent_data;

    lv_obj_t *content = ui_common_content_area(screen);

    /* Title */
    lv_obj_t *title = lv_label_create(content);
    lv_label_set_text(title, "S3 CYBER-DECK");
    ui_theme_style_label(title, &CYBERDECK_FONT_XL);

    /* Divider */
    ui_common_divider(content);

    /* Status text */
    lv_obj_t *status = lv_label_create(content);
    lv_label_set_text(status, "Phase 2: UI Framework OK\n"
                              "Theme + StatusBar + Activity\n"
                              "Effect + Common + Keyboard");
    ui_theme_style_label_dim(status, &CYBERDECK_FONT_MD);

    /* Demo list */
    lv_obj_t *list = ui_common_list(content);
    ui_common_list_add(list, "> Books", 0, NULL, NULL);
    ui_common_list_add(list, "> Notes", 1, NULL, NULL);
    ui_common_list_add(list, "> Tasks", 2, NULL, NULL);
    ui_common_list_add(list, "> Music", 3, NULL, NULL);
    ui_common_list_add(list, "> Podcasts", 4, NULL, NULL);
    ui_common_list_add(list, "> Calculator", 5, NULL, NULL);
    ui_common_list_add(list, "> Bluesky", 6, NULL, NULL);
    ui_common_list_add(list, "> Settings", 7, NULL, NULL);

    ui_statusbar_set_title("LAUNCHER");

    ESP_LOGI(TAG, "Launcher created");
}

static void launcher_on_resume(lv_obj_t *screen, void *state)
{
    (void)screen;
    (void)state;
    ui_statusbar_set_title("LAUNCHER");
}

static const activity_cbs_t launcher_cbs = {
    .on_create  = launcher_on_create,
    .on_resume  = launcher_on_resume,
    .on_pause   = NULL,
    .on_destroy = NULL,
};

/* ---------- Boot sequence ---------- */

void app_main(void)
{
    ESP_LOGI(TAG, "=== S3 CYBER-DECK BOOT ===");

    /* 1. I2C + CH422G */
    esp_err_t ret = hal_ch422g_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CH422G init failed: %s", esp_err_to_name(ret));
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }
    ESP_LOGI(TAG, "CH422G OK");

    /* 2. LCD + Touch */
    esp_lcd_panel_handle_t lcd_handle = NULL;
    esp_lcd_touch_handle_t tp_handle = NULL;
    ESP_ERROR_CHECK(hal_lcd_init(&lcd_handle, &tp_handle));
    ESP_LOGI(TAG, "LCD + Touch OK");

    /* 3. Backlight on */
    hal_backlight_on();
    ESP_LOGI(TAG, "Backlight ON");

    /* 4. LVGL engine */
    ESP_ERROR_CHECK(ui_engine_init(lcd_handle, tp_handle));
    ESP_LOGI(TAG, "UI Engine OK");

    /* 5. Theme + Status bar + Activity system */
    if (ui_lock(1000)) {
        ui_theme_apply(THEME_GREEN);
        ui_statusbar_init();
        ui_activity_init();

        /* Push launcher as the root activity */
        ui_activity_push(0, 0, &launcher_cbs, NULL);

        /* Set initial status bar state */
        ui_statusbar_set_time(0, 0);
        ui_statusbar_set_wifi(false, 0);
        ui_statusbar_set_battery(0, false);

        /* Show a boot toast */
        ui_effect_toast("BOOT OK", 2000);

        ui_unlock();
    }

    /* 6. USB Serial/JTAG for debug */
    usb_serial_jtag_driver_config_t usj_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    esp_err_t usj_ret = usb_serial_jtag_driver_install(&usj_cfg);
    ESP_LOGI(TAG, "USB Serial/JTAG: %s", usj_ret == ESP_OK ? "OK" : esp_err_to_name(usj_ret));

    ESP_LOGI(TAG, "Boot complete, entering idle");

    /* Keep main task alive */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
