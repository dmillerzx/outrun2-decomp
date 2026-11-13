cmake_minimum_required(VERSION 3.20)
project(OutRunAndroid LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(OR2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(SDL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/extern/sdl2")

add_compile_definitions(
    ANDROID=1
    __ANDROID__=1
    OR2_ANDROID=1
    OR2_X87_FAST=1
    OR2_HAVE_VORBIS=1
    OR2_VULKAN=1
    OR2_PS5_VULKAN=1
)

# Core engine sources
file(GLOB OR2_DRIVING_SOURCES "${OR2_ROOT}/src/driving/*.cpp")
file(GLOB OR2_PLATFORM_SOURCES "${OR2_ROOT}/src/platform/*.cpp")
file(GLOB OR2_SYSTEM_SOURCES "${OR2_ROOT}/src/system/*.cpp")
file(GLOB OR2_RUNTIME_SOURCES "${OR2_ROOT}/src/runtime/*.cpp")
file(GLOB OR2_ENHANCEMENT_SOURCES "${OR2_ROOT}/src/enhancements/*.cpp")

set(OR2_CORE_SOURCES
    ${OR2_DRIVING_SOURCES}
    ${OR2_PLATFORM_SOURCES}
    ${OR2_SYSTEM_SOURCES}
    ${OR2_RUNTIME_SOURCES}
    ${OR2_ENHANCEMENT_SOURCES}
    "${OR2_ROOT}/src/asset_views.cpp"
    "${OR2_ROOT}/src/pc_pmt.cpp"
    "${OR2_ROOT}/src/ports/pecompact_memory.cpp"
    "${OR2_ROOT}/src/input/dual_sense.cpp"
    "${OR2_ROOT}/src/input/runtime_adapter.cpp"
)

# Vulkan and platform glue from ps5/source
set(OR2_PLATFORM_GLUE
    "${OR2_ROOT}/ps5/source/audio.cpp"
    "${OR2_ROOT}/ps5/source/platform_api.cpp"
    "${OR2_ROOT}/ps5/source/renderer.cpp"
    "${OR2_ROOT}/ps5/source/setup_display.cpp"
    "${OR2_ROOT}/ps5/source/vk_api.cpp"
    "${OR2_ROOT}/ps5/source/vk_context.cpp"
    "${OR2_ROOT}/ps5/source/vk_d3d9.cpp"
    "${OR2_ROOT}/ps5/source/vk_display.cpp"
    "${OR2_ROOT}/ps5/source/sdl_backend.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/source/stb_vorbis.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/source/main.cpp"
)

add_library(main SHARED
    ${OR2_CORE_SOURCES}
    ${OR2_PLATFORM_GLUE}
)

target_include_directories(main PRIVATE
    "${OR2_ROOT}/src"
    "${OR2_ROOT}/ps5/source"
    "${CMAKE_CURRENT_SOURCE_DIR}/source"
    "${SDL_ROOT}/include"
)

target_compile_options(main PRIVATE
    -O3
    -ffp-contract=off
    -fno-fast-math
    -Wall
    -Wextra
    -Wno-unused-parameter
    -Wno-missing-field-initializers
    -Wno-unused-variable
)

# Import prebuilt SDL2
add_library(SDL2 SHARED IMPORTED)
set_target_properties(SDL2 PROPERTIES
    IMPORTED_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/app/src/main/jniLibs/arm64-v8a/libSDL2.so"
)

find_library(VULKAN_LIB vulkan REQUIRED)
find_library(LOG_LIB log REQUIRED)
find_library(ANDROID_LIB android REQUIRED)
find_library(Z_LIB z REQUIRED)

target_link_libraries(main PRIVATE
    SDL2
    ${VULKAN_LIB}
    ${LOG_LIB}
    ${ANDROID_LIB}
    ${Z_LIB}
    m
)
