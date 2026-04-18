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
    if (s_state.backdrop) {
        lv_obj_del(s_state.backdrop);
        s_state.backdrop = NULL;
    }
    s_state.dots = NULL;
    s_state.pin_len = 0;
    memset(s_state.pin_buf, 0, sizeof(s_state.pin_buf));
    deck_shell_nav_lock(false);
}

static void verify_and_maybe_dismiss(void)
{
    s_state.pin_buf[s_state.pin_len] = '\0';
    deck_sdi_err_t r = deck_sdi_security_verify_pin(s_state.pin_buf);
    if (r == DECK_SDI_OK) {
        ESP_LOGI(TAG, "unlock OK");
        deck_shell_unlock_cb_t cb = s_state.on_unlocked;
        dismiss_lock();
        if (cb) cb();
    } else {
        ESP_LOGW(TAG, "unlock FAILED (%s)", deck_sdi_strerror(r));
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
    lv_obj_set_size(btn, 80, 56);
    lv_obj_set_style_bg_color(btn, CD_BG_CARD, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, CD_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 6, LV_PART_MAIN);

    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, CD_PRIMARY, LV_PART_MAIN);
    lv_obj_center(l);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_CLICK_FOCUSABLE);

    lv_obj_add_event_cb(btn, key_event_cb, LV_EVENT_CLICKED,
                         (void *)(intptr_t)id_char);
}

static void show_full_lockscreen(void)
{
    if (s_state.backdrop) return;
    if (!deck_bridge_ui_lock(500)) return;

    lv_obj_t *b = lv_obj_create(lv_layer_top());
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
    lv_obj_set_style_text_color(header, CD_PRIMARY, LV_PART_MAIN);

    /* Dots row. */
    lv_obj_t *dots = lv_obj_create(b);
    lv_obj_set_size(dots, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(dots, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(dots, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(dots, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(dots, 16, LV_PART_MAIN);
    lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
    for (int i = 0; i < 4; i++) {
        lv_obj_t *d = lv_label_create(dots);
        lv_label_set_text(d, "-");
        lv_obj_set_style_text_color(d, CD_PRIMARY_DIM, LV_PART_MAIN);
    }
    s_state.dots = dots;

    /* Numpad — 4 rows × 3 cols. */
    lv_obj_t *pad = lv_obj_create(b);
    lv_obj_set_size(pad, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(pad, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(pad, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(pad, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(pad, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_row(pad, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(pad, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(pad, LV_FLEX_ALIGN_CENTER,
                            LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_width(pad, 3 * 80 + 2 * 8);

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
    if (!deck_sdi_security_has_pin()) {
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
