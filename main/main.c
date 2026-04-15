/*
 * S3 Cyber-Deck — Main boot sequence
 * Phase 3: OS Services (event bus, settings, WiFi, time, battery, SD, gestures, OTA)
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lvgl.h"

/* HAL */
#include "hal_ch422g.h"
#include "hal_backlight.h"
#include "hal_lcd.h"
#include "hal_rtc.h"
#include "hal_battery.h"
#include "hal_sdcard.h"
#include "hal_gesture.h"

/* UI */
#include "ui_engine.h"
#include "ui_theme.h"
#include "ui_statusbar.h"
#include "ui_activity.h"
#include "ui_common.h"
#include "ui_effect.h"

/* Services */
#include "svc_event.h"
#include "svc_settings.h"
#include "svc_battery.h"
#include "svc_wifi.h"
#include "svc_time.h"
#include "svc_downloader.h"
#include "svc_ota.h"

#include "driver/usb_serial_jtag.h"

static const char *TAG = "cyberdeck";

/* ---------- Gesture -> Event bridge ---------- */

static void gesture_cb(hal_gesture_type_t gesture)
{
    switch (gesture) {
    case HAL_GESTURE_HOME:
        svc_event_post(EVT_GESTURE_HOME, NULL, 0);
        break;
    case HAL_GESTURE_BACK:
        svc_event_post(EVT_GESTURE_BACK, NULL, 0);
        break;
    case HAL_GESTURE_LOCK:
        svc_event_post(EVT_GESTURE_LOCK, NULL, 0);
        break;
    }
}

/* ---------- Gesture event handler (pop activity on BACK/HOME) ---------- */

static void on_gesture_event(void *arg, esp_event_base_t base,
                             int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;
    if (ui_lock(100)) {
        switch ((cyberdeck_event_id_t)id) {
        case EVT_GESTURE_HOME:
            ESP_LOGI(TAG, "HOME gesture -> pop to home");
            ui_activity_pop_to_home();
            break;
        case EVT_GESTURE_BACK:
            ESP_LOGI(TAG, "BACK gesture -> pop activity");
            ui_activity_pop();
            break;
        case EVT_GESTURE_LOCK:
            ESP_LOGI(TAG, "LOCK gesture (TODO: lock screen)");
            ui_effect_toast("LOCK", 1000);
            break;
        default:
            break;
        }
        ui_unlock();
    }
}

/* ---------- Display rotation handler ---------- */

static void on_display_rotated(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg; (void)base; (void)id;
    uint8_t rotation = data ? *(uint8_t *)data : 0;
    ESP_LOGI(TAG, "EVT_DISPLAY_ROTATED (%s) -> recreating all activities",
             rotation ? "portrait" : "landscape");
    if (ui_lock(500)) {
        ui_activity_recreate_all();
        ui_unlock();
    }
}

/* ---------- Launcher app grid ---------- */

typedef struct {
    const char *icon;   /* Short symbol for the card */
    const char *name;   /* App name below the card */
} launcher_app_t;

static const launcher_app_t launcher_apps[] = {
    { "Bk",  "Books"   },
    { "Nt",  "Notes"   },
    { "Tk",  "Tasks"   },
    { "Mu",  "Music"   },
    { "Pd",  "Podcasts"},
    { "Ca",  "Calc"    },
    { "Bs",  "Bluesky" },
    { "Fl",  "Files"   },
    { "St",  "Settings"},
};

#define LAUNCHER_APP_COUNT  (sizeof(launcher_apps) / sizeof(launcher_apps[0]))

static void launcher_on_create(lv_obj_t *screen, void *intent_data)
{
    (void)intent_data;
    const cyberdeck_theme_t *t = ui_theme_get();

    /*
     * Derive grid columns from the actual display width reported by LVGL.
     * This is rotation-agnostic: landscape (800px) → 5 cols, portrait (480px) → 3 cols.
     * No need to read the rotation setting here.
     */
    const lv_coord_t gap = 16;
    lv_disp_t *disp = lv_disp_get_default();
    const lv_coord_t avail_w = lv_disp_get_hor_res(disp);
    uint8_t cols = (avail_w >= 600) ? 5 : 3;
    uint8_t rows = (LAUNCHER_APP_COUNT + cols - 1) / cols;  /* ceil */
    const lv_coord_t avail_h = lv_disp_get_ver_res(disp) - UI_STATUSBAR_HEIGHT;
    lv_coord_t card_from_w = (avail_w - gap * (cols + 1)) / cols;
    lv_coord_t card_from_h = (avail_h - gap * (rows + 1)) / rows;
    lv_coord_t card_sz = (card_from_w < card_from_h) ? card_from_w : card_from_h;

    /* Compute centering margins so gap is uniform in both axes */
    lv_coord_t margin_h = (avail_w - cols * card_sz - (cols - 1) * gap) / 2;
    lv_coord_t margin_v = (avail_h - rows * card_sz - (rows - 1) * gap) / 2;

    /* Content area — flex wrap with explicit uniform gaps */
    lv_obj_t *cont = lv_obj_create(screen);
    lv_obj_set_size(cont, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_radius(cont, 0, 0);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(cont, margin_h, 0);
    lv_obj_set_style_pad_right(cont, margin_h, 0);
    lv_obj_set_style_pad_top(cont, margin_v, 0);
    lv_obj_set_style_pad_bottom(cont, margin_v, 0);
    lv_obj_set_style_pad_column(cont, gap, 0);
    lv_obj_set_style_pad_row(cont, gap, 0);

    /* Create app cards — fixed square size */
    for (uint8_t i = 0; i < LAUNCHER_APP_COUNT && i < (uint8_t)(cols * rows); i++) {
        const launcher_app_t *app = &launcher_apps[i];

        lv_obj_t *card = lv_obj_create(cont);
        lv_obj_set_size(card, card_sz, card_sz);
        lv_obj_set_style_bg_color(card, t->bg_dark, 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(card, t->primary_dim, 0);
        lv_obj_set_style_border_width(card, 2, 0);
        lv_obj_set_style_border_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, 16, 0);
        lv_obj_set_style_pad_all(card, 4, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

        /* Flex column: icon centered, name at bottom */
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(card, 4, 0);

        /* Icon label */
        lv_obj_t *icon = lv_label_create(card);
        lv_label_set_text(icon, app->icon);
        lv_obj_set_style_text_color(icon, t->primary, 0);
        lv_obj_set_style_text_font(icon, &CYBERDECK_FONT_LG, 0);

        /* App name label */
        lv_obj_t *name = lv_label_create(card);
        lv_label_set_text(name, app->name);
        lv_obj_set_style_text_color(name, t->text_dim, 0);
        lv_obj_set_style_text_font(name, &CYBERDECK_FONT_SM, 0);

        /* Press feedback: invert */
        lv_obj_set_style_bg_color(card, t->primary, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_border_color(card, t->primary, LV_STATE_PRESSED);
    }

    ui_statusbar_set_title("S3 CYBER-DECK");

    ESP_LOGI(TAG, "Launcher created (%dx%d grid)", cols, rows);
}

static void launcher_on_resume(lv_obj_t *screen, void *state)
{
    (void)screen;
    (void)state;
    ui_statusbar_set_title("S3 CYBER-DECK");
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

    /* 1. NVS + Settings (must be first for WiFi/theme preferences) */
    ESP_ERROR_CHECK(svc_settings_init());
    ESP_LOGI(TAG, "Settings OK");

    /* 2. Event bus */
    ESP_ERROR_CHECK(svc_event_init());
    ESP_LOGI(TAG, "Event bus OK");

    /* 3. I2C + CH422G */
    ESP_ERROR_CHECK(hal_ch422g_init());
    ESP_LOGI(TAG, "CH422G OK");

    /* 4. LCD + Touch */
    esp_lcd_panel_handle_t lcd_handle = NULL;
    esp_lcd_touch_handle_t tp_handle = NULL;
    ESP_ERROR_CHECK(hal_lcd_init(&lcd_handle, &tp_handle));
    ESP_LOGI(TAG, "LCD + Touch OK");

    /* 5. Backlight on (AFTER lcd_init — touch_reset overwrites CH422G OUT) */
    hal_backlight_on();
    ESP_LOGI(TAG, "Backlight ON");

    /* 6. LVGL engine */
    ESP_ERROR_CHECK(ui_engine_init(lcd_handle, tp_handle));
    ESP_LOGI(TAG, "UI Engine OK");

    /* 7. Rotation + Theme + Status bar + Activity system */
    if (ui_lock(1000)) {
        uint8_t saved_rotation = 0;
        svc_settings_get_rotation(&saved_rotation);
        saved_rotation = 1;  /* TEMP: force portrait until Settings UI exists */
        ui_engine_set_rotation(saved_rotation);
        ESP_LOGI(TAG, "Rotation: %s", saved_rotation ? "portrait" : "landscape");

        uint8_t saved_theme = 0;
        svc_settings_get_theme(&saved_theme);
        if (saved_theme >= THEME_COUNT) saved_theme = 0;
        ui_theme_apply((cyberdeck_theme_id_t)saved_theme);
        ui_statusbar_init();
        ui_activity_init();

        /* Push launcher as the root activity */
        ui_activity_push(0, 0, &launcher_cbs, NULL);

        /* Set initial status bar state */
        ui_statusbar_set_time(0, 0, 0);
        ui_statusbar_set_wifi(false, 0);
        ui_statusbar_set_battery(0, false);
        ui_statusbar_set_sdcard(false);  /* TODO: set from hal_sdcard_mount result */

        ui_unlock();
    }

    /* 8. RTC -> system time */
    if (hal_rtc_init() == ESP_OK) {
        hal_rtc_sync_to_system();
        ESP_LOGI(TAG, "RTC OK");
    } else {
        ESP_LOGW(TAG, "RTC init failed (continuing without RTC)");
    }

    /* 9. Gesture detection (needs LVGL mutex) */
    if (ui_lock(1000)) {
        hal_gesture_init(gesture_cb);
        ui_unlock();
    }
    /* Register gesture event handlers */
    svc_event_register(EVT_GESTURE_HOME, on_gesture_event, NULL);
    svc_event_register(EVT_GESTURE_BACK, on_gesture_event, NULL);
    svc_event_register(EVT_GESTURE_LOCK, on_gesture_event, NULL);
    ESP_LOGI(TAG, "Gestures OK");

    /* Register display rotation handler (recreates all activity layouts) */
    svc_event_register(EVT_DISPLAY_ROTATED, on_display_rotated, NULL);

    /* 10. Battery ADC + monitor task */
    if (hal_battery_init() == ESP_OK) {
        svc_battery_start();
        ESP_LOGI(TAG, "Battery monitor OK");
    } else {
        ESP_LOGW(TAG, "Battery ADC init failed (continuing)");
    }

    /* 11. SD card mount */
    if (hal_sdcard_mount() == ESP_OK) {
        svc_event_post(EVT_SDCARD_MOUNTED, NULL, 0);
        ESP_LOGI(TAG, "SD card mounted");
    } else {
        ESP_LOGW(TAG, "SD card not available (continuing without SD)");
    }

    /* 12. Time service (SNTP + RTC bridge + status bar clock) */
    ESP_ERROR_CHECK(svc_time_init());
    ESP_LOGI(TAG, "Time service OK");

    /* 13. Downloader + OTA (ready but idle until needed) */
    ESP_ERROR_CHECK(svc_downloader_init());
    ESP_ERROR_CHECK(svc_ota_init());
    ESP_LOGI(TAG, "Downloader + OTA OK");

    /* 14. USB Serial/JTAG for debug console */
    usb_serial_jtag_driver_config_t usj_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    esp_err_t usj_ret = usb_serial_jtag_driver_install(&usj_cfg);
    ESP_LOGI(TAG, "USB Serial/JTAG: %s", usj_ret == ESP_OK ? "OK" : esp_err_to_name(usj_ret));

    /* 15. WiFi (last — auto-connects in background, triggers SNTP on connect) */
    ESP_ERROR_CHECK(svc_wifi_init());
    svc_wifi_auto_connect();  /* non-blocking, will post EVT_WIFI_CONNECTED */
    ESP_LOGI(TAG, "WiFi init OK (auto-connecting in background)");

    /* Boot toast */
    if (ui_lock(100)) {
        ui_effect_toast("BOOT OK — Phase 3", 2000);
        ui_unlock();
    }

    ESP_LOGI(TAG, "Boot complete, entering idle");

    /* Keep main task alive */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
