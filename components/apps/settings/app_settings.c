/*
 * CyberDeck — Settings main menu
 * List of settings categories; each item opens a sub-screen via os_view_push (D5).
 */

#include "app_settings.h"
#include "app_registry.h"
#include "os_nav.h"
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
    const view_cbs_t *cbs;
} menu_item_t;

static const menu_item_t s_items[] = {
    { "WiFi",       "Networks & connections",       SETTINGS_SCR_WIFI,      &settings_wifi_cbs      },
    { "Display",    "Theme, rotation, brightness",  SETTINGS_SCR_DISPLAY,   &settings_display_cbs   },
    { "Time",       "Timezone & sync",              SETTINGS_SCR_TIME,      &settings_time_cbs      },
    { "Storage",    "SD card info",                 SETTINGS_SCR_STORAGE,   &settings_storage_cbs   },
    { "Security",   "PIN lock",                     SETTINGS_SCR_SECURITY,  &settings_security_cbs  },
    { "Bluetooth",  "Audio module",                 SETTINGS_SCR_BT,        &settings_bluetooth_cbs },
    { "Audio",      "Volume",                       SETTINGS_SCR_AUDIO,     &settings_audio_cbs     },
    { "About",      "Firmware & OTA",               SETTINGS_SCR_ABOUT,     &settings_about_cbs     },
};
#define ITEMS_COUNT  ((uint8_t)(sizeof(s_items) / sizeof(s_items[0])))

/* ---- Click callback context ---- */

typedef struct {
    uint8_t              scr_id;
    const view_cbs_t *cbs;
} item_ctx_t;

static void item_click_cb(uint32_t index, void *data)
{
    (void)index;
    item_ctx_t *ctx = (item_ctx_t *)data;
    if (!ctx || !ctx->cbs) return;
    /* D5: use os_view_push instead of direct ui_activity_push */
    os_view_push(APP_ID_SETTINGS, ctx->scr_id, ctx->cbs, NULL);
}

/* ---- Activity callbacks (D1) ---- */

static void *settings_on_create(lv_obj_t *screen, const view_args_t *args, void *app_data)
{
    (void)args;
    (void)app_data;

    ui_statusbar_set_title("SETTINGS");

    lv_obj_t *content = ui_common_content_area(screen);
    lv_obj_t *list    = ui_common_list(content);

    for (uint8_t i = 0; i < ITEMS_COUNT; i++) {
        const menu_item_t *item = &s_items[i];

        item_ctx_t *ctx = (item_ctx_t *)lv_mem_alloc(sizeof(item_ctx_t));
        if (!ctx) continue;
        ctx->scr_id = item->scr_id;
        ctx->cbs    = item->cbs;

        ui_common_list_add_two_line(list, item->label, item->subtitle,
                                    i, item_click_cb, ctx);
    }

    ESP_LOGI(TAG, "Settings menu created");
    return NULL;  /* no per-screen state needed */
}

static void settings_on_resume(lv_obj_t *screen, void *view_state, void *app_data)
{
    (void)screen;
    (void)view_state;
    (void)app_data;
    ui_statusbar_set_title("SETTINGS");
}

/* ---- Registration ---- */

esp_err_t app_settings_register(void)
{
    static const app_manifest_t manifest = {
        .id          = APP_ID_SETTINGS,
        .name        = "Settings",
        .icon        = "St",
        .type        = APP_TYPE_BUILTIN,
        .permissions = APP_PERM_SETTINGS | APP_PERM_WIFI | APP_PERM_SD,
        .storage_dir = NULL,
    };
    static const view_cbs_t cbs = {
        .on_create  = settings_on_create,
        .on_resume  = settings_on_resume,
        .on_pause   = NULL,
        .on_destroy = NULL,
    };
    os_app_register(&manifest, NULL, &cbs);
    ESP_LOGI(TAG, "Settings registered");
    return ESP_OK;
}
