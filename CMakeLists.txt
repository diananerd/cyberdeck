cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/components/board"
    "${CMAKE_CURRENT_LIST_DIR}/components/ui_engine"
    "${CMAKE_CURRENT_LIST_DIR}/components/app_framework"
    "${CMAKE_CURRENT_LIST_DIR}/components/apps/launcher"
    "${CMAKE_CURRENT_LIST_DIR}/components/apps/settings"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
add_compile_options("-Wno-attributes")
project(cyberdeck)
