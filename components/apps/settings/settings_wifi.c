/*
 * S3 Cyber-Deck — Settings > WiFi
 *
 * Main screen: current connection details + auto-scanned network list.
 * No saved-networks management — credentials are stored transparently
 * on connect and used silently for auto-reconnect on boot.
 *
 * Auto-scan fires on create and every WIFI_SCAN_INTERVAL_MS while visible.
 * Connect sub-screen: SSID + password form with keyboard shown immediately.
 *
 * Thread-safety:
 *   EVT_WIFI_SCAN_DONE fires on the event-loop task → must lock LVGL.
 *   Button callbacks fire on the LVGL task → no lock needed.
 *   g_wifi_scr_state is NULL while the screen is not active.
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
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_wifi_types.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "settings_wifi";

#define WIFI_SCAN_INTERVAL_MS  12000   /* re-scan every 12 s while on screen */
#define WIFI_SCAN_MAX          SVC_WIFI_SCAN_MAX

/* ================================================================
 * Auth-mode helper
 * ================================================================ */

static const char *authmode_str(wifi_auth_mode_t m)
{
    switch (m) {
    case WIFI_AUTH_OPEN:         return "OPEN";
    case WIFI_AUTH_WEP:          return "WEP";
    case WIFI_AUTH_WPA_PSK:      return "WPA";
    case WIFI_AUTH_WPA2_PSK:     return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/2";
    case WIFI_AUTH_WPA3_PSK:     return "WPA3";
    default:                     return "SECURE";
    }
}

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
    ui_effect_loading(true);

    /* Persist credentials in first available slot (or update existing) */
    for (uint8_t i = 0; i < SVC_SETTINGS_WIFI_MAX; i++) {
        char existing_ssid[33] = {0};
        char existing_pass[65] = {0};
        svc_settings_wifi_get(i, existing_ssid, sizeof(existing_ssid),
                              existing_pass, sizeof(existing_pass));
        if (existing_ssid[0] == '\0' ||
            strncmp(existing_ssid, s->ssid, 32) == 0) {
            svc_settings_wifi_set(i, s->ssid, pass);
            svc_settings_wifi_set_auto_idx(i);
            break;
        }
    }

    svc_wifi_connect(s->ssid, pass);
    ui_effect_loading(false);
    ui_effect_toast("Connecting...", 1500);
    ui_activity_pop();
    ESP_LOGI(TAG, "Connecting to '%s'", s->ssid);
}

static void cancel_btn_cb(lv_event_t *e)
{
    (void)e;
    ui_keyboard_hide();
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

    if (intent_data) {
        strncpy(s->ssid, (char *)intent_data, sizeof(s->ssid) - 1);
        free(intent_data);
    }

    ui_statusbar_set_title("SETTINGS");

    lv_obj_t *content = ui_common_content_area(screen);

    /* SSID — large, primary */
    lv_obj_t *ssid_lbl = lv_label_create(content);
    lv_label_set_text(ssid_lbl, s->ssid);
    lv_obj_set_style_text_font(ssid_lbl, &CYBERDECK_FONT_LG, 0);
    lv_obj_set_style_text_color(ssid_lbl, t->primary, 0);
    lv_label_set_long_mode(ssid_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ssid_lbl, LV_PCT(100));

    /* Password field */
    lv_obj_t *pass_lbl = lv_label_create(content);
    lv_label_set_text(pass_lbl, "PASSWORD:");
    ui_theme_style_label_dim(pass_lbl, &CYBERDECK_FONT_SM);

    s->pass_ta = lv_textarea_create(content);
    lv_obj_set_width(s->pass_ta, LV_PCT(100));
    lv_textarea_set_one_line(s->pass_ta, true);
    lv_textarea_set_password_mode(s->pass_ta, true);
    lv_textarea_set_placeholder_text(s->pass_ta, "Enter password...");
    ui_theme_style_textarea(s->pass_ta);
    lv_obj_add_event_cb(s->pass_ta, pass_ta_clicked_cb,
                        LV_EVENT_CLICKED, s->pass_ta);

    /* Buttons pinned to bottom */
    ui_common_spacer(content);

    lv_obj_t *btn_row = ui_common_action_row(content);

    lv_obj_t *cancel_btn = ui_common_btn(btn_row, "Cancel");
    lv_obj_add_event_cb(cancel_btn, cancel_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *conn_btn = ui_common_btn(btn_row, "Connect");
    ui_common_btn_style_primary(conn_btn);
    lv_obj_add_event_cb(conn_btn, connect_btn_cb, LV_EVENT_CLICKED, s);
}

static void connect_on_resume(lv_obj_t *screen, void *state)
{
    (void)screen;
    connect_state_t *s = (connect_state_t *)state;
    /* Show keyboard now — screen is active so lv_scr_act() is correct */
    if (s) ui_keyboard_show(s->pass_ta);
}

static void connect_on_destroy(lv_obj_t *screen, void *state)
{
    (void)screen;
    ui_keyboard_hide();
    lv_mem_free(state);
}

static const activity_cbs_t s_connect_cbs = {
    .on_create  = connect_on_create,
    .on_resume  = connect_on_resume,
    .on_pause   = NULL,
    .on_destroy = connect_on_destroy,
};

/* ================================================================
 * WiFi main screen
 * ================================================================ */

typedef struct {
    lv_obj_t    *scan_status_lbl;  /* "SCANNING..." / "X APs" / "NO SIGNAL" */
    lv_obj_t    *scan_list;        /* container for scan results              */
    lv_timer_t  *scan_timer;       /* periodic auto-scan timer                */
    char         forget_ssid[33];  /* SSID to clear on Forget tap             */
} wifi_scr_state_t;

static wifi_scr_state_t *g_wifi_scr_state = NULL;

/* ---- AP tap ---- */

typedef struct { char ssid[33]; } ap_ctx_t;

static void ap_tap_cb(lv_event_t *e)
{
    ap_ctx_t *ctx = (ap_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;

    char *ssid_copy = (char *)malloc(33);
    if (!ssid_copy) return;
    strncpy(ssid_copy, ctx->ssid, 32);
    ssid_copy[32] = '\0';

    ui_activity_push(APP_ID_SETTINGS, SETTINGS_SCR_WIFI + 100,
                     &s_connect_cbs, ssid_copy);
}

/* ---- Disconnect ---- */

static void wifi_disconnect_cb(lv_event_t *e) { (void)e; svc_wifi_disconnect(); }

/* ---- Forget (clear saved credentials + disconnect) ---- */

static void wifi_forget_cb(lv_event_t *e)
{
    wifi_scr_state_t *s = (wifi_scr_state_t *)lv_event_get_user_data(e);
    if (!s || s->forget_ssid[0] == '\0') return;

    for (uint8_t i = 0; i < SVC_SETTINGS_WIFI_MAX; i++) {
        char slot_ssid[33] = {0};
        char slot_pass[65] = {0};
        svc_settings_wifi_get(i, slot_ssid, sizeof(slot_ssid),
                              slot_pass, sizeof(slot_pass));
        if (strncmp(slot_ssid, s->forget_ssid, 32) == 0) {
            svc_settings_wifi_set(i, "", "");
            ESP_LOGI(TAG, "Cleared saved slot %d for '%s'", i, s->forget_ssid);
            break;
        }
    }

    svc_wifi_disconnect();
    ui_effect_toast("Network forgotten", 1500);
    ESP_LOGI(TAG, "Forgot network '%s'", s->forget_ssid);
}

/* ---- Populate scan results ---- */

static void populate_scan_results(wifi_scr_state_t *s,
                                   wifi_ap_record_t *results, uint16_t count)
{
    lv_obj_clean(s->scan_list);

    if (count == 0) {
        lv_obj_t *lbl = lv_label_create(s->scan_list);
        lv_label_set_text(lbl, "No networks found");
        ui_theme_style_label_dim(lbl, &CYBERDECK_FONT_SM);
        char buf[16];
        snprintf(buf, sizeof(buf), "0 APs");
        lv_label_set_text(s->scan_status_lbl, buf);
        return;
    }

    char buf[24];
    snprintf(buf, sizeof(buf), "%d APs", count);
    lv_label_set_text(s->scan_status_lbl, buf);

    const cyberdeck_theme_t *t = ui_theme_get();
    char connected_ssid[33] = {0};
    bool is_connected = svc_wifi_is_connected();
    if (is_connected) svc_wifi_get_ssid(connected_ssid, sizeof(connected_ssid));

    for (uint16_t i = 0; i < count; i++) {
        const char *ssid = (char *)results[i].ssid;
        if (ssid[0] == '\0') continue;  /* hidden SSID */

        bool is_current = is_connected &&
                          (strncmp(ssid, connected_ssid, 32) == 0);

        /* Build secondary line: signal + auth */
        char secondary[48];
        snprintf(secondary, sizeof(secondary), "%d dBm  |  %s",
                 results[i].rssi, authmode_str(results[i].authmode));

        /* Row — text_color set on ROW so child labels inherit it on press too */
        lv_obj_t *row = lv_obj_create(s->scan_list);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        ui_theme_style_list_item(row);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(row, 3, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICK_FOCUSABLE);

        /* Normal state: connected rows use primary color, others use text */
        lv_obj_set_style_text_color(row, is_current ? t->primary : t->text, 0);
        /* Pressed state: always invert to bg_dark (children inherit this) */
        lv_obj_set_style_text_color(row, t->bg_dark, LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(row, t->primary, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);

        /* SSID label: font only, inherits color from row */
        char ssid_display[40];
        snprintf(ssid_display, sizeof(ssid_display),
                 is_current ? "> %s" : "  %s", ssid);
        lv_obj_t *ssid_lbl = lv_label_create(row);
        lv_label_set_text(ssid_lbl, ssid_display);
        lv_obj_set_style_text_font(ssid_lbl, &CYBERDECK_FONT_MD, 0);

        /* Secondary: font + dim opacity; color still inherited from row */
        lv_obj_t *detail_lbl = lv_label_create(row);
        lv_label_set_text(detail_lbl, secondary);
        lv_obj_set_style_text_font(detail_lbl, &CYBERDECK_FONT_SM, 0);
        lv_obj_set_style_text_opa(detail_lbl, LV_OPA_60, 0);

        ap_ctx_t *ctx = (ap_ctx_t *)lv_mem_alloc(sizeof(ap_ctx_t));
        if (ctx) {
            strncpy(ctx->ssid, ssid, sizeof(ctx->ssid) - 1);
            lv_obj_add_event_cb(row, ap_tap_cb, LV_EVENT_CLICKED, ctx);
        }
    }
}

/* ---- Scan done event handler (event-loop task) ---- */

static void scan_done_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg; (void)base; (void)id; (void)data;

    wifi_ap_record_t results[WIFI_SCAN_MAX];
    uint16_t count = WIFI_SCAN_MAX;
    svc_wifi_get_scan_results(results, &count);
    ESP_LOGI(TAG, "Scan done: %d APs", count);

    if (!g_wifi_scr_state) return;

    if (ui_lock(200)) {
        if (g_wifi_scr_state) {
            populate_scan_results(g_wifi_scr_state, results, count);
        }
        ui_unlock();
    }
}

/* ---- Auto-scan timer callback (LVGL task) ---- */

static void auto_scan_timer_cb(lv_timer_t *timer)
{
    wifi_scr_state_t *s = (wifi_scr_state_t *)timer->user_data;
    if (!s) return;
    lv_label_set_text(s->scan_status_lbl, "SCANNING...");
    svc_wifi_start_scan();
}

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

    svc_event_register(EVT_WIFI_SCAN_DONE, scan_done_handler, NULL);

    ui_statusbar_set_title("SETTINGS");
    lv_obj_t *content = ui_common_content_area(screen);

    /* ---- Current connection section ---- */
    if (svc_wifi_is_connected()) {
        char ssid[33] = {0};
        svc_wifi_get_ssid(ssid, sizeof(ssid));
        strncpy(s->forget_ssid, ssid, sizeof(s->forget_ssid) - 1);

        /* STATUS — label+value */
        ui_common_data_row(content, "STATUS:", "CONNECTED");

        /* SSID data row */
        ui_common_data_row(content, "SSID:", ssid);

        /* Signal row */
        char rssi_str[20];
        snprintf(rssi_str, sizeof(rssi_str), "%d dBm", svc_wifi_get_rssi());
        ui_common_data_row(content, "SIGNAL:", rssi_str);

        /* BSSID + channel from ap_info */
        wifi_ap_record_t ap = {0};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            char bssid_str[20];
            snprintf(bssid_str, sizeof(bssid_str),
                     "%02X:%02X:%02X:%02X:%02X:%02X",
                     ap.bssid[0], ap.bssid[1], ap.bssid[2],
                     ap.bssid[3], ap.bssid[4], ap.bssid[5]);
            ui_common_data_row(content, "BSSID:", bssid_str);

            char ch_str[8];
            snprintf(ch_str, sizeof(ch_str), "%d", ap.primary);
            ui_common_data_row(content, "CHANNEL:", ch_str);

            ui_common_data_row(content, "SECURITY:", authmode_str(ap.authmode));
        }

        /* IP address */
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif) {
            esp_netif_ip_info_t ip_info = {0};
            if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                char ip_str[24];
                snprintf(ip_str, sizeof(ip_str), IPSTR,
                         IP2STR(&ip_info.ip));
                ui_common_data_row(content, "IP ADDRESS:", ip_str);
            }
        }

        /* Action row: [Forget] secondary, [Disconnect] primary */
        lv_obj_t *dc_row = ui_common_action_row(content);

        lv_obj_t *forget_btn = ui_common_btn(dc_row, "Forget");
        lv_obj_add_event_cb(forget_btn, wifi_forget_cb, LV_EVENT_CLICKED, s);

        lv_obj_t *dc_btn = ui_common_btn(dc_row, "Disconnect");
        ui_common_btn_style_primary(dc_btn);
        lv_obj_add_event_cb(dc_btn, wifi_disconnect_cb, LV_EVENT_CLICKED, NULL);

    } else {
        ui_common_data_row(content, "STATUS:", "NOT CONNECTED");
    }

    /* Section gap: connection details → scan list */
    ui_common_section_gap(content);

    /* ---- Scan section header ---- */
    lv_obj_t *scan_hdr = lv_obj_create(content);
    lv_obj_set_width(scan_hdr, LV_PCT(100));
    lv_obj_set_height(scan_hdr, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(scan_hdr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(scan_hdr, 0, 0);
    lv_obj_set_style_pad_all(scan_hdr, 0, 0);
    lv_obj_set_flex_flow(scan_hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(scan_hdr, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(scan_hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *avail_lbl = lv_label_create(scan_hdr);
    lv_label_set_text(avail_lbl, "AVAILABLE NETWORKS");
    ui_theme_style_label_dim(avail_lbl, &CYBERDECK_FONT_SM);

    s->scan_status_lbl = lv_label_create(scan_hdr);
    lv_label_set_text(s->scan_status_lbl, "SCANNING...");
    ui_theme_style_label_dim(s->scan_status_lbl, &CYBERDECK_FONT_SM);

    /* ---- Scan results list ---- */
    s->scan_list = ui_common_list(content);

    lv_obj_t *hint = lv_label_create(s->scan_list);
    lv_label_set_text(hint, "Scanning...");
    ui_theme_style_label_dim(hint, &CYBERDECK_FONT_SM);

    /* Start first scan immediately + periodic timer */
    svc_wifi_start_scan();
    s->scan_timer = lv_timer_create(auto_scan_timer_cb, WIFI_SCAN_INTERVAL_MS, s);

    ESP_LOGI(TAG, "WiFi screen created, auto-scan started");
}

static void wifi_on_resume(lv_obj_t *screen, void *state)
{
    (void)screen;
    wifi_scr_state_t *s = (wifi_scr_state_t *)state;
    if (!s) return;
    /* Restart scan after returning from connect sub-screen */
    lv_label_set_text(s->scan_status_lbl, "SCANNING...");
    svc_wifi_start_scan();
    if (s->scan_timer) lv_timer_reset(s->scan_timer);
}

static void wifi_on_pause(lv_obj_t *screen, void *state)
{
    (void)screen; (void)state;
}

static void wifi_on_destroy(lv_obj_t *screen, void *state)
{
    (void)screen;
    wifi_scr_state_t *s = (wifi_scr_state_t *)state;
    svc_event_unregister(EVT_WIFI_SCAN_DONE, scan_done_handler);
    if (s && s->scan_timer) {
        lv_timer_del(s->scan_timer);
    }
    g_wifi_scr_state = NULL;
    lv_mem_free(state);
    ESP_LOGI(TAG, "WiFi screen destroyed");
}

const activity_cbs_t settings_wifi_cbs = {
    .on_create  = wifi_on_create,
    .on_resume  = wifi_on_resume,
    .on_pause   = wifi_on_pause,
    .on_destroy = wifi_on_destroy,
};
