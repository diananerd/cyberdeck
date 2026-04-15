/*
 * CyberDeck — Settings main menu
 * List of settings categories; each item opens a sub-screen via ui_activity_push.
 */

#include "app_settings.h"
#include "app_registry.h"
#include "ui_activity.h"
#include "ui_theme.h"
#include "ui_statusbar.h"
#include "ui_common.h"
#include "esp_log.h"

static const char *TAG = "settings";

/* ---- Menu item definition ---- */

typedef struct {
    const char         *label;
    const char         *subtitle;
    uint8_t             scr_id;
    const activity_cbs_t *cbs;
} menu_item_t;

static const menu_item_t s_items[] = {
    { "WiFi",       "Networks & connections",     SETTINGS_SCR_WIFI,     &settings_wifi_cbs      },
    { "Display",    "Theme, rotation, brightness", SETTINGS_SCR_DISPLAY,  &settings_display_cbs   },
    { "Time",       "Timezone & sync",             SETTINGS_SCR_TIME,     &settings_time_cbs      },
    { "Storage",    "SD card info",                SETTINGS_SCR_STORAGE,  &settings_storage_cbs   },
    { "Security",   "PIN lock",                    SETTINGS_SCR_SECURITY, &settings_security_cbs  },
    { "Bluetooth",  "Audio module",                SETTINGS_SCR_BT,       &settings_bluetooth_cbs },
    { "Audio",      "Volume",                      SETTINGS_SCR_AUDIO,    &settings_audio_cbs     },
    { "About",      "Firmware & OTA",              SETTINGS_SCR_ABOUT,    &settings_about_cbs     },
};
#define ITEMS_COUNT  ((uint8_t)(sizeof(s_items) / sizeof(s_items[0])))

/* ---- Click callback context ---- */

typedef struct {
    uint8_t              scr_id;
    const activity_cbs_t *cbs;
} item_ctx_t;

static void item_click_cb(uint32_t index, void *data)
{
    (void)index;
    item_ctx_t *ctx = (item_ctx_t *)data;
    if (!ctx || !ctx->cbs) return;
    ui_activity_push(APP_ID_SETTINGS, ctx->scr_id, ctx->cbs, NULL);
}

/* ---- Activity callbacks ---- */

static void settings_on_create(lv_obj_t *screen, void *intent_data)
{
    (void)intent_data;

    ui_statusbar_set_title("SETTINGS");

    lv_obj_t *content = ui_common_content_area(screen);
    lv_obj_t *list    = ui_common_list(content);

    for (uint8_t i = 0; i < ITEMS_COUNT; i++) {
        const menu_item_t *item = &s_items[i];

        /* Allocate context from LVGL heap (lives for screen lifetime) */
        item_ctx_t *ctx = (item_ctx_t *)lv_mem_alloc(sizeof(item_ctx_t));
        if (!ctx) continue;
        ctx->scr_id = item->scr_id;
        ctx->cbs    = item->cbs;

        ui_common_list_add_two_line(list, item->label, item->subtitle,
                                    i, item_click_cb, ctx);
    }

    ESP_LOGI(TAG, "Settings menu created");
}

static void settings_on_resume(lv_obj_t *screen, void *state)
{
    (void)screen;
    (void)state;
    ui_statusbar_set_title("SETTINGS");
}

/* ---- Registration ---- */

esp_err_t app_settings_register(void)
{
    static const app_entry_t entry = {
        .app_id = APP_ID_SETTINGS,
        .name   = "Settings",
        .icon   = "St",
        .cbs    = {
            .on_create  = settings_on_create,
            .on_resume  = settings_on_resume,
            .on_pause   = NULL,
            .on_destroy = NULL,
        },
    };
    app_registry_register(&entry);
    ESP_LOGI(TAG, "Settings registered");
    return ESP_OK;
}
