cmake_minimum_required(VERSION 3.22)

list(
    APPEND EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../components/app_command"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/app_command_coordinator"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/app_storage"
)

# Build only main and its declared transitive dependencies.
set(
    APP_STORAGE_NVS_PARTITION_OVERRIDE
    nvs_test
    CACHE STRING
    "NVS partition used by app_storage in the H1 test application"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(wifi_manager_h1_component_test)
