// svr07 - high-frame-rate stage 1 (reports/L6-fps-plan.md, reports/L7-fps-stage1.md).
#pragma once

#include <cstdint>

namespace svr07_fps {
// Entrance scene timing for [fpslog]: start = the game's 60 -> 30 switch sub_82429A30 (hooked in
// svr07_hooks.cpp for entrance_60fps), end = its 30 -> 60 switch sub_82429AA8 (svr07_fps.cpp).
// L17: also the scene gate's "entrance open" state.
void OnEntranceStart();
// L17 scene gate (reports/L17-fps-scene-gating.md): the heap layout sub_824150D0 carves (r4 = list).
void OnHeapLayout(uint32_t list);
// false when the high-fps mode wants the game's own 60 -> 30 entrance switch (console rate).
bool KeepEntrance60();
// The high rate is on this frame (live match).
bool HighFpsLive();
// Guest vblanks this guest frame waits for (native pacer, one thread): hz / rate outside the live match in the
// high-fps mode, else 1.
int PaceVblanks();
}  // namespace svr07_fps
