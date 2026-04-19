/* deck_shell_apps — F29 system demo apps.
 *
 * Implements four bundled apps as bridge_ui activities:
 *
 *   APP_ID_TASKMAN   = 1   F29.2  Task Manager — walks activity stack
 *   APP_ID_COUNTER   = 4   F29.4  Counter      — local state + button
 *   APP_ID_NET_HELLO = 7   F29.3  Net Hello    — WiFi + HTTP demo
 *
 * The launcher in deck_shell_dl2.c registers cards for each + pushes
 * via the intent registry. SETTINGS (id=9) lives in
 * deck_shell_settings.c.
 *
 * These are NOT Deck source apps — they are system apps written in C
 * that ship with the OS. F30 will add actual .deck source apps that
 * load from /deck/apps/ via the runtime.
 */

#include "deck_shell_apps.h"
#include "deck_shell_intent.h"

#include "deck_bridge_ui.h"

#include "drivers/deck_sdi_wifi.h"
#include "drivers/deck_sdi_http.h"

#include "lvgl.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "shell.apps";

#define CD_PRIMARY      lv_color_hex(0x00FF41)
#define CD_PRIMARY_DIM  lv_color_hex(0x004D13)
#define CD_BG_DARK      lv_color_black()
#define CD_BG_CARD      lv_color_hex(0x0A0A0A)

#define UI_STATUSBAR_H  36
#define UI_NAVBAR_H     48

#define APP_ID_TASKMAN     1
#define APP_ID_COUNTER     4
#define APP_ID_NET_HELLO   7

/* Shared chrome — content area between statusbar and navbar. */
static lv_obj_t *make_content_area(deck_bridge_ui_activity_t *act,
                                    bool flex_column)
{
    lv_obj_t *scr = (lv_obj_t *)act->lvgl_screen;
    lv_obj_set_style_bg_color(scr, CD_BG_DARK, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);

    lv_obj_t *area = lv_obj_create(scr);
    lv_obj_set_size(area, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_top(area, UI_STATUSBAR_H + 16, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(area, UI_NAVBAR_H + 16, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(area, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_row(area, 14, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(area, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(area, 0, LV_PART_MAIN);
    if (flex_column) {
        lv_obj_set_flex_flow(area, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(area, LV_FLEX_ALIGN_START,
                                LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    }
    return area;
}

static lv_obj_t *make_title(lv_obj_t *parent, const char *text)
{
    lv_obj_t *t = lv_label_create(parent);
    lv_label_set_text(t, text);
    lv_obj_set_style_text_color(t, CD_PRIMARY, LV_PART_MAIN);
    return t;
}

static lv_obj_t *make_dim(lv_obj_t *parent, const char *text)
{
    lv_obj_t *t = lv_label_create(parent);
    lv_label_set_text(t, text);
    lv_obj_set_style_text_color(t, CD_PRIMARY_DIM, LV_PART_MAIN);
    return t;
}

static lv_obj_t *make_btn(lv_obj_t *parent, const char *text,
                           lv_event_cb_t cb, void *ud, bool primary)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_height(b, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(b, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(b, 8,  LV_PART_MAIN);
    lv_obj_set_style_radius(b, 12, LV_PART_MAIN);
    lv_obj_set_style_border_color(b, CD_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_border_width(b, 2, LV_PART_MAIN);
    if (primary) {
        lv_obj_set_style_bg_color(b, CD_PRIMARY, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    } else {
        lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, LV_PART_MAIN);
    }
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, primary ? CD_BG_DARK : CD_PRIMARY, LV_PART_MAIN);
    lv_obj_center(l);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

/* ============================================================ */
/*    F29.4 — Counter app                                       */
/* ============================================================ */

typedef struct {
    int32_t   count;
    lv_obj_t *value_label;
} counter_state_t;

static void counter_inc_cb(lv_event_t *e)
{
    counter_state_t *s = (counter_state_t *)lv_event_get_user_data(e);
    if (!s) return;
    s->count++;
    char buf[16];
    snprintf(buf, sizeof(buf), "%ld", (long)s->count);
    lv_label_set_text(s->value_label, buf);
}

static void counter_dec_cb(lv_event_t *e)
{
    counter_state_t *s = (counter_state_t *)lv_event_get_user_data(e);
    if (!s) return;
    s->count--;
    char buf[16];
    snprintf(buf, sizeof(buf), "%ld", (long)s->count);
    lv_label_set_text(s->value_label, buf);
}

static void counter_on_create(deck_bridge_ui_activity_t *act, void *intent_data)
{
    (void)intent_data;
    counter_state_t *s = lv_mem_alloc(sizeof(*s));
    if (!s) return;
    memset(s, 0, sizeof(*s));
    deck_bridge_ui_activity_set_state(s);

    lv_obj_t *area = make_content_area(act, true);
    make_title(area, "COUNTER");
    make_dim  (area, "VALUE:");

    s->value_label = lv_label_create(area);
    lv_label_set_text(s->value_label, "0");
    lv_obj_set_style_text_color(s->value_label, CD_PRIMARY, LV_PART_MAIN);

    lv_obj_t *row = lv_obj_create(area);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(row, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                            LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    make_btn(row, "DEC", counter_dec_cb, s, false);
    make_btn(row, "INC", counter_inc_cb, s, true);
}

static void counter_on_destroy(deck_bridge_ui_activity_t *act, void *intent_data)
{
    (void)intent_data;
    if (act->state) lv_mem_free(act->state);
    act->state = NULL;
}

static const deck_bridge_ui_lifecycle_t s_counter_cbs = {
    .on_create  = counter_on_create,
    .on_destroy = counter_on_destroy,
};

static deck_err_t counter_intent_resolver(const deck_shell_intent_t *intent)
{
    deck_bridge_ui_activity_push(APP_ID_COUNTER, intent->screen_id,
                                  &s_counter_cbs, NULL);
    return DECK_RT_OK;
}

/* ============================================================ */
/*    F29.2 — Task Manager                                      */
/* ============================================================ */

/* Human-readable label for a known-C-side app_id. Keeps the taskman list
 * legible; unknown ids (e.g. .deck apps with ids ≥ 100) fall through to
 * the raw number. */
static const char *taskman_app_name(uint16_t app_id)
{
    switch (app_id) {
        case 0: return "LAUNCHER";
        case 1: return "TASKMAN";
        case 4: return "COUNTER";
        case 7: return "NET HELLO";
        case 9: return "SETTINGS";
        default: return NULL;
    }
}

static void taskman_refresh_list(lv_obj_t *list_area)
{
    lv_obj_clean(list_area);
    size_t depth = deck_bridge_ui_activity_depth();
    char hdr[72];
    snprintf(hdr, sizeof(hdr), "ACTIVE: %u / 4    HEAP: %u KB",
             (unsigned)depth,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    make_dim(list_area, hdr);

    /* Iterate the whole stack, slot[0] (launcher) first → top last so
     * the "top" (which you're looking at) shows at the bottom of the
     * list — matches the mental model of a stack. */
    for (size_t i = 0; i < depth; i++) {
        deck_bridge_ui_activity_t *a = deck_bridge_ui_activity_at(i);
        if (!a) continue;
        const char *nm = taskman_app_name(a->app_id);
        char row[96];
        bool is_top = (i + 1 == depth);
        if (nm) {
            snprintf(row, sizeof(row), "%s %s (scr=%u)",
                     is_top ? "->" : "  ", nm, (unsigned)a->screen_id);
        } else {
            snprintf(row, sizeof(row), "%s app=%u scr=%u",
                     is_top ? "->" : "  ",
                     (unsigned)a->app_id, (unsigned)a->screen_id);
        }
        lv_obj_t *r = lv_label_create(list_area);
        lv_label_set_text(r, row);
        lv_obj_set_style_text_color(r, is_top ? CD_PRIMARY : CD_PRIMARY_DIM,
                                     LV_PART_MAIN);
    }
}

static lv_obj_t *s_taskman_list_area = NULL;
static lv_timer_t *s_taskman_refresh_timer = NULL;

/* Deferred pop — called from the confirm OK callback. lv_async_call so we
 * don't mutate the activity stack from inside the dialog dismiss path. */
static void taskman_kill_top_async(void *ud)
{
    (void)ud;
    /* Never pop when only the launcher is on the stack — pop_to_home is
     * a no-op in that case but we log something clearer. Also refuse to
     * pop if the top IS the taskman itself (that's us killing ourselves
     * — legal but surprising; we let it through so users can verify the
     * pop-to-launcher path works). */
    if (deck_bridge_ui_activity_depth() <= 1) {
        deck_bridge_ui_overlay_toast("Nothing to kill", 1200);
        return;
    }
    deck_sdi_err_t rc = deck_bridge_ui_activity_pop();
    if (rc != DECK_SDI_OK) {
        deck_bridge_ui_overlay_toast("Kill failed", 1500);
    }
}

static void taskman_kill_on_ok(void *ud)
{
    (void)ud;
    lv_async_call(taskman_kill_top_async, NULL);
}

static void taskman_kill_confirm_cb(lv_event_t *e)
{
    (void)e;
    deck_bridge_ui_overlay_confirm_cb(
        "KILL TOP APP",
        "Pop the topmost activity? Launcher cannot be killed.",
        "KILL", "CANCEL",
        taskman_kill_on_ok, NULL, NULL);
}

static void taskman_refresh_cb(lv_event_t *e)
{
    (void)e;
    if (s_taskman_list_area) taskman_refresh_list(s_taskman_list_area);
}

/* Auto-refresh every 1.5s so the HEAP counter behaves like a live gauge.
 * Also picks up depth changes if another app is pushed in the background
 * (unlikely today but defends against future async flows). */
static void taskman_auto_refresh_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_taskman_list_area) taskman_refresh_list(s_taskman_list_area);
}

static void taskman_on_create(deck_bridge_ui_activity_t *act, void *intent_data)
{
    (void)intent_data;
    lv_obj_t *area = make_content_area(act, true);
    make_title(area, "TASK MANAGER");

    s_taskman_list_area = lv_obj_create(area);
    lv_obj_set_size(s_taskman_list_area, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_taskman_list_area, CD_BG_CARD, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_taskman_list_area, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_taskman_list_area, CD_PRIMARY_DIM, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_taskman_list_area, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(s_taskman_list_area, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_taskman_list_area, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_taskman_list_area, 6, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_taskman_list_area, LV_FLEX_FLOW_COLUMN);

    taskman_refresh_list(s_taskman_list_area);

    /* Live heap gauge — timers fire on the LVGL task with the UI lock
     * held, so we can update labels without locking ourselves. Always
     * (re)create the timer on on_create — if a previous instance leaked
     * one (e.g. the activity got recreated during rotation before its
     * on_destroy fired), we'd end up with a stale timer firing against
     * a freed list area and a use-after-free on the next refresh. Be
     * defensive and delete any lingering timer before recreating. */
    if (s_taskman_refresh_timer) {
        lv_timer_del(s_taskman_refresh_timer);
        s_taskman_refresh_timer = NULL;
    }
    s_taskman_refresh_timer =
        lv_timer_create(taskman_auto_refresh_timer_cb, 1500, NULL);

    lv_obj_t *row = lv_obj_create(area);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(row, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    make_btn(row, "REFRESH", taskman_refresh_cb,    NULL, false);
    make_btn(row, "KILL TOP", taskman_kill_confirm_cb, NULL, true);
}

static void taskman_on_destroy(deck_bridge_ui_activity_t *act, void *intent_data)
{
    (void)act; (void)intent_data;
    if (s_taskman_refresh_timer) {
        lv_timer_del(s_taskman_refresh_timer);
        s_taskman_refresh_timer = NULL;
    }
    s_taskman_list_area = NULL;
}

static const deck_bridge_ui_lifecycle_t s_taskman_cbs = {
    .on_create  = taskman_on_create,
    .on_destroy = taskman_on_destroy,
};

static deck_err_t taskman_intent_resolver(const deck_shell_intent_t *intent)
{
    deck_bridge_ui_activity_push(APP_ID_TASKMAN, intent->screen_id,
                                  &s_taskman_cbs, NULL);
    return DECK_RT_OK;
}

/* ============================================================ */
/*    F29.3 — Net Hello                                         */
/* ============================================================ */

typedef struct {
    lv_obj_t *status_label;
    lv_obj_t *body_label;
} nethello_state_t;

static void nethello_show_status(nethello_state_t *s, const char *text,
                                  bool error)
{
    if (s && s->status_label) {
        lv_label_set_text(s->status_label, text);
        lv_obj_set_style_text_color(s->status_label,
                                     error ? CD_PRIMARY_DIM : CD_PRIMARY,
                                     LV_PART_MAIN);
    }
}

static void nethello_fetch_cb(lv_event_t *e)
{
    nethello_state_t *s = (nethello_state_t *)lv_event_get_user_data(e);
    if (!s) return;

    if (deck_sdi_wifi_status() != DECK_SDI_WIFI_CONNECTED) {
        nethello_show_status(s, "No WiFi — connect first", true);
        return;
    }

    nethello_show_status(s, "Fetching...", false);

    static char body[1024];
    deck_sdi_http_response_t resp = {0};
    deck_sdi_err_t r = deck_sdi_http_get("https://httpbin.org/get",
                                          body, sizeof(body) - 1, &resp);
    if (r != DECK_SDI_OK) {
        char msg[64];
        snprintf(msg, sizeof(msg), "HTTP error: %s", deck_sdi_strerror(r));
        nethello_show_status(s, msg, true);
        return;
    }
    body[resp.body_bytes] = '\0';
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "HTTP %d  %u bytes%s",
             resp.status_code, (unsigned)resp.body_bytes,
             resp.truncated ? " (truncated)" : "");
    nethello_show_status(s, hdr, false);
    /* Show first 200 bytes. */
    if (resp.body_bytes > 200) body[200] = '\0';
    if (s->body_label) lv_label_set_text(s->body_label, body);
}

static void nethello_connect_cb(lv_event_t *e)
{
    nethello_state_t *s = (nethello_state_t *)lv_event_get_user_data(e);
    if (!s) return;
    char ssid[33] = {0}, psk[65] = {0};
    /* Read credentials from NVS (cyberdeck namespace). User sets these
     * out of band; if absent, we surface a dim message. */
    extern deck_sdi_err_t deck_sdi_nvs_get_str(const char *ns, const char *key,
                                                 char *out, size_t out_size);
    deck_sdi_err_t a = deck_sdi_nvs_get_str("cyberdeck", "wifi_ssid",
                                              ssid, sizeof(ssid));
    deck_sdi_err_t b = deck_sdi_nvs_get_str("cyberdeck", "wifi_psk",
                                              psk,  sizeof(psk));
    if (a != DECK_SDI_OK || !*ssid) {
        nethello_show_status(s,
            "Set NVS: cyberdeck/wifi_ssid + wifi_psk", true);
        return;
    }
    nethello_show_status(s, "Connecting...", false);
    deck_sdi_err_t r = deck_sdi_wifi_connect(ssid,
                                              b == DECK_SDI_OK ? psk : NULL,
                                              15000);
    if (r != DECK_SDI_OK) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Connect failed: %s",
                 deck_sdi_strerror(r));
        nethello_show_status(s, msg, true);
        return;
    }
    char ip[16] = {0};
    deck_sdi_wifi_get_ip(ip, sizeof(ip));
    char ok[64];
    snprintf(ok, sizeof(ok), "Connected — %s", ip);
    nethello_show_status(s, ok, false);
}

static void nethello_on_create(deck_bridge_ui_activity_t *act, void *intent_data)
{
    (void)intent_data;
    nethello_state_t *s = lv_mem_alloc(sizeof(*s));
    if (!s) return;
    memset(s, 0, sizeof(*s));
    deck_bridge_ui_activity_set_state(s);

    lv_obj_t *area = make_content_area(act, true);
    make_title(area, "NET HELLO");

    lv_obj_t *row = lv_obj_create(area);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(row, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    make_btn(row, "CONNECT", nethello_connect_cb, s, false);
    make_btn(row, "FETCH",   nethello_fetch_cb,   s, true);

    s->status_label = lv_label_create(area);
    lv_label_set_text(s->status_label, "Idle");
    lv_obj_set_style_text_color(s->status_label, CD_PRIMARY_DIM, LV_PART_MAIN);

    s->body_label = lv_label_create(area);
    lv_label_set_long_mode(s->body_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s->body_label, lv_pct(100));
    lv_label_set_text(s->body_label, "");
    lv_obj_set_style_text_color(s->body_label, CD_PRIMARY, LV_PART_MAIN);
}

static void nethello_on_destroy(deck_bridge_ui_activity_t *act, void *intent_data)
{
    (void)intent_data;
    if (act->state) lv_mem_free(act->state);
    act->state = NULL;
}

static const deck_bridge_ui_lifecycle_t s_nethello_cbs = {
    .on_create  = nethello_on_create,
    .on_destroy = nethello_on_destroy,
};

static deck_err_t nethello_intent_resolver(const deck_shell_intent_t *intent)
{
    deck_bridge_ui_activity_push(APP_ID_NET_HELLO, intent->screen_id,
                                  &s_nethello_cbs, NULL);
    return DECK_RT_OK;
}

/* ============================================================ */

deck_err_t deck_shell_apps_register(void)
{
    deck_err_t r;
    if ((r = deck_shell_intent_register(APP_ID_COUNTER,   counter_intent_resolver))   != DECK_RT_OK) return r;
    if ((r = deck_shell_intent_register(APP_ID_TASKMAN,   taskman_intent_resolver))   != DECK_RT_OK) return r;
    if ((r = deck_shell_intent_register(APP_ID_NET_HELLO, nethello_intent_resolver)) != DECK_RT_OK) return r;
    ESP_LOGI(TAG, "registered demo apps: counter(4), taskman(1), net_hello(7)");
    return DECK_RT_OK;
}
