idf_component_register(
    SRCS "app_settings.c"
         "settings_wifi.c"
         "settings_display.c"
         "settings_time.c"
         "settings_storage.c"
         "settings_security.c"
         "settings_about.c"
         "settings_bluetooth.c"
         "settings_audio.c"
    INCLUDE_DIRS "include"
    REQUIRES app_framework ui_engine sys_services board os_core
    PRIV_REQUIRES esp_app_format
)
