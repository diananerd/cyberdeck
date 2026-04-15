idf_component_register(
    SRCS "app_registry.c"
         "app_state.c"
         "app_manager.c"
         "os_nav.c"
    INCLUDE_DIRS "include"
    REQUIRES ui_engine sys_services os_core
)
