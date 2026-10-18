// svr07 - game-code hooks.
//
// Each hook replaces a weak recompiled function by name; the original stays
// callable as __imp__<name>.

#include "generated/default/svr07_init.h"

#include <algorithm>
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>

REXCVAR_DEFINE_BOOL(entrance_60fps, true, "Game",
                    "Keep entrances (and other scenes the game drops to 30 fps) at 60 fps");

// The game's frame-rate mode lives in a global struct at 0x8295D1A4: +8 is the
// target fps (60/50, or 30/25), +28/+48/+52 the matching timesteps. Logic and
// animation advance by those timesteps, and the D3D presentation interval is
// derived from +8 (30/25 -> wait 2 vblanks per frame).
//
// sub_82429A30 switches 60 -> 30 (50 -> 25). The entrance code calls it on the
// 360 to fit its frame budget; sub_82429AA8 switches back afterwards. Skipping
// the switch keeps the 60 fps mode, so entrances render at 60 fps at the right
// speed.
REX_HOOK_RAW(sub_82429A30) {
  if (REXCVAR_GET(entrance_60fps)) {
    REXKRNL_INFO("entrance_60fps: kept 60 fps mode (skipped switch to 30)");
    return;
  }
  __imp__sub_82429A30(ctx, base);
}

REXCVAR_DEFINE_BOOL(log_heap_failures, true, "Game",
                    "Log when the game's own heap allocator runs out of room");

// The game's allocator: sub_82414850(size, tag) returns a block from the calling
// thread's heap (sub_82414380 maps thread -> heap index), or 0 when that heap has
// no free block big enough. Heaps: table at [0x8295B9F4], 24 bytes each
// ({start, end, ...}); blocks have a 16-byte header, word 0 = (tag << 28) | index
// of the next block (in 16-byte units from start), free blocks are chained via +8
// starting at the head block's +8.
REX_HOOK_RAW(sub_82414850) {
  uint32_t size = ctx.r3.u32;
  uint32_t tag = ctx.r4.u32;
  __imp__sub_82414850(ctx, base);
  if (ctx.r3.u32 != 0 || size == 0 || !REXCVAR_GET(log_heap_failures)) {
    return;
  }
  REXKRNL_WARN("game heap: alloc of {:#x} bytes (tag {}) failed", size, tag);
  uint32_t table = REX_LOAD_U32(0x8295B9F4);
  for (uint32_t h = 0; h < 8 && table; ++h) {
    uint32_t start = REX_LOAD_U32(table + h * 24);
    uint32_t end = REX_LOAD_U32(table + h * 24 + 4);
    if (!start || end <= start) break;
    uint32_t units = (end - start) >> 4;
    uint64_t free_units = 0;
    uint32_t largest = 0, blocks = 0;
    for (uint32_t idx = REX_LOAD_U32(start + 8); idx && idx < units && blocks < 1000000; ++blocks) {
      uint32_t block = start + idx * 16;
      uint32_t len = (REX_LOAD_U32(block) & 0x0FFFFFFF) - idx - 1;
      free_units += len;
      largest = std::max(largest, len);
      idx = REX_LOAD_U32(block + 8);
    }
    REXKRNL_WARN("  heap {}: {:#x}-{:#x} ({} MB), free {} KB in {} blocks, largest {} KB", h,
                 start, end, (end - start) >> 20, free_units * 16 / 1024, blocks,
                 uint64_t(largest) * 16 / 1024);
  }
}

REXCVAR_DEFINE_BOOL(log_loadscreen, false, "Game",
                    "Log the loading-screen animation controller (diagnostics)");

// Loading-screen controller (singleton at [0x8293C278], made by sub_821BA120):
// +8 busy, +16 state (0/1 default animation at +32, 2 = object at +36,
// 3 = object at +40), +300/+304/+308 a request queued while busy.
// Animation objects (ctor sub_82271640) get their track array at +84 when their
// data arrives (sub_82271F08 for the startup one, sub_82272470 for resources).
namespace {
uint32_t AnimTracks(uint8_t* base, uint32_t obj) {
  return obj ? REX_LOAD_U32(obj + 84) : 0;
}
void LogController(uint8_t* base, const char* what, uint32_t c) {
  uint32_t a = REX_LOAD_U32(c + 32), b = REX_LOAD_U32(c + 36), d = REX_LOAD_U32(c + 40);
  REXKRNL_INFO("loadscreen {}: busy={} state={} +32={:#x}(tracks {:#x}) +36={:#x}(tracks {:#x}) "
               "+40={:#x}(tracks {:#x})",
               what, REX_LOAD_U32(c + 8), REX_LOAD_U32(c + 16), a, AnimTracks(base, a), b,
               AnimTracks(base, b), d, AnimTracks(base, d));
}
}  // namespace

// play(controller, mode, arg)
REX_HOOK_RAW(sub_821BA460) {
  if (REXCVAR_GET(log_loadscreen)) {
    REXKRNL_INFO("loadscreen play mode={} arg={} lr={:#x}", ctx.r4.u32, ctx.r5.u32,
                 uint32_t(ctx.lr));
    LogController(base, "before play", ctx.r3.u32);
  }
  __imp__sub_821BA460(ctx, base);
}

// Switch to an unlocked loading screen (starts its resource load).
REX_HOOK_RAW(sub_821BAAA8) {
  uint32_t c = ctx.r3.u32;
  __imp__sub_821BAAA8(ctx, base);
  if (REXCVAR_GET(log_loadscreen)) {
    const char* name = reinterpret_cast<const char*>(REX_RAW_ADDR(c + 44));
    REXKRNL_INFO("loadscreen switch to {}", std::string(name, strnlen(name, 11)));
    LogController(base, "after switch", c);
  }
}

// Animation object: resource loaded.
REX_HOOK_RAW(sub_82272470) {
  uint32_t obj = ctx.r3.u32;
  __imp__sub_82272470(ctx, base);
  if (REXCVAR_GET(log_loadscreen)) {
    REXKRNL_INFO("loadscreen anim {:#x} loaded: tracks {:#x} textures(+56) {:#x} +60 {:#x} "
                 "ids {}/{} res(+40) {:#x}",
                 obj, AnimTracks(base, obj), REX_LOAD_U32(obj + 56), REX_LOAD_U32(obj + 60),
                 REX_LOAD_U32(obj + 44), REX_LOAD_U32(obj + 48), REX_LOAD_U32(obj + 40));
  }
}
