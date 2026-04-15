/*
 * CyberDeck — Settings App
 * Main menu + sub-screen IDs for all settings categories.
 */

#pragma once

#include "esp_err.h"
#include "ui_activity.h"
#include "os_nav.h"       /* os_view_push/pop for sub-screens (D5) */
#include "app_registry.h" /* APP_PERM_*, os_app_register (C1/C3) */

#ifdef __cplusplus
extern "C" {
#endif

/* Settings screen IDs (screen_id field in activity push calls) */
#define SETTINGS_SCR_MAIN     0
#define SETTINGS_SCR_WIFI     1
#define SETTINGS_SCR_DISPLAY  2
#define SETTINGS_SCR_TIME     3
#define SETTINGS_SCR_STORAGE  4
#define SETTINGS_SCR_SECURITY 5
#define SETTINGS_SCR_ABOUT    6
#define SETTINGS_SCR_BT       7
#define SETTINGS_SCR_AUDIO    8

/**
 * Register the settings app with the app registry.
 * Call during boot after app_registry_init().
 */
esp_err_t app_settings_register(void);

/* Sub-screen activity callbacks — defined in each settings_*.c file */
extern const activity_cbs_t settings_wifi_cbs;
extern const activity_cbs_t settings_display_cbs;
extern const activity_cbs_t settings_time_cbs;
extern const activity_cbs_t settings_storage_cbs;
extern const activity_cbs_t settings_security_cbs;
extern const activity_cbs_t settings_about_cbs;
extern const activity_cbs_t settings_bluetooth_cbs;
extern const activity_cbs_t settings_audio_cbs;

#ifdef __cplusplus
}
#endif
