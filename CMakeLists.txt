cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/components/board"
    "${CMAKE_CURRENT_LIST_DIR}/components/deck_sdi"
    "${CMAKE_CURRENT_LIST_DIR}/components/deck_runtime"
    "${CMAKE_CURRENT_LIST_DIR}/components/deck_shell"
    "${CMAKE_CURRENT_LIST_DIR}/components/deck_conformance"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(cyberdeck)

# Bundle apps/ into the "apps" SPIFFS partition.
spiffs_create_partition_image(apps apps FLASH_IN_PROJECT)

# Flash size regression guard. DL1 budget was ≤ 256 KB (spec 16 §4.9);
# DL2 explodes that — esp_wifi alone adds ~500 KB, esp_lcd + GT911 +
# esp_http_client another ~200 KB. Spec 16 §4.9 budgets DL2 at ≤ 1 MB
# total platform image. Current DL2 baseline ~1.1 MB; we enforce 1.5 MB
# as a runaway-bloat ceiling, not a tight-fit target.
set(CYBERDECK_BIN_MAX_BYTES 1572864)  # 1.5 MB
add_custom_target(cyberdeck_bin_size_guard ALL
    COMMAND ${CMAKE_COMMAND}
        -DBIN_FILE=${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.bin
        -DMAX_BYTES=${CYBERDECK_BIN_MAX_BYTES}
        -P ${CMAKE_CURRENT_LIST_DIR}/tools/assert_bin_size.cmake
    DEPENDS gen_project_binary
    VERBATIM
)
