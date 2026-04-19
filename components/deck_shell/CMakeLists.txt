idf_component_register(
    SRCS "src/deck_shell.c"
         "src/deck_shell_dl2.c"
         "src/deck_shell_lockscreen.c"
         "src/deck_shell_intent.c"
         "src/deck_shell_settings.c"
         "src/deck_shell_rotation.c"
         "src/deck_shell_apps.c"
         "src/deck_shell_deck_apps.c"
    INCLUDE_DIRS "include"
    REQUIRES log deck_runtime deck_sdi deck_bridge_ui
             lvgl__lvgl
)
