idf_component_register(
    SRCS "app_taskman.c"
         "taskman_detail.c"
         "taskman_sysview.c"
    INCLUDE_DIRS "include"
    REQUIRES app_framework ui_engine os_core sys_services
)
