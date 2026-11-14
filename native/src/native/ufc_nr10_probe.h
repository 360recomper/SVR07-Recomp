// RR stand-in for UFC-native's NR-10 probe (UFC's DXT compressor; RR has no memexport / GPU
// compressor, FINDINGS-rr-nr1). The callers in ufc_draw_log.cpp / ufc_resolve_writeback.cpp /
// ufc_resources.cpp keep compiling; every probe is off.

#pragma once

#include <cstdint>
#include <string>

namespace bd::gpu {
struct NativeMirrorSource;
}

inline bool UfcNr10On() { return false; }
std::string UfcNr10DumpDir();  // ufc_resolve_writeback.cpp's dumps (svr07_native_wb_dump) still use it
inline void UfcNr10NoteResolve(uint32_t, uint32_t, uint32_t, uint32_t, bool, int32_t, int32_t, int32_t, int32_t,
                               int32_t, int32_t, uint32_t, uint32_t) {}
inline void UfcNr10NoteBind(uint32_t, const bd::gpu::NativeMirrorSource&) {}
inline void UfcNr10NoteShortcut(uint32_t) {}
inline void UfcNr10NoteDraw(uint32_t, uint32_t, uint32_t, bool) {}
