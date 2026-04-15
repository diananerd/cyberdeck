idf_component_register(
    SRCS "ui_engine.c"
         "ui_theme.c"
         "ui_statusbar.c"
         "ui_activity.c"
         "ui_intent.c"
         "ui_effect.c"
         "ui_common.c"
         "ui_keyboard.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_lcd lvgl__lvgl
    PRIV_REQUIRES esp_timer
)
