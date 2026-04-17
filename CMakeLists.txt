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

# Flash size regression guard. DL1 budget: total platform image ≤ 256 KB
# per spec 16 §4.9. Current baseline ~310 KB (includes USB-JTAG, esp_stdio
# logging, NVS — non-Deck code). We enforce a pragmatic upper bound that
# catches runaway bloat but reflects the actual stack.
set(CYBERDECK_BIN_MAX_BYTES 500000)
add_custom_target(cyberdeck_bin_size_guard ALL
    COMMAND ${CMAKE_COMMAND}
        -DBIN_FILE=${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.bin
        -DMAX_BYTES=${CYBERDECK_BIN_MAX_BYTES}
        -P ${CMAKE_CURRENT_LIST_DIR}/tools/assert_bin_size.cmake
    DEPENDS gen_project_binary
    VERBATIM
)
