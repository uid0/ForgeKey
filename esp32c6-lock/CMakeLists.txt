# ESP32-C6 Lock Device - CMakeLists.txt

cmake_minimum_required(VERSION 3.16)

set(FORGEKEY_LOCK_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${FORGEKEY_LOCK_GENERATED_DIR}")
execute_process(
    COMMAND python3 "${CMAKE_CURRENT_LIST_DIR}/../scripts/build/version.py"
            --output "${FORGEKEY_LOCK_GENERATED_DIR}/forgekey_build_metadata.h"
            --target esp32c6-lock
            --project-dir "${CMAKE_CURRENT_LIST_DIR}"
            --repo-root "${CMAKE_CURRENT_LIST_DIR}/.."
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/.."
    RESULT_VARIABLE FORGEKEY_VERSION_RESULT
)
if(NOT FORGEKEY_VERSION_RESULT EQUAL 0)
    message(FATAL_ERROR "ForgeKey build metadata generation failed")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lock_device)
