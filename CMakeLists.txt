cmake_minimum_required(VERSION 3.21)

if (APPLE AND (NOT DEFINED GEODE_TARGET_PLATFORM OR GEODE_TARGET_PLATFORM STREQUAL "MacOS"))
    if (NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET OR
        CMAKE_OSX_DEPLOYMENT_TARGET STREQUAL "" OR
        CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS 11.0)
        set(CMAKE_OSX_DEPLOYMENT_TARGET 11.0 CACHE STRING "Minimum supported macOS version" FORCE)
    endif()
    if (NOT DEFINED CMAKE_OSX_ARCHITECTURES OR CMAKE_OSX_ARCHITECTURES STREQUAL "")
        set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures" FORCE)
    endif()
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

project(AutoDeafen VERSION 2.0.5)

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

add_library(${PROJECT_NAME} SHARED
    src/globals.cpp
    src/helpers.cpp
    src/ipc.cpp
    src/linux_setup.cpp
    src/main.cpp
    src/oauth.cpp
)

setup_geode_mod(${PROJECT_NAME})

# Keep direct CMake builds consistent with Geode CLI builds. This also makes
# local compile-only validation produce the final package binary name when CLI
# packaging is intentionally disabled.
set_target_properties(${PROJECT_NAME} PROPERTIES
    PREFIX ""
    OUTPUT_NAME "justa_doge.autodeafen_unix"
)

if (GEODE_TARGET_PLATFORM STREQUAL "Win64")
    target_compile_definitions(${PROJECT_NAME} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(${PROJECT_NAME} ws2_32 shell32)
endif()
