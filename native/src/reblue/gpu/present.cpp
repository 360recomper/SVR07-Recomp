/**
 * @file    gpu/present.cpp
 * @brief   End of frame: the deferred clear, the swapchain
 * acquire/blit/present, and the pre-Runtime overlay-only present the installer
 * uses.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#if defined(SVR_NATIVE_RENDERER)
#include "ufc_command_stream.h"
#include "ufc_frame_diag.h"
#endif
#include "gpu/frame.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include <plume_render_interface.h>
#if defined(REBLUE_D3D12) && defined(SVR_NATIVE_RENDERER)
#include <plume_d3d12.h>
#endif

#include "gpu/gpu_profiling.h"

#include "core/logging.h"
#include "engine/engine.h"
#include "gpu/backend.h"
#include "gpu/constant_buffers.h"
#include "gpu/output.h"
#include "gpu/settings.h"

#include <rex/cvar.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include <windows.h>
#endif

REXCVAR_DEFINE_INT32(svr07_native_pace, 2, "Native",
                     "Native renderer (RR-NR-10): the 60 fps frame limiter. 2 = none outside idle (the "
                     "game's own guest-vblank wait paces alone; default since nr10d with "
                     "svr07_native_vblank_precise: 3D 99.95% of frames <= 20 ms), 1 = precise host limiter "
                     "(high-resolution timer + short spin; use with svr07_native_vblank_precise false), "
                     "0 = the old sleep_until (+- 2-3 ms on Windows)");

// UFC-fps 9de23c2 (ufc3_async_present, UFC default true, but it only acts with threaded recording,
// which UFC turns on in high-fps mode). SvR07: same default; threaded recording is off by default
// here, so a default run does not change.
REXCVAR_DEFINE_BOOL(svr07_native_async_present, true, "SvR",
                    "Native renderer (fps): with threaded recording on (svr_threaded_recording or "
                    "svr07_native_threaded_hfps), the recording worker ends, submits and presents the "
                    "frame, so the game thread goes straight on to the next one. Also hands list "
                    "splits to the worker (svr07_native_async_split)");

#if defined(SVR_NATIVE_RENDERER)
REXCVAR_DEFINE_STRING(svr07_native_upscale, "auto", "Native",
                      "Native renderer (NR-13): how the picture is scaled to the window. auto = AMD "
                      "FSR 1 when the picture is smaller than the window area, bilinear otherwise; "
                      "fsr = FSR 1 also at the same size (sharpening only); bilinear = the old pass "
                      "always. A picture bigger than the window is always scaled down bilinear");
REXCVAR_DEFINE_DOUBLE(svr07_native_sharpness, 1.0, "Native",
                      "Native renderer (NR-13): FSR 1 sharpening in stops, 0 = most, 2 = little. "
                      "1.0, not AMD's 0.2: 0.2 made skin visibly grainy at 1x (nr13-f1 vs -f1s1)");
#endif

namespace bd::gpu {

namespace {

void ApplyVsync(VideoState &s) {
  if (s.swap_chain) {
    s.swap_chain->setVsyncEnabled(Settings::Get().Vsync());
  }
}

// NR-F1 async present. The worker presents frame N while the game thread records N+1, so the game
// thread cannot ask DXGI for N+1's back buffer (it would still be N's). A flip-model chain hands out
// its buffers in order, one step per Present, so the index is predicted; the worker checks it
// against DXGI before presenting and turns async present off for good on a mismatch.
bool g_async_index_valid = false;
u32 g_async_next_index = 0;
std::atomic<bool> g_async_broken{false};

struct AsyncFinish {
  plume::RenderCommandList *list = nullptr;
  plume::RenderCommandFence *fence = nullptr;
  plume::RenderCommandSemaphore *wait = nullptr;
  plume::RenderCommandSemaphore *signal = nullptr;
  u32 texture_index = 0;
  bool vsync = false;
};
AsyncFinish g_async_finish[kNumFrames];

bool AsyncPresentOn() {
#if defined(SVR_NATIVE_RENDERER) && defined(REBLUE_D3D12)
  return REXCVAR_GET(svr07_native_async_present) && SvrThreadedRecordingOn() &&
         !g_async_broken.load(std::memory_order_relaxed);
#else
  return false;
#endif
}

// Runs on the recording worker, in stream order after the frame's last command.
void AsyncFinishTask(void *arg) {
  const auto &f = *static_cast<const AsyncFinish *>(arg);
  auto &s = state();
  f.list->end();
  const plume::RenderCommandList *lists[] = {f.list};
  plume::RenderCommandSemaphore *waits[] = {f.wait};
  plume::RenderCommandSemaphore *signals[] = {f.signal};
  s.queue->executeCommandLists(lists, 1, waits, 1, signals, 1, f.fence);
  s.swap_chain->setVsyncEnabled(f.vsync);
  u32 actual = f.texture_index;
  s.swap_chain->acquireTexture(f.wait, &actual);
  if (actual != f.texture_index) {
    BD_ERROR("async present: back buffer {} predicted, DXGI has {}; async present off",
             f.texture_index, actual);
    g_async_broken.store(true, std::memory_order_relaxed);
  }
  if (!s.swap_chain->present(f.texture_index, signals, 1))
    s.resize_requested.store(true, std::memory_order_release);
}

constexpr i32 kIdleFPS = 30;

// RR-NR-10: sleep to ~1 ms before the deadline on a high-resolution waitable timer, then spin.
// sleep_until alone wakes 0-3 ms late on Windows, so frame times read 13-20 ms around 16.7.
void PreciseSleepUntil(std::chrono::steady_clock::time_point deadline) {
  using Clock = std::chrono::steady_clock;
#if defined(_WIN32)
  static HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr,
                                               CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                               TIMER_ALL_ACCESS);
  const auto coarse = deadline - std::chrono::microseconds(1000);
  const auto now = Clock::now();
  if (coarse > now) {
    if (timer) {
      LARGE_INTEGER due;
      due.QuadPart = -std::max<long long>(
          1, std::chrono::duration_cast<std::chrono::nanoseconds>(coarse - now).count() / 100);
      if (SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0))
        WaitForSingleObject(timer, INFINITE);
      else
        std::this_thread::sleep_until(coarse);
    } else {
      std::this_thread::sleep_until(coarse);
    }
  }
  while (Clock::now() < deadline)
    YieldProcessor();
#else
  std::this_thread::sleep_until(deadline);
#endif
}

#if defined(REBLUE_D3D12) && defined(SVR_NATIVE_RENDERER)
// RR-NR-10: display-side frame times from DXGI frame statistics (flip model, vsync): each new frame
// that reached the screen gives its refresh count; refreshes since the frame before x the refresh
// period = how long the frame before stayed on screen. Only while svr07_native_perf is on.
void SampleDisplayStats(plume::RenderSwapChain *chain) {
  if (!g_svr_perf_on.load(std::memory_order_relaxed) || !chain)
    return;
  auto *d3d = static_cast<plume::D3D12SwapChain *>(chain)->d3d;
  if (!d3d)
    return;
  static UINT last_present = 0, last_refresh = 0;
  static UINT last_sync_refresh = 0;
  static LONGLONG last_sync_qpc = 0;
  static double refresh_ms = 0.0;
  static LARGE_INTEGER qpf = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
  DXGI_FRAME_STATISTICS st{};
  if (FAILED(d3d->GetFrameStatistics(&st))) {
    last_present = 0;  // disjoint or not yet available: start over
    return;
  }
  if (last_sync_qpc && st.SyncRefreshCount > last_sync_refresh + 30) {
    refresh_ms = double(st.SyncQPCTime.QuadPart - last_sync_qpc) * 1000.0 / double(qpf.QuadPart) /
                 double(st.SyncRefreshCount - last_sync_refresh);
    last_sync_refresh = st.SyncRefreshCount;
    last_sync_qpc = st.SyncQPCTime.QuadPart;
  } else if (!last_sync_qpc) {
    last_sync_refresh = st.SyncRefreshCount;
    last_sync_qpc = st.SyncQPCTime.QuadPart;
  }
  if (last_present && st.PresentCount > last_present && refresh_ms > 0.0) {
    const UINT shown = st.PresentCount - last_present;
    const UINT refreshes = st.PresentRefreshCount - last_refresh;
    // Several frames shown since the last sample: spread the refreshes evenly over them.
    const double each = double(refreshes) * refresh_ms / double(shown);
    for (UINT i = 0; i < shown && i < 8; ++i)
      SvrPerfDisplayFrame(each, refresh_ms);
  }
  if (st.PresentCount != last_present) {
    last_present = st.PresentCount;
    last_refresh = st.PresentRefreshCount;
  }
}
#endif

void PaceFrame(bool idle = false) {
  using Clock = std::chrono::steady_clock;
  static Clock::time_point next{};
  static const i32 mode = REXCVAR_GET(svr07_native_pace);
  i32 fps = bd::engine::Settings::Get().FPSLimit();
  if (bd::engine::SofdecPlayer::Playing())
    fps = 30;
  if (idle && (fps <= 0 || fps > kIdleFPS))
    fps = kIdleFPS;
  if (fps <= 0 || (mode == 2 && !idle)) {
    next = {};
    return;
  }
  const auto period = std::chrono::duration_cast<Clock::duration>(
      std::chrono::duration<double, std::milli>(1000.0 / fps));
  const auto now = Clock::now();
  if (next.time_since_epoch().count() != 0 && now < next) {
    if (mode == 1)
      PreciseSleepUntil(next);
    else
      std::this_thread::sleep_until(next);
    next += period;
  } else {
    next = now + period;
  }
}

void AbandonFrame(VideoState &s, std::unique_lock<VideoMutex> &lock) {
  s.frame_present_committed = true;
  if (!s.command_list_open)
    return;
  SubmitOpenListLocked(s);
  AdvanceAndWaitReused(s);
  const u32 reclaimed = s.frame.load(std::memory_order_relaxed);
  lock.unlock();
  DrainSlot(s, reclaimed);
}

void RebuildSwapChain(VideoState &s) {
  SubmitOpenListLocked(s);
  SvrWaitAsyncSubmits();
  g_async_index_valid = false;
  for (u32 i = 0; i < kNumFrames; ++i) {
    if (s.command_list_submitted[i]) {
      s.queue->waitForCommandFence(s.fences[i].get());
      s.command_list_submitted[i] = false;
    }
  }
  s.framebuffers.clear();
  if (!s.swap_chain->resize() || !BuildFramebuffers(s) ||
      !BuildPresentSemaphores(s)) {
    if (s.swap_chain->getWidth() && s.swap_chain->getHeight())
      BD_ERROR("Swap chain resize failed");
    return;
  }
  if (static_cast<AspectMode>(Settings::Get().AspectRatio()) ==
      AspectMode::Auto)
    Output::Recompute();
}

GuestTexture *SelectPresentSource(VideoState &s, GuestTexture *frontBuffer) {
  GuestTexture *last_rt = s.last_drawn_rt;
  GuestTexture *rt = (s.back_buffer_surface && last_rt == s.back_buffer_surface)
                         ? s.back_buffer_surface
                     : (frontBuffer && frontBuffer->texture) ? frontBuffer
                     : last_rt                               ? last_rt
                     : s.last_resolved_dst ? s.last_resolved_dst
                                           : s.back_buffer_surface;
  if (rt && rt->sourceSurface && rt->sourceSurface != rt &&
      rt->sourceSurface->texture &&
      rt->sourceSurface->sampleCount == plume::RenderSampleCount::COUNT_1 &&
      rt->sourceSurface->descriptorIndex != kInvalidDescriptorIndex) {
    rt = rt->sourceSurface;
  }
  return rt;
}

#if defined(SVR_NATIVE_RENDERER)
// NR-13: FSR 1 when the cvar asks for it and the picture is scaled up (or kept the same size
// with "fsr"); a picture scaled down on both axes keeps the bilinear pass.
bool UseFsr(const VideoState &s, const GuestTexture *rt, u32 fit_w, u32 fit_h) {
  if (!s.fsr_easu_pipeline || !s.fsr_rcas_pipeline || fit_w > 0xFFFF || fit_h > 0xFFFF)
    return false;
  const std::string &mode = REXCVAR_GET(svr07_native_upscale);
  const u32 src_w = rt->width; // host size (scaled)
  const u32 src_h = rt->height;
  bool fsr = false;
  if (mode == "fsr")
    fsr = fit_w >= src_w || fit_h >= src_h;
  else if (mode == "auto")
    fsr = fit_w > src_w || fit_h > src_h;
  // One line whenever the choice or a size changes.
  static u64 s_logged = ~0ull;
  const u64 key = (u64(fsr) << 63) ^ (u64(src_w) << 48) ^ (u64(src_h) << 32) ^
                  (u64(fit_w) << 16) ^ u64(fit_h);
  if (key != s_logged) {
    s_logged = key;
    BD_INFO("[nr13] present: {} {}x{} -> {}x{} (svr07_native_upscale {}, sharpness {})",
            fsr ? "FSR 1" : "bilinear", src_w, src_h, fit_w, fit_h, mode,
            REXCVAR_GET(svr07_native_sharpness));
  }
  return fsr;
}

// An FSR surface at w x h (made on first use); on a size change the old one goes to the fence
// graveyards.
bool EnsureFsrSurfaceLocked(VideoState &s, FsrSurface &f, u32 w, u32 h, const char *tag) {
  if (f.texture && f.w == w && f.h == h)
    return true;
  const auto retire = [&] {
    const u32 slot = Video::RetireSlot(tag);
    f.fb.reset();
    s.texture_view_graveyard[slot].push_back(std::move(f.view));
    s.texture_graveyard[slot].push_back(std::move(f.texture));
    if (f.slot != kInvalidDescriptorIndex)
      ParkDescriptorSlotLocked(s, f.slot, kNullTexture2DDescriptorIndex);
    f.slot = kInvalidDescriptorIndex;
    f.w = f.h = 0;
  };
  if (f.texture)
    retire();
  plume::RenderTextureDesc desc;
  desc.dimension = plume::RenderTextureDimension::TEXTURE_2D;
  desc.width = w;
  desc.height = h;
  desc.depth = 1;
  desc.mipLevels = 1;
  desc.arraySize = 1;
  desc.format = plume::RenderFormat::B8G8R8A8_UNORM;
  desc.flags = plume::RenderTextureFlag::RENDER_TARGET;
  desc.multisampling.sampleCount = plume::RenderSampleCount::COUNT_1;
  desc.committed = true; // not the target pool: freed on a size change
  f.texture = CreateHostTexture(s.device.get(), desc, tag);
  if (!f.texture)
    return false;
  plume::RenderTextureViewDesc view_desc;
  view_desc.format = desc.format;
  view_desc.dimension = plume::RenderTextureViewDimension::TEXTURE_2D;
  view_desc.mipLevels = 1;
  f.view = f.texture->createTextureView(view_desc);
  const plume::RenderTexture *color_attachments[1] = {f.texture.get()};
  f.fb = s.device->createFramebuffer(plume::RenderFramebufferDesc(color_attachments, 1));
  f.slot = AllocateSlot(s);
  if (!f.view || !f.fb || f.slot == kInvalidDescriptorIndex) {
    retire();
    BD_ERROR("[nr13] {} {}x{} failed: bilinear", tag, w, h);
    return false;
  }
  s.texture_descriptor_set->setTexture(f.slot, f.texture.get(),
                                       plume::RenderTextureLayout::SHADER_READ, f.view.get());
  f.w = w;
  f.h = h;
  return true;
}

// One full-surface pass into f; leaves f readable for the next pass.
void RecordFsrPassLocked(VideoState &s, FsrSurface &f, plume::RenderPipeline *pipeline,
                         u32 src_desc, u32 pair, float param0, float param1) {
  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS,
                           plume::RenderTextureBarrier(f.texture.get(),
                                                       plume::RenderTextureLayout::COLOR_WRITE));
  s.command_list->setFramebuffer(f.fb.get());
  s.command_list->setViewports(plume::RenderViewport(0.0f, 0.0f, float(f.w), float(f.h)));
  s.command_list->setScissors(plume::RenderRect(0, 0, i32(f.w), i32(f.h)));
  s.command_list->setPipeline(pipeline);
  struct {
    u32 descriptor_index;
    u32 pair;
    float param0;
    float param1;
  } pc{src_desc, pair, param0, param1};
  s.command_list->setGraphicsPushConstants(kCopyPushConstantRangeIndex, &pc,
                                           kCopyPushConstantByteOffset, sizeof(pc));
  s.command_list->drawInstanced(3, 1, 0, 0);
  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS,
                           plume::RenderTextureBarrier(f.texture.get(),
                                                       plume::RenderTextureLayout::SHADER_READ));
}
#endif

void RecordPresentPass(VideoState &s, GuestTexture *rt,
                       plume::RenderTexture *back,
                       plume::RenderFramebuffer *back_fb) {
  BD_GPU_ZONE("RecordPresentPass");
  if (rt->layout != plume::RenderTextureLayout::SHADER_READ) {
    const bool needs_discard =
        (rt->layout == plume::RenderTextureLayout::UNKNOWN);
    if (needs_discard) {
      s.command_list->barriers(
          plume::RenderBarrierStage::GRAPHICS,
          plume::RenderTextureBarrier(rt->texture,
                                      plume::RenderTextureLayout::COLOR_WRITE));
      s.command_list->discardTexture(rt->texture);
    }
    s.command_list->barriers(
        plume::RenderBarrierStage::GRAPHICS,
        plume::RenderTextureBarrier(rt->texture,
                                    plume::RenderTextureLayout::SHADER_READ));
    rt->layout = plume::RenderTextureLayout::SHADER_READ;
  }

  const u32 swap_w = s.swap_chain->getWidth();
  const u32 swap_h = s.swap_chain->getHeight();
  const u32 gamma_src_desc = rt->descriptorIndex;

  const double present_aspect =
      (bd::engine::SofdecPlayer::Playing() && !Output::StretchToFill())
          ? kDesignCanvasAspect
          : Output::RenderAspect();
  u32 fit_w = swap_w, fit_h = swap_h;
  i32 off_x = 0, off_y = 0;
  Output::ComputeFit(swap_w, swap_h, present_aspect, fit_w, fit_h, off_x,
                          off_y);

#if defined(SVR_NATIVE_RENDERER)
  // NR-13: the same gamma 1 and display correction 1 as the bilinear pass below, applied to each
  // texel before EASU filters it.
  // First the bilinear pass's colour change at the picture's own size (in EASU's 12 gathers it
  // cost ~2.5 ms at 4K output, nr13-o15), then EASU to the fit size.
  const bool fsr = UseFsr(s, rt, fit_w, fit_h) && off_x >= 0 && off_y >= 0 &&
                   EnsureFsrSurfaceLocked(s, s.fsr_input, rt->width, rt->height, "fsr-input") &&
                   EnsureFsrSurfaceLocked(s, s.fsr_target, fit_w, fit_h, "fsr-target");
  if (fsr) {
    RecordFsrPassLocked(s, s.fsr_input, s.gamma_correction_pipeline.get(), gamma_src_desc, 0,
                        1.0f, 1.0f);
    RecordFsrPassLocked(s, s.fsr_target, s.fsr_easu_pipeline.get(), s.fsr_input.slot,
                        fit_w | (fit_h << 16), 0.0f, 0.0f);
  }
#endif

  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS,
                           plume::RenderTextureBarrier(
                               back, plume::RenderTextureLayout::COLOR_WRITE));
  s.command_list->setFramebuffer(back_fb);
  if (fit_w != swap_w || fit_h != swap_h) {
    s.command_list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
  }
  s.command_list->setViewports(plume::RenderViewport(
      static_cast<float>(off_x), static_cast<float>(off_y),
      static_cast<float>(fit_w), static_cast<float>(fit_h)));
  s.command_list->setScissors(
      plume::RenderRect(off_x, off_y, off_x + static_cast<i32>(fit_w),
                        off_y + static_cast<i32>(fit_h)));
#if defined(SVR_NATIVE_RENDERER)
  // UFC's front buffer is already display-encoded (Blue Dragon's is linear and relies on the
  // pow(x, 0.5) scanout ramp its bdBuildGammaRampLUT hook captures), so no exponent here. The
  // emulated renderer's picture does have the TV correction on it: Rec709_encode(sRGB_decode(x))
  // of the native picture matches it on static screens (nr8b12: 50 -> 34, 83 -> 68, 131 -> 119).
  constexpr float kPresentGamma = 1.0f;
  constexpr float kPresentDisplayCorrection = 1.0f;
  s.guest_gamma = 1.0f;
#else
  constexpr float kPresentGamma = 1.0f;
  constexpr float kPresentDisplayCorrection = 1.0f;
#endif
  struct PresentPushConstants {
    u32 descriptor_index;
    u32 descriptor_index_2;
    float gamma;
    float display_correction;
  } pc{gamma_src_desc, 0, s.guest_gamma * kPresentGamma,
       kPresentDisplayCorrection};
#if defined(SVR_NATIVE_RENDERER)
  if (fsr) {
    pc = {s.fsr_target.slot, u32(off_x) | (u32(off_y) << 16),
          float(REXCVAR_GET(svr07_native_sharpness)), 0.0f};
    s.command_list->setPipeline(s.fsr_rcas_pipeline.get());
  } else
#endif
  s.command_list->setPipeline(s.gamma_correction_pipeline.get());
  s.command_list->setGraphicsPushConstants(kCopyPushConstantRangeIndex, &pc,
                                           kCopyPushConstantByteOffset,
                                           sizeof(pc));
  s.command_list->drawInstanced(3, 1, 0, 0);

  s.command_list->setViewports(plume::RenderViewport(
      0.0f, 0.0f, static_cast<float>(swap_w), static_cast<float>(swap_h)));
  s.command_list->setScissors(plume::RenderRect(0, 0, static_cast<i32>(swap_w),
                                                static_cast<i32>(swap_h)));

  if (g_overlay_draw_hook) {
    g_overlay_draw_hook(s.command_list, back_fb, swap_w, swap_h);
  }

  s.command_list->setFramebuffer(nullptr);
  s.command_list->barriers(
      plume::RenderBarrierStage::GRAPHICS,
      plume::RenderTextureBarrier(back, plume::RenderTextureLayout::PRESENT));
}

} // namespace

void Video::Clear(u32 flags, u32 color_argb, float depth, u32 stencil,
                  const plume::RenderRect *rects, u32 rect_count) {
  SvrZone zone(SvrZoneId::kClear);
  auto &s = state();
  std::lock_guard lock(s.mutex);
  s.frame_present_committed = false;
  BeginCommandList(s);
  if (!s.command_list_open)
    return;
  GuestTexture *rt = nullptr;
  GuestTexture *ds = nullptr;
  ResolveEffectiveTargets(s, rt, ds);
  const bool clear_color = (flags & kClearTarget) != 0 && rt != nullptr;
  const bool clear_depth = (flags & kClearZBuffer) != 0 && ds != nullptr;
  const bool clear_stencil = (flags & kClearStencil) != 0 && ds != nullptr;
  if (!clear_color && !clear_depth && !clear_stencil)
    return;
  if (!BindDrawFramebufferLocked(clear_color))
    return;
  // Rect clears: each rect clamped to its target (D3D12 rejects empty or outside rects).
  plume::RenderRect clamped[8];
  const auto clamp_to = [&](const GuestTexture *target) -> u32 {
    u32 n = 0;
    for (u32 i = 0; rects && target && i < rect_count && i < 8; ++i) {
      plume::RenderRect r = rects[i];
      r.left = std::clamp<int32_t>(r.left, 0, int32_t(target->width));
      r.top = std::clamp<int32_t>(r.top, 0, int32_t(target->height));
      r.right = std::clamp<int32_t>(r.right, 0, int32_t(target->width));
      r.bottom = std::clamp<int32_t>(r.bottom, 0, int32_t(target->height));
      if (r.right > r.left && r.bottom > r.top)
        clamped[n++] = r;
    }
    return n;
  };
  const bool use_rects = rects != nullptr && rect_count != 0;
  if (clear_color) {
    const u32 n = use_rects ? clamp_to(rt) : 0;
    if (!use_rects)
      s.command_list->clearColor(0, ArgbToRenderColor(color_argb));
    else if (n)
      s.command_list->clearColor(0, ArgbToRenderColor(color_argb), clamped, n);
  }
  if (clear_depth || clear_stencil) {
    const u32 n = use_rects ? clamp_to(ds) : 0;
    if (!use_rects)
      s.command_list->clearDepthStencil(clear_depth, clear_stencil, depth, stencil);
    else if (n)
      s.command_list->clearDepthStencil(clear_depth, clear_stencil, depth, stencil, clamped, n);
  }
}

bool SvrAsyncPresentOn() { return AsyncPresentOn(); }

void Video::RequestResize() {
  state().resize_requested.store(true, std::memory_order_release);
}

void Video::Present(GuestTexture *frontBuffer) {
  SvrZone zone(SvrZoneId::kPresentRecord);
  auto &s = state();
  if (s.shutting_down.load(std::memory_order_acquire))
    return;
  std::unique_lock lock(s.mutex);
  if (!s.ready || s.shutting_down.load(std::memory_order_acquire)) {
    return;
  }
  if (s.frame_present_committed) {
    return;
  }

  const bool resize_requested =
      s.resize_requested.exchange(false, std::memory_order_acq_rel);
  if (s.swap_chain->needsResize() || resize_requested) {
    RebuildSwapChain(s);
  }

  if (s.framebuffers.empty()) {
    AbandonFrame(s, lock);
    if (lock.owns_lock())
      lock.unlock();
    PaceFrame(true);
    return;
  }

  u32 texture_index = 0;
  const bool async_present = AsyncPresentOn();
  if (!async_present && g_async_index_valid) {
    // Leaving async present: the worker's presents must land before DXGI is asked.
    SvrWaitAsyncSubmits();
    g_async_index_valid = false;
  }
  if (async_present && g_async_index_valid) {
    texture_index = g_async_next_index;
  } else {
    if (async_present)
      SvrWaitAsyncSubmits();
    BD_CPU_ZONE("AcquireTexture");
#if defined(SVR_NATIVE_RENDERER)
    SvrDiagTimer acquire(SvrDiag::kAcquire);
#endif
    if (!s.swap_chain->acquireTexture(
            s.acquire_semaphores[s.frame.load(std::memory_order_relaxed)].get(),
            &texture_index)) {
      RebuildSwapChain(s);
      if (s.framebuffers.empty() ||
          !s.swap_chain->acquireTexture(
              s.acquire_semaphores[s.frame.load(std::memory_order_relaxed)]
                  .get(),
              &texture_index)) {
        CheckDeviceRemoved("swapchain acquire");
        AbandonFrame(s, lock);
        return;
      }
    }
  }
  if (async_present && texture_index >= s.framebuffers.size()) {
    // Cannot happen with a sane chain; never index past it.
    g_async_broken.store(true, std::memory_order_relaxed);
    AbandonFrame(s, lock);
    return;
  }

  plume::RenderTexture *back = s.swap_chain->getTexture(texture_index);
  plume::RenderFramebuffer *back_fb = s.framebuffers[texture_index].get();

  GuestTexture *rt = SelectPresentSource(s, frontBuffer);

  const bool have_rt_blit =
      rt && rt->texture && rt->descriptorIndex != kInvalidDescriptorIndex &&
      rt->sampleCount == plume::RenderSampleCount::COUNT_1;
  if (!have_rt_blit) {
    static std::atomic<u32> s_log{0};
    const u32 n = s_log.fetch_add(1, std::memory_order_relaxed);
    if (n < 5) {
      BD_ERROR("Present #{} skipped: no drawable RT", n);
    }
    AbandonFrame(s, lock);
    return;
  }

  BeginCommandList(s);
  RecordPresentPass(s, rt, back, back_fb);

  const u32 cur = s.frame.load(std::memory_order_relaxed);
  SvrGpuTimeEnd(cur);
#if defined(SVR_NATIVE_RENDERER)
  if (async_present) {
    s.command_list_open = false;
    auto &f = g_async_finish[cur];
    f.list = s.command_lists[cur].get();
    f.fence = s.fences[cur].get();
    f.wait = s.acquire_semaphores[cur].get();
    f.signal = s.render_semaphores[texture_index].get();
    f.texture_index = texture_index;
    f.vsync = Settings::Get().Vsync();
    {
      SvrZone zone_submit(SvrZoneId::kSubmit);
      SvrAsyncSubmitPos(cur) = SvrCommandStreamTask(&AsyncFinishTask, &f);
    }
    s.command_list_submitted[cur] = true;
    g_async_next_index = (texture_index + 1) % static_cast<u32>(s.framebuffers.size());
    g_async_index_valid = true;
  } else {
  SvrRecordingList(cur, s.command_lists[cur].get())->end();  // waits for threaded recording
#else
  s.command_lists[cur]->end();
#endif
  s.command_list_open = false;

  const plume::RenderCommandList *lists[] = {s.command_lists[cur].get()};
  plume::RenderCommandSemaphore *waits[] = {s.acquire_semaphores[cur].get()};
  plume::RenderCommandSemaphore *signals[] = {
      s.render_semaphores[texture_index].get()};
  {
    BD_CPU_ZONE("Submit");
    SvrZone zone_submit(SvrZoneId::kSubmit);
    s.queue->executeCommandLists(lists, 1, waits, 1, signals, 1,
                                 s.fences[cur].get());
  }
  s.command_list_submitted[cur] = true;
  ApplyVsync(s);
  {
    BD_CPU_ZONE("PresentSwap");
    SvrZone zone_present(SvrZoneId::kPresentCall);
    if (!s.swap_chain->present(texture_index, signals, 1)) {
      if (!CheckDeviceRemoved("swapchain present"))
        s.resize_requested.store(true, std::memory_order_release);
    }
  }
#if defined(REBLUE_D3D12) && defined(SVR_NATIVE_RENDERER)
  SampleDisplayStats(s.swap_chain.get());
#endif
#if defined(SVR_NATIVE_RENDERER)
  }  // !async_present. SvR07: SampleDisplayStats stays on this side: with async present the
     // worker calls Present, and the display stats (frame thread only) are not sampled then.
#endif
  {
    BD_CPU_ZONE("WaitFence");
    AdvanceAndWaitReused(s);
  }
  const u32 reclaimed = s.frame.load(std::memory_order_relaxed);
  s.frame_present_committed = true;
  BD_FRAME_MARK();
  Video::SyncBackBufferSizeLocked();
  lock.unlock();
  {
    BD_CPU_ZONE("DrainSlot");
    SvrZone zone_drain(SvrZoneId::kDrainSlot);
    DrainSlot(s, reclaimed);
  }
  {
    BD_CPU_ZONE("PaceFrame");
    SvrZone zone_pace(SvrZoneId::kPaceFrame);
    PaceFrame();
  }
}

void Video::SkipPresent() {
  auto &s = state();
  if (s.shutting_down.load(std::memory_order_acquire))
    return;
  std::unique_lock lock(s.mutex);
  if (!s.ready || s.shutting_down.load(std::memory_order_acquire) ||
      s.frame_present_committed) {
    return;
  }
  AbandonFrame(s, lock);
}

void Video::PresentOverlayFrame() {
  auto &s = state();
  if (s.shutting_down.load(std::memory_order_acquire))
    return;
  std::unique_lock lock(s.mutex);
  if (!s.swap_chain || s.ready)
    return;

  const bool resize_requested =
      s.resize_requested.exchange(false, std::memory_order_acq_rel);
  if (s.swap_chain->needsResize() || resize_requested) {
    SvrWaitAsyncSubmits();
    g_async_index_valid = false;
    for (u32 i = 0; i < kNumFrames; ++i) {
      if (s.command_list_submitted[i]) {
        s.queue->waitForCommandFence(s.fences[i].get());
        s.command_list_submitted[i] = false;
      }
    }
    s.framebuffers.clear();
    if (!s.swap_chain->resize() || !BuildFramebuffers(s) ||
        !BuildPresentSemaphores(s)) {
      if (s.swap_chain->getWidth() && s.swap_chain->getHeight())
        BD_ERROR("Swap chain resize failed");
    }
  }
  if (s.framebuffers.empty())
    return;

  const u32 cur = s.frame.load(std::memory_order_relaxed);
  u32 texture_index = 0;
  if (!s.swap_chain->acquireTexture(s.acquire_semaphores[cur].get(),
                                    &texture_index)) {
    s.resize_requested.store(true, std::memory_order_release);
    return;
  }
  plume::RenderTexture *back = s.swap_chain->getTexture(texture_index);
  plume::RenderFramebuffer *back_fb = s.framebuffers[texture_index].get();
  const u32 swap_w = s.swap_chain->getWidth();
  const u32 swap_h = s.swap_chain->getHeight();

  BeginCommandList(s);
  if (!s.command_list_open)
    return;

  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS,
                           plume::RenderTextureBarrier(
                               back, plume::RenderTextureLayout::COLOR_WRITE));
  s.command_list->setFramebuffer(back_fb);
  s.command_list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
  s.command_list->setViewports(plume::RenderViewport(
      0.0f, 0.0f, static_cast<float>(swap_w), static_cast<float>(swap_h)));
  s.command_list->setScissors(plume::RenderRect(0, 0, static_cast<i32>(swap_w),
                                                static_cast<i32>(swap_h)));
  if (g_overlay_draw_hook) {
    g_overlay_draw_hook(s.command_list, back_fb, swap_w, swap_h);
  }
  s.command_list->setFramebuffer(nullptr);
  s.command_list->barriers(
      plume::RenderBarrierStage::GRAPHICS,
      plume::RenderTextureBarrier(back, plume::RenderTextureLayout::PRESENT));

#if defined(SVR_NATIVE_RENDERER)
  SvrRecordingList(cur, s.command_lists[cur].get())->end();  // waits for threaded recording
#else
  s.command_lists[cur]->end();
#endif
  s.command_list_open = false;
  const plume::RenderCommandList *lists[] = {s.command_lists[cur].get()};
  plume::RenderCommandSemaphore *waits[] = {s.acquire_semaphores[cur].get()};
  plume::RenderCommandSemaphore *signals[] = {
      s.render_semaphores[texture_index].get()};
  s.queue->executeCommandLists(lists, 1, waits, 1, signals, 1,
                               s.fences[cur].get());
  s.command_list_submitted[cur] = true;
  ApplyVsync(s);
  if (!s.swap_chain->present(texture_index, signals, 1)) {
    CheckDeviceRemoved("swapchain present (overlay)");
  }
  AdvanceAndWaitReused(s);
  const u32 reclaimed = s.frame.load(std::memory_order_relaxed);
  lock.unlock();
  DrainSlot(s, reclaimed);
}

} // namespace bd::gpu
