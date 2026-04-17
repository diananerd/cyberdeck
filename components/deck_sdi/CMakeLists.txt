idf_component_register(
    SRCS "src/deck_sdi_registry.c"
         "src/deck_sdi_strerror.c"
    INCLUDE_DIRS "include"
    REQUIRES log
)
