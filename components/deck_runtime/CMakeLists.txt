idf_component_register(
    SRCS "src/deck_error.c"
         "src/deck_types.c"
         "src/deck_alloc.c"
         "src/deck_intern.c"
         "src/deck_lexer.c"
         "src/deck_lexer_test.c"
         "src/deck_arena.c"
         "src/deck_ast.c"
         "src/deck_parser.c"
         "src/deck_parser_test.c"
         "src/deck_loader.c"
         "src/deck_loader_test.c"
         "src/deck_interp.c"
         "src/deck_interp_test.c"
         "src/deck_runtime.c"
    INCLUDE_DIRS "include"
    REQUIRES log deck_sdi esp_hw_support
)
