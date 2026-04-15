/*
 * S3 Cyber-Deck — Settings > WiFi
 * Shows current connection, stored networks, and live AP scan results.
 * Connect sub-screen: SSID + password form.
 *
 * Thread-safety notes:
 *   - EVT_WIFI_SCAN_DONE fires on the event loop task → must lock LVGL.
 *   - All button callbacks fire on the LVGL task → no lock needed.
 *   - g_wifi_scr_state is NULL while the screen is not active.
 */

#include "app_settings.h"
#include "app_registry.h"
#include "ui_activity.h"
#include "ui_theme.h"
#include "ui_engine.h"
#include "ui_statusbar.h"
#include "ui_common.h"
#include "ui_effect.h"
#include "ui_keyboard.h"
#include "svc_settings.h"
#include "svc_wifi.h"
#include "svc_event.h"
#include "esp_wifi_types.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "settings_wifi";

/* ================================================================
 * WiFi Connect sub-screen
 * intent_data: heap-allocated char[33] with the target SSID.
 * ================================================================ */

typedef struct {
    char     ssid[33];
    lv_obj_t *pass_ta;
} connect_state_t;

static void connect_btn_cb(lv_event_t *e)
{
    connect_state_t *s = (connect_state_t *)lv_event_get_user_data(e);
    if (!s) return;

    const char *pass = lv_textarea_get_text(s->pass_ta);
    ui_keyboard_hide();

    ESP_LOGI(TAG, "Connecting to '%s'...", s->ssid);
    ui_effect_loading(true);

    /* Save credentials (slot 0 for simplicity) */
    svc_settings_wifi_set(0, s->ssid, pass);
    svc_settings_wifi_set_auto_idx(0);

    /* Trigger connection — result will come via EVT_WIFI_CONNECTED/DISCONNECTED */
    svc_wifi_connect(s->ssid, pass);

    ui_effect_loading(false);
    ui_effect_toast("Connecting...", 1500);

    /* Go back to the WiFi list screen */
    ui_activity_pop();
}

static void pass_ta_clicked_cb(lv_event_t *e)
{
    lv_obj_t *ta = (lv_obj_t *)lv_event_get_user_data(e);
    ui_keyboard_show(ta);
}

static void connect_on_create(lv_obj_t *screen, void *intent_data)
{
    const cyberdeck_theme_t *t = ui_theme_get();

    connect_state_t *s = (connect_state_t *)lv_mem_alloc(sizeof(connect_state_t));
    if (!s) {
        if (intent_data) free(intent_data);
        return;
    }
    memset(s, 0, sizeof(*s));
    ui_activity_set_state(s);

    /* Copy SSID from intent data and free it */
    if (intent_data) {
        strncpy(s->ssid, (char *)intent_data, sizeof(s->ssid) - 1);
        free(intent_data);
    }

    ui_statusbar_set_title("SETTINGS");

    lv_obj_t *content = ui_common_content_area(screen);

    /* SSID display */
    lv_obj_t *ssid_lbl = lv_label_create(content);
    lv_label_set_text(ssid_lbl, s->ssid);
    lv_obj_set_style_text_font(ssid_lbl, &CYBERDECK_FONT_MD, 0);
    lv_obj_set_style_text_color(ssid_lbl, t->primary, 0);

    /* Password field */
    lv_obj_t *pass_lbl = lv_label_create(content);
    lv_label_set_text(pass_lbl, "Password:");
    ui_theme_style_label_dim(pass_lbl, &CYBERDECK_FONT_SM);

    s->pass_ta = lv_textarea_create(content);
    lv_obj_set_width(s->pass_ta, LV_PCT(100));
    lv_textarea_set_one_line(s->pass_ta, true);
    lv_textarea_set_password_mode(s->pass_ta, true);
    lv_textarea_set_placeholder_text(s->pass_ta, "Enter password...");
    ui_theme_style_textarea(s->pass_ta);
    lv_obj_add_event_cb(s->pass_ta, pass_ta_clicked_cb,
                        LV_EVENT_CLICKED, s->pass_ta);

    ui_common_divider(content);

    lv_obj_t *connect_btn = ui_common_btn_full(content, "Connect");
    lv_obj_add_event_cb(connect_btn, connect_btn_cb, LV_EVENT_CLICKED, s);
}

static void connect_on_destroy(lv_obj_t *screen, void *state)
{
    (void)screen;
    ui_keyboard_hide();
    lv_mem_free(state);
}

static const activity_cbs_t s_connect_cbs = {
    .on_create  = connect_on_create,
    .on_resume  = NULL,
    .on_pause   = NULL,
    .on_destroy = connect_on_destroy,
};

/* ================================================================
 * WiFi main screen
 * ================================================================ */

typedef struct {
    lv_obj_t    *status_lbl;
    lv_obj_t    *scan_list;         /* container for scan results */
    lv_obj_t    *scan_btn_lbl;      /* label on scan button to show scanning status */
} wifi_scr_state_t;

/* Global pointer — NULL when the WiFi screen is not active */
static wifi_scr_state_t *g_wifi_scr_state = NULL;

/* ---- AP tap ---- */

typedef struct {
    char ssid[33];
} ap_ctx_t;

static void ap_tap_cb(lv_event_t *e)
{
    ap_ctx_t *ctx = (ap_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;

    /* Pass SSID as heap-allocated string to the connect sub-screen */
    char *ssid_copy = (char *)malloc(33);
    if (!ssid_copy) return;
    strncpy(ssid_copy, ctx->ssid, 32);
    ssid_copy[32] = '\0';

    ui_activity_push(APP_ID_SETTINGS, SETTINGS_SCR_WIFI + 100,
                     &s_connect_cbs, ssid_copy);
}

/* ---- Stored network forget ---- */

typedef struct {
    uint8_t slot;
} forget_ctx_t;

static void forget_cb(lv_event_t *e)
{
    forget_ctx_t *ctx = (forget_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;
    svc_settings_wifi_set(ctx->slot, "", "");
    ui_effect_toast("Network removed", 1200);
    ESP_LOGI(TAG, "Removed stored network at slot %d", ctx->slot);
}

/* ---- Populate scan results list ---- */

static void populate_scan_results(wifi_scr_state_t *s,
                                   wifi_ap_record_t *results, uint16_t count)
{
    lv_obj_clean(s->scan_list);

    if (count == 0) {
        lv_obj_t *lbl = lv_label_create(s->scan_list);
        lv_label_set_text(lbl, "No networks found");
        ui_theme_style_label_dim(lbl, &CYBERDECK_FONT_SM);
        return;
    }

    for (uint16_t i = 0; i < count; i++) {
        char entry[64];
        snprintf(entry, sizeof(entry), "%s  (%d dBm)",
                 (char *)results[i].ssid, results[i].rssi);

        ap_ctx_t *ctx = (ap_ctx_t *)lv_mem_alloc(sizeof(ap_ctx_t));
        if (!ctx) continue;
        strncpy(ctx->ssid, (char *)results[i].ssid, sizeof(ctx->ssid) - 1);

        lv_obj_t *row = lv_obj_create(s->scan_list);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        ui_theme_style_list_item(row);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_add_event_cb(row, ap_tap_cb, LV_EVENT_CLICKED, ctx);

        const cyberdeck_theme_t *t = ui_theme_get();
        lv_obj_set_style_bg_color(row, t->primary, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, entry);
        ui_theme_style_label(lbl, &CYBERDECK_FONT_SM);
    }
}

/* ---- Scan done event handler (called on event loop task) ---- */

static void scan_done_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg; (void)base; (void)id; (void)data;

    wifi_ap_record_t results[SVC_WIFI_SCAN_MAX];
    uint16_t count = SVC_WIFI_SCAN_MAX;
    svc_wifi_get_scan_results(results, &count);
    ESP_LOGI(TAG, "Scan done: %d APs", count);

    if (!g_wifi_scr_state) return;   /* Screen was closed before scan finished */

    if (ui_lock(200)) {
        if (g_wifi_scr_state) {
            populate_scan_results(g_wifi_scr_state, results, count);
            if (g_wifi_scr_state->scan_btn_lbl) {
                lv_label_set_text(g_wifi_scr_state->scan_btn_lbl, "Scan Networks");
            }
        }
        ui_unlock();
    }
}

/* ---- Scan button ---- */

static void scan_btn_cb(lv_event_t *e)
{
    wifi_scr_state_t *s = (wifi_scr_state_t *)lv_event_get_user_data(e);
    if (!s) return;
    lv_label_set_text(s->scan_btn_lbl, "Scanning...");
    lv_obj_clean(s->scan_list);
    svc_wifi_start_scan();
    ESP_LOGI(TAG, "WiFi scan started");
}

/* ---- Disconnect wrapper ---- */

static void wifi_disconnect_cb(lv_event_t *e) { (void)e; svc_wifi_disconnect(); }

/* ---- Activity callbacks ---- */

static void wifi_on_create(lv_obj_t *screen, void *intent_data)
{
    (void)intent_data;

    wifi_scr_state_t *s =
        (wifi_scr_state_t *)lv_mem_alloc(sizeof(wifi_scr_state_t));
    if (!s) return;
    memset(s, 0, sizeof(*s));
    ui_activity_set_state(s);
    g_wifi_scr_state = s;

    /* Register for scan results (fired from event loop task) */
    svc_event_register(EVT_WIFI_SCAN_DONE, scan_done_handler, NULL);

    ui_statusbar_set_title("SETTINGS");
    lv_obj_t *content = ui_common_content_area(screen);

    /* ---- Current connection ---- */
    char status_str[64];
    if (svc_wifi_is_connected()) {
        char ssid[33] = {0};
        svc_wifi_get_ssid(ssid, sizeof(ssid));
        snprintf(status_str, sizeof(status_str),
                 "Connected: %s (%d dBm)", ssid, svc_wifi_get_rssi());
    } else {
        snprintf(status_str, sizeof(status_str), "Not connected");
    }
    s->status_lbl = lv_label_create(content);
    lv_label_set_text(s->status_lbl, status_str);
    ui_theme_style_label(s->status_lbl, &CYBERDECK_FONT_MD);

    /* Disconnect button (only if connected) */
    if (svc_wifi_is_connected()) {
        lv_obj_t *dc_btn = ui_common_btn_full(content, "Disconnect");
        lv_obj_add_event_cb(dc_btn, wifi_disconnect_cb, LV_EVENT_CLICKED, NULL);
    }

    ui_common_divider(content);

    /* ---- Stored networks ---- */
    lv_obj_t *stored_lbl = lv_label_create(content);
    lv_label_set_text(stored_lbl, "Saved Networks");
    ui_theme_style_label(stored_lbl, &CYBERDECK_FONT_MD);

    lv_obj_t *stored_list = ui_common_list(content);
    bool any_stored = false;

    for (uint8_t i = 0; i < SVC_SETTINGS_WIFI_MAX; i++) {
        char ssid[33] = {0}, pass[65] = {0};
        if (svc_settings_wifi_get(i, ssid, sizeof(ssid),
                                  pass, sizeof(pass)) != ESP_OK) continue;
        if (ssid[0] == '\0') continue;
        any_stored = true;

        /* Row with SSID and Forget button */
        lv_obj_t *row = lv_obj_create(stored_list);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        ui_theme_style_list_item(row);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *ssid_lbl = lv_label_create(row);
        lv_label_set_text(ssid_lbl, ssid);
        ui_theme_style_label(ssid_lbl, &CYBERDECK_FONT_SM);

        forget_ctx_t *fctx = (forget_ctx_t *)lv_mem_alloc(sizeof(forget_ctx_t));
        if (fctx) {
            fctx->slot = i;
            lv_obj_t *forget_btn = ui_common_btn(row, "X");
            lv_obj_add_event_cb(forget_btn, forget_cb, LV_EVENT_CLICKED, fctx);
        }
    }

    if (!any_stored) {
        lv_obj_t *none_lbl = lv_label_create(stored_list);
        lv_label_set_text(none_lbl, "No saved networks");
        ui_theme_style_label_dim(none_lbl, &CYBERDECK_FONT_SM);
    }

    ui_common_divider(content);

    /* ---- Scan section ---- */
    lv_obj_t *scan_title = lv_label_create(content);
    lv_label_set_text(scan_title, "Available Networks");
    ui_theme_style_label(scan_title, &CYBERDECK_FONT_MD);

    /* Scan button */
    lv_obj_t *scan_btn = ui_common_btn_full(content, "Scan Networks");
    s->scan_btn_lbl = lv_obj_get_child(scan_btn, 0);  /* label inside button */
    lv_obj_add_event_cb(scan_btn, scan_btn_cb, LV_EVENT_CLICKED, s);

    /* Scan results list (starts empty) */
    s->scan_list = ui_common_list(content);
    lv_obj_t *hint = lv_label_create(s->scan_list);
    lv_label_set_text(hint, "Tap 'Scan Networks' to search");
    ui_theme_style_label_dim(hint, &CYBERDECK_FONT_SM);

    ESP_LOGI(TAG, "WiFi settings screen created");
}

static void wifi_on_pause(lv_obj_t *screen, void *state)
{
    (void)screen; (void)state;
    /* Keep g_wifi_scr_state so scan results can still update if we come back */
}

static void wifi_on_destroy(lv_obj_t *screen, void *state)
{
    (void)screen;
    svc_event_unregister(EVT_WIFI_SCAN_DONE, scan_done_handler);
    g_wifi_scr_state = NULL;
    lv_mem_free(state);
    ESP_LOGI(TAG, "WiFi settings screen destroyed");
}

const activity_cbs_t settings_wifi_cbs = {
    .on_create  = wifi_on_create,
    .on_resume  = NULL,
    .on_pause   = wifi_on_pause,
    .on_destroy = wifi_on_destroy,
};
