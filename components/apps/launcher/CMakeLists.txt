idf_component_register(
    SRCS "app_launcher.c"
         "launcher_lockscreen.c"
    INCLUDE_DIRS "include"
    REQUIRES app_framework ui_engine sys_services
)
