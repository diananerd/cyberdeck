#pragma once

#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Cyberdeck event base */
ESP_EVENT_DECLARE_BASE(CYBERDECK_EVENT);

/* Event IDs */
typedef enum {
    /* WiFi */
    EVT_WIFI_CONNECTED,         /* data: NULL */
    EVT_WIFI_DISCONNECTED,      /* data: NULL */
    EVT_WIFI_SCAN_DONE,         /* data: NULL (results via svc_wifi API) */

    /* Time */
    EVT_TIME_SYNCED,            /* data: NULL */

    /* Battery */
    EVT_BATTERY_UPDATED,        /* data: uint8_t* pct */

    /* Audio */
    EVT_AUDIO_STATE_CHANGED,    /* data: NULL */
    EVT_AUDIO_NO_OUTPUT,        /* data: NULL (no BT module) */

    /* Downloads */
    EVT_DOWNLOAD_PROGRESS,      /* data: svc_download_progress_t* */
    EVT_DOWNLOAD_COMPLETE,      /* data: uint32_t* request_id */
    EVT_DOWNLOAD_ERROR,         /* data: uint32_t* request_id */

    /* OTA */
    EVT_OTA_STARTED,            /* data: NULL */
    EVT_OTA_PROGRESS,           /* data: uint8_t* pct */
    EVT_OTA_COMPLETE,           /* data: NULL */
    EVT_OTA_ERROR,              /* data: NULL */

    /* SD Card */
    EVT_SDCARD_MOUNTED,         /* data: NULL */
    EVT_SDCARD_UNMOUNTED,       /* data: NULL */

    /* Settings */
    EVT_SETTINGS_CHANGED,       /* data: const char* key name */

    /* Gestures */
    EVT_GESTURE_HOME,           /* data: NULL */
    EVT_GESTURE_BACK,           /* data: NULL */

    /* Display */
    EVT_DISPLAY_ROTATED,        /* data: uint8_t* rotation (0=landscape, 1=portrait) */

    /* Navigation bar */
    EVT_NAV_PROCESSES,          /* data: NULL — show process manager */
} cyberdeck_event_id_t;

/**
 * @brief Initialize the cyberdeck event loop.
 *        Call once early in boot.
 */
esp_err_t svc_event_init(void);

/**
 * @brief Post an event to the cyberdeck event bus.
 * @param event_id   Event ID from cyberdeck_event_id_t
 * @param data       Event data (copied internally), or NULL
 * @param data_size  Size of data, or 0
 */
esp_err_t svc_event_post(cyberdeck_event_id_t event_id,
                         const void *data, size_t data_size);

/**
 * @brief Register a handler for a specific event.
 * @param event_id  Event to listen for
 * @param handler   esp_event handler callback
 * @param ctx       User context
 */
esp_err_t svc_event_register(cyberdeck_event_id_t event_id,
                             esp_event_handler_t handler, void *ctx);

/**
 * @brief Register a handler for ALL cyberdeck events.
 */
esp_err_t svc_event_register_all(esp_event_handler_t handler, void *ctx);

/**
 * @brief Unregister a handler.
 */
esp_err_t svc_event_unregister(cyberdeck_event_id_t event_id,
                               esp_event_handler_t handler);

#ifdef __cplusplus
}
#endif
