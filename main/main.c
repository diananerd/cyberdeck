/*
 * S3 Cyber-Deck — Main boot sequence
 * Phase 4: Launcher + Settings (app_framework, app_launcher, app_settings)
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
#include "ui_navbar.h"
#include "ui_activity.h"
#include "ui_effect.h"

/* Services */
#include "svc_event.h"
#include "svc_settings.h"
#include "svc_battery.h"
#include "svc_wifi.h"
#include "svc_time.h"
#include "svc_downloader.h"
#include "svc_ota.h"

/* App Framework */
#include "app_registry.h"
#include "app_state.h"
#include "app_manager.h"

/* Apps */
#include "app_launcher.h"
#include "app_settings.h"

#include "driver/usb_serial_jtag.h"

static const char *TAG = "cyberdeck";

/* ================================================================
 * Event handlers — update app_state and statusbar
 * ================================================================ */

static void on_wifi_connected(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg; (void)base; (void)id; (void)data;
    char ssid[33] = {0};
    svc_wifi_get_ssid(ssid, sizeof(ssid));
    int8_t rssi = svc_wifi_get_rssi();
    app_state_update_wifi(true, ssid, rssi);

    /* Update statusbar from LVGL task via event */
    if (ui_lock(200)) {
        ui_statusbar_set_wifi(true, rssi);
        ui_unlock();
    }
    ESP_LOGI(TAG, "WiFi connected: %s (%d dBm)", ssid, rssi);
}

static void on_wifi_disconnected(void *arg, esp_event_base_t base,
                                  int32_t id, void *data)
{
    (void)arg; (void)base; (void)id; (void)data;
    app_state_update_wifi(false, NULL, 0);

    if (ui_lock(200)) {
        ui_statusbar_set_wifi(false, 0);
        ui_unlock();
    }
    ESP_LOGI(TAG, "WiFi disconnected");
}

static void on_battery_updated(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    (void)arg; (void)base; (void)id;
    if (!data) return;
    uint8_t pct = *(uint8_t *)data;
    app_state_update_battery(pct);

    /* Battery charging detection: TODO — use GPIO when available */
    if (ui_lock(100)) {
        ui_statusbar_set_battery(pct, false);
        ui_unlock();
    }
}

static void on_sdcard_mounted(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg; (void)base; (void)id; (void)data;
    app_state_update_sd(true);
    if (ui_lock(500)) {
        ui_statusbar_set_sdcard(true);
        ui_unlock();
    }
    ESP_LOGI(TAG, "SD card mounted → state updated");
}

static void on_sdcard_unmounted(void *arg, esp_event_base_t base,
                                 int32_t id, void *data)
{
    (void)arg; (void)base; (void)id; (void)data;
    app_state_update_sd(false);
    if (ui_lock(500)) {
        ui_statusbar_set_sdcard(false);
        ui_unlock();
    }
}

/* ================================================================
 * Gesture → navigation
 * ================================================================ */

static void gesture_cb(hal_gesture_type_t gesture)
{
    switch (gesture) {
    case HAL_GESTURE_HOME: svc_event_post(EVT_GESTURE_HOME, NULL, 0); break;
    case HAL_GESTURE_BACK: svc_event_post(EVT_GESTURE_BACK, NULL, 0); break;
    case HAL_GESTURE_LOCK: svc_event_post(EVT_GESTURE_LOCK, NULL, 0); break;
    }
}

static void on_gesture_event(void *arg, esp_event_base_t base,
                              int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;
    switch ((cyberdeck_event_id_t)id) {
    case EVT_GESTURE_HOME:
        ESP_LOGI(TAG, "HOME gesture");
        app_manager_go_home();
        break;
    case EVT_GESTURE_BACK:
        ESP_LOGI(TAG, "BACK gesture");
        app_manager_go_back();
        break;
    case EVT_GESTURE_LOCK:
        ESP_LOGI(TAG, "LOCK gesture");
        app_manager_lock();
        break;
    default:
        break;
    }
}

/* ================================================================
 * Display rotation
 * ================================================================ */

static void on_display_rotated(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    (void)arg; (void)base; (void)id;
    uint8_t rotation = data ? *(uint8_t *)data : 0;
    ESP_LOGI(TAG, "EVT_DISPLAY_ROTATED (%s) → recreating all activities",
             rotation ? "portrait" : "landscape");
    if (ui_lock(500)) {
        /* Adapt statusbar width to new display dimensions */
        ui_statusbar_refresh_theme();
        /* Rebuild navbar for new orientation (portrait ↔ landscape) */
        ui_navbar_adapt();
        /* Recreate gesture strips (sizing depends on display dimensions) */
        hal_gesture_recreate();
        /* Recreate all activity screens */
        ui_activity_recreate_all();
        ui_unlock();
    }
}

static void on_nav_processes(void *arg, esp_event_base_t base,
                              int32_t id, void *data)
{
    (void)arg; (void)base; (void)id; (void)data;
    ESP_LOGI(TAG, "NAV: Processes");
    /* TODO: push a process manager activity once implemented */
    if (ui_lock(100)) {
        ui_effect_toast("Processes — coming soon", 1500);
        ui_unlock();
    }
}

/* ================================================================
 * SD card background polling
 * ================================================================ */

static void sd_poll_task(void *arg)
{
    (void)arg;
    /* Wait for system to settle before first poll */
    vTaskDelay(pdMS_TO_TICKS(8000));

    bool last_mounted = hal_sdcard_is_mounted();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));   /* poll every 5s */

        bool mounted = hal_sdcard_is_mounted();

        if (!mounted) {
            /* Try to mount in case a card was inserted since last check */
            if (hal_sdcard_mount() == ESP_OK) {
                mounted = true;
                ESP_LOGI(TAG, "sd_poll: card mounted");
            }
        }

        if (mounted != last_mounted) {
            last_mounted = mounted;
            app_state_update_sd(mounted);

            if (!mounted) hal_sdcard_unmount();

            /* Update statusbar directly — more reliable than bouncing through
             * the event loop where ui_lock() can silently time out. */
            for (int retry = 0; retry < 5; retry++) {
                if (ui_lock(500)) {
                    ui_statusbar_set_sdcard(mounted);
                    ui_unlock();
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(200));
            }

            /* Also post event for any other listeners */
            svc_event_post(mounted ? EVT_SDCARD_MOUNTED : EVT_SDCARD_UNMOUNTED,
                           NULL, 0);
            ESP_LOGI(TAG, "sd_poll: SD %s", mounted ? "mounted" : "unmounted");
        }
    }
}

/* ================================================================
 * Boot sequence
 * ================================================================ */

void app_main(void)
{
    ESP_LOGI(TAG, "=== S3 CYBER-DECK BOOT (Phase 4) ===");

    /* 1. NVS + Settings */
    ESP_ERROR_CHECK(svc_settings_init());
    svc_settings_inc_boot_count();
    ESP_LOGI(TAG, "Settings OK");

    /* 2. Event bus */
    ESP_ERROR_CHECK(svc_event_init());
    ESP_LOGI(TAG, "Event bus OK");

    /* 3. I2C + CH422G */
    ESP_ERROR_CHECK(hal_ch422g_init());
    ESP_LOGI(TAG, "CH422G OK");

    /* 4. LCD + Touch */
    esp_lcd_panel_handle_t lcd_handle = NULL;
    esp_lcd_touch_handle_t tp_handle  = NULL;
    ESP_ERROR_CHECK(hal_lcd_init(&lcd_handle, &tp_handle));
    ESP_LOGI(TAG, "LCD + Touch OK");

    /* 5. Backlight on */
    hal_backlight_on();
    ESP_LOGI(TAG, "Backlight ON");

    /* 6. LVGL engine */
    ESP_ERROR_CHECK(ui_engine_init(lcd_handle, tp_handle));
    ESP_LOGI(TAG, "UI Engine OK");

    /* 7. App framework */
    app_registry_init();
    app_state_init();
    ESP_ERROR_CHECK(app_manager_init());
    ESP_LOGI(TAG, "App framework OK");

    /* 8. Register apps */
    ESP_ERROR_CHECK(app_launcher_register());   /* also registers lockscreen */
    ESP_ERROR_CHECK(app_settings_register());
    ESP_LOGI(TAG, "Apps registered");

    /* 9. Theme + rotation + status bar + activity system */
    if (ui_lock(1000)) {
        uint8_t saved_rotation = 0;
        svc_settings_get_rotation(&saved_rotation);
        ui_engine_set_rotation(saved_rotation);
        ESP_LOGI(TAG, "Rotation: %s", saved_rotation ? "portrait" : "landscape");

        uint8_t saved_theme = 0;
        svc_settings_get_theme(&saved_theme);
        if (saved_theme >= THEME_COUNT) saved_theme = 0;
        ui_theme_apply((cyberdeck_theme_id_t)saved_theme);

        ui_statusbar_init();
        ui_navbar_init();
        ui_activity_init();

        /* Push launcher as root activity */
        ui_activity_push(APP_ID_LAUNCHER, 0, app_launcher_get_cbs(), NULL);

        /* Initial statusbar placeholders */
        ui_statusbar_set_time(0, 0, 0);
        ui_statusbar_set_wifi(false, 0);
        ui_statusbar_set_battery(0, false);
        ui_statusbar_set_sdcard(false);

        ui_unlock();
    }

    /* 10. Check PIN lock */
    bool pin_enabled = false;
    svc_settings_get_pin_enabled(&pin_enabled);
    if (pin_enabled) {
        ESP_LOGI(TAG, "PIN lock enabled — pushing lockscreen");
        app_manager_lock();
    }

    /* 11. RTC → system time */
    if (hal_rtc_init() == ESP_OK) {
        hal_rtc_sync_to_system();
        ESP_LOGI(TAG, "RTC OK");
    } else {
        ESP_LOGW(TAG, "RTC init failed (continuing)");
    }

    /* 12. Gesture detection */
    if (ui_lock(1000)) {
        hal_gesture_init(gesture_cb);
        ui_unlock();
    }
    svc_event_register(EVT_GESTURE_HOME, on_gesture_event, NULL);
    svc_event_register(EVT_GESTURE_BACK, on_gesture_event, NULL);
    svc_event_register(EVT_GESTURE_LOCK, on_gesture_event, NULL);
    svc_event_register(EVT_DISPLAY_ROTATED, on_display_rotated, NULL);
    svc_event_register(EVT_NAV_PROCESSES, on_nav_processes, NULL);
    ESP_LOGI(TAG, "Gestures + nav OK");

    /* 13. Battery */
    if (hal_battery_init() == ESP_OK) {
        svc_battery_start();
        svc_event_register(EVT_BATTERY_UPDATED, on_battery_updated, NULL);
        ESP_LOGI(TAG, "Battery monitor OK");
    } else {
        ESP_LOGW(TAG, "Battery ADC init failed (continuing)");
    }

    /* 14. SD card */
    svc_event_register(EVT_SDCARD_MOUNTED,   on_sdcard_mounted,   NULL);
    svc_event_register(EVT_SDCARD_UNMOUNTED, on_sdcard_unmounted, NULL);
    if (hal_sdcard_mount() == ESP_OK) {
        svc_event_post(EVT_SDCARD_MOUNTED, NULL, 0);
        ESP_LOGI(TAG, "SD card mounted");
    } else {
        ESP_LOGW(TAG, "SD card not available (continuing)");
    }
    /* Background SD card poll task (low priority, checks every 5s) */
    xTaskCreate(sd_poll_task, "sd_poll", 4096, NULL, 2, NULL);

    /* 15. Time service */
    ESP_ERROR_CHECK(svc_time_init());
    ESP_LOGI(TAG, "Time service OK");

    /* 16. Downloader + OTA */
    ESP_ERROR_CHECK(svc_downloader_init());
    ESP_ERROR_CHECK(svc_ota_init());
    ESP_LOGI(TAG, "Downloader + OTA OK");

    /* 17. USB Serial/JTAG */
    usb_serial_jtag_driver_config_t usj_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    esp_err_t usj_ret = usb_serial_jtag_driver_install(&usj_cfg);
    ESP_LOGI(TAG, "USB Serial/JTAG: %s",
             usj_ret == ESP_OK ? "OK" : esp_err_to_name(usj_ret));

    /* 18. WiFi (last — auto-connects in background) */
    svc_event_register(EVT_WIFI_CONNECTED,    on_wifi_connected,    NULL);
    svc_event_register(EVT_WIFI_DISCONNECTED, on_wifi_disconnected, NULL);
    ESP_ERROR_CHECK(svc_wifi_init());
    svc_wifi_auto_connect();
    ESP_LOGI(TAG, "WiFi init OK (auto-connecting)");

    /* Boot complete toast */
    if (ui_lock(100)) {
        ui_effect_toast("BOOT OK — Phase 4", 2000);
        ui_unlock();
    }

    ESP_LOGI(TAG, "Boot complete");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
