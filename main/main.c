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
    ESP_ERROR_CHECK(deck_sdi_nvs_register_esp32() == DECK_SDI_OK ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(deck_sdi_fs_register_spiffs() == DECK_SDI_OK ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(deck_sdi_info_register()       == DECK_SDI_OK ? ESP_OK : ESP_FAIL);
    deck_sdi_log_registered();

    deck_sdi_err_t nvs_st = deck_sdi_nvs_selftest();
    if (nvs_st != DECK_SDI_OK) {
        ESP_LOGE(TAG, "NVS selftest FAILED: %s", deck_sdi_strerror(nvs_st));
    }
    deck_sdi_err_t fs_st = deck_sdi_fs_selftest();
    if (fs_st != DECK_SDI_OK) {
        ESP_LOGE(TAG, "FS selftest FAILED: %s", deck_sdi_strerror(fs_st));
    }
    deck_sdi_err_t info_st = deck_sdi_info_selftest();
    if (info_st != DECK_SDI_OK) {
        ESP_LOGE(TAG, "INFO selftest FAILED: %s", deck_sdi_strerror(info_st));
    }

    log_heap("idle");

    ESP_LOGI(TAG, "Bootstrap complete — idle loop (10s heartbeat)");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        log_heap("heartbeat");
    }
}
