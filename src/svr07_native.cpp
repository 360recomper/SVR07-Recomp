// Native renderer scaffolding (SvR07 native port, lane L4): compiled only with SVR07_NATIVE_RENDERER
// (cmake/native_renderer.cmake). Checks that the SvR07 D3D layout headers in native/src build with the
// game's flags. Template: Rumble Roses XX src/rrxx_native.cpp.
//
// The size below is SvR07's (NR-1: Direct3D_CreateDevice sub_821DECF0, li r3,20480). Until lane L3
// rewrites native/src/svr07_d3d.h (it still carries RR's 0x5080) this assert fails on purpose: a
// scaffolding build must not pass with Rumble Roses's device layout.

#include <chrono>
#include <cstdint>

#include <rex/logging.h>

#include "svr07_d3d.h"
#include "svr07_d3d_names.h"

static_assert(svr07::d3d::kDeviceSize == 0x5000, "SvR07 XDK 2.0.3215.1 D3DDevice size (NR-1)");

// The emulated build logs "FPS: 59.9 (300 swaps in 5.0s)" every 5 s from the SDK's patched IssueSwap
// (sdk-patch/rexglue-sdk-v0.10-svr07.patch, D3D12 and Vulkan command processors). On the native
// renderer the guest GPU is the null backend, so that line never comes. The native Swap hook
// (native/src/reblue/gpu/hooks/device.cpp, D3DDevice_Swap_hook) calls this once per guest Swap and
// the same line comes out, same text and log category, so logs of both renderers compare 1:1.
// Only the guest (D3D) thread calls Swap: no locking.
void Svr07FpsTick() {
  static uint32_t fps_frames = 0;
  static auto fps_t0 = std::chrono::steady_clock::now();
  ++fps_frames;
  const auto fps_now = std::chrono::steady_clock::now();
  const double fps_dt = std::chrono::duration<double>(fps_now - fps_t0).count();
  if (fps_dt >= 5.0) {
    REXGPU_INFO("FPS: {:.1f} ({} swaps in {:.1f}s)", fps_frames / fps_dt, fps_frames, fps_dt);
    fps_frames = 0;
    fps_t0 = fps_now;
  }
}
