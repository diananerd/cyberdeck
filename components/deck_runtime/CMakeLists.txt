idf_component_register(
    SRCS "src/deck_error.c"
         "src/deck_types.c"
         "src/deck_alloc.c"
         "src/deck_runtime.c"
    INCLUDE_DIRS "include"
    REQUIRES log deck_sdi esp_hw_support
)
