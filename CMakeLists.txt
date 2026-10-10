# Dragon Ball Z: Burst Limit Recompiled
#
# SDK boilerplate lives in generated/rexglue.cmake. The ReXGlue SDK is taken
# from thirdparty/rexglue-sdk (git submodule) unless REXSDK_DIR is set.

cmake_minimum_required(VERSION 3.25)
project(burstlimit LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT REXSDK_DIR AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/rexglue-sdk/CMakeLists.txt")
    set(REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/rexglue-sdk" CACHE PATH
        "Path to rexglue-sdk source tree")
endif()

include(generated/rexglue.cmake)

set(BURSTLIMIT_SOURCES
    src/main.cpp
    src/burstlimit_branding.cpp
    src/burstlimit_buttons.cpp
    src/burstlimit_camera.cpp
    src/burstlimit_cpk.cpp
    src/burstlimit_forms.cpp
    src/burstlimit_install.cpp
    src/burstlimit_kicharge.cpp
    src/burstlimit_launcher.cpp
    src/burstlimit_discord.cpp
    src/burstlimit_menuart.cpp
    src/burstlimit_menusound.cpp
    src/burstlimit_mods.cpp
    src/burstlimit_netinput.cpp
    src/burstlimit_online.cpp
    src/burstlimit_patches.cpp
    src/burstlimit_saves.cpp
    src/burstlimit_stages.cpp
)

if(WIN32)
    # Exe / window / taskbar icon.
    enable_language(RC)
    list(APPEND BURSTLIMIT_SOURCES res/burstlimit.rc)
    add_executable(burstlimit WIN32 ${BURSTLIMIT_SOURCES})
else()
    add_executable(burstlimit ${BURSTLIMIT_SOURCES})
endif()

rexglue_setup_target(burstlimit GPU_PLUGINS xenos)

# Online version string: the lobby only lists rooms between identical strings.
# Release tag + git hash; a build id when the tree isn't a git checkout (point
# BURSTLIMIT_GIT_DIR at one to use its tag and hash).
set(BURSTLIMIT_VERSION_OVERRIDE "" CACHE STRING "Online version string (empty = git tag + hash)")
set(BURSTLIMIT_GIT_DIR "" CACHE PATH "Git checkout that names this build (default: the source tree)")
set(_bl_version "${BURSTLIMIT_VERSION_OVERRIDE}")
if(NOT _bl_version)
    set(_bl_git_dir "${CMAKE_CURRENT_SOURCE_DIR}")
    if(BURSTLIMIT_GIT_DIR)
        set(_bl_git_dir "${BURSTLIMIT_GIT_DIR}")
    endif()
    find_package(Git QUIET)
    set(_bl_hash "")
    set(_bl_tag "")
    if(GIT_FOUND)
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${_bl_git_dir}" rev-parse --short HEAD
            OUTPUT_VARIABLE _bl_hash OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
            RESULT_VARIABLE _bl_hash_result)
        if(NOT _bl_hash_result EQUAL 0)
            set(_bl_hash "")
        endif()
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${_bl_git_dir}" describe --tags
            --match "v[0-9]*" --abbrev=0
            OUTPUT_VARIABLE _bl_tag OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
            RESULT_VARIABLE _bl_tag_result)
        if(NOT _bl_tag_result EQUAL 0)
            set(_bl_tag "")
        endif()
    endif()
    if(_bl_hash AND _bl_tag)
        set(_bl_version "${_bl_tag}+${_bl_hash}")
    elseif(_bl_hash)
        set(_bl_version "g${_bl_hash}")
    else()
        string(TIMESTAMP _bl_stamp "%Y%m%d%H%M" UTC)
        set(_bl_version "build${_bl_stamp}")
    endif()
endif()
message(STATUS "Online version: burstlimit-${_bl_version}")
target_compile_definitions(burstlimit PRIVATE "BURSTLIMIT_VERSION=\"${_bl_version}\"")

# ReXGlue UI sources use ImGui directly.
target_link_libraries(burstlimit PRIVATE imgui::imgui)
# stb_image's zlib decoder (save import reads compressed zips).
target_include_directories(burstlimit PRIVATE "${REXSDK_DIR}/thirdparty/stb")
# nlohmann/json (the start screen reads the lobby's room list).
target_include_directories(burstlimit PRIVATE "${REXSDK_DIR}/thirdparty/inja/third_party/include")
if(WIN32)
    target_link_libraries(burstlimit PRIVATE shell32)
endif()
target_include_directories(burstlimit PRIVATE "${CMAKE_BINARY_DIR}/rexglue-sdk/include")
