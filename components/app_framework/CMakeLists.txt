# cJSON is shipped with ESP-IDF but its top-level directory has no CMakeLists.
# We include the source directly to avoid adding an external dependency.
set(CJSON_DIR $ENV{IDF_PATH}/components/json/cJSON)

idf_component_register(
    SRCS "app_registry.c"
         "app_state.c"
         "app_manager.c"
         "os_nav.c"
         "os_manifest.c"
         "os_script.c"
         "os_app_discover.c"
         "${CJSON_DIR}/cJSON.c"
    INCLUDE_DIRS "include"
                 "${CJSON_DIR}"
    REQUIRES ui_engine sys_services os_core board
)
