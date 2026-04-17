idf_component_register(
    SRCS "src/deck_error.c"
         "src/deck_types.c"
    INCLUDE_DIRS "include"
    REQUIRES log deck_sdi
)
