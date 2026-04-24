idf_component_register(
    SRCS "src/deck_bridge_ui.c"
         "src/deck_bridge_ui_lvgl.c"
         "src/deck_bridge_ui_decode.c"
         "src/deck_bridge_ui_overlays.c"
         "src/deck_bridge_ui_statusbar.c"
         "src/deck_bridge_ui_navbar.c"
         "src/deck_bridge_ui_activity.c"
         "src/deck_bridge_ui_patch.c"
    INCLUDE_DIRS "include"
    REQUIRES log esp_timer freertos esp_lcd
             deck_sdi deck_runtime board
             lvgl__lvgl
)
