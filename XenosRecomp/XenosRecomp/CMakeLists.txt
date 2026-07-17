project(XenosRecomp)

if (WIN32)
    option(XENOS_RECOMP_DXIL "Generate DXIL shader cache" ON)
endif()

set(SMOLV_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/smol-v/source")

add_executable(XenosRecomp
    constant_table.h
    dxc_compiler.cpp
    dxc_compiler.h
    fxc_compiler.cpp
    fxc_compiler.h
    main.cpp
    pch.h
    shader.h
    shader_code.h
    shader_recompiler.cpp
    shader_recompiler.h
    sm3_transform.cpp
    sm3_transform.h
    "${SMOLV_SOURCE_DIR}/smolv.cpp")

target_link_libraries(XenosRecomp PRIVATE
    Microsoft::DirectXShaderCompiler
    xxHash::xxhash
    libzstd_static
    fmt::fmt)

target_include_directories(XenosRecomp PRIVATE ${SMOLV_SOURCE_DIR})

target_precompile_headers(XenosRecomp PRIVATE pch.h)

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    target_compile_options(XenosRecomp PRIVATE -Wno-switch -Wno-unused-variable -Wno-null-arithmetic -fms-extensions)

    include(CheckCXXSymbolExists)
    check_cxx_symbol_exists(_LIBCPP_VERSION version LIBCPP)
    if(LIBCPP)
        # Allows using std::execution
        target_compile_options(XenosRecomp PRIVATE -fexperimental-library)
    endif()
endif()

if (WIN32)
    # SM3 (vs_3_0/ps_3_0) lowering for the native D3D9 renderer.
    target_link_libraries(XenosRecomp PRIVATE d3dcompiler)
    target_compile_definitions(XenosRecomp PRIVATE _CRT_SECURE_NO_WARNINGS)
    # 16 MB default stack for EVERY thread (the PE-header reserve is inherited by parallel-STL
    # pool threads too). MSVC's std::regex recurses proportionally to the shader body it walks,
    # and the corpus's biggest shaders blow the default 1 MB -- as random-looking 0xC0000409 /
    # 0xC0000005 deaths that move with the thread schedule. They predate the parallel loop:
    # the rare serial-build crashes were the same overflow on the main thread.
    target_link_options(XenosRecomp PRIVATE "LINKER:/STACK:16777216")
    add_custom_command(TARGET XenosRecomp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_RUNTIME_DLLS:XenosRecomp> $<TARGET_FILE_DIR:XenosRecomp>
        COMMAND_EXPAND_LISTS
    )
endif()

if (XENOS_RECOMP_DXIL)
    target_compile_definitions(XenosRecomp PRIVATE XENOS_RECOMP_DXIL)
    target_link_libraries(XenosRecomp PRIVATE Microsoft::DXIL)
endif()
