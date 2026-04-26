/*
 * CyberDeck — DL1 bootstrap
 *
 * Minimal boot path: init NVS + USB Serial/JTAG console, print device id
 * and free heap, then idle. The Deck runtime is wired in from F1 onward
 * (components/deck_sdi, components/deck_runtime, components/deck_shell).
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "driver/usb_serial_jtag.h"

#include "deck_sdi_registry.h"
#include "drivers/deck_sdi_nvs.h"
#include "drivers/deck_sdi_fs.h"
#include "drivers/deck_sdi_info.h"
#include "drivers/deck_sdi_time.h"
#include "drivers/deck_sdi_shell.h"
#include "drivers/deck_sdi_wifi.h"
#include "drivers/deck_sdi_http.h"
#include "drivers/deck_sdi_battery.h"
#include "drivers/deck_sdi_security.h"
#include "drivers/deck_sdi_bridge_ui.h"
#include "drivers/deck_sdi_display.h"
#include "drivers/deck_sdi_touch.h"

#include "deck_runtime.h"
#include "deck_dvc.h"
#include "deck_bridge_ui.h"
#include "deck_conformance.h"
#include "deck_shell.h"
#include "deck_shell_dl2.h"
#include "deck_shell_intent.h"

static const char *TAG = "cyberdeck";

static void log_device_id(void)
{
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        ESP_LOGI(TAG, "Device ID: %02x%02x%02x%02x%02x%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        ESP_LOGW(TAG, "Could not read MAC");
    }
}

static void log_heap(const char *label)
{
    size_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t spiram   = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "Heap %s: internal=%u bytes, spiram=%u bytes",
             label, (unsigned)internal, (unsigned)spiram);
}

#if CONFIG_DECK_SDI_SELFTEST
static void run_one(const char *name, deck_sdi_err_t (*fn)(void))
{
    deck_sdi_err_t r = fn();
    if (r != DECK_SDI_OK) {
        ESP_LOGE(TAG, "%s selftest FAILED: %s", name, deck_sdi_strerror(r));
    }
}

static void run_dvc_selftest(void)
{
    deck_err_t r = deck_dvc_selftest();
    if (r != DECK_RT_OK) {
        ESP_LOGE(TAG, "dvc selftest FAILED: %s", deck_err_name(r));
    }
}

static void run_sdi_selftests(void)
{
    ESP_LOGI(TAG, "--- SDI selftests ---");
    run_one("nvs",       deck_sdi_nvs_selftest);
    run_one("fs",        deck_sdi_fs_selftest);
    run_one("info",      deck_sdi_info_selftest);
    run_one("time",      deck_sdi_time_selftest);
    run_one("shell",     deck_sdi_shell_selftest);
    /* DL2 drivers — registered conditionally below; selftests no-op
     * gracefully if the driver isn't registered yet. */
    run_one("wifi",      deck_sdi_wifi_selftest);
    run_one("http",      deck_sdi_http_selftest);
    run_one("battery",   deck_sdi_battery_selftest);
    run_one("security",  deck_sdi_security_selftest);
    run_one("display",   deck_sdi_display_selftest);
    run_one("touch",     deck_sdi_touch_selftest);
    run_one("bridge_ui", deck_bridge_ui_selftest);
    run_dvc_selftest();
    ESP_LOGI(TAG, "--- SDI selftests done ---");

    (void)deck_conformance_run();
}
#endif

void app_main(void)
{
    ESP_LOGI(TAG, "=== CyberDeck DL1 bootstrap ===");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase — reinitializing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }
    ESP_LOGI(TAG, "NVS OK");

    usb_serial_jtag_driver_config_t usj_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    esp_err_t usj_ret = usb_serial_jtag_driver_install(&usj_cfg);
    ESP_LOGI(TAG, "USB Serial/JTAG: %s",
             usj_ret == ESP_OK ? "OK" : esp_err_to_name(usj_ret));

    log_device_id();

    deck_sdi_registry_init();
    /* DL1 mandatory drivers. */
    ESP_ERROR_CHECK(deck_sdi_nvs_register_esp32()   == DECK_SDI_OK ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(deck_sdi_fs_register_spiffs()   == DECK_SDI_OK ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(deck_sdi_info_register()        == DECK_SDI_OK ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(deck_sdi_time_register()        == DECK_SDI_OK ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(deck_sdi_shell_register_stub()  == DECK_SDI_OK ? ESP_OK : ESP_FAIL);
    /* DL2 drivers (F25). Registration is non-fatal individually so a
     * missing dependency on a stripped build doesn't brick boot. */
    (void)deck_sdi_wifi_register_esp32();
    (void)deck_sdi_http_register_esp32();
    (void)deck_sdi_battery_register_esp32();
    (void)deck_sdi_security_register_esp32();
    (void)deck_sdi_display_register_esp32();
    (void)deck_sdi_touch_register_esp32();
    /* bridge.ui must register AFTER display + touch because its LVGL
     * init pulls those drivers up. */
    (void)deck_bridge_ui_register_lvgl();
    /* Statusbar + navbar mount on the active screen. Navbar BACK/HOME
     * route through the DL2 shell intent stack. */
    (void)deck_bridge_ui_statusbar_init();
    (void)deck_bridge_ui_navbar_init(deck_shell_navbar_back,
                                       deck_shell_navbar_home);
    deck_bridge_ui_navbar_set_tasks_cb(deck_shell_navbar_tasks);
    deck_sdi_log_registered();

    /* Runtime init — heap hard-limit for DL1 is 64 KB (spec 16 §4.4). */
    ESP_ERROR_CHECK(deck_runtime_init(64 * 1024) == DECK_RT_OK ? ESP_OK : ESP_FAIL);

#if CONFIG_DECK_SDI_SELFTEST
    run_sdi_selftests();
#endif

    /* Test-mode PIN seed: must run AFTER run_sdi_selftests because
     * deck_sdi_security_selftest wipes the PIN slot before its own
     * test pin set/clear cycle. Idempotent — once the slot is populated
     * on a subsequent boot, this leaves it alone. */
    if (!deck_sdi_security_has_pin()) {
        deck_sdi_err_t pin_rc = deck_sdi_security_set_pin(NULL, "1234");
        ESP_LOGI(TAG, "test PIN seed (1234): %s", deck_sdi_strerror(pin_rc));
    }

    log_heap("idle");

    ESP_LOGI(TAG, "--- booting DL2 shell ---");
    deck_err_t shell_rc = deck_shell_dl2_boot();
    if (shell_rc != DECK_RT_OK) {
        ESP_LOGE(TAG, "DL2 shell boot FAILED: %s", deck_err_name(shell_rc));
    }
    /* NOTE: deck_shell_boot() (DL1 sample that re-runs launcher.deck) is
     * intentionally NOT invoked here. It executes the .deck launcher's
     * state machine, which emits a push_snapshot to the active screen —
     * directly over the C launcher's grid — leaving [dvc_type=1] in
     * place of the apps. The DL2 path already exercises the runtime via
     * deck_shell_deck_apps_scan_and_register's load_one. */
    (void)deck_shell_boot;

    ESP_LOGI(TAG, "Bootstrap complete — idle loop (10s heartbeat)");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        log_heap("heartbeat");
    }
}
