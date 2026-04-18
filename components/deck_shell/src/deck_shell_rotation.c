/* deck_shell_rotation — display rotation persisted in NVS.
 *
 * Reads/writes namespace "cyberdeck" key "display_rot" (i64).
 * On boot, deck_shell_rotation_restore() applies the saved rotation
 * to the bridge UI; settings can call deck_shell_rotation_set() to
 * change + persist.
 */

#include "deck_shell_rotation.h"
#include "deck_bridge_ui.h"
#include "drivers/deck_sdi_nvs.h"

#include "esp_log.h"

static const char *TAG = "shell.rot";
#define NVS_NS    "cyberdeck"
#define NVS_KEY   "display_rot"

deck_err_t deck_shell_rotation_restore(void)
{
    int64_t v = 0;
    deck_sdi_err_t r = deck_sdi_nvs_get_i64(NVS_NS, NVS_KEY, &v);
    if (r != DECK_SDI_OK) {
        if (r != DECK_SDI_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "nvs get failed: %s", deck_sdi_strerror(r));
        }
        return DECK_RT_OK;     /* nothing saved → keep default 0 */
    }
    if (v < 0 || v > 3) {
        ESP_LOGW(TAG, "stored rotation out of range (%lld) — ignoring",
                 (long long)v);
        return DECK_RT_OK;
    }
    if (v == 0) return DECK_RT_OK; /* already default */
    deck_sdi_err_t rr = deck_bridge_ui_set_rotation(
        (deck_bridge_ui_rotation_t)v);
    if (rr != DECK_SDI_OK) {
        ESP_LOGW(TAG, "set_rotation: %s", deck_sdi_strerror(rr));
        return DECK_RT_INTERNAL;
    }
    ESP_LOGI(TAG, "restored rotation %d", (int)v);
    return DECK_RT_OK;
}

deck_err_t deck_shell_rotation_set(deck_bridge_ui_rotation_t rot)
{
    if (rot > DECK_BRIDGE_UI_ROT_270) return DECK_RT_INTERNAL;
    deck_sdi_err_t rr = deck_bridge_ui_set_rotation(rot);
    if (rr != DECK_SDI_OK) {
        ESP_LOGW(TAG, "set_rotation: %s", deck_sdi_strerror(rr));
        return DECK_RT_INTERNAL;
    }
    deck_sdi_err_t r = deck_sdi_nvs_set_i64(NVS_NS, NVS_KEY, (int64_t)rot);
    if (r != DECK_SDI_OK) {
        ESP_LOGW(TAG, "nvs persist failed: %s", deck_sdi_strerror(r));
    }
    return DECK_RT_OK;
}
