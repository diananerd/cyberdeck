/*
 * CyberDeck — Launcher home screen
 * Reads app list from the app registry, sorts alphabetically, and shows a
 * flex-wrap grid of app cards.  Tapping a card navigates if on_create is
 * registered, otherwise shows a "Coming soon" toast.
 *
 * app_launcher_register() also registers stub entries for all built-in apps
 * that don't yet have a full implementation, so the launcher always shows the
 * full grid even before every app is wired up.
 */

#include "app_launcher.h"
#include "app_registry.h"
#include "ui_activity.h"
#include "ui_intent.h"
#include "ui_theme.h"
#include "ui_statusbar.h"
#include "ui_effect.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "launcher";

/* ---- Card click context ---- */

typedef struct {
    uint8_t app_id;
} card_ctx_t;

static void card_click_cb(lv_event_t *e)
{
    card_ctx_t *ctx = (card_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) {
        ui_effect_toast("tap:no-ctx", 1200);
        return;
    }

    ESP_LOGI(TAG, "Tapped app_id=%d", ctx->app_id);

    if (!app_registry_get(ctx->app_id)) {
        ui_effect_toast("Coming soon...", 1500);
        return;
    }

    intent_t intent = {
        .app_id    = ctx->app_id,
        .screen_id = 0,
        .data      = NULL,
        .data_size = 0,
    };
    ui_intent_navigate(&intent);
}

/* ---- Activity callbacks ---- */

static void launcher_on_create(lv_obj_t *screen, void *intent_data)
{
    (void)intent_data;
    const cyberdeck_theme_t *t = ui_theme_get();

    /* ---- Collect apps from registry (skip id=0, the launcher itself) ---- */
    typedef struct {
        uint8_t     app_id;
        const char *name;
        const char *icon;
    } app_info_t;

    app_info_t apps[APP_ID_COUNT];
    uint8_t    app_count = 0;

    for (uint8_t id = 1; id < APP_ID_COUNT; id++) {
        const app_entry_t *e = app_registry_get_raw(id);
        if (!e || !e->name) continue;
        apps[app_count].app_id = id;
        apps[app_count].name   = e->name;
        apps[app_count].icon   = e->icon ? e->icon : "?";
        app_count++;
    }

    /* ---- Sort alphabetically (insertion sort, small N) ---- */
    for (uint8_t i = 1; i < app_count; i++) {
        app_info_t key = apps[i];
        int j = (int)i - 1;
        while (j >= 0 && strcmp(apps[j].name, key.name) > 0) {
            apps[j + 1] = apps[j];
            j--;
        }
        apps[j + 1] = key;
    }

    /* ---- Grid sizing — use screen content area (accounts for statusbar + navbar padding) ---- */
    const lv_coord_t gap     = 16;
    lv_coord_t       avail_w = lv_obj_get_content_width(screen);
    lv_coord_t       avail_h = lv_obj_get_content_height(screen);

    uint8_t    cols   = (avail_w >= 600) ? 5 : 3;
    uint8_t    rows   = ((uint8_t)app_count + cols - 1) / cols;
    if (rows == 0) rows = 1;

    lv_coord_t card_w  = (avail_w - gap * (cols + 1)) / cols;
    lv_coord_t card_h  = (avail_h - gap * (rows + 1)) / rows;
    lv_coord_t card_sz = (card_w < card_h) ? card_w : card_h;

    lv_coord_t margin_h = (avail_w - cols * card_sz - (cols - 1) * gap) / 2;
    lv_coord_t margin_v = (avail_h - rows * card_sz - (rows - 1) * gap) / 2;

    /* ---- Full-screen flex container ---- */
    lv_obj_t *cont = lv_obj_create(screen);
    lv_obj_set_size(cont, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_radius(cont, 0, 0);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(cont,   margin_h, 0);
    lv_obj_set_style_pad_right(cont,  margin_h, 0);
    lv_obj_set_style_pad_top(cont,    margin_v, 0);
    lv_obj_set_style_pad_bottom(cont, margin_v, 0);
    lv_obj_set_style_pad_column(cont, gap, 0);
    lv_obj_set_style_pad_row(cont,    gap, 0);

    /* ---- App cards ---- */
    for (uint8_t i = 0; i < app_count; i++) {
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
        lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(card, 4, 0);

        /* Press invert */
        lv_obj_set_style_bg_color(card, t->primary, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_border_color(card, t->primary, LV_STATE_PRESSED);

        /* Dim unimplemented apps */
        bool implemented = (app_registry_get(apps[i].app_id) != NULL);
        lv_color_t icon_color = implemented ? t->primary : t->primary_dim;
        lv_color_t text_color = implemented ? t->text_dim : t->text_dim;

        lv_obj_t *icon_lbl = lv_label_create(card);
        lv_label_set_text(icon_lbl, apps[i].icon);
        lv_obj_set_style_text_color(icon_lbl, icon_color, 0);
        lv_obj_set_style_text_font(icon_lbl, &CYBERDECK_FONT_LG, 0);
        lv_obj_set_style_text_color(icon_lbl, t->bg_dark, LV_STATE_PRESSED);

        lv_obj_t *name_lbl = lv_label_create(card);
        lv_label_set_text(name_lbl, apps[i].name);
        lv_obj_set_style_text_color(name_lbl, text_color, 0);
        lv_obj_set_style_text_font(name_lbl, &CYBERDECK_FONT_SM, 0);
        lv_obj_set_style_text_color(name_lbl, t->bg_dark, LV_STATE_PRESSED);

        card_ctx_t *ctx = (card_ctx_t *)lv_mem_alloc(sizeof(card_ctx_t));
        if (ctx) {
            ctx->app_id = apps[i].app_id;
            lv_obj_add_event_cb(card, card_click_cb, LV_EVENT_CLICKED, ctx);
        }
    }

    ui_statusbar_set_title("CYBERDECK");
    ESP_LOGI(TAG, "Created (%dx%d, card=%dpx, apps=%d)",
             cols, rows, (int)card_sz, app_count);
}

static void launcher_on_resume(lv_obj_t *screen, void *state)
{
    (void)screen;
    (void)state;
    ui_statusbar_set_title("CYBERDECK");
}

static const activity_cbs_t s_launcher_cbs = {
    .on_create  = launcher_on_create,
    .on_resume  = launcher_on_resume,
    .on_pause   = NULL,
    .on_destroy = NULL,
};

/* ---- Public API ---- */

const activity_cbs_t *app_launcher_get_cbs(void)
{
    return &s_launcher_cbs;
}

esp_err_t app_launcher_register(void)
{
    /* Register the launcher itself */
    static const app_entry_t launcher_entry = {
        .app_id = APP_ID_LAUNCHER,
        .name   = "Launcher",
        .icon   = NULL,
        .cbs    = {
            .on_create  = launcher_on_create,
            .on_resume  = launcher_on_resume,
            .on_pause   = NULL,
            .on_destroy = NULL,
        },
    };
    app_registry_register(&launcher_entry);

    /* Register lockscreen with app_manager */
    extern void launcher_lockscreen_register(void);
    launcher_lockscreen_register();

    ESP_LOGI(TAG, "Launcher registered");
    return ESP_OK;
}
