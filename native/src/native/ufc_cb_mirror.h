// Stand-in for UFC-native's ufc_cb_mirror.h (NR-8b-2k command-buffer replay), from the RR port. SvR07
// (XDK 2.0.3215.1) is not expected to record command buffers (NR-1: none found; the census shows the
// command-buffer share), so no draw is ever captured; the mirror helpers defined in hooks/state.cpp /
// hooks/draw.cpp stay declared.

#pragma once

#include <cstdint>

inline bool UfcCbCaptureDraw(uint32_t, bool, uint32_t, uint32_t, uint32_t, uint32_t) { return false; }
inline bool UfcCbRecordedDraw(uint32_t, const char*, uint32_t, uint32_t, uint32_t, int32_t, uint64_t, uint64_t) {
  return false;
}

void UfcDrawRecorded(uint32_t dev, bool indexed, uint32_t prim, uint32_t base_vertex, uint32_t start,
                     uint32_t count);
// SvR07: +0x3000 .. +0x3068 (viewport) = 26 slots (svr07_d3d.h kTextureSlots), as RR and UFC.
constexpr uint32_t kUfcMirrorTextures = 26;
void UfcMirrorSave();
void UfcMirrorApply(uint32_t vs, uint32_t ps, uint32_t decl, const uint32_t* textures);
void UfcMirrorRestore(uint32_t vs, uint32_t ps, uint32_t decl);
