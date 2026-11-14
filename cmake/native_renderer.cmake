# Native renderer for WWE SmackDown vs. Raw 2007 (SvR07 native port, started 2026-11-10;
# brief: NATIVE-PORT-BRIEF.md, lane L4).
#
# Ported from the Rumble Roses XX native tree (rumble-roses-xx-recomp-native/cmake/native_renderer.cmake),
# which came from UFC-native, which took it from the SvR 2010 recomp by nefariousjosiah (GPL-3.0),
# adapted from re:Blue (BSD-3-Clause); see native/LICENSE*, native/THIRD-PARTY.md, native/UPSTREAM.txt.
#
# Run-time switch: the exe always has the svr07_renderer cvar when SVR07_NATIVE_RENDERER is on
# (default "emulated", src/svr07_native_flags.cpp, latched in Svr07App::LatchRenderer). With
# svr07_renderer=native and no shader cache beside the exe the game stays emulated (logged).
# This file only compiles the native pieces in. Two options, both OFF by default:
#   SVR07_NATIVE_RENDERER  compile in the native scaffolding (SVR07_NATIVE_RENDERER=1, native/src include dir,
#                          src/svr07_native.cpp layout compile check, src/svr07_native_flags.cpp cvars).
#                          Without hooks this is the census build: --svr07_d3d_census counts the game's
#                          D3D calls (native/src/dev/svr07_d3d_census.cpp) and svr07_renderer=native logs
#                          "not implemented" and stays on the emulated renderer.
#   SVR07_NATIVE_HOOKS     the D3D hooks + plume D3D12 device. Every hook is gated at run time
#                          (native/src/native/ufc_hook_gate.h): on the emulated renderer it runs the game's body.
# The options stay in the CMake cache (UFC trap 2): ..\build-native.bat passes both on every configure.
#
# Self-contained: everything is under native/ in this tree (no ../research paths):
#   native/src                         renderer sources (re:Blue + UFC/RR native layer, renamed for SvR07)
#   native/third_party/plume           plume (D3D12 only)
#   native/third_party/XenosRecomp     patched XenosRecomp (headers, xxHash, smol-v, dxc-bin)
#   native/third_party/reblue_thirdparty  zstd decoder, miniz
#   native/third_party/fidelityfx      FSR 1 headers
# The shader cache is built offline by svr07_shader_cache.exe (native/tools/build-shader-tools.bat,
# lane L2) and read at run time (svr07_shader_cache cvar, default shader_cache.bin beside the exe).

set(SVR07_NATIVE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/native" CACHE PATH
    "Native renderer sources (this tree's native/ folder)")
set(SVR07_THIRD_PARTY "${SVR07_NATIVE_ROOT}/third_party" CACHE PATH
    "plume, reblue_thirdparty, fidelityfx, XenosRecomp (native/tools/svr07_fetch_third_party.py)")
set(SVR07_XENOS_DIR "${SVR07_THIRD_PARTY}/XenosRecomp" CACHE PATH
    "Patched XenosRecomp source tree (headers, xxHash, smol-v, dxc-bin)")
option(SVR07_NATIVE_HOOKS "Compile the native D3D hooks + plume D3D12 device" OFF)

if(SVR07_NATIVE_HOOKS)
    enable_language(C)  # zstd and xxHash are C
    if(NOT EXISTS "${SVR07_THIRD_PARTY}/plume/CMakeLists.txt" OR
       NOT EXISTS "${SVR07_XENOS_DIR}/thirdparty/dxc-bin/bin/x64/dxc.exe")
        message(FATAL_ERROR "SVR07_NATIVE_HOOKS: ${SVR07_THIRD_PARTY}/plume or ${SVR07_XENOS_DIR} dxc missing: "
            "run python -I native/tools/svr07_fetch_third_party.py")
    endif()
    set(SVR07_NATIVE_GEN "${CMAKE_BINARY_DIR}/native_gen")

    # D3D12 only (as UFC and RR).
    set(PLUME_D3D12_ENABLED ON CACHE BOOL "" FORCE)
    add_subdirectory("${SVR07_THIRD_PARTY}/plume" "${CMAKE_BINARY_DIR}/plume" EXCLUDE_FROM_ALL)

    add_library(svr07_native_thirdparty STATIC
        "${SVR07_THIRD_PARTY}/reblue_thirdparty/zstd/zstddeclib.c"
        "${SVR07_XENOS_DIR}/thirdparty/xxHash/xxhash.c"
        "${SVR07_XENOS_DIR}/thirdparty/smol-v/source/smolv.cpp"
        "${SVR07_THIRD_PARTY}/reblue_thirdparty/miniz/miniz.cpp")
    target_include_directories(svr07_native_thirdparty PUBLIC
        "${SVR07_THIRD_PARTY}/reblue_thirdparty/zstd"
        "${SVR07_XENOS_DIR}/thirdparty/xxHash"
        "${SVR07_XENOS_DIR}/thirdparty/smol-v/source"
        "${SVR07_THIRD_PARTY}/reblue_thirdparty/miniz")

    # Host shaders (copy, ImGui overlay, MSAA resolve, gamma, FSR 1) compiled with DXC to DXIL headers.
    set(SVR07_DXC_DIR "${SVR07_XENOS_DIR}/thirdparty/dxc-bin/bin/x64")
    set(SVR07_DXC "${SVR07_DXC_DIR}/dxc.exe")
    set(SVR07_HLSL_DIR "${SVR07_NATIVE_ROOT}/src/reblue/gpu/shaders/hlsl")
    set(SVR07_HOST_SHADER_HEADERS "")
    function(svr07_host_shader STEM PROFILE)
        file(GLOB hlsl_includes "${SVR07_HLSL_DIR}/*.hlsli" "${SVR07_THIRD_PARTY}/fidelityfx/gpu/*.h"
            "${SVR07_THIRD_PARTY}/fidelityfx/gpu/fsr1/*.h")
        set(out "${SVR07_NATIVE_GEN}/src/gpu/shaders/hlsl/${STEM}.hlsl.dxil.h")
        add_custom_command(
            OUTPUT "${out}"
            COMMAND "${SVR07_DXC}" -T ${PROFILE} -HV 2021 -all-resources-bound -Wno-ignored-attributes
                    -I "${SVR07_THIRD_PARTY}/.." -Fh "${out}" "${SVR07_HLSL_DIR}/${STEM}.hlsl"
                    -Vn g_${STEM}_dxil
            DEPENDS "${SVR07_HLSL_DIR}/${STEM}.hlsl" ${hlsl_includes}
            COMMENT "Compiling ${STEM}.hlsl (${PROFILE}, dxil)"
            VERBATIM)
        set(SVR07_HOST_SHADER_HEADERS ${SVR07_HOST_SHADER_HEADERS} "${out}" PARENT_SCOPE)
    endfunction()
    foreach(shader IN ITEMS copy_vs imgui_vs)
        svr07_host_shader(${shader} vs_6_0)
    endforeach()
    foreach(shader IN ITEMS copy_color_ps copy_depth_ps gamma_correction_ps fsr_easu_ps fsr_rcas_ps pfx_occlusion_count_ps
            imgui_ps resolve_msaa_color_2x resolve_msaa_color_4x resolve_msaa_color_8x
            resolve_msaa_depth_2x resolve_msaa_depth_4x resolve_msaa_depth_8x)
        svr07_host_shader(${shader} ps_6_0)
    endforeach()
    add_custom_target(svr07_host_shaders DEPENDS ${SVR07_HOST_SHADER_HEADERS})

    # DXC's import library for the run-time spec-constant linker (dxc_link.cpp).
    add_library(svr07_dxcompiler SHARED IMPORTED)
    set_target_properties(svr07_dxcompiler PROPERTIES
        IMPORTED_LOCATION "${SVR07_DXC_DIR}/dxcompiler.dll"
        IMPORTED_IMPLIB "${SVR07_XENOS_DIR}/thirdparty/dxc-bin/lib/x64/dxcompiler.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${SVR07_XENOS_DIR}/thirdparty/dxc-bin/inc")

    # svr07_prelink (UFC's ufc3_prelink): DXC-links every spec-constant variant of a shader_cache.bin
    # into shader_cache_linked.bin, so the game never links one while drawing.
    add_executable(svr07_prelink
        "${SVR07_NATIVE_ROOT}/tools/native/prelink_shader_cache.cpp"
        "${SVR07_NATIVE_ROOT}/src/reblue/gpu/shaders/dxc_link.cpp"
        "${SVR07_NATIVE_ROOT}/src/reblue/gpu/shaders/shader_cache_file.cpp")
    target_include_directories(svr07_prelink PRIVATE "${SVR07_NATIVE_ROOT}/src/reblue")
    target_link_libraries(svr07_prelink PRIVATE svr07_native_thirdparty svr07_dxcompiler)

    file(GLOB_RECURSE SVR07_NATIVE_SOURCES CONFIGURE_DEPENDS
        "${SVR07_NATIVE_ROOT}/src/reblue/*.cpp" "${SVR07_NATIVE_ROOT}/src/reblue/*.h"
        "${SVR07_NATIVE_ROOT}/src/native/*.cpp" "${SVR07_NATIVE_ROOT}/src/native/*.h")
    list(APPEND SVR07_NATIVE_SOURCES "${SVR07_NATIVE_ROOT}/src/gpu_preference.cpp")
endif()

function(svr07_add_native_renderer target)
    if(NOT EXISTS "${SVR07_NATIVE_ROOT}/src/svr07_d3d.h" OR
       NOT EXISTS "${SVR07_NATIVE_ROOT}/src/svr07_d3d_names.h")
        message(FATAL_ERROR "SVR07_NATIVE_RENDERER: ${SVR07_NATIVE_ROOT}/src/svr07_d3d.h or svr07_d3d_names.h "
            "missing (set SVR07_NATIVE_ROOT)")
    endif()
    target_compile_definitions(${target} PRIVATE SVR07_NATIVE_RENDERER=1)
    target_include_directories(${target} PRIVATE "${SVR07_NATIVE_ROOT}/src")
    # Compile check of the SvR07 D3D layout headers with the game's flags, and the svr07_renderer
    # cvars (svr07_renderer, svr07_test_config, svr07_native_test_device_fail, svr07_selftest_keys).
    target_sources(${target} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/svr07_native.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/svr07_native_flags.cpp")
    if(NOT SVR07_NATIVE_HOOKS)
        # D3D call census (--svr07_d3d_census, observe only). It overrides the same D3D functions as
        # the native hooks, so it is built only without them (as UFC's NR-6 and RR-NR-5 census).
        target_sources(${target} PRIVATE "${SVR07_NATIVE_ROOT}/src/dev/svr07_d3d_census.cpp")
        message(STATUS "SVR07 native renderer: scaffolding + D3D census on (hooks off), sources ${SVR07_NATIVE_ROOT}")
        return()
    endif()
    target_include_directories(${target} PRIVATE
        "${SVR07_NATIVE_ROOT}/src/reblue"
        "${SVR07_NATIVE_ROOT}/src/native"
        "${SVR07_NATIVE_GEN}"
        "${SVR07_THIRD_PARTY}/plume"
        "${SVR07_XENOS_DIR}/XenosRecomp")
    target_link_libraries(${target} PRIVATE plume svr07_native_thirdparty)
    add_dependencies(${target} svr07_host_shaders svr07_prelink)
    target_sources(${target} PRIVATE ${SVR07_NATIVE_SOURCES} ${SVR07_HOST_SHADER_HEADERS})
    # SVR_NATIVE_RENDERER selects SvR's observe-the-game's-D3D design in the re:Blue sources;
    # SVR07_NATIVE_RENDERER picks SvR07's device / resource layouts inside it (gpu/d3d.h, resources.h).
    # SVR07_NATIVE_HOOKS also switches on the native parts of src/svr07_app.h.
    target_compile_definitions(${target} PRIVATE REBLUE_D3D12=1 SVR_NATIVE_RENDERER=1 SVR07_NATIVE_HOOKS=1)
    target_link_libraries(${target} PRIVATE d3d12 dxgi dwmapi svr07_dxcompiler rex::o1heap)
    # Native-only startup errors and the native F4 settings menu (src/svr07_renderer_fail.cpp, src/svr07_settings_menu.cpp).
    target_sources(${target} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/svr07_renderer_fail.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/svr07_settings_menu.cpp")
    # SvR07's addresses for the D3D functions re:Blue's hooks name (base default.xex, no TU1),
    # then the run-time gate. The two DXC files run outside the game (also in svr07_prelink).
    set(SVR07_HOOKED_SOURCES ${SVR07_NATIVE_SOURCES})
    list(REMOVE_ITEM SVR07_HOOKED_SOURCES
        "${SVR07_NATIVE_ROOT}/src/reblue/gpu/shaders/dxc_link.cpp"
        "${SVR07_NATIVE_ROOT}/src/reblue/gpu/shaders/shader_cache_file.cpp")
    set_source_files_properties(${SVR07_HOOKED_SOURCES} PROPERTIES
        COMPILE_OPTIONS "-include;${SVR07_NATIVE_ROOT}/src/svr07_d3d_names.h;-include;${SVR07_NATIVE_ROOT}/src/native/ufc_hook_gate.h")
    target_link_options(${target} PRIVATE "-Wl,/MAP:${CMAKE_BINARY_DIR}/svr07.map")
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SVR07_DXC_DIR}/dxcompiler.dll" "${SVR07_DXC_DIR}/dxil.dll" "$<TARGET_FILE_DIR:${target}>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:svr07_prelink>" "$<TARGET_FILE_DIR:${target}>"
        VERBATIM)
    # pipelines_seed.bin is per game (RR's was deleted): copied beside the exe once a SvR07 seed exists
    # (built later from native test logs with nr_pso_seed.py).
    if(EXISTS "${SVR07_NATIVE_ROOT}/data/pipelines_seed.bin")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${SVR07_NATIVE_ROOT}/data/pipelines_seed.bin" "$<TARGET_FILE_DIR:${target}>"
            VERBATIM)
    endif()
    message(STATUS "SVR07 native renderer: hooks + plume D3D12 device on, sources ${SVR07_NATIVE_ROOT}")
endfunction()
