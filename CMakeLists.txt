cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/components/board"
    "${CMAKE_CURRENT_LIST_DIR}/components/deck_sdi"
    "${CMAKE_CURRENT_LIST_DIR}/components/deck_runtime"
    "${CMAKE_CURRENT_LIST_DIR}/components/deck_shell"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(cyberdeck)

# Bundle apps/ into the "apps" SPIFFS partition.
spiffs_create_partition_image(apps apps FLASH_IN_PROJECT)
