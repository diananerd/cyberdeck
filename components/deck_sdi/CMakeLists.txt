idf_component_register(
    SRCS "src/deck_sdi_registry.c"
         "src/deck_sdi_services.c"
         "src/deck_sdi_strerror.c"
         "src/drivers/deck_sdi_nvs_esp32.c"
         "src/drivers/deck_sdi_fs_spiffs.c"
         "src/drivers/deck_sdi_info.c"
         "src/drivers/deck_sdi_time.c"
         "src/drivers/deck_sdi_shell_stub.c"
         "src/drivers/deck_sdi_wifi_esp32.c"
         "src/drivers/deck_sdi_http_esp32.c"
         "src/drivers/deck_sdi_battery_esp32.c"
         "src/drivers/deck_sdi_security_esp32.c"
         "src/drivers/deck_sdi_bridge_ui_skel.c"
         "src/drivers/deck_sdi_display_esp32.c"
    INCLUDE_DIRS "include"
    REQUIRES log nvs_flash spiffs esp_hw_support esp_timer
             esp_wifi esp_netif esp_event esp_http_client esp-tls
             lwip mbedtls
             board
             espressif__esp_lcd_touch
)
