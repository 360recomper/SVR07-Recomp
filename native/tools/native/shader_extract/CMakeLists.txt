# svr07_shader_cache: builds the SvR07 native renderer's shader cache on the player's machine (from the
# RRXX tool, a port of UFC-native's cache tool). No Python, no rexglue SDK: Windows crypto (bcrypt) for the
# xex and the file names, xxHash for the cache hash. SvR07's _startup.pac is PACH + BPE (own code), no zlib.
#
# On its own (MSVC env + LLVM + Ninja on PATH):
#   cmake -S native/tools/native/shader_extract -B <build> -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
#         -DCMAKE_CXX_COMPILER=clang++ [-DSVR07_SC_THIRD_PARTY=<folder with XenosRecomp/>]
#   cmake --build <build>
# SVR07_SC_THIRD_PARTY defaults to native/third_party (patched XenosRecomp: UFC's stack + xenosrecomp-0e.patch).

cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(svr07_shader_cache LANGUAGES CXX)
endif()

get_filename_component(SVR07_SC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(SVR07_SC_THIRD_PARTY "${SVR07_SC_ROOT}/third_party" CACHE PATH "folder holding the patched XenosRecomp")
set(SVR07_SC_XENOS "${SVR07_SC_THIRD_PARTY}/XenosRecomp")
set(SVR07_SC_COMMON "${SVR07_SC_XENOS}/XenosRecomp/shader_common.h")
if(NOT EXISTS "${SVR07_SC_COMMON}")
    message(FATAL_ERROR "svr07_shader_cache: ${SVR07_SC_XENOS} (patched XenosRecomp) missing")
endif()

# XenosRecomp reads shader_common.h from a file at run time. Embed it (as bytes, so the tool
# writes back exactly this file) instead of shipping it loose; a change re-runs the configure.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SVR07_SC_COMMON}")
file(READ "${SVR07_SC_COMMON}" SVR07_SC_COMMON_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," SVR07_SC_COMMON_BYTES "${SVR07_SC_COMMON_HEX}")
string(REGEX REPLACE "(0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,)"
       "\\1\n" SVR07_SC_COMMON_BYTES "${SVR07_SC_COMMON_BYTES}")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/shader_common_embed.inc"
     CONTENT "${SVR07_SC_COMMON_BYTES}\n" @ONLY)

add_executable(svr07_shader_cache
    main.cpp
    shader_set.cpp
    shader_set.h
    win_crypto.cpp
    win_crypto.h
    xex_image.cpp
    xex_image.h)
target_compile_features(svr07_shader_cache PRIVATE cxx_std_20)
target_include_directories(svr07_shader_cache PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}"           # shader_common_embed.inc
    "${SVR07_SC_XENOS}/thirdparty/xxHash")
target_compile_definitions(svr07_shader_cache PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_link_libraries(svr07_shader_cache PRIVATE bcrypt)
# Static C runtime: the tool runs before anything else of the native renderer does.
set_target_properties(svr07_shader_cache PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
