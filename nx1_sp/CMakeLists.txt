# nx1_sp - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(nx1_sp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING
        "Use the static MSVC runtime for portable NX1 executables" FORCE)
endif()

set(REXGLUE_BUILD_MONOLITHIC_RUNTIME ON CACHE BOOL
    "Build ReXGlue runtime into the NX1 executable" FORCE)

include(generated/rexglue.cmake)

if(WIN32)
    set(NX1_DISCORD_SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../Discord SDK" CACHE PATH
        "Path to the Discord Social SDK root")
    set(NX1_DISCORD_SDK_DLL_RELEASE "${NX1_DISCORD_SDK_DIR}/bin/release/discord_partner_sdk.dll")
    set(NX1_DISCORD_SDK_DLL_DEBUG "${NX1_DISCORD_SDK_DIR}/bin/debug/discord_partner_sdk.dll")
    if(EXISTS "${NX1_DISCORD_SDK_DLL_RELEASE}")
        file(TO_CMAKE_PATH "${NX1_DISCORD_SDK_DLL_RELEASE}" NX1_DISCORD_SDK_DLL_EMBED)
        set(NX1_EMBED_DISCORD_SDK 1)
    else()
        set(NX1_DISCORD_SDK_DLL_EMBED "")
        set(NX1_EMBED_DISCORD_SDK 0)
    endif()
endif()

# Sources
set(NX1SP_SOURCES
    src/main.cpp
    src/nx1_devgui_overlay.cpp
    src/platform_compat.cpp
)

if(WIN32)
    enable_language(RC)
    file(TO_CMAKE_PATH "${CMAKE_CURRENT_LIST_DIR}/../res/nx1.ico" NX1_ICON_PATH)
    configure_file(src/nx1_sp.rc.in "${CMAKE_CURRENT_BINARY_DIR}/nx1_sp.rc" @ONLY)
    list(APPEND NX1SP_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/nx1_sp.rc")
endif()

if(WIN32)
    add_executable(nx1_sp WIN32 ${NX1SP_SOURCES})
else()
    add_executable(nx1_sp ${NX1SP_SOURCES})
endif()

if(WIN32)
    target_link_options(nx1_sp PRIVATE "LINKER:/MAP:${CMAKE_CURRENT_BINARY_DIR}/nx1_sp.map")
    if(NX1_EMBED_DISCORD_SDK)
        target_link_options(nx1_sp PRIVATE "LINKER:/DELAYLOAD:discord_partner_sdk.dll")
        target_link_libraries(nx1_sp PRIVATE delayimp)
    endif()
endif()

rexglue_setup_target(nx1_sp)
