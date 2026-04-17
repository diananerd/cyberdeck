idf_component_register(
    SRCS "src/deck_sdi_registry.c"
         "src/deck_sdi_strerror.c"
         "src/drivers/deck_sdi_nvs_esp32.c"
         "src/drivers/deck_sdi_fs_spiffs.c"
         "src/drivers/deck_sdi_info.c"
         "src/drivers/deck_sdi_time.c"
         "src/drivers/deck_sdi_shell_stub.c"
    INCLUDE_DIRS "include"
    REQUIRES log nvs_flash spiffs esp_hw_support esp_timer
)
