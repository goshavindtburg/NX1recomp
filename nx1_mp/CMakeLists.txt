# nx1_mp - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(nx1_mp LANGUAGES CXX)

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
set(NX1MP_SOURCES
    src/main.cpp
    src/platform_compat.cpp
    src/d3d9/d3d9_renderer.cpp
    src/d3d9/d3d9_constants.cpp
    src/d3d9/d3d9_hooks.cpp
    src/d3d9/pool_watch.cpp
    src/d3d9/d3d9_resources.cpp
    src/d3d9/d3d9_shaders.cpp
    src/d3d9/d3d9_cmdbuf.cpp
    src/d3d9/d3d9_overlay.cpp
    src/d3d9/guest_d3d.cpp
)

# Dear ImGui backends for the native renderer's own overlay. The `imgui` target builds only
# the core library -- the SDK never needed a backend because it renders the UI through its own
# D3D12 ImmediateDrawer, and that path draws into a swapchain our D3D9 present covers up.
if(WIN32)
    list(APPEND NX1MP_SOURCES
        "${CMAKE_CURRENT_LIST_DIR}/../rexglue-sdk/thirdparty/imgui/backends/imgui_impl_dx9.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../rexglue-sdk/thirdparty/imgui/backends/imgui_impl_win32.cpp"
    )
endif()

# Shader Model 3.0 cache for the native D3D9 renderer, emitted by
#   XenosRecomp <dumps> <dxil.cpp> <shader_common.h> --sm3 <this file>
# Optional: without it the renderer reports a cache miss for every shader.
set(NX1MP_SM3_SHADER_CACHE "${CMAKE_CURRENT_LIST_DIR}/../tools/nx1_sm3_shader_cache.cpp")
if(WIN32 AND EXISTS "${NX1MP_SM3_SHADER_CACHE}")
    list(APPEND NX1MP_SOURCES "${NX1MP_SM3_SHADER_CACHE}")
    set(NX1MP_HAVE_SM3_SHADER_CACHE TRUE)
endif()

if(WIN32)
    enable_language(RC)
    file(TO_CMAKE_PATH "${CMAKE_CURRENT_LIST_DIR}/../res/nx1.ico" NX1_ICON_PATH)
    configure_file(src/nx1_mp.rc.in "${CMAKE_CURRENT_BINARY_DIR}/nx1_mp.rc" @ONLY)
    list(APPEND NX1MP_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/nx1_mp.rc")
endif()

if(WIN32)
    add_executable(nx1_mp WIN32 ${NX1MP_SOURCES})
else()
    add_executable(nx1_mp ${NX1MP_SOURCES})
endif()

if(WIN32)
    # Native D3D9 renderer (nx1_d3d9 cvar). d3dcompiler is used to build
    # vs_3_0/ps_3_0 from the HLSL that XenosRecomp emits for Xenos microcode.
    # xxHash: the SM3 cache is keyed by the same Xenos microcode hash the Xenia
    # backend uses, so the two caches stay in lockstep.
    target_link_libraries(nx1_mp PRIVATE d3d9 d3dcompiler xxHash::xxhash)
    target_include_directories(nx1_mp PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src/d3d9")
    if(NX1MP_HAVE_SM3_SHADER_CACHE)
        target_compile_definitions(nx1_mp PRIVATE NX1_HAVE_SM3_SHADER_CACHE)
        target_link_libraries(nx1_mp PRIVATE libzstd_static)
    endif()
    target_link_options(nx1_mp PRIVATE "LINKER:/MAP:${CMAKE_CURRENT_BINARY_DIR}/nx1_mp.map")
    if(NX1_EMBED_DISCORD_SDK)
        target_link_options(nx1_mp PRIVATE "LINKER:/DELAYLOAD:discord_partner_sdk.dll")
        target_link_libraries(nx1_mp PRIVATE delayimp)
    endif()
endif()

rexglue_setup_target(nx1_mp)
