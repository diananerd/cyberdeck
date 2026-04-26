/* deck_shell_lockscreen — PIN entry overlay.
 *
 * Renders a full-screen lockscreen on lv_layer_top:
 *   - "ENTER PIN" header
 *   - dot indicators (4 dots, fill as digits entered)
 *   - 3x4 numpad: 1-9, BACKSPACE, 0, OK
 *
 * Verify hits deck_sdi_security_verify_pin. On success, removes itself
 * + invokes on_unlocked. Wrong PIN flashes a "WRONG PIN" toast and
 * resets the entry buffer.
 *
 * While visible, navigation is locked (deck_shell_nav_lock(true)).
 */

#include "deck_shell_lockscreen.h"
#include "deck_shell_intent.h"

#include "deck_bridge_ui.h"
#include "drivers/deck_sdi_security.h"
#include "drivers/deck_sdi_nvs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "esp_log.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "shell.lock";

#define PIN_MAX_LEN  6

#define CD_PRIMARY      lv_color_hex(0x00FF41)
#define CD_PRIMARY_DIM  lv_color_hex(0x004D13)
#define CD_BG_DARK      lv_color_black()
#define CD_BG_CARD      lv_color_hex(0x0A0A0A)

typedef struct {
    lv_obj_t              *backdrop;
    lv_obj_t              *dots;             /* container with N dot labels */
    char                   pin_buf[PIN_MAX_LEN + 1];
    uint8_t                pin_len;
    deck_shell_unlock_cb_t on_unlocked;
} lock_state_t;

static lock_state_t s_state = {0};

static void update_dots(void)
{
    if (!s_state.dots) return;
    uint32_t n = lv_obj_get_child_cnt(s_state.dots);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *d = lv_obj_get_child(s_state.dots, i);
        if (i < s_state.pin_len) {
            lv_label_set_text(d, LV_SYMBOL_BULLET);
            lv_obj_set_style_text_color(d, CD_PRIMARY, LV_PART_MAIN);
        } else {
            lv_label_set_text(d, "-");
            lv_obj_set_style_text_color(d, CD_PRIMARY_DIM, LV_PART_MAIN);
        }
    }
}

static void dismiss_lock(void)
{
    /* Reveal the docks (they were started hidden by statusbar_init /
     * navbar_init). The chrome appears the same render frame as the
     * backdrop disappears. */
    deck_bridge_ui_statusbar_set_visible(true);
    deck_bridge_ui_navbar_set_visible(true);
    if (deck_bridge_ui_lock(200)) {
        if (s_state.backdrop) {
            lv_obj_del(s_state.backdrop);
            s_state.backdrop = NULL;
        }
        deck_bridge_ui_unlock();
    }
    s_state.dots = NULL;
    s_state.pin_len = 0;
    memset(s_state.pin_buf, 0, sizeof(s_state.pin_buf));
    deck_shell_nav_lock(false);
}

/* Defer the unlock callback off the LVGL task. verify_and_maybe_dismiss
 * runs inside an LVGL key event callback. If the unlock cb does heavy
 * work (push activity, scan apps, run canaries) directly on the LVGL
 * task, the LCD's GDMA frame queue starves and the descriptor chain
 * corrupts (panics gdma_default_tx_isr).
 *
 * We hand the cb off to a dedicated FreeRTOS task. The cb path includes
 * the full deck parser + interpreter + state-machine + push_snapshot
 * recursion for each of the 6 reference apps; 8 KB overflows. The main
 * task is sized at 24 KB for exactly this reason (Concept #68 in
 * REPORTS.md), so we match it. */
#define UNLOCK_TASK_STACK 24576
static void unlock_worker_task(void *arg)
{
    deck_shell_unlock_cb_t cb = (deck_shell_unlock_cb_t)arg;
    /* Yield 100 ms so the lockscreen-dismiss frame finishes hitting the
     * LCD before the cb starts pushing more renders. */
    vTaskDelay(pdMS_TO_TICKS(100));
    if (cb) cb();
    vTaskDelete(NULL);
}

/* Run from lv_async_call AFTER the LVGL key event finishes propagating.
 * Doing the dismiss + worker spawn directly from the key event handler
 * deletes the numpad button mid-event, leaving LVGL's post-processing
 * with a dangling pointer → SoC reset on the 4th digit. */
static void unlock_async_cb(void *arg)
{
    (void)arg;
    deck_shell_unlock_cb_t cb = s_state.on_unlocked;
    dismiss_lock();
    if (cb) {
        BaseType_t rc = xTaskCreatePinnedToCore(
            unlock_worker_task, "deck.unlock", UNLOCK_TASK_STACK,
            (void *)cb, 5, NULL, 0);
        if (rc != pdPASS) {
            ESP_LOGE(TAG, "unlock worker task create failed; running inline");
            cb();
        }
    }
}

static void verify_and_maybe_dismiss(void)
{
    s_state.pin_buf[s_state.pin_len] = '\0';
    bool ok;
    if (!deck_sdi_security_has_pin()) {
        ok = true;
    } else {
        ok = (deck_sdi_security_verify_pin(s_state.pin_buf) == DECK_SDI_OK);
    }
    if (ok) {
        ESP_LOGI(TAG, "unlock OK — deferring dismiss to async");
        lv_async_call(unlock_async_cb, NULL);
    } else {
        ESP_LOGW(TAG, "unlock FAILED");
        deck_bridge_ui_overlay_toast("WRONG PIN", 1500);
        s_state.pin_len = 0;
        update_dots();
    }
}

static void key_event_cb(lv_event_t *e)
{
    char k = (char)(intptr_t)lv_event_get_user_data(e);
    if (k == 'B') {
        if (s_state.pin_len > 0) s_state.pin_len--;
        update_dots();
        return;
    }
    if (k == 'K') {
        if (s_state.pin_len >= 4) verify_and_maybe_dismiss();
        return;
    }
    if (s_state.pin_len < PIN_MAX_LEN) {
        s_state.pin_buf[s_state.pin_len++] = k;
        update_dots();
        if (s_state.pin_len == 4) verify_and_maybe_dismiss();
    }
}

static void make_key(lv_obj_t *parent, const char *text, char id_char)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 96, 64);
    lv_obj_set_style_bg_color(btn, CD_BG_CARD, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, CD_PRIMARY, 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_radius(btn, 6, 0);
    /* Press inverts. */
    lv_obj_set_style_bg_color(btn, CD_PRIMARY, LV_STATE_PRESSED);

    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, CD_PRIMARY, 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(l, CD_BG_DARK, LV_STATE_PRESSED);
    lv_obj_center(l);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_CLICK_FOCUSABLE);

    lv_obj_add_event_cb(btn, key_event_cb, LV_EVENT_CLICKED,
                         (void *)(intptr_t)id_char);
}

static void show_full_lockscreen(void)
{
    if (s_state.backdrop) return;
    /* Docks start hidden in statusbar_init / navbar_init and stay that
     * way while the lockscreen is up. dismiss_lock reveals them. */

    if (!deck_bridge_ui_lock(500)) return;

    /* Mount on lv_layer_sys() — same layer as the navbar — so the dialog
     * is on top of every other UI element when navbar is reshown via a
     * future force-show path. With the docks hidden in this flow, this
     * is mostly defensive. */
    lv_obj_t *b = lv_obj_create(lv_layer_sys());
    lv_obj_set_size(b, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(b, CD_BG_DARK, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(b, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(b, 24, LV_PART_MAIN);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER,
                            LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(b, 24, LV_PART_MAIN);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    s_state.backdrop = b;

    lv_obj_t *header = lv_label_create(b);
    lv_label_set_text(header, "ENTER PIN");
    lv_obj_set_style_text_color(header, CD_PRIMARY, 0);
    lv_obj_set_style_text_font(header, &lv_font_montserrat_24, 0);

    /* Dots row — XL font, primary when filled, dim when empty. */
    lv_obj_t *dots = lv_obj_create(b);
    lv_obj_set_size(dots, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(dots, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dots, 0, 0);
    lv_obj_set_style_pad_all(dots, 0, 0);
    lv_obj_set_style_pad_column(dots, 24, 0);
    lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(dots, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 4; i++) {
        lv_obj_t *d = lv_label_create(dots);
        lv_label_set_text(d, "-");
        lv_obj_set_style_text_color(d, CD_PRIMARY_DIM, 0);
        lv_obj_set_style_text_font(d, &lv_font_montserrat_40, 0);
    }
    s_state.dots = dots;

    /* Numpad — 4 rows × 3 cols. */
    lv_obj_t *pad = lv_obj_create(b);
    lv_obj_set_size(pad, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(pad, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pad, 0, 0);
    lv_obj_set_style_pad_all(pad, 0, 0);
    lv_obj_set_style_pad_column(pad, 12, 0);
    lv_obj_set_style_pad_row(pad, 12, 0);
    lv_obj_set_flex_flow(pad, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(pad, LV_FLEX_ALIGN_CENTER,
                            LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(pad, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(pad, 3 * 96 + 2 * 12);

    const char *labels[] = {
        "1","2","3", "4","5","6", "7","8","9",
        LV_SYMBOL_BACKSPACE, "0", LV_SYMBOL_OK,
    };
    const char ids[]    = { '1','2','3','4','5','6','7','8','9','B','0','K' };
    for (int i = 0; i < 12; i++) {
        make_key(pad, labels[i], ids[i]);
    }

    deck_shell_nav_lock(true);
    deck_bridge_ui_unlock();
    ESP_LOGI(TAG, "lockscreen visible");
}

void deck_shell_lockscreen_show(deck_shell_unlock_cb_t cb)
{
    s_state.on_unlocked = cb;
    s_state.pin_len     = 0;
    /* Diagnostic — read raw NVS state directly (independent of the
     * security driver) so we can tell whether the slot is empty vs
     * the read happening too early. NVS is initialized in app_main
     * before deck_shell_dl2_boot, but this surfaces any race or
     * namespace mismatch in the log. */
    {
        uint8_t probe[16];
        size_t plen = sizeof(probe);
        deck_sdi_err_t pr = deck_sdi_nvs_get_blob("deck.sec", "salt", probe, &plen);
        ESP_LOGI(TAG, "NVS probe deck.sec/salt: %s (len=%u)",
                 deck_sdi_strerror(pr), (unsigned)plen);
    }
    bool has = deck_sdi_security_has_pin();
    ESP_LOGI(TAG, "security.has_pin → %s", has ? "true" : "false");
    if (!has) {
        ESP_LOGI(TAG, "no PIN set — skipping lockscreen");
        if (cb) cb();
        return;
    }
    show_full_lockscreen();
}

void deck_shell_lockscreen_lock(deck_shell_unlock_cb_t cb)
{
    s_state.on_unlocked = cb;
    s_state.pin_len     = 0;
    show_full_lockscreen();
}

bool deck_shell_lockscreen_is_visible(void)
{
    return s_state.backdrop != NULL;
}
