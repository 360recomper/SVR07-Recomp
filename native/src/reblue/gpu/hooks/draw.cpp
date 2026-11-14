/**
 * @file    gpu/hooks/draw.cpp
 * @brief   Guest draw calls, and the EDRAM resolve / tiling calls that bracket
 *          them.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#if defined(SVR_NATIVE_RENDERER)
#include "ufc_frame_diag.h"
#endif
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#if defined(SVR_NATIVE_RENDERER)
#include <array>
#include <set>
#endif

#include <rex/cvar.h>
#include <rex/graphics/xenos.h>
#include <rex/hook.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/types.h>

#include <plume_render_interface.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "core/profiling.h"
#include "gpu/constant_buffers.h"
#include "gpu/d3d.h"
#include "gpu/frame.h"
#if defined(SVR_NATIVE_RENDERER)
#include "ufc_cb_mirror.h"
#include "ufc_command_stream.h"
#include "ufc_draw_log.h"
#include "ufc_geometry.h"
#include "ufc_nr10_probe.h"
#include "ufc_resolve_writeback.h"

#if defined(SVR_NATIVE_RENDERER)
REXCVAR_DEFINE_INT32(svr07_native_off, 0, "Native",
                     "Native renderer dev switch, bits that turn a path off for bisecting: 1 partial "
                     "resolves (the whole surface is copied), 2 clear after resolve, 4 rectangle "
                     "list rebuilds, 8 the game's indices on indexed quad lists, 16 partial constant "
                     "uploads and texture slot rebuilds, 32 trusted geometry checks (when "
                     "svr07_native_geom_trust is on), 64 command buffer replay, 128 integer vertex "
                     "elements in vertex buffers, 256 linear row copies in texture mirrors (per-block "
                     "copies instead), 1024 leaving out a depth target smaller than the "
                     "colour target, 2048 fractional scale and 1x targets, 4096 resolve write-back and "
                     "read watches, 8192 filling a new resolve target from its guest memory, "
                     "16384 a resolve target's 1x by its size only (not by its source surface), "
                     "32768 lazy target fill (every level filled when the target is made), 65536 "
                     "scaling a target fill on the GPU (scaled on the CPU and uploaded whole), "
                     "131072 the target fill's 4-texel conversion (one texel at a time), 262144 the "
                     "render target pool, 524288 the Swap fence sleep (the game spins, NR-P6d), "
                     "1048576 one write-back copy of all its rects' pages (rect by rect, NR-B3a), "
                     "2097152 threaded write-back conversion of big rects (one thread, NR-14), "
                     "4194304 reading write-back texels in place (copied out first, NR-14b), "
                     "8388608 early list submits during the frame (one list to the end, NR-14c), "
                     "16777216 the texture mirror pool (default pools, NR-14d), "
                     "33554432 partial constant uploads only (all registers), 67108864 texture slot "
                     "rebuilds only (every slot every draw), 16 = both; 134217728 constant arrays "
                     "uploaded at their declared size only (not to register 255), "
                     "268435456 upload chunks made ahead (made on the frame thread, NR-14d), "
                     "536870912 threaded untile of big textures (one thread, NR-14d), "
                     "1073741824 freeing retired render targets at frame end (kept, NR-14d2); "
                     "bits 2^31 and up: svr07_native_off2");
REXCVAR_DEFINE_INT32(svr07_native_off2, 0, "Native",
                     "Native renderer dev switch, more bits of svr07_native_off (set = the old path): "
                     "1 filling the half-pixel gap at the left/top of scaled colour resolves (the "
                     "first covered host column/row copied over the uncovered ones, NR-CE1), 2 the "
                     "one-pass constant copy (raw copy + byte swap as two passes, L14; same bytes), "
                     "4 the shared block's skip of an unchanged computed-fetch table clear (L14)");
REXCVAR_DEFINE_INT32(svr07_native_list_split_us, 2000, "Native",
                     "Native renderer (NR-14c): the frame's command list goes to the GPU, with no "
                     "wait, at the first draw this many microseconds after it was opened (at most 8 "
                     "times a frame), so a write-back's wait covers only what came since. 0 = off");
// RR: 3 since RR-NR-10's full-res A/B (nr10d s3matchgs/s3lockergs = bit 1, s1moviegs3 = bits 1+2 vs nr10b/nr10a/nr8e:
// same pictures; it skips guest-side D3D work only, gd3d there is mostly the vblank wait).
// SVR07-UNVERIFIED: default 3 is RR's A/B; on SvR07 the skip zeroes the masks at +0x00..+0x27 (UfcDropGuestFlush), so the guest commit sub_821E2760 / sub_821E1D88 (which is handed the stride table +0x2FF0) is skipped too | census: first native runs A/B guest_skip 0 vs 3 on the same scene (draw log + shots)
REXCVAR_DEFINE_INT32(svr07_native_guest_skip, 3, "Native",
                     "Native renderer: game D3D work to leave out because only the null GPU would "
                     "read it (NR-P6; 3 since RR-NR-10's A/B, 0 = the old path). Bits: 1 "
                     "DrawIndexedVertices state flush, 2 DrawVertices state flush (the video player's "
                     "quads)");
// L14: the same skip for BeginVertices (bit 4 of the effective mask). In the heavy entrance scenes
// (crowd/pyro, 120-200 fps at 240) 1300-1400 of ~3300 draws a frame are BeginVertices draws, and
// each still ran the game's full state flush into the null GPU's ring: gd3d 0.24 us per such draw
// against 0.05 for the indexed draws that already skip it (240e-fps7 regression over 238 s).
// -1 = auto: on while svr07_high_fps_hz is above 60 (stock 60 unchanged), 0 = off, 1 = on.
REXCVAR_DEFINE_INT32(svr07_native_guest_skip_up, -1, "Native",
                     "Native renderer (fps, L14): leave out the game's state flush in BeginVertices "
                     "(only the null GPU reads it). -1 = auto (on in the high-fps mode), 0 = off, "
                     "1 = on");
REXCVAR_DECLARE(double, svr07_high_fps_hz);
// L16 (reports/L16-fps-quad-batching.md): BeginVertices draws whose state, constants and shared
// block equal the draw before them are added to that draw's host draw call (held in the threaded
// command stream) instead of making their own. -1 = auto (on while svr07_high_fps_hz is above 60
// and threaded recording is on), 0 = off, 1 = on (still needs threaded recording).
REXCVAR_DEFINE_INT32(svr07_native_up_batch, -1, "Native",
                     "Native renderer (fps, L16): merge consecutive BeginVertices draws that share "
                     "all state and constants into one host draw. -1 = auto (on in the high-fps "
                     "mode with threaded recording), 0 = off, 1 = on");
REXCVAR_DEFINE_INT32(svr07_native_up_batch_room, 4096, "Native",
                     "Native renderer (fps, L16): bytes of upload space reserved after a batched "
                     "BeginVertices draw's vertices for the draws merged into it (while merges are "
                     "seen)");
// fps15: default 0. On in fps14 (desk-240l) it fired on ~85% of the indexed draws and cut draw+rs by
// ~0.5 ms, yet the entrance fell 223.9 -> 216 fps (guest time and the test itself grew more).
REXCVAR_DEFINE_INT32(svr07_native_idx_fast, 0, "Native",
                     "Native renderer (fps, L16 fps13): plain DrawIndexedVertices draws whose pipeline "
                     "inputs equal the held draw before them bind only constants and buffers and draw "
                     "(needs svr07_native_up_batch). -1 = auto (on in the high-fps mode), 0 = off "
                     "(default: slower in fps14), 1 = on");
REXCVAR_DEFINE_STRING(svr07_native_skip_vs, "", "Native",
                      "Native renderer dev switch: comma-separated vertex shader hashes (hex) whose "
                      "draws are skipped, for bisecting");
REXCVAR_DEFINE_STRING(svr07_native_probe_clear, "", "Native",
                      "Native renderer dev switch: guest VA (hex) of a resolve target that is filled "
                      "with 50% grey straight after each resolve into it, for bisecting");
REXCVAR_DEFINE_BOOL(svr_edge_blur, false, "SvR",
                    "SvR 2010's own edge-blur filter (the console's anti-aliasing): off keeps the image "
                    "sharp; the higher internal resolution already smooths edges");
#endif
#include "ufc_hook.h"
#include "ufc_resources.h"
#endif
#include "gpu/device.h"
#include "gpu/format.h"
#include "gpu/host_resource_heap.h"
#include "gpu/output.h"
#include "gpu/shaders/shader_cache.h"

namespace {

namespace xe = rex::graphics::xenos;

// Xenos primitive types the hooks handle themselves (D3D12 has no triangle fans).
constexpr u32 kPrimTriangleList = 4;
constexpr u32 kPrimTriangleFan = 5;
constexpr u32 kPrimRectList = 8;
constexpr u32 kPrimTriangleStrip = 6;
constexpr u32 kPrimQuadList = 13;

struct DrawArgs {
  bool indexed = false;
  bool is_up = false;
  u32 vertexOrIndexCount = 0;
  i32 baseVertexIndex = 0;
  u32 startIndex = 0;
  u32 startVertex = 0;
  // L16: hold the host draw call in the command stream so later BeginVertices draws can be added
  // to it (UpBatchDraw). Only set with threaded recording on.
  bool hold = false;
};

plume::RenderPrimitiveTopology MapPrimitiveType(u32 prim) {
  switch (static_cast<xe::PrimitiveType>(prim)) {
  case xe::PrimitiveType::kPointList:
    return plume::RenderPrimitiveTopology::POINT_LIST;
  case xe::PrimitiveType::kLineList:
    return plume::RenderPrimitiveTopology::LINE_LIST;
  case xe::PrimitiveType::kLineStrip:
    return plume::RenderPrimitiveTopology::LINE_STRIP;
  case xe::PrimitiveType::kTriangleList:
    return plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  case xe::PrimitiveType::kTriangleFan:
#if defined(SVR_NATIVE_RENDERER)
  {
    // The hooks turn fans into triangle lists (D3D12 has none and plume asserts). One that gets
    // here came through a path that doesn't yet: drawn as a list rather than ending the game.
    static std::atomic<u32> s_fan{0};
    if (s_fan.fetch_add(1, std::memory_order_relaxed) < 4)
      BD_WARN("MapPrimitiveType: triangle fan reached the host unconverted, drawn as a list");
    return plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  }
#else
    return plume::RenderPrimitiveTopology::TRIANGLE_FAN;
#endif
  case xe::PrimitiveType::kTriangleStrip:
    return plume::RenderPrimitiveTopology::TRIANGLE_STRIP;
  case xe::PrimitiveType::kQuadList:
    return plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  default: {
    // Unmapped X360 primitive type (e.g. kRectangleList). Drawing as a
    // triangle list is almost certainly wrong, so warn rather than fail
    // silently.
    static std::atomic<u32> s_warn{0};
    if (s_warn.fetch_add(1, std::memory_order_relaxed) < 4) {
      BD_WARN("MapPrimitiveType: unmapped X360 primitive type {} drawn as "
              "TRIANGLE_LIST - rect/unknown topology not implemented",
              prim);
    }
    return plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  }
  }
}

void DispatchDraw(u32 device_guest, u32 primitive_type, const char *name,
                  const DrawArgs &args = {}) {
#if defined(REXGLUE_ENABLE_PROFILING)
  // Per-draw pass/shader attribution, only formatted while a profiler is
  // connected (TRACY_ON_DEMAND).
  char zone_name[64] = "Draw";
  if (BD_PROFILER_CONNECTED()) {
    const auto *ps = bd::gpu::state().pixel_shader;
    const u64 ps_hash =
        (ps && ps->shaderCacheEntry) ? ps->shaderCacheEntry->hash : 0;
    std::snprintf(zone_name, sizeof(zone_name), "Draw pass=%u ps=%016llX",
                  bd::gpu::CurrentRenderPassId(),
                  static_cast<unsigned long long>(ps_hash));
  }
  // CPU zone only: per-draw GPU timestamps serialize the GPU and poison every
  // GPU number in the capture. Coarse GPU cost comes from the per-frame zones.
  BD_CPU_ZONE_DYN(zone_name);
#endif
#if defined(SVR_NATIVE_RENDERER)
  // NR-12a: a bound resolve target whose fill from guest memory is still owed gets it before the
  // draw that samples it (state().mutex isn't held yet: the fill takes it).
  bd::gpu::SvrFillSampledTargets(bd::gpu::state().textures, bd::gpu::kTextureSlots);
  {
    const auto hash_of = [](const auto *shader) -> u64 {
      return shader && shader->shaderCacheEntry ? shader->shaderCacheEntry->hash : 0u;
    };
    if (UfcCbRecordedDraw(device_guest, name, primitive_type, args.vertexOrIndexCount,
                          args.indexed ? args.startIndex : args.startVertex, args.baseVertexIndex,
                          hash_of(bd::gpu::state().vertex_shader),
                          hash_of(bd::gpu::state().pixel_shader)))
      return;
  }
#endif
  // One lock across the whole recording sequence: loader threads record texture
  // uploads and Present records under the same mutex, and the per-frame command
  // list they all write is single-producer.
#if defined(SVR_NATIVE_RENDERER)
  bd::gpu::SvrDiagTimer diag_draw(bd::gpu::SvrDiag::kDraw);
  bd::gpu::SvrZone zone_draw(bd::gpu::SvrZoneId::kDraw);
#endif
  auto &s = bd::gpu::state();
  std::unique_lock lock(s.mutex, std::try_to_lock);
  if (!lock.owns_lock()) {
    bd::gpu::SvrPerfCount(bd::gpu::SvrCount::kLockMisses, 1);
    lock.lock();
  }
  bd::gpu::Video::OpenCommandListLocked();
#if defined(SVR_NATIVE_RENDERER)
  // NR-14c: between draws, so this draw re-issues its state into the fresh list.
  if (!(REXCVAR_GET(svr07_native_off) & 8388608))
    bd::gpu::SvrSplitListIfDueLocked(s, REXCVAR_GET(svr07_native_list_split_us));
#endif

#if defined(SVR_NATIVE_RENDERER)
  // Every way out of this function counts as a drawn or a dropped draw (SvrOnFrame's log line).
  bool drawn = false;
  struct NoteDraw {
    bool &drawn;
    // Rectangle lists drawn through a path that doesn't rebuild them yet (only BeginVertices
    // does): say which, so the next run's log names the path to add.
    NoteDraw(bool &d, u32 prim, const char *call, const DrawArgs &a) : drawn(d) {
      if (prim == kPrimRectList) {
        static std::atomic<u32> s_rects{0};
        if (s_rects.fetch_add(1, std::memory_order_relaxed) < 8)
          BD_WARN("native: rectangle list through {} (count {}, {}) is not rebuilt: half of each "
                  "rectangle is missing",
                  call, a.vertexOrIndexCount, a.indexed ? "indexed" : "not indexed");
      }
    }
    ~NoteDraw() {
      bd::gpu::SvrNoteDraw(drawn);
      if (!drawn && bd::gpu::SvrDrawLogActive())
        BD_INFO("[drawlog]   ^ DROPPED (no framebuffer, shader, declaration or pipeline)");
    }
  } note_draw{drawn, primitive_type, name, args};
  bd::gpu::SvrDrawLogDraw(device_guest, name, primitive_type, args.indexed,
                          args.vertexOrIndexCount,
                          args.indexed ? args.startIndex : args.startVertex,
                          args.baseVertexIndex);
  // Dev bisection switch: SVR_SKIP_DRAWS bitmask, 1 indexed, 2 DrawVertices, 4 BeginVertices (UP),
  // 8 any draw into a render target other than R8G8B8A8, 16 indexed with DHEN3N normals (lit
  // meshes), 32 indexed without, 64 unlit indexed skinned (real BLENDINDICES), 128 unlit indexed
  // static.
  {
    static const u32 skip_mask = [] {
      const char *env = std::getenv("SVR_SKIP_DRAWS");
      return env ? u32(std::strtoul(env, nullptr, 0)) : 0u;
    }();
    if (skip_mask) {
      const bool up = args.is_up;
      const auto *decl = s.pipelineState.vertexDeclaration;
      const bool lit = decl && decl->hasDhen3nNormal;
      bool skinned = false;
      if (decl) {
        for (u32 i = 0; i < decl->inputElementCount; ++i) {
          const auto &e = decl->inputElements[i];
          if (e.slotIndex != 15 && e.semanticName &&
              std::strcmp(e.semanticName, "BLENDINDICES") == 0)
            skinned = true;
        }
      }
      if (((skip_mask & 1) && args.indexed) || ((skip_mask & 2) && !args.indexed && !up) ||
          ((skip_mask & 4) && up) ||
          ((skip_mask & 8) && s.pipelineState.renderTargetFormat !=
                                  plume::RenderFormat::R8G8B8A8_UNORM) ||
          ((skip_mask & 16) && args.indexed && lit) ||
          ((skip_mask & 32) && args.indexed && !lit) ||
          ((skip_mask & 64) && args.indexed && !lit && skinned) ||
          ((skip_mask & 128) && args.indexed && !lit && !skinned))
        return;
    }
  }
  // Dev bisection switch: svr07_native_skip_vs, the draws of these vertex shaders.
  {
    static const std::vector<u64> skip_vs = [] {
      std::vector<u64> hashes;
      const std::string list = REXCVAR_GET(svr07_native_skip_vs);
      for (const char *p = list.c_str(); *p;) {
        char *end = nullptr;
        const u64 hash = std::strtoull(p, &end, 16);
        if (end == p)
          break;
        hashes.push_back(hash);
        BD_INFO("native: skipping draws of vs {:016X}", hash);
        p = *end == ',' ? end + 1 : end;
      }
      return hashes;
    }();
    if (!skip_vs.empty() && s.vertex_shader && s.vertex_shader->shaderCacheEntry &&
        std::find(skip_vs.begin(), skip_vs.end(), s.vertex_shader->shaderCacheEntry->hash) !=
            skip_vs.end())
      return;
  }
  // SvR 2010's edge-blur post pass (pixel shader 0x73413A409D5E296B): a 3x3 Sobel edge test that
  // replaces every edge pixel with the average of its 9 neighbours. On the console that softened
  // jaggies at 720p; here its 1-pixel 720p offsets are 2+ pixels at the internal resolution, so it
  // blurs hair, tattoos, ropes and the crowd while the supersampling already smooths edges. Skipping
  // the draw leaves the scene as rendered (the pass only rewrites the scene with a blurred copy).
  if (!REXCVAR_GET(svr_edge_blur) && s.pixel_shader && s.pixel_shader->shaderCacheEntry &&
      s.pixel_shader->shaderCacheEntry->hash == 0x73413A409D5E296Bull)
    return;
  // Vulkan has no "no index buffer" state: an indexed draw with none bound, or reaching past the
  // end of the bound one, can hang the GPU. Such a draw is skipped (and counted in the log).
  if (args.indexed && primitive_type != 13) {
    const u32 index_size = s.index_view.format == plume::RenderFormat::R32_UINT ? 4u : 2u;
    const u64 end = (u64(args.startIndex) + args.vertexOrIndexCount) * index_size;
    if (s.index_view.buffer.ref == nullptr || end > s.index_view.size) {
      static std::atomic<u32> s_skipped{0};
      const u32 k = s_skipped.fetch_add(1, std::memory_order_relaxed);
      if (k < 16 || (k & (k - 1)) == 0)
        BD_WARN("[draw] SvR: skipped indexed draw #{} ({}): {} count={} startI={} ib size={}", k,
                s.index_view.buffer.ref ? "past end of index buffer" : "no index buffer", name,
                args.vertexOrIndexCount, args.startIndex, s.index_view.size);
      return;
    }
    const auto hash_of = [](const auto *shader) -> u64 {
      return shader && shader->shaderCacheEntry ? shader->shaderCacheEntry->hash : 0u;
    };
    bd::gpu::SvrNoteWordRoundedIndices(device_guest, primitive_type, args.startIndex,
                                       args.vertexOrIndexCount, hash_of(s.vertex_shader),
                                       hash_of(s.pixel_shader));
  }
#endif

#if defined(SVR_NATIVE_RENDERER)
  // SvR's SetViewport isn't hooked: the viewport the game's draw uses is the one its D3D keeps in
  // the device (SvR 2008: floats at +0x3160; SvR07: integer X/Y/W/H + float MinZ/MaxZ at +0x3068,
  // W/H already clamped to the target by SetViewport sub_821D7768).
  if (const auto *dev = bd::mem::at<const bd::gpu::D3DDevice>(device_guest)) {
    const auto &vp = dev->viewport;
    // In host pixels of the bound target. By edges, so an odd size (45 rows at 1.5x) still reaches
    // the target's last row.
    const bd::gpu::GuestTexture *target = s.render_target ? s.render_target : s.depth_stencil;
    const float left = bd::gpu::SvrScaleEdge(float(vp.X), target);
    const float top = bd::gpu::SvrScaleEdge(float(vp.Y), target);
    bd::gpu::Video::SetDirtyValue<float>(s.dirtyStates.viewport, s.viewport.x, left);
    bd::gpu::Video::SetDirtyValue<float>(s.dirtyStates.viewport, s.viewport.y, top);
    bd::gpu::Video::SetDirtyValue<float>(
        s.dirtyStates.viewport, s.viewport.width,
        bd::gpu::SvrScaleEdge(float(vp.X) + float(vp.Width), target) - left);
    bd::gpu::Video::SetDirtyValue<float>(
        s.dirtyStates.viewport, s.viewport.height,
        bd::gpu::SvrScaleEdge(float(vp.Y) + float(vp.Height), target) - top);
    bd::gpu::Video::SetDirtyValue<float>(s.dirtyStates.viewport, s.viewport.minDepth,
                                         float(vp.MinZ));
    bd::gpu::Video::SetDirtyValue<float>(s.dirtyStates.viewport, s.viewport.maxDepth,
                                         float(vp.MaxZ));
    s.dirtyStates.scissorRect = s.dirtyStates.scissorRect || s.dirtyStates.viewport;
  }
#endif

  // PSO key includes topology, so set it before any flush.
  bd::gpu::Video::SetDirtyValue<plume::RenderPrimitiveTopology>(
      s.dirtyStates.pipelineState, s.pipelineState.primitiveTopology,
      MapPrimitiveType(primitive_type));

  if (!bd::gpu::Video::BindDrawFramebufferLocked()) {
    return;
  }
#if defined(SVR_NATIVE_RENDERER)
  s.vertex_id_base = args.indexed ? args.baseVertexIndex : static_cast<i32>(args.startVertex);
#endif
  if (!bd::gpu::Video::FlushRenderStateLocked(device_guest)) {
    return; // FlushRenderState logs its own reason
  }

  auto *cmd_list = s.command_list;
  if (!cmd_list)
    return;

  bd::gpu::SvrZone zone_call(bd::gpu::SvrZoneId::kDrawCall);
  if (args.is_up)
    bd::gpu::SvrPerfCount(bd::gpu::SvrCount::kUpDraws, 1);
  if (primitive_type == 13) {
    u32 quads = args.vertexOrIndexCount / 4;
    const u32 max_quads = bd::gpu::Video::QuadlistMaxQuads();
    if (quads > max_quads)
      quads = max_quads;
    const auto *ib = bd::gpu::Video::QuadlistExpansionIBView();
    if (quads && ib) {
      const i32 base = args.indexed ? args.baseVertexIndex
                                    : static_cast<i32>(args.startVertex);
      cmd_list->setIndexBuffer(ib);
#if defined(SVR_NATIVE_RENDERER)
      if (args.hold)
        bd::gpu::SvrStreamSetHoldNextDraw(true);
#endif
      cmd_list->drawIndexedInstanced(quads * 6, 1, 0, base, 0);
#if defined(SVR_NATIVE_RENDERER)
      if (args.hold)
        bd::gpu::SvrStreamSetHoldNextDraw(false);
#endif
      // The expansion IB replaced the tracked guest IB on the command list, and
      // without re-dirtying the next indexed draw reads quad pattern indices.
      s.dirtyStates.indices = true;
    }
  } else if (args.indexed) {
    // No IB bound on an indexed draw: FlushRenderState skipped setIndexBuffer
    // and D3D12 fires EXECUTION WARNING #211, every index reads as 0,
    // geometry collapses, scene goes black.
    if (s.index_view.buffer.ref == nullptr) {
      static std::atomic<u32> s_no_ib{0};
      const u32 k = s_no_ib.fetch_add(1, std::memory_order_relaxed);
      if (k < 16) {
        BD_WARN("[draw] indexed draw with no IB bound: #{} {} count={} "
                "startI={} baseV={}",
                k, name, args.vertexOrIndexCount, args.startIndex,
                args.baseVertexIndex);
      }
    }
#if defined(SVR_NATIVE_RENDERER)
    if (args.hold)
      bd::gpu::SvrStreamSetHoldNextDraw(true);
#endif
    cmd_list->drawIndexedInstanced(args.vertexOrIndexCount, 1, args.startIndex,
                                   args.baseVertexIndex, 0);
#if defined(SVR_NATIVE_RENDERER)
    if (args.hold)
      bd::gpu::SvrStreamSetHoldNextDraw(false);
#endif
  } else {
#if defined(SVR_NATIVE_RENDERER)
    if (args.hold)
      bd::gpu::SvrStreamSetHoldNextDraw(true);
#endif
    cmd_list->drawInstanced(args.vertexOrIndexCount, 1, args.startVertex, 0);
#if defined(SVR_NATIVE_RENDERER)
    if (args.hold)
      bd::gpu::SvrStreamSetHoldNextDraw(false);
#endif
  }
#if defined(SVR_NATIVE_RENDERER)
  drawn = true;
#endif
}

// bdBuildQuadVertices assembles every glyph into this one buffer, and unlike
// bdPrim it divides each position by the 2D basis on the way in, so text
// arrives in the unit square while sprites arrive in canvas pixels.
constexpr u32 kTextQuadBatchEA = 0x82DBECF0;

// Which edges of a quad's UV rect the inset is allowed to move.
enum class UVEdges {
  All,      // every edge, for a batch whose cells are known to abut
  CellSeam, // only edges sitting on an interior texel boundary
};

void FitDesignCanvasVertices(u8 *verts, u32 primitiveType, u32 vertexCount,
                             u32 vertexStride, bool normalized);
void InsetQuadUVs(u8 *verts, u32 vertexCount, u32 vertexStride, UVEdges edges);
bool IsScreenSpriteQuad(u32 primitiveType, u32 vertexCount, u32 vertexStride);

#if defined(SVR_NATIVE_RENDERER)
// float -> IEEE half, round to nearest even (integers up to 2048 are exact: UFC's text positions
// and texel coordinates stay below that).
u16 FloatToHalf(float value) {
  u32 f;
  std::memcpy(&f, &value, 4);
  const u32 sign = (f >> 16) & 0x8000u;
  const i32 exponent = i32((f >> 23) & 0xFFu) - 127 + 15;
  u32 mantissa = f & 0x7FFFFFu;
  if (exponent >= 31)
    return u16(sign | 0x7C00u);  // too large (or inf/NaN): infinity
  if (exponent <= 0) {
    if (exponent < -10)
      return u16(sign);
    mantissa |= 0x800000u;
    const u32 shift = u32(14 - exponent);
    u32 half = mantissa >> shift;
    if ((mantissa >> (shift - 1)) & 1u)
      ++half;
    return u16(sign | half);
  }
  u32 half = sign | (u32(exponent) << 10) | (mantissa >> 13);
  if (mantissa & 0x1000u)
    ++half;  // may carry into the exponent, which is the right result
  return u16(half);
}

// The current declaration's integer elements (GuestVertexDeclaration::cpuConverts), rewritten in
// `verts` (already byte-swapped per 32-bit word, host order) so the host's float formats read them.
void ConvertIntegerElements(const bd::gpu::GuestVertexDeclaration *decl, u8 *verts, u32 vertexCount,
                            u32 vertexStride) {
  for (u32 n = 0; n < decl->cpuConvertCount; ++n) {
    const auto &c = decl->cpuConverts[n];
    const u32 bytes = c.kind == 1 ? 4u : u32(c.count) * 2u;
    if (c.stream != 0 || u32(c.offset) + bytes > vertexStride)
      continue;
    for (u32 v = 0; v < vertexCount; ++v) {
      u8 *p = verts + size_t(v) * vertexStride + c.offset;
      if (c.kind == 1) {
        i32 value;
        std::memcpy(&value, p, 4);
        const float as_float = float(value);
        std::memcpy(p, &as_float, 4);
      } else {
        for (u32 k = 0; k < c.count; ++k) {
          u16 value;
          std::memcpy(&value, p + k * 2u, 2);
          const u16 half = FloatToHalf(float(value));
          std::memcpy(p + k * 2u, &half, 2);
        }
      }
    }
  }
}
#endif

#if defined(SVR_NATIVE_RENDERER)
// Rectangle lists (Xenos kRectangleList, no host has them): three vertices give a rectangle. The
// GPU makes the fourth corner, opposite the right-angle vertex, as the sum of the other two minus
// that one, for every value the vertex shader passes on. Here that is done on the vertex data
// itself, which is the same thing for the screen-space quads games draw this way (positions and
// texture coordinates the shader only scales and offsets). `verts` = count vertices of `stride`
// bytes in host order; the result is two triangles per rectangle (6 vertices), same winding.
// Float elements of stream 0 are combined; anything else in the new vertex is the corner's.
std::vector<u8> ExpandRectList(const bd::gpu::GuestVertexDeclaration *decl, const u8 *verts,
                               u32 count, u32 stride) {
  struct FloatRun {
    u32 offset;
    u32 floats;
  };
  FloatRun runs[16];
  u32 run_count = 0;
  u32 pos_off = 0;
  if (decl) {
    for (u32 i = 0; i < decl->inputElementCount && run_count < 16; ++i) {
      const auto &e = decl->inputElements[i];
      if (e.slotIndex != 0)
        continue;
      u32 floats = 0;
      switch (e.format) {
      case plume::RenderFormat::R32_FLOAT: floats = 1; break;
      case plume::RenderFormat::R32G32_FLOAT: floats = 2; break;
      case plume::RenderFormat::R32G32B32_FLOAT: floats = 3; break;
      case plume::RenderFormat::R32G32B32A32_FLOAT: floats = 4; break;
      default: break;
      }
      if (!floats || e.alignedByteOffset + floats * 4u > stride)
        continue;
      runs[run_count++] = {e.alignedByteOffset, floats};
      if (e.semanticName && std::strcmp(e.semanticName, "POSITION") == 0 && e.semanticIndex == 0)
        pos_off = e.alignedByteOffset;
    }
  }
  if (!run_count && stride >= 8)
    runs[run_count++] = {0, 2};
  const u32 rects = count / 3u;
  std::vector<u8> out(size_t(rects) * 6u * stride);
  for (u32 r = 0; r < rects; ++r) {
    const u8 *v[3] = {verts + size_t(r * 3u) * stride, verts + size_t(r * 3u + 1u) * stride,
                      verts + size_t(r * 3u + 2u) * stride};
    float p[3][2] = {};
    if (pos_off + 8u <= stride) {
      for (u32 k = 0; k < 3; ++k)
        std::memcpy(p[k], v[k] + pos_off, 8);
    }
    const auto dist2 = [&](u32 a, u32 b) {
      const float dx = p[a][0] - p[b][0], dy = p[a][1] - p[b][1];
      return dx * dx + dy * dy;
    };
    // The right-angle corner faces the longest edge.
    const float d01 = dist2(0, 1), d12 = dist2(1, 2), d20 = dist2(2, 0);
    const u32 corner = (d01 >= d12 && d01 >= d20) ? 2u : (d12 >= d20 ? 0u : 1u);
    const u32 a = (corner + 1u) % 3u, b = (corner + 2u) % 3u;
    std::vector<u8> fourth(v[corner], v[corner] + stride);
    for (u32 n = 0; n < run_count; ++n) {
      for (u32 f = 0; f < runs[n].floats; ++f) {
        const u32 off = runs[n].offset + f * 4u;
        float fa, fb, fc;
        std::memcpy(&fa, v[a] + off, 4);
        std::memcpy(&fb, v[b] + off, 4);
        std::memcpy(&fc, v[corner] + off, 4);
        const float value = fa + fb - fc;
        std::memcpy(fourth.data() + off, &value, 4);
      }
    }
    // Second triangle with the first one's winding (see the corner cases in FINDINGS NR-8b).
    const u8 *second[3];
    if (corner == 1u) {
      second[0] = v[0]; second[1] = v[2]; second[2] = fourth.data();
    } else if (corner == 0u) {
      second[0] = v[1]; second[1] = fourth.data(); second[2] = v[2];
    } else {
      second[0] = v[0]; second[1] = fourth.data(); second[2] = v[1];
    }
    u8 *o = out.data() + size_t(r) * 6u * stride;
    for (u32 k = 0; k < 3; ++k)
      std::memcpy(o + size_t(k) * stride, v[k], stride);
    for (u32 k = 0; k < 3; ++k)
      std::memcpy(o + size_t(3u + k) * stride, second[k], stride);
  }
  return out;
}
#endif

#if defined(SVR_NATIVE_RENDERER)
// Integer elements in a vertex buffer: the host copy holds the raw integers, which the float input
// would read as denormals (UFC's flare sprites pick a channel by exact compare with a vertex's 1, 2
// or 3, so every channel landed in 0). Stream 0 is converted into the upload ring per draw, which
// is what UploadAndBindUpVertices does for UP data. Call after SvrBindDrawGeometry.
void BindConvertedStream0(u32 device_guest) {
  constexpr u32 kMaxConvertBytes = 1u << 20;
  if (REXCVAR_GET(svr07_native_off) & 128)
    return;
  const auto *device = bd::mem::at<const bd::gpu::D3DDevice>(device_guest);
  const auto *decl =
      device ? bd::gpu::SvrLookupVertexDeclaration(u32(device->vertexDeclaration)) : nullptr;
  if (!decl || !decl->cpuConvertCount)
    return;
  std::vector<u8> verts;
  u32 stride = 0;
  const bool read = bd::gpu::SvrReadWholeStream0(device_guest, kMaxConvertBytes, verts, stride);
  static std::mutex s_mutex;
  static std::set<const void *> s_logged;
  {
    std::lock_guard lock(s_mutex);
    if (s_logged.insert(decl).second)
      BD_INFO("native: buffer draw with integer elements: decl 0x{:X} stride {} bytes {}{}",
              decl->hash, stride, verts.size(), read ? "" : " NOT converted (unreadable or too big)");
  }
  if (!read)
    return;
  ConvertIntegerElements(decl, verts.data(), u32(verts.size()) / stride, stride);
  auto upload = bd::gpu::UploadHostBytes(verts.data(), u32(verts.size()), /*alignment=*/4);
  if (upload.memory)
    bd::gpu::Video::SetVertexStream(0, upload.ref, upload.size, stride);
}

void DispatchDraw(u32 device_guest, u32 primitive_type, const char *name, const DrawArgs &args);

// A rectangle list whose vertices live in a vertex buffer (DrawVertices, DrawIndexedVertices):
// `verts` = its vertices in draw order, host byte order. Rebuilt as triangles and drawn from the
// upload ring on stream 0. Declarations that read other streams too are not handled (none seen).
bool DrawRebuiltRectList(u32 device_guest, std::vector<u8> &verts, u32 count, u32 stride,
                         const char *name) {
  if (REXCVAR_GET(svr07_native_off) & 4)
    return false;
  const auto *decl = bd::gpu::state().vertex_declaration;
  if (decl) {
    for (u32 n = 1; n < 15; ++n) {
      if (decl->vertexStreams[n])
        return false;
    }
    if (decl->cpuConvertCount)
      ConvertIntegerElements(decl, verts.data(), count, stride);
  }
  const std::vector<u8> tris = ExpandRectList(decl, verts.data(), count, stride);
  if (tris.empty())
    return false;
  auto upload = bd::gpu::UploadHostBytes(tris.data(), u32(tris.size()), /*alignment=*/4);
  if (!upload.memory)
    return false;
  bd::gpu::Video::SetVertexStream(0, upload.ref, upload.size, stride);
  DrawArgs args{};
  args.is_up = true;
  args.vertexOrIndexCount = (count / 3u) * 6u;
  DispatchDraw(device_guest, kPrimTriangleList, name, args);
  return true;
}
#endif

// vertexCount is updated when the data is rebuilt (rectangle lists become triangle lists).
bool UploadAndBindUpVertices(u32 primitiveType, u32 pVertexData,
                             u32 &vertexCount, u32 vertexStride) {
  if (!pVertexData || !vertexCount || !vertexStride)
    return false;
  bd::gpu::SvrZone zone_up(bd::gpu::SvrZoneId::kUpVerts);
  const u32 totalSize = vertexCount * vertexStride;
  bd::gpu::SvrPerfCount(bd::gpu::SvrCount::kUpBytes, totalSize);
#if defined(SVR_NATIVE_RENDERER)
  // UFC's text quads carry integer elements, and rectangle lists need a fourth vertex: both work
  // on a swapped copy, which is then uploaded.
  const auto *decl = bd::gpu::state().vertex_declaration;
  const bool rect_list = primitiveType == kPrimRectList && vertexCount >= 3 &&
                         !(REXCVAR_GET(svr07_native_off) & 4);
  if ((decl && decl->cpuConvertCount) || rect_list) {
    const auto *src = bd::mem::try_at<const u8>(pVertexData);
    if (!src)
      return false;
    std::vector<u8> swapped((totalSize + 3u) & ~3u);
    rex::memory::copy_and_swap_32_unaligned(swapped.data(), src, swapped.size() / 4);
    if (decl && decl->cpuConvertCount)
      ConvertIntegerElements(decl, swapped.data(), vertexCount, vertexStride);
    u32 upload_size = totalSize;
    if (rect_list) {
      swapped = ExpandRectList(decl, swapped.data(), vertexCount, vertexStride);
      vertexCount = (vertexCount / 3u) * 6u;
      upload_size = u32(swapped.size());
    }
    auto converted = bd::gpu::UploadHostBytes(swapped.data(), upload_size, /*alignment=*/4);
    if (!converted.memory)
      return false;
    bd::gpu::Video::SetVertexStream(0, converted.ref, converted.size, vertexStride);
    return true;
  }
#endif
  auto alloc = bd::gpu::UploadGuestBytesByteSwap32(pVertexData, totalSize,
                                                   /*alignment=*/4);
  if (!alloc.memory)
    return false;
#if !defined(SVR_NATIVE_RENDERER)
  // Blue Dragon's 2D canvas fit-ups (its text batch address, sprite UV insets); SvR's UI vertices
  // are drawn as the game wrote them.
  const bool text_batch = pVertexData == kTextQuadBatchEA;
  FitDesignCanvasVertices(alloc.memory, primitiveType, vertexCount,
                          vertexStride, text_batch);
  if (text_batch)
    InsetQuadUVs(alloc.memory, vertexCount, vertexStride, UVEdges::All);
  else if (IsScreenSpriteQuad(primitiveType, vertexCount, vertexStride))
    InsetQuadUVs(alloc.memory, vertexCount, vertexStride, UVEdges::CellSeam);
#endif
  bd::gpu::Video::SetVertexStream(0, alloc.ref, alloc.size, vertexStride);
  return true;
}

// Zooming an event sprite about an off-canvas pivot retreats the authored art
// inside the frame and lets the power-of-two padding take the edge, which X360
// TV overscan hid. Pull the sampled UV back until the art edge meets the
// frame edge. Sibling of bdMotionBlurQuadInsetHook in gpu/hooks/tweaks.cpp.
constexpr u32 kQuadVertices = 4;

// An overshoot an artist could hide had to fit inside title-safe overscan. Past
// that, the art fills its canvas and the UV means what it says.
constexpr float kOverscanFraction = 0.05f;

// bdPrimPushVertex2D's layout. bdDrawRectPrimitive shares the stride but puts
// the color where u sits here, and submits nothing but QUADLIST or LINESTRIP,
// so the primitive type is what tells the two apart.
struct ScreenSpriteVertex {
  be_f32 x;
  be_f32 y;
  be_f32 u;
  be_f32 v;
  be_u32 color;
};
static_assert(sizeof(ScreenSpriteVertex) == 0x14);

// Stride tells the prim system's three vertex layouts apart. The two 2D ones
// lead with a canvas position, the 3D one with a world position.
constexpr u32 kPrimVertex2DDualTexStride = 0x1C;

bool Is2DPrimStride(u32 stride) {
  return stride == sizeof(ScreenSpriteVertex) ||
         stride == kPrimVertex2DDualTexStride;
}

// bdPrimPushVertex2D writes x,y,u,v,color and always submits a strip, while
// bdDrawRectPrimitive shares the stride but puts the color where u sits and
// submits QUADLIST or LINESTRIP. Matching all three tells the two apart.
bool IsScreenSpriteQuad(u32 primitiveType, u32 vertexCount, u32 vertexStride) {
  return static_cast<xe::PrimitiveType>(primitiveType) ==
             xe::PrimitiveType::kTriangleStrip &&
         vertexCount == kQuadVertices &&
         vertexStride == sizeof(ScreenSpriteVertex);
}

// uv runs linearly from uv_lo at pos_lo to uv_hi at pos_hi. Yields the uv_hi
// that puts art_uv on the frame edge, or nothing when the quad already keeps
// the padding off screen.
std::optional<float> InsetSampledSpan(float pos_lo, float pos_hi, float uv_lo,
                                      float uv_hi, float frame_extent,
                                      float art_uv) {
  const float span = pos_hi - pos_lo;
  const float reach = frame_extent - pos_lo;
  if (span <= 0.0f || uv_hi <= uv_lo || uv_lo >= art_uv)
    return std::nullopt;
  // Only a quad covering the whole frame can put padding on screen.
  if (pos_lo > 0.0f || pos_hi < frame_extent)
    return std::nullopt;
  const float at_edge = uv_lo + reach * (uv_hi - uv_lo) / span;
  if (at_edge <= art_uv || at_edge > art_uv * (1.0f + kOverscanFraction))
    return std::nullopt;
  return uv_lo + (art_uv - uv_lo) * span / reach;
}

void InsetOverscanScreenSprite(u32 pVertexData, u32 primitiveType,
                               u32 vertexCount, u32 vertexStride) {
  if (!IsScreenSpriteQuad(primitiveType, vertexCount, vertexStride))
    return;

  const auto *tex = bd::gpu::state().textures[0];
  if (!tex)
    return;
  const auto tex_w = static_cast<float>(tex->width);
  const auto tex_h = static_cast<float>(tex->height);
  if (tex_w <= bd::gpu::kDesignCanvasWidth ||
      tex_h <= bd::gpu::kDesignCanvasHeight)
    return;

  auto *v = bd::mem::at<ScreenSpriteVertex>(pVertexData);
  if (!v)
    return;

  // Strip order is top-left, bottom-left, top-right, bottom-right, so u is
  // shared down each column and v across each row.
  if (auto inset_u = InsetSampledSpan(v[0].x, v[2].x, v[0].u, v[2].u,
                                      bd::gpu::kDesignCanvasWidth,
                                      bd::gpu::kDesignCanvasWidth / tex_w)) {
    v[2].u = *inset_u;
    v[3].u = *inset_u;
  }
  if (auto inset_v = InsetSampledSpan(v[0].y, v[1].y, v[0].v, v[1].v,
                                      bd::gpu::kDesignCanvasHeight,
                                      bd::gpu::kDesignCanvasHeight / tex_h)) {
    v[1].v = *inset_v;
    v[3].v = *inset_v;
  }
}

// Guest UV rects land on exact texel boundaries, so a fraction of a texel of
// slack is float noise rather than an authored position.
constexpr float kTexelBoundaryTolerance = 0.01f;

// An atlas packs its cells flush against each other, so a UV edge on a texel
// boundary with texture on both sides is a cell seam. An edge at 0 or 1 has no
// neighbor to reach across, and CLAMP already returns the border texel there.
bool OnCellSeam(float uv, float extent) {
  if (uv <= 0.0f || uv >= 1.0f)
    return false;
  const float texel = uv * extent;
  return std::abs(texel - std::round(texel)) < kTexelBoundaryTolerance;
}

// At any scale but 1:1 a bilinear tap at a quad edge reaches past it, so an
// edge sitting on a cell seam returns the neighbor cell: the next glyph's ink,
// the orange copy of a gauge behind the gray one, the far half of a nine slice.
// Pull the edge half a source texel inward, which keeps every tap inside its
// own cell at any scale and still puts the 1:1 case on texel centers.
//
// UVEdges::All moves both edges whether or not they measure onto a boundary,
// for the glyph batch, whose cells are known to abut. CellSeam moves only the
// edges that do. That is what leaves a quad spanning a whole texture, the frame
// composite above all, sampling its own texel grid untouched.
//
// Applied to the uploaded copy, after the byte swap, so guest memory is
// untouched.
void InsetQuadUVs(u8 *verts, u32 vertexCount, u32 vertexStride, UVEdges edges) {
  if (vertexStride != sizeof(ScreenSpriteVertex))
    return;
  const auto *tex = bd::gpu::state().textures[0];
  if (!tex || !tex->width || !tex->height)
    return;
  const float extent[2] = {static_cast<float>(tex->width),
                           static_cast<float>(tex->height)};

  for (u32 base = 0; base + kQuadVertices <= vertexCount;
       base += kQuadVertices) {
    auto uv = [verts, vertexStride, base](u32 i) {
      return reinterpret_cast<float *>(verts +
                                       (base + i) * size_t{vertexStride}) +
             2;
    };
    for (u32 axis = 0; axis < 2; ++axis) {
      const float inset = 0.5f / extent[axis];
      float lo = uv(0)[axis], hi = lo;
      for (u32 i = 1; i < kQuadVertices; ++i) {
        lo = std::min(lo, uv(i)[axis]);
        hi = std::max(hi, uv(i)[axis]);
      }
      if (hi - lo <= 2.0f * inset)
        continue;
      const bool move_lo =
          edges == UVEdges::All || OnCellSeam(lo, extent[axis]);
      const bool move_hi =
          edges == UVEdges::All || OnCellSeam(hi, extent[axis]);
      // bdBuildQuadVertices copies the rect's two u and two v values verbatim
      // into the corners, so comparing against the extremes recovers which
      // corner each vertex is regardless of winding or mirroring.
      for (u32 i = 0; i < kQuadVertices; ++i) {
        float &c = uv(i)[axis];
        if (move_lo && c == lo)
          c = lo + inset;
        else if (move_hi && c == hi)
          c = hi - inset;
      }
    }
  }
}

// Authored extents this far apart still count as the same canvas edge.
constexpr float kCanvasEdgeTolerance = 8.0f;

bool IsOnePrimitive(u32 primitiveType, u32 vertexCount) {
  switch (static_cast<xe::PrimitiveType>(primitiveType)) {
  case xe::PrimitiveType::kTriangleStrip:
  case xe::PrimitiveType::kTriangleFan:
  case xe::PrimitiveType::kLineStrip:
    return true;
  default:
    return vertexCount <= 4;
  }
}

// Scales one 2D draw about the canvas center. It has to be the geometry rather
// than a shrunk viewport: the rasterizer clips to the NDC box before the
// viewport transform, so a backdrop reaching for the surface edge would be cut
// off at exactly the rect it was trying to escape.
//
// Applied to the uploaded copy, so guest memory is untouched. 'normalized' says
// the draw arrived already divided by the 2D basis, as the text batch is.
void FitDesignCanvasVertices(u8 *verts, u32 primitiveType, u32 vertexCount,
                             u32 vertexStride, bool normalized) {
  if (!verts || !bd::gpu::Video::DesignCanvasDrain() ||
      !Is2DPrimStride(vertexStride) || !vertexCount)
    return;
  const float kx = bd::gpu::Output::DesignScaleX();
  const float ky = bd::gpu::Output::DesignScaleY();
  if (kx == 1.0f && ky == 1.0f)
    return;

  const float canvas_x = normalized ? 1.0f : bd::gpu::kDesignCanvasWidth;
  const float canvas_y = normalized ? 1.0f : bd::gpu::kDesignCanvasHeight;
  const float tol_x = kCanvasEdgeTolerance * canvas_x / bd::gpu::kDesignCanvasWidth;
  const float tol_y =
      kCanvasEdgeTolerance * canvas_y / bd::gpu::kDesignCanvasHeight;

  // Uploaded already byte-swapped, so position is two host floats at the front
  // of each vertex.
  auto pos = [verts, vertexStride](u32 i) {
    return reinterpret_cast<float *>(verts + i * vertexStride);
  };

  float min_x = 999999.0f, min_y = 999999.0f;
  float max_x = -999999.0f, max_y = -999999.0f;
  for (u32 i = 0; i < vertexCount; ++i) {
    const float *p = pos(i);
    min_x = std::min(min_x, p[0]);
    min_y = std::min(min_y, p[1]);
    max_x = std::max(max_x, p[0]);
    max_y = std::max(max_y, p[1]);
  }

  // Covering the canvas, not matching it: a menu's ground is often authored
  // well past the edges, and scaling one of those inward opens a gap at the
  // very edge it was oversized to reach.
  const bool one = IsOnePrimitive(primitiveType, vertexCount);
  const bool covers_x = one && min_x <= tol_x && max_x >= canvas_x - tol_x;
  const bool covers_y = one && min_y <= tol_y && max_y >= canvas_y - tol_y;
  const bool flush_x = one && (min_x <= tol_x || max_x >= canvas_x - tol_x);
  const bool flush_y = one && (min_y <= tol_y || max_y >= canvas_y - tol_y);
  const bool spans_x = covers_x || (covers_y && flush_x);
  const bool spans_y = covers_y || (covers_x && flush_y);
  const float scale_x = spans_x ? 1.0f : kx;
  const float scale_y = spans_y ? 1.0f : ky;
  if (scale_x == 1.0f && scale_y == 1.0f)
    return;

  const float mid_x = canvas_x * 0.5f;
  const float mid_y = canvas_y * 0.5f;
  for (u32 i = 0; i < vertexCount; ++i) {
    float *p = pos(i);
    p[0] = mid_x + (p[0] - mid_x) * scale_x;
    p[1] = mid_y + (p[1] - mid_y) * scale_y;
  }
}

u32 D3DDevice_DrawVerticesUP_hook(u32 device_guest, u32 primitiveType,
                                  u32 vertexCount, u32 pVertexData,
                                  u32 vertexStride) {
  InsetOverscanScreenSprite(pVertexData, primitiveType, vertexCount,
                            vertexStride);
  u32 uploaded_count = vertexCount;
  const bool ok =
      UploadAndBindUpVertices(primitiveType, pVertexData, uploaded_count,
                              vertexStride);
  DrawArgs args{};
  args.is_up = ok;
  args.vertexOrIndexCount = vertexCount;
  args.startVertex = 0;
  DispatchDraw(device_guest, primitiveType, "DrawVerticesUP", args);
  return 0;
}

// The streaming draw-up pair. Their recompiled bodies drive the PM4 ring buffer
// reblue stubs out, so an unhooked BeginVertices spins forever in
// D3DDevice_RingBufferFlush. EndVertices copies synchronously, so the grow-only
// scratch is free again for the next BeginVertices.
struct {
  u32 va = 0;       // guest VA of the scratch buffer, 0 until first use
  u32 capacity = 0; // bytes currently allocated
} g_begin_vertices_scratch;

struct {
  u32 device = 0;
  u32 primitive_type = 0;
  u32 vertex_count = 0;
  u32 stride = 0;
  u32 data_va = 0; // 0 when nothing is pending to draw
  // UFC BeginIndexedVertices: the indices the game writes next to the vertices.
  u32 index_va = 0; // 0 for BeginVertices
  u32 index_count = 0;
  bool index32 = false;
  i32 base_vertex = 0;
} g_begin_vertices_pending;

u32 D3DDevice_BeginVertices_hook(u32 device_guest, u32 primitiveType,
                                 u32 vertexCount, u32 vertexStride) {
  g_begin_vertices_pending = {};
  const u64 size =
      static_cast<u64>(vertexCount) * static_cast<u64>(vertexStride);
  // Nothing to draw: return null so the caller skips its memcpy + EndVertices
  // (Visual__DrawVerticesUP gates both on a non-null BeginVertices result).
  if (size == 0 || size > 0xFFFFFFFFull)
    return 0;

  auto *memory = REX_KERNEL_MEMORY();
  if (g_begin_vertices_scratch.capacity < size) {
    // Safe to free the old block here: EndVertices copies the bytes out
    // synchronously, so nothing references the previous scratch by now.
    const u32 new_capacity = static_cast<u32>(size);
    const u32 va = memory->SystemHeapAlloc(new_capacity, 0x20);
    if (!va)
      return 0;
    if (g_begin_vertices_scratch.va)
      memory->SystemHeapFree(g_begin_vertices_scratch.va);
    g_begin_vertices_scratch.va = va;
    g_begin_vertices_scratch.capacity = new_capacity;
  }

  g_begin_vertices_pending.device = device_guest;
  g_begin_vertices_pending.primitive_type = primitiveType;
  g_begin_vertices_pending.vertex_count = vertexCount;
  g_begin_vertices_pending.stride = vertexStride;
  g_begin_vertices_pending.data_va = g_begin_vertices_scratch.va;
  return g_begin_vertices_scratch.va;
}

#if defined(SVR_NATIVE_RENDERER) && defined(SVR07_NATIVE_RENDERER)
// L16: UP quad batching (svr07_native_up_batch, reports/L16-fps-quad-batching.md).
//
// The heavy entrance scenes draw ~1400 small BeginVertices draws a frame (crowd / pyro sprites,
// ~150 bytes each), mostly with the pipeline and textures of the draw before. Each one costs the
// frame thread the whole DispatchDraw: ~0.6-1.0 us. Here a BeginVertices draw whose every input
// equals the previous BeginVertices draw's is appended to that draw's host draw call instead:
//  - the previous draw's call is held in the threaded command stream (ufc_command_stream.cpp), and
//    is written just before the next command of any kind. So nothing at all was recorded between
//    the two (no bind, no clear, no resolve, no other draw) when this one is appended to it;
//  - its vertices sit in a reserve right after the held draw's own, in the vertex view the held
//    draw binds, so the appended primitives follow on in the same buffer and draw in guest order;
//  - every input the flush would read is checked equal: shaders, declaration, targets, texture
//    slots and the shared block (SvrSharedBlockWouldMatch), the device's register shadow, render
//    target / depth / declaration words, texture table and viewport, the VS/PS float constants the
//    shaders read (SvrStageConstantsWouldMatch), and no dirty bit the flush would act on;
//  - list primitives only (point, line, triangle, rectangle -> triangle, quad -> expansion IB),
//    stream 0 only, no vertex shader that reads the vertex index (it would see the running index).
// A merged draw makes no host call of its own; its result is the same as its own draw right after
// the previous one with nothing between.
// Fast path: when all of that holds except the VS/PS float constants (per-sprite constants), the
// draw binds its own constant blocks (the same uploads the flush makes) and draws from the reserve
// on the pipeline, shared block, targets and vertex view still bound on the list: everything else
// the full DispatchDraw would do is a no-op for it. Its call is held in turn.
struct UpBatchSig {
  const bd::gpu::GuestShader *vs, *ps;
  const bd::gpu::GuestVertexDeclaration *decl;
  const bd::gpu::GuestTexture *rt, *ds;
  const bd::gpu::GuestTexture *textures[bd::gpu::kTextureSlots];
  u8 regs[svr07::d3d::kRegsEnd - svr07::d3d::kRegs2000];               // register shadow
  u8 decl_word[4];                                                      // +0x2D20
  u8 targets[svr07::d3d::kStreamSource - svr07::d3d::kRenderTarget0];  // RT0-3 + depth
  u8 table[0x3094 - svr07::d3d::kTexture];  // texture table, viewport, shaders, to +0x3094
};

bool FillUpBatchSig(UpBatchSig &g, u32 device_guest) {
  std::memset(&g, 0, sizeof(g));
  auto &s = bd::gpu::state();
  g.vs = s.vertex_shader;
  g.ps = s.pixel_shader;
  g.decl = s.vertex_declaration;
  g.rt = s.render_target;
  g.ds = s.depth_stencil;
  for (u32 i = 0; i < bd::gpu::kTextureSlots; ++i)
    g.textures[i] = s.textures[i];
  const u8 *dev = bd::mem::try_at<const u8>(device_guest);
  if (!dev)
    return false;
  std::memcpy(g.regs, dev + svr07::d3d::kRegs2000, sizeof(g.regs));
  std::memcpy(g.decl_word, dev + svr07::d3d::kVertexDeclaration, sizeof(g.decl_word));
  std::memcpy(g.targets, dev + svr07::d3d::kRenderTarget0, sizeof(g.targets));
  std::memcpy(g.table, dev + svr07::d3d::kTexture, sizeof(g.table));
  return true;
}

// fps15: the signature compared straight against the device (no copy). `textures` = also the
// host texture slots and the device texture table (BeginVertices batches; indexed draws re-run
// the steps that read textures, so they leave them out).
bool SigMatchesDevice(const UpBatchSig &g, u32 device_guest, bool textures) {
  const auto &s = bd::gpu::state();
  if (s.vertex_shader != g.vs || s.pixel_shader != g.ps || s.vertex_declaration != g.decl ||
      s.render_target != g.rt || s.depth_stencil != g.ds)
    return false;
  const u8 *dev = bd::mem::try_at<const u8>(device_guest);
  if (!dev)
    return false;
  constexpr u32 kViewportAt = svr07::d3d::kViewport - svr07::d3d::kTexture;
  if (std::memcmp(g.regs, dev + svr07::d3d::kRegs2000, sizeof(g.regs)) != 0 ||
      std::memcmp(g.decl_word, dev + svr07::d3d::kVertexDeclaration, sizeof(g.decl_word)) != 0 ||
      std::memcmp(g.targets, dev + svr07::d3d::kRenderTarget0, sizeof(g.targets)) != 0 ||
      std::memcmp(g.table + kViewportAt, dev + svr07::d3d::kViewport,
                  sizeof(g.table) - kViewportAt) != 0)
    return false;
  if (textures) {
    if (std::memcmp(g.table, dev + svr07::d3d::kTexture, kViewportAt) != 0 ||
        std::memcmp(g.textures, s.textures, sizeof(g.textures)) != 0)
      return false;
  }
  return true;
}

struct UpBatch {
  bool active = false;
  u64 token = 0;
  u32 device = 0;
  u32 prim = 0;       // guest primitive type
  u32 stride = 0;
  u32 quads = 0;      // quad lists: quads in the held draw
  u8 *base = nullptr; // the reserve view's first byte (vertex 0 of the batch)
  u8 *room = nullptr; // where the next merged draw's vertices go
  u32 room_left = 0;
  u32 room_size = 0;  // reserve made for this batch
  u32 merges = 0;
  bool want_room = true;
  u32 idle_batches = 0;  // batches with a reserve that closed with no merge
  bool indexed = false;  // fps13: the held draw is a DrawIndexedVertices one (indexed fast path)
  UpBatchSig sig;
} g_upb;

bool IndexedBatchOn() {
  static const bool on = [] {
    const i32 v = REXCVAR_GET(svr07_native_idx_fast);
    const bool r = v > 0 || (v < 0 && REXCVAR_GET(svr07_high_fps_hz) > 60.0);
    BD_INFO("native: DrawIndexedVertices fast path {} (svr07_native_idx_fast {}, needs "
            "svr07_native_up_batch)",
            r ? "on" : "off", v);
    return r;
  }();
  return on;
}

bool UpBatchOn() {
  static const bool on = [] {
    const i32 v = REXCVAR_GET(svr07_native_up_batch);
    const bool want = v > 0 || (v < 0 && REXCVAR_GET(svr07_high_fps_hz) > 60.0);
    const bool threaded = bd::gpu::SvrThreadedRecordingOn();
    const bool skip_env = std::getenv("SVR_SKIP_DRAWS") != nullptr;
    const bool r = want && threaded && !skip_env;
    BD_INFO("native: BeginVertices batching {} (svr07_native_up_batch {}, threaded recording {}, "
            "reserve {} bytes)",
            r ? "on" : "off", v, threaded ? "on" : "off", REXCVAR_GET(svr07_native_up_batch_room));
    return r;
  }();
  return on;
}

// Vertices per primitive for the list types batched (0 = not batched).
u32 UpBatchUnit(u32 prim) {
  switch (prim) {
  case 1: return 1;  // point list
  case 2: return 2;  // line list
  case kPrimTriangleList: return 3;
  case kPrimRectList: return (REXCVAR_GET(svr07_native_off) & 4) ? 0u : 3u;
  case kPrimQuadList: return 4;
  case kPrimTriangleStrip: return 1;  // fps13: any count >= 3, rebuilt as a triangle list
  default: return 0;  // fans, line strips, loops
  }
}

// The first distinct shapes and constant differences, so a run's log says what the quads are.
std::atomic<u32> g_upb_logged{0};
u32 g_upb_note_calls = 0;  // frame thread; fps13: the log closes after this many tries too
constexpr u32 kUpShapeLogs = 48;
constexpr u32 kUpShapeTries = 8192;
bool UpShapeLogOpen() {
  return g_upb_note_calls < kUpShapeTries &&
         g_upb_logged.load(std::memory_order_relaxed) < kUpShapeLogs;
}

void NoteUpShape(const char *why, u32 prim, u32 stride, u32 count, int stage, int first, int regs) {
  auto &s_logged = g_upb_logged;
  if (!UpShapeLogOpen())
    return;
  ++g_upb_note_calls;
  static std::mutex s_mutex;
  static std::set<std::array<u64, 6>> s_seen;
  const auto &st = bd::gpu::state();
  const auto hash_of = [](const auto *shader) -> u64 {
    return shader && shader->shaderCacheEntry ? shader->shaderCacheEntry->hash : 0u;
  };
  const u64 vs = hash_of(st.vertex_shader), ps = hash_of(st.pixel_shader);
  const std::array<u64, 6> key = {(u64(u8(why[0])) << 32) | prim, stride, vs, ps,
                                  (u64(u32(stage)) << 32) | u32(first),
                                  u64(regs > 16 ? 17 : regs)};
  std::lock_guard lock(s_mutex);
  if (!s_seen.insert(key).second)
    return;
  s_logged.fetch_add(1, std::memory_order_relaxed);
  if (stage >= 0)
    BD_INFO("native: [upbatch] {} prim {} stride {} count {} vs {:016X} ps {:016X} | {} constants "
            "differ from c{}, {} registers",
            why, prim, stride, count, vs, ps, stage ? "PS" : "VS", first, regs);
  else
    BD_INFO("native: [upbatch] {} prim {} stride {} count {} vs {:016X} ps {:016X}", why, prim,
            stride, count, vs, ps);
}

// The draw's vertices in host order, as UploadAndBindUpVertices makes them (byte swap, integer
// elements, rectangle rebuild). `out` = the bytes the host draw reads, `host_count` its count.
bool BuildUpVertices(u32 prim, u32 data_va, u32 count, u32 stride, std::vector<u8> &out,
                     u32 &host_count) {
  const auto *decl = bd::gpu::state().vertex_declaration;
  const auto *src = bd::mem::try_at<const u8>(data_va);
  if (!src)
    return false;
  const u32 bytes = count * stride;
  out.resize(bytes);
  rex::memory::copy_and_swap_32_unaligned(out.data(), src, bytes / 4);
  if (decl && decl->cpuConvertCount)
    ConvertIntegerElements(decl, out.data(), count, stride);
  host_count = count;
  if (prim == kPrimRectList) {
    out = ExpandRectList(decl, out.data(), count, stride);
    host_count = (count / 3u) * 6u;
  } else if (prim == kPrimTriangleStrip) {
    // fps13: the strip's triangles in Direct3D's own strip order (0 1 2, 1 3 2, 2 3 4, 3 5 4 ...):
    // same triangles, same winding and the same first (provoking) vertex as the strip draw.
    static thread_local std::vector<u8> strip;
    strip.swap(out);
    const u32 tris = count - 2u;
    out.resize(size_t(tris) * 3u * stride);
    for (u32 t = 0; t < tris; ++t) {
      const u32 pick[3] = {t, (t & 1u) ? t + 2u : t + 1u, (t & 1u) ? t + 1u : t + 2u};
      for (u32 k = 0; k < 3; ++k)
        std::memcpy(out.data() + size_t(t * 3u + k) * stride, strip.data() + size_t(pick[k]) * stride,
                    stride);
    }
    host_count = tris * 3u;
  }
  return true;
}

// Handles a BeginVertices draw when it can be batched: appended to the held draw, or drawn as the
// start of a new batch. False = not batchable, the caller draws it the old way.
bool UpBatchDraw(const decltype(g_begin_vertices_pending) &p) {
  const u32 unit = UpBatchUnit(p.primitive_type);
  auto &s = bd::gpu::state();
  const auto *decl = s.vertex_declaration;
  bool streams_ok = decl != nullptr;
  if (decl) {
    for (u32 n = 1; n < 16; ++n)
      streams_ok = streams_ok && !decl->vertexStreams[n];
  }
  if (!unit || (p.vertex_count % unit) != 0 || (p.stride % 4) != 0 || !p.stride || !streams_ok ||
      (p.primitive_type == kPrimTriangleStrip && p.vertex_count < 3) ||
      !s.vertex_shader || u64(p.vertex_count) * p.stride > (1u << 20) ||
      (p.primitive_type == kPrimQuadList &&
       (!bd::gpu::Video::QuadlistExpansionIBView() ||
        p.vertex_count / 4 > bd::gpu::Video::QuadlistMaxQuads()))) {
    bd::gpu::SvrPerfCount(bd::gpu::SvrCount::kUpBreakType, 1);
    NoteUpShape("not batchable", p.primitive_type, p.stride, p.vertex_count, -1, 0, 0);
    return false;
  }
  if (s.vertex_shader->constantUse.readsVertexId || !s.vertex_shader->constantUse.known) {
    bd::gpu::SvrPerfCount(bd::gpu::SvrCount::kUpBreakVid, 1);
    NoteUpShape("vertex index", p.primitive_type, p.stride, p.vertex_count, -1, 0, 0);
    return false;
  }
  if (bd::gpu::SvrDrawLogActive())
    return false;

  static thread_local std::vector<u8> verts;
  u32 host_count = 0;
  {
    bd::gpu::SvrZone zone_up(bd::gpu::SvrZoneId::kUpVerts);
    if (!BuildUpVertices(p.primitive_type, p.data_va, p.vertex_count, p.stride, verts, host_count))
      return false;
  }
  const u32 bytes = u32(verts.size());

  // Append to the held draw, or draw on the fast path behind it?
  {
    bd::gpu::SvrZone zone(bd::gpu::SvrZoneId::kUpBatch);
    std::unique_lock lock(s.mutex, std::try_to_lock);
    if (!lock.owns_lock())
      lock.lock();
    auto why = bd::gpu::SvrCount::kUpMerged;
    int stage = -1, first = 0, regs = 0;
    bool constants_same = false;
    if (!g_upb.active || bd::gpu::SvrStreamHeldToken() != g_upb.token) {
      g_upb.active = false;
      why = bd::gpu::SvrCount::kUpBreakStream;
    } else if (g_upb.indexed) {
      why = bd::gpu::SvrCount::kUpBreakState;  // the held draw is an indexed one
    } else if (p.device != g_upb.device || p.primitive_type != g_upb.prim ||
               p.stride != g_upb.stride) {
      why = bd::gpu::SvrCount::kUpBreakType;
    } else {
      const auto &d = s.dirtyStates;
      const bool dirty = d.renderTargetAndDepthStencil || d.viewport || d.pipelineState ||
                         d.depthBias || d.scissorRect;
      const bool host_same =
          s.vertex_shader == g_upb.sig.vs && s.pixel_shader == g_upb.sig.ps &&
          s.vertex_declaration == g_upb.sig.decl &&
          s.pipelineState.vertexShader == s.vertex_shader &&
          s.pipelineState.pixelShader == s.pixel_shader &&
          s.pipelineState.vertexDeclaration == s.vertex_declaration && !s.occlusion_counting &&
          s.command_list_open && s.draw_framebuffer_bound && s.command_list != nullptr &&
          s.current_pso != nullptr;
      if (dirty || !host_same) {
        why = bd::gpu::SvrCount::kUpBreakState;
      } else if (!SigMatchesDevice(g_upb.sig, p.device, /*textures=*/true)) {
        why = bd::gpu::SvrCount::kUpBreakState;
      } else if (!bd::gpu::SvrSharedBlockWouldMatch(p.device)) {
        why = bd::gpu::SvrCount::kUpBreakShared;
      } else if (bytes > g_upb.room_left ||
                 (p.primitive_type == kPrimQuadList &&
                  g_upb.quads + p.vertex_count / 4 > bd::gpu::Video::QuadlistMaxQuads())) {
        why = bd::gpu::SvrCount::kUpBreakRoom;
        g_upb.want_room = true;
      } else {
        constants_same = bd::gpu::SvrStageConstantsWouldMatch(
            p.device, UpShapeLogOpen() ? &stage : nullptr, &first, &regs);
        why = constants_same ? bd::gpu::SvrCount::kUpMerged : bd::gpu::SvrCount::kUpFast;
      }
    }
    if (why == bd::gpu::SvrCount::kUpMerged || why == bd::gpu::SvrCount::kUpFast) {
      // The vertices go into the reserve, right after the held draw's.
      const u32 start_vertex = u32(g_upb.room - g_upb.base) / p.stride;
      const u32 quads = p.primitive_type == kPrimQuadList ? p.vertex_count / 4 : 0;
      bool done = false;
      // Written before any draw can read them (unused if this draw ends up on the full path).
      std::memcpy(g_upb.room, verts.data(), bytes);
      if (why == bd::gpu::SvrCount::kUpMerged) {
        // Nothing the draw reads differs: the held draw call takes its primitives too.
        const u32 add = p.primitive_type == kPrimQuadList ? quads * 6 : host_count;
        if (bd::gpu::SvrStreamExtendHeldDraw(g_upb.token, add)) {
          ++g_upb.merges;
          g_upb.quads += quads;
          done = true;
        }
      } else {
        // Only the float constants differ: bind this draw's VS/PS blocks (the stream writes the
        // held draw first) and draw from the reserve on the same pipeline, view and shared block.
        // This draw's call is held in turn, so the next one can join it.
        bd::gpu::SvrUploadStageConstantsLocked(p.device);
        auto *cmd = s.command_list;
        bd::gpu::SvrZone zone_call(bd::gpu::SvrZoneId::kDrawCall);
        bd::gpu::SvrStreamSetHoldNextDraw(true);
        if (p.primitive_type == kPrimQuadList)
          cmd->drawIndexedInstanced(quads * 6, 1, 0, i32(start_vertex), 0);
        else
          cmd->drawInstanced(host_count, 1, start_vertex, 0);
        bd::gpu::SvrStreamSetHoldNextDraw(false);
        const u64 token = bd::gpu::SvrStreamHeldToken();
        if (token && token != g_upb.token) {
          g_upb.token = token;
          g_upb.quads = quads;
          ++g_upb.merges;
          done = true;
        } else {
          g_upb.active = false;  // not expected: the draw went out unheld; drawn all the same
          done = true;
        }
      }
      if (done) {
        g_upb.room += bytes;
        g_upb.room_left -= bytes;
        bd::gpu::SvrPerfCount(why, 1);
        bd::gpu::SvrDiagAddTicks(bd::gpu::SvrDiag::kDraw, 0);  // fps15: still a draw in [nperf] draws
        bd::gpu::SvrPerfCount(bd::gpu::SvrCount::kUpDraws, 1);
        bd::gpu::SvrPerfCount(bd::gpu::SvrCount::kUpBytes, u64(p.vertex_count) * p.stride);
        lock.unlock();
        bd::gpu::SvrNoteDraw(true);
        if (why == bd::gpu::SvrCount::kUpFast)
          NoteUpShape("constants", p.primitive_type, p.stride, p.vertex_count, stage, first, regs);
        return true;
      }
      g_upb.active = false;
      why = bd::gpu::SvrCount::kUpBreakStream;
    }
    bd::gpu::SvrPerfCount(why, 1);
    if (why == bd::gpu::SvrCount::kUpBreakState)
      NoteUpShape("state", p.primitive_type, p.stride, p.vertex_count, -1, 0, 0);
    else if (why == bd::gpu::SvrCount::kUpBreakShared)
      NoteUpShape("shared", p.primitive_type, p.stride, p.vertex_count, -1, 0, 0);
  }

  // Start a batch: this draw's vertices with a reserve behind them, its host draw held.
  if (g_upb.room_size) {
    if (g_upb.merges == 0) {
      if (++g_upb.idle_batches >= 64) {
        g_upb.want_room = false;
        g_upb.idle_batches = 0;
      }
    } else {
      g_upb.idle_batches = 0;
    }
  }
  g_upb.active = false;
  g_upb.room_size = 0;
  const u32 room =
      g_upb.want_room
          ? u32(std::clamp<i32>(REXCVAR_GET(svr07_native_up_batch_room), 0, 1 << 20)) & ~3u
          : 0u;
  bd::gpu::ConstantAllocation alloc;
  {
    bd::gpu::SvrZone zone_up(bd::gpu::SvrZoneId::kUpVerts);
    alloc = bd::gpu::AllocateUploadBytes(bytes + room, 4);
    if (!alloc.memory)
      return false;
    bd::gpu::SvrPerfCount(bd::gpu::SvrCount::kUpBytes, u64(p.vertex_count) * p.stride);
    std::memcpy(alloc.memory, verts.data(), bytes);
    bd::gpu::Video::SetVertexStream(0, alloc.ref, bytes + room, p.stride);
  }
  DrawArgs args{};
  args.is_up = true;
  args.hold = true;
  const bool rebuilt = p.primitive_type == kPrimRectList || p.primitive_type == kPrimTriangleStrip;
  const u32 host_prim = rebuilt ? kPrimTriangleList : p.primitive_type;
  args.vertexOrIndexCount = rebuilt ? host_count : p.vertex_count;
  args.startVertex = 0;
  const u64 held_before = bd::gpu::SvrStreamHeldToken();
  DispatchDraw(p.device, host_prim,
               p.primitive_type == kPrimRectList    ? "BeginVertices(rects)"
               : p.primitive_type == kPrimTriangleStrip ? "BeginVertices(strip)"
                                                        : "BeginVertices",
               args);
  std::lock_guard lock(s.mutex);
  const u64 token = bd::gpu::SvrStreamHeldToken();
  // Not held by this draw (dropped, or an older held draw still waiting): no batch.
  if (!token || token == held_before || !FillUpBatchSig(g_upb.sig, p.device))
    return true;
  g_upb.active = true;
  g_upb.indexed = false;
  g_upb.token = token;
  g_upb.device = p.device;
  g_upb.prim = p.primitive_type;
  g_upb.stride = p.stride;
  g_upb.quads = p.primitive_type == kPrimQuadList ? p.vertex_count / 4 : 0;
  g_upb.base = alloc.memory;
  g_upb.room = alloc.memory + bytes;
  g_upb.room_left = room;
  g_upb.room_size = room;
  g_upb.merges = 0;
  return true;
}
#endif

#if defined(SVR_NATIVE_RENDERER) && defined(SVR07_NATIVE_RENDERER)
// fps13/fps14: a fast path for plain DrawIndexedVertices draws. The previous draw (indexed, or a
// BeginVertices one) is held, so nothing was recorded since and the list still has its pipeline,
// viewport, scissor and framebuffer bound. If this draw's inputs to those are equal (shaders,
// declaration, topology, all vertex strides, targets, the device's register shadow, RT/depth and
// declaration words, viewport and shader words, no dirty bit the flush acts on), DispatchDraw's
// viewport, render state, scissor and pipeline steps would record nothing new: they are left out.
// Everything else the full path does for the draw is done the same way: owed target fills, the
// framebuffer bind's transitions for this draw's textures, VS/PS constants, the shared block (its
// texture slots, bools and this draw's base vertex), vertex/index buffer binds, the draw call.
// fps14: textures may differ (the steps that read them are run), and vertex-index shaders are
// allowed (no merging here: each draw keeps its own call, base vertex and shared block). Only
// computed-fetch shaders are left out (the flush binds their streams as root descriptors).
enum class IdxReject { kNone, kHeld, kShader, kDirty, kPipeline, kSig, kIndex, kFramebuffer };

void NoteIdxReject(IdxReject why, const char *what, u32 prim) {
  static u32 s_tries = 0;  // frame thread
  static u32 s_logged = 0;
  static std::set<std::pair<int, std::string>> s_seen;
  if (s_tries >= 8192 || s_logged >= 32)
    return;
  ++s_tries;
  if (!s_seen.insert({int(why), what}).second)
    return;
  ++s_logged;
  const auto &st = bd::gpu::state();
  const auto hash_of = [](const auto *shader) -> u64 {
    return shader && shader->shaderCacheEntry ? shader->shaderCacheEntry->hash : 0u;
  };
  BD_INFO("native: [idxfast] full path: {} (prim {} vs {:016X} ps {:016X})", what, prim,
          hash_of(st.vertex_shader), hash_of(st.pixel_shader));
}

// Which part of the signature differs; textures (host slots and the device's texture table) are
// not compared for indexed draws.
const char *IdxSigDiff(const UpBatchSig &a, const UpBatchSig &b) {
  if (a.vs != b.vs || a.ps != b.ps)
    return "shaders differ";
  if (a.decl != b.decl || std::memcmp(a.decl_word, b.decl_word, sizeof(a.decl_word)) != 0)
    return "declaration differs";
  if (a.rt != b.rt || a.ds != b.ds || std::memcmp(a.targets, b.targets, sizeof(a.targets)) != 0)
    return "targets differ";
  constexpr u32 kViewportAt = svr07::d3d::kViewport - svr07::d3d::kTexture;
  if (std::memcmp(a.table + kViewportAt, b.table + kViewportAt, sizeof(a.table) - kViewportAt) != 0)
    return "viewport / shader words differ";
  for (u32 i = 0; i < sizeof(a.regs); i += 4) {
    if (std::memcmp(a.regs + i, b.regs + i, 4) != 0) {
      static thread_local char buf[48];
      // Register number from the block layout (svr07_d3d.h RegisterOffset), for the log.
      std::snprintf(buf, sizeof(buf), "register shadow +0x%X differs",
                    unsigned(svr07::d3d::kRegs2000 + i));
      return buf;
    }
  }
  return nullptr;
}

bool IndexedFastDraw(u32 device_guest, u32 prim, i32 base, u32 start, u32 count) {
  auto &s = bd::gpu::state();
  bd::gpu::SvrZone zone(bd::gpu::SvrZoneId::kUpBatch);
  IdxReject why = IdxReject::kNone;
  const char *what = "";
  const auto reject = [&](IdxReject w, const char *text) {
    why = w;
    what = text;
  };
  const auto *vs = s.vertex_shader;
  if (!g_upb.active || device_guest != g_upb.device) {
    reject(IdxReject::kHeld, "no held draw");
  } else if (!vs || !vs->shaderCacheEntry ||
             (vs->shaderCacheEntry->flags & kShaderFlagComputedFetch) ||
             bd::gpu::SvrDrawLogActive()) {
    reject(IdxReject::kShader, "computed fetch / no shader / draw log");
  }
  if (why == IdxReject::kNone) {
    // As DispatchDraw starts: a sampled resolve target owing its fill gets it (may record; then
    // the held draw is gone and this draw takes the full path, where nothing is owed any more).
    bd::gpu::SvrFillSampledTargets(s.textures, bd::gpu::kTextureSlots);
  }
  std::unique_lock lock(s.mutex, std::try_to_lock);
  if (!lock.owns_lock())
    lock.lock();
  UpBatchSig now;
  if (why == IdxReject::kNone) {
    const auto &d = s.dirtyStates;
    if (bd::gpu::SvrStreamHeldToken() != g_upb.token) {
      reject(IdxReject::kHeld, "held draw already written");
    } else if (d.renderTargetAndDepthStencil || d.viewport || d.pipelineState || d.depthBias ||
               d.scissorRect) {
      reject(IdxReject::kDirty, d.pipelineState ? "pipeline state dirty"
                                : d.viewport    ? "viewport dirty"
                                : d.scissorRect ? "scissor dirty"
                                                : "targets / depth bias dirty");
    } else if (s.pipelineState.vertexShader != vs ||
               s.pipelineState.pixelShader != s.pixel_shader ||
               s.pipelineState.vertexDeclaration != s.vertex_declaration || s.occlusion_counting ||
               !s.command_list_open || !s.draw_framebuffer_bound || !s.command_list ||
               !s.current_pso) {
      reject(IdxReject::kPipeline, "shaders / declaration differ from the bound pipeline");
    } else if (s.pipelineState.primitiveTopology != MapPrimitiveType(prim)) {
      reject(IdxReject::kPipeline, "topology differs");
    } else {
      for (u32 i = 0; i < 16; ++i) {
        if (s.pipelineState.vertexStrides[i] != static_cast<u8>(s.input_slots[i].stride)) {
          reject(IdxReject::kPipeline, "vertex stride differs");
          break;
        }
      }
    }
  }
  if (why == IdxReject::kNone) {
    const u32 index_size = s.index_view.format == plume::RenderFormat::R32_UINT ? 4u : 2u;
    if (!s.index_view.buffer.ref || (u64(start) + count) * index_size > s.index_view.size)
      reject(IdxReject::kIndex, "index range");
  }
  if (why == IdxReject::kNone) {
    if (!SigMatchesDevice(g_upb.sig, device_guest, /*textures=*/false)) {
      // Which part, for the bounded log only.
      const char *diff = FillUpBatchSig(now, device_guest) ? IdxSigDiff(now, g_upb.sig) : nullptr;
      reject(IdxReject::kSig, diff ? diff : "signature differs");
    }
  }
  if (why == IdxReject::kNone && !bd::gpu::Video::BindDrawFramebufferLocked())
    reject(IdxReject::kFramebuffer, "framebuffer bind failed");
  if (why != IdxReject::kNone) {
    bd::gpu::SvrPerfCount(bd::gpu::SvrCount::kIdxFull, 1);
    bd::gpu::SvrPerfCount(why == IdxReject::kHeld       ? bd::gpu::SvrCount::kIdxNoHeld
                          : why == IdxReject::kSig      ? bd::gpu::SvrCount::kIdxSig
                          : why == IdxReject::kPipeline ? bd::gpu::SvrCount::kIdxPipeline
                                                        : bd::gpu::SvrCount::kIdxOther,
                          1);
    lock.unlock();
    NoteIdxReject(why, what, prim);
    return false;
  }

  // The rest of the full path for this draw.
  s.vertex_id_base = base;
  bd::gpu::SvrUploadStageConstantsLocked(device_guest);
  auto *cmd = s.command_list;
  {
    bd::gpu::SvrZone zone_sc(bd::gpu::SvrZoneId::kSharedConstants);
    bd::gpu::SvrUploadSharedConstantsLocked(device_guest);
  }
  {
    bd::gpu::SvrZone zone_bind(bd::gpu::SvrZoneId::kVertexBind);
    if (s.dirtyStates.vertexStreamFirst <= s.dirtyStates.vertexStreamLast) {
      const u32 first = s.dirtyStates.vertexStreamFirst;
      const u32 n = u32{s.dirtyStates.vertexStreamLast} - first + 1u;
      cmd->setVertexBuffers(first, s.vertex_views + first, n, s.input_slots + first);
      s.dirtyStates.vertexStreamFirst = 255;
      s.dirtyStates.vertexStreamLast = 0;
    }
    if (s.dirtyStates.indices) {
      cmd->setIndexBuffer(&s.index_view);
      s.dirtyStates.indices = false;
    }
  }
  const auto hash_of = [](const auto *shader) -> u64 {
    return shader && shader->shaderCacheEntry ? shader->shaderCacheEntry->hash : 0u;
  };
  bd::gpu::SvrNoteWordRoundedIndices(device_guest, prim, start, count, hash_of(s.vertex_shader),
                                     hash_of(s.pixel_shader));
  {
    bd::gpu::SvrZone zone_call(bd::gpu::SvrZoneId::kDrawCall);
    bd::gpu::SvrStreamSetHoldNextDraw(true);
    cmd->drawIndexedInstanced(count, 1, start, base, 0);
    bd::gpu::SvrStreamSetHoldNextDraw(false);
  }
  // The held draw keeps the pipeline/viewport/targets of the signature; only its textures changed,
  // which indexed draws don't compare, so the signature stays the batch's.
  const u64 token = bd::gpu::SvrStreamHeldToken();
  g_upb.active = token != 0 && token != g_upb.token;
  g_upb.token = token;
  g_upb.indexed = true;
  g_upb.room_left = 0;
  bd::gpu::SvrPerfCount(bd::gpu::SvrCount::kIdxFast, 1);
  bd::gpu::SvrDiagAddTicks(bd::gpu::SvrDiag::kDraw, 0);
  lock.unlock();
  bd::gpu::SvrNoteDraw(true);
  return true;
}

// fps13: after a full-path DrawIndexedVertices draw that was held, the next draw may go fast behind it.
void IndexedNoteHeld(u32 device_guest, u64 held_before) {
  auto &s = bd::gpu::state();
  std::lock_guard lock(s.mutex);
  const u64 token = bd::gpu::SvrStreamHeldToken();
  if (!token || token == held_before || !FillUpBatchSig(g_upb.sig, device_guest)) {
    g_upb.active = false;
    return;
  }
  g_upb.active = true;
  g_upb.indexed = true;
  g_upb.token = token;
  g_upb.device = device_guest;
  g_upb.room_left = 0;
  g_upb.room_size = 0;
  g_upb.merges = 0;
}
#endif

u32 D3DDevice_EndVertices_hook(u32 /*device_guest*/) {
  const auto p = g_begin_vertices_pending;
  g_begin_vertices_pending = {}; // consume: a stray EndVertices must not redraw
  if (!p.data_va || !p.vertex_count)
    return 0;

#if defined(SVR_NATIVE_RENDERER)
  bd::gpu::SvrDrawLogVertices(p.data_va, p.vertex_count, p.stride);
#endif
#if defined(SVR_NATIVE_RENDERER) && defined(SVR07_NATIVE_RENDERER)
  // L16: batched (merged into the draw before, or drawn as a batch start) when it qualifies.
  if (!p.index_va && UpBatchOn() && UpBatchDraw(p))
    return 0;
#endif
  // An indexed rectangle list keeps its vertices (the indexed paths don't rebuild them yet).
  u32 uploaded_count = p.vertex_count;
  const bool ok = UploadAndBindUpVertices(p.index_va ? kPrimTriangleList : p.primitive_type,
                                         p.data_va, uploaded_count, p.stride);
  DrawArgs args{};
  args.is_up = ok;
#if defined(SVR_NATIVE_RENDERER)
  if (p.index_va) {
    // BeginIndexedVertices: the game's indices, as the GPU reads them (INDEX16 8in16, INDEX32
    // 8in32), through the per-frame upload ring like the vertices. A fan becomes a triangle list
    // (first, i, i + 1).
    const u32 width = p.index32 ? 4u : 2u;
    const bool fan = p.primitive_type == kPrimTriangleFan && p.index_count >= 3;
    const u32 out_count = fan ? (p.index_count - 2u) * 3u : p.index_count;
    const u32 index_bytes = out_count * width;
    const auto *src = bd::mem::try_at<const u8>(p.index_va);
    auto alloc = bd::gpu::AllocateUploadBytes((index_bytes + 3u) & ~3u, 4);
    if (!ok || !src || !alloc.memory || (p.primitive_type == kPrimTriangleFan && !fan)) {
      bd::gpu::SvrNoteDraw(false);
      return 0;
    }
    std::vector<u8> host(size_t(p.index_count) * width);
    if (p.index32)
      rex::memory::copy_and_swap_32_unaligned(host.data(), src, p.index_count);
    else
      rex::memory::copy_and_swap_16_unaligned(host.data(), src, p.index_count);
    if (fan) {
      for (u32 t = 0; t < p.index_count - 2u; ++t) {
        const u32 pick[3] = {0u, t + 1u, t + 2u};
        for (u32 k = 0; k < 3; ++k)
          std::memcpy(alloc.memory + size_t(t * 3u + k) * width,
                      host.data() + size_t(pick[k]) * width, width);
      }
    } else {
      std::memcpy(alloc.memory, host.data(), host.size());
    }
    bd::gpu::Video::SetIndexBufferRef(alloc.ref, index_bytes,
                                      p.index32 ? plume::RenderFormat::R32_UINT
                                                : plume::RenderFormat::R16_UINT);
    args.indexed = true;
    args.vertexOrIndexCount = out_count;
    args.startIndex = 0;
    args.baseVertexIndex = p.base_vertex;
    DispatchDraw(p.device, fan ? kPrimTriangleList : p.primitive_type,
                 fan ? "BeginIndexedVertices(fan)" : "BeginIndexedVertices", args);
    return 0;
  }
#endif
#if defined(SVR_NATIVE_RENDERER)
  if (p.primitive_type == kPrimRectList && !(REXCVAR_GET(svr07_native_off) & 4)) {
    if (!ok) {
      bd::gpu::SvrNoteDraw(false);
      return 0;
    }
    args.vertexOrIndexCount = uploaded_count;
    DispatchDraw(p.device, kPrimTriangleList, "BeginVertices(rects)", args);
    return 0;
  }
  if (p.primitive_type == kPrimTriangleFan) {
    const u32 fan_indices = ok ? bd::gpu::SvrBindFanIndices(p.vertex_count) : 0u;
    if (!fan_indices) {
      bd::gpu::SvrNoteDraw(false);
      return 0;
    }
    args.indexed = true;
    args.vertexOrIndexCount = fan_indices;
    DispatchDraw(p.device, kPrimTriangleList, "BeginVertices(fan)", args);
    return 0;
  }
#endif
  args.vertexOrIndexCount = p.vertex_count;
  args.startVertex = 0;
  DispatchDraw(p.device, p.primitive_type, "BeginVertices", args);
  return 0;
}

// All five params must be marshaled: a 4-arg signature drops the real
// IndexCount in r7 and submits zero count draws.
u32 D3DDevice_DrawIndexedVertices_hook(u32 device_guest, u32 primitiveType,
                                       u32 baseVertexIndex, u32 startIndex,
                                       u32 indexCount) {
#if defined(SVR_NATIVE_RENDERER)
  if (UfcCbCaptureDraw(device_guest, true, primitiveType, baseVertexIndex, startIndex, indexCount))
    return 0;
  // Streams and indices exactly as SvR's D3D left them in the device for this draw (its own
  // SetStreamSource/SetIndices are not observed any more).
  bd::gpu::SvrBindDrawGeometry(device_guest, /*indexed=*/true);
  BindConvertedStream0(device_guest);
  if (primitiveType == kPrimRectList && indexCount >= 3) {
    // Gather the rectangles' vertices in index order, then rebuild them as triangles.
    std::vector<u32> indices;
    std::vector<u8> range, verts;
    u32 stride = 0;
    if (bd::gpu::SvrReadDeviceIndices(device_guest, startIndex, indexCount, indices)) {
      const auto [lo, hi] = std::minmax_element(indices.begin(), indices.end());
      const u32 first = u32(i32(*lo) + i32(baseVertexIndex));
      if (bd::gpu::SvrReadStream0Vertices(device_guest, first, *hi - *lo + 1u, range, stride)) {
        verts.resize(size_t(indexCount) * stride);
        for (u32 i = 0; i < indexCount; ++i)
          std::memcpy(verts.data() + size_t(i) * stride,
                      range.data() + size_t(indices[i] - *lo) * stride, stride);
        if (DrawRebuiltRectList(device_guest, verts, indexCount, stride,
                                "DrawIndexedVertices(rects)"))
          return 0;
      }
    }
  }
  if (primitiveType == kPrimTriangleFan) {
    const u32 fan_indices =
        bd::gpu::SvrBindFanIndicesFromDevice(device_guest, startIndex, indexCount);
    if (!fan_indices) {
      bd::gpu::SvrNoteDraw(false);
      return 0;
    }
    DrawArgs fan{};
    fan.indexed = true;
    fan.vertexOrIndexCount = fan_indices;
    fan.baseVertexIndex = static_cast<i32>(baseVertexIndex);
    DispatchDraw(device_guest, kPrimTriangleList, "DrawIndexedVertices(fan)", fan);
    return 0;
  }
  if (primitiveType == kPrimQuadList && !(REXCVAR_GET(svr07_native_off) & 8)) {
    const auto *vs = bd::gpu::state().vertex_shader;
    const u32 quad_indices = bd::gpu::SvrBindQuadIndicesFromDevice(
        device_guest, startIndex, indexCount, static_cast<i32>(baseVertexIndex),
        vs && vs->shaderCacheEntry ? u64(vs->shaderCacheEntry->hash) : 0u);
    if (!quad_indices) {
      bd::gpu::SvrNoteDraw(false);
      return 0;
    }
    DrawArgs quad{};
    quad.indexed = true;
    quad.vertexOrIndexCount = quad_indices;
    quad.baseVertexIndex = static_cast<i32>(baseVertexIndex);
    DispatchDraw(device_guest, kPrimTriangleList, "DrawIndexedVertices(quads)", quad);
    return 0;
  }
#endif
#if defined(SVR_NATIVE_RENDERER) && defined(SVR07_NATIVE_RENDERER)
  // fps13: plain indexed draws behind a held draw with the same pipeline inputs take the fast path;
  // the others are held themselves so the next draw can.
  const bool batching = UpBatchOn() && IndexedBatchOn();
  if (batching && IndexedFastDraw(device_guest, primitiveType, static_cast<i32>(baseVertexIndex),
                                  startIndex, indexCount))
    return 0;
#endif
  DrawArgs args{};
  args.indexed = true;
  args.vertexOrIndexCount = indexCount;
  args.baseVertexIndex = static_cast<i32>(baseVertexIndex);
  args.startIndex = startIndex;
#if defined(SVR_NATIVE_RENDERER) && defined(SVR07_NATIVE_RENDERER)
  args.hold = batching;
  const u64 held_before = batching ? bd::gpu::SvrStreamHeldToken() : 0;
#endif
  DispatchDraw(device_guest, primitiveType, "DrawIndexedVertices", args);
#if defined(SVR_NATIVE_RENDERER) && defined(SVR07_NATIVE_RENDERER)
  if (batching)
    IndexedNoteHeld(device_guest, held_before);
#endif
  return 0;
}

u32 D3DDevice_Resolve_hook(u32 device_guest, u32 Flags, u32 pSourceRect,
                           u32 pDestTexture, u32 pDestPoint, u32 DestLevel,
                           u32 DestSliceOrFace, u32 /*pClearColor*/,
                           u32 /*ClearZHi*/, u32 /*ClearZLo*/,
                           u32 /*ClearStencil*/, u32 /*pParameters*/) {
#if defined(SVR_NATIVE_RENDERER)
  bd::gpu::SvrDrawLogResolve(device_guest, Flags, pSourceRect, pDestTexture, pDestPoint);
  auto *dst = bd::gpu::SvrResolveTarget(pDestTexture);
#else
  auto *dst =
      bd::gpu::HostResourceHeap::FromGuest<bd::gpu::GuestTexture>(pDestTexture);
#endif
  if (!dst) {
    static std::atomic<u32> s_miss{0};
    const u32 n = s_miss.fetch_add(1, std::memory_order_relaxed);
    if (n < 8) {
      BD_WARN("D3DDevice_Resolve: destination guest VA 0x{:08X} not a "
              "host texture",
              pDestTexture);
    }
    return 0;
  }
#if defined(SVR_NATIVE_RENDERER)
  bd::gpu::SvrPrepareTargetResolve(pDestTexture, /*exact=*/false);
#endif
  bd::gpu::Video::TrackResolveSource(Flags, dst, DestLevel, DestSliceOrFace);
  bd::gpu::Video::ResolveRtToTexture(dst);
  return 0;
}

u32 D3DDevice_BeginTiling_hook(u32 /*device_guest*/, u32 /*Flags*/,
                               u32 /*Count*/, u32 /*pTileRects*/,
                               mapped_f32 pClearColor, f64 ClearZ,
                               u32 /*z_gpr_slot*/, u32 ClearStencil) {
  u32 color_argb = 0;
  if (const be_f32 *color_vec = pClearColor) {
    const auto pack = [](float v) -> u32 {
      return static_cast<u32>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    color_argb = (pack(color_vec[3]) << 24) | (pack(color_vec[0]) << 16) |
                 (pack(color_vec[1]) << 8) | (pack(color_vec[2]) << 0);
  }
  bd::gpu::Video::Clear(bd::gpu::kClearAll, color_argb, float(ClearZ),
                        ClearStencil);
  return 0;
}

u32 D3DDevice_EndTiling_hook(u32 /*device_guest*/, u32 ResolveFlags,
                             u32 /*pResolveRects*/, u32 pDestTexture,
                             u32 /*pClearColor*/, u32 /*ClearZ*/,
                             u32 /*ClearStencil*/, u32 /*pParameters*/) {
  if (!pDestTexture) {
    return 0;
  }
#if defined(SVR_NATIVE_RENDERER)
  auto *dst = bd::gpu::SvrResolveTarget(pDestTexture);
#else
  auto *dst =
      bd::gpu::HostResourceHeap::FromGuest<bd::gpu::GuestTexture>(pDestTexture);
#endif
  if (!dst) {
    static std::atomic<u32> s_miss{0};
    const u32 n = s_miss.fetch_add(1, std::memory_order_relaxed);
    if (n < 8) {
      BD_WARN("D3DDevice_EndTiling: destination guest VA 0x{:08X} not a "
              "host texture",
              pDestTexture);
    }
    return 0;
  }
#if defined(SVR_NATIVE_RENDERER)
  bd::gpu::SvrPrepareTargetResolve(pDestTexture, /*exact=*/false);
#endif
  bd::gpu::Video::TrackResolveSource(ResolveFlags, dst);
  bd::gpu::Video::ResolveRtToTexture(dst);
  return 0;
}

u32 D3DDevice_DrawVertices_hook(u32 device_guest, u32 primitiveType,
                                u32 startVertex, u32 vertexCount) {
#if defined(SVR_NATIVE_RENDERER)
  if (UfcCbCaptureDraw(device_guest, false, primitiveType, 0, startVertex, vertexCount))
    return 0;
  bd::gpu::SvrBindDrawGeometry(device_guest, /*indexed=*/false);
  BindConvertedStream0(device_guest);
  if (primitiveType == kPrimRectList && vertexCount >= 3) {
    std::vector<u8> verts;
    u32 stride = 0;
    if (bd::gpu::SvrReadStream0Vertices(device_guest, startVertex, vertexCount, verts, stride) &&
        DrawRebuiltRectList(device_guest, verts, vertexCount, stride, "DrawVertices(rects)"))
      return 0;
  }
  if (primitiveType == kPrimTriangleFan) {
    const u32 fan_indices = bd::gpu::SvrBindFanIndices(vertexCount);
    if (!fan_indices) {
      bd::gpu::SvrNoteDraw(false);
      return 0;
    }
    DrawArgs fan{};
    fan.indexed = true;
    fan.vertexOrIndexCount = fan_indices;
    fan.baseVertexIndex = static_cast<i32>(startVertex);
    DispatchDraw(device_guest, kPrimTriangleList, "DrawVertices(fan)", fan);
    return 0;
  }
#endif
  DrawArgs args{};
  args.vertexOrIndexCount = vertexCount;
  args.startVertex = startVertex;
  DispatchDraw(device_guest, primitiveType, "DrawVertices", args);
  return 0;
}

#if defined(SVR_NATIVE_RENDERER)
// UFC: D3DDevice_Resolve (sub_8238D4F0) as the game called it: (dev, Flags, pSourceRect,
// pDestTexture, pDestPoint, DestLevel, DestSliceOrFace, pClearColor in r10, ClearZ in f1,
// ClearStencil at caller frame + 0x5C, pParameters at + 0x64). UFC resolves parts of a surface
// (each select-screen fighter is drawn into a 480x720 corner of the 1280x720 surface and that
// rectangle is resolved into its own texture; the shadow blur resolves 128x128 and 32x32 corners
// of a 512x512 surface), and it lets the resolve clear the surface afterwards
// (D3DRESOLVE_CLEARRENDERTARGET 0x100, D3DRESOLVE_CLEARDEPTHSTENCIL 0x200).

// One partial resolve for the log. A bad copy takes the graphics device down at the next present,
// and the bloom chain repeats the same few every frame: each distinct one is logged when first
// seen, and the last ones are kept for the device-lost report.
struct ResolveLogEntry {
  u32 frame, flags, src_w, src_h, src_format, src_samples, dest_va, dst_w, dst_h, dst_format,
      dst_levels, level, face;
  i32 left, top, right, bottom, dst_x, dst_y;
};
std::mutex g_resolve_log_mutex;
std::set<std::array<int64_t, 15>> g_resolve_log_seen;
std::array<ResolveLogEntry, 64> g_resolve_log_recent{};
u32 g_resolve_log_count = 0;
constexpr size_t kResolveLogMaxDistinct = 4000;

void LogResolveEntry(const char *what, const ResolveLogEntry &e) {
  BD_INFO("native: {} frame {} flags 0x{:X} src {}x{} f{} s{} rect {},{}-{},{} -> "
          "0x{:08X} {}x{} f{} levels {} at {},{} level {} face {}",
          what, e.frame, e.flags, e.src_w, e.src_h, e.src_format, e.src_samples, e.left, e.top,
          e.right, e.bottom, e.dest_va, e.dst_w, e.dst_h, e.dst_format, e.dst_levels, e.dst_x,
          e.dst_y, e.level, e.face);
}

void NoteResolveEntry(const ResolveLogEntry &e) {
  std::lock_guard lock(g_resolve_log_mutex);
  g_resolve_log_recent[g_resolve_log_count++ % g_resolve_log_recent.size()] = e;
  if (g_resolve_log_seen.size() < kResolveLogMaxDistinct &&
      g_resolve_log_seen
          .insert(std::array<int64_t, 15>{e.flags, e.src_w, e.src_h, e.src_format, e.dest_va,
                                          e.dst_w, e.dst_h, e.left, e.top, e.right, e.bottom,
                                          e.dst_x, e.dst_y, e.level, e.face})
          .second)
    LogResolveEntry("partial resolve", e);
}

void UfcResolveObserved(PPCContext &args, u32, u8 *) {
  bd::gpu::SvrZone zone(bd::gpu::SvrZoneId::kResolve);
  bd::gpu::SvrPerfCount(bd::gpu::SvrCount::kResolves, 1);
  const u32 device_guest = args.r3.u32, flags = args.r4.u32, src_rect_va = args.r5.u32;
  const u32 dest_va = args.r6.u32, dest_point_va = args.r7.u32, level = args.r8.u32;
  const u32 face = args.r9.u32, clear_color_va = args.r10.u32;
  bd::gpu::SvrDrawLogResolve(device_guest, flags, src_rect_va, dest_va, dest_point_va);
  const auto *src = (flags & bd::gpu::kResolveDepthStencil) ? bd::gpu::state().depth_stencil
                                                             : bd::gpu::state().render_target;
  // What ResolveSourceForFlagsLocked will copy from. No source rect means all of it.
  const auto *full = (src || (flags & bd::gpu::kResolveDepthStencil))
                         ? src
                         : bd::gpu::state().back_buffer_surface;
  plume::RenderRect rect{};
  bool have_rect = false;
  if (const auto *r = bd::mem::try_at<const bd::gpu::D3DRect>(src_rect_va)) {
    rect = plume::RenderRect(
        bd::gpu::SvrScalePx(i32(r->left), full), bd::gpu::SvrScalePx(i32(r->top), full),
        bd::gpu::SvrScalePx(i32(r->right), full), bd::gpu::SvrScalePx(i32(r->bottom), full));
    have_rect = true;
  }
  if (dest_va && UfcNr10On()) {
    const auto *r = bd::mem::try_at<const bd::gpu::D3DRect>(src_rect_va);
    const auto *pt = bd::mem::try_at<const be_i32>(dest_point_va);
    UfcNr10NoteResolve(dest_va, level, face, flags, r != nullptr, r ? i32(r->left) : 0,
                       r ? i32(r->top) : 0, r ? i32(r->right) : 0, r ? i32(r->bottom) : 0,
                       pt ? i32(pt[0]) : 0, pt ? i32(pt[1]) : 0, full ? full->width : 0,
                       full ? full->height : 0);
  }
  if (dest_va) {
    if (auto *dst = bd::gpu::SvrResolveTarget(dest_va, full, /*defer_fill=*/true)) {
      // NR-10g: the game binds a header, resolves into it, then draws sampling it (the CAF
      // composite's face, eyebrow and decal passes read the torso part they are built into).
      // SetTexture ran before the host target existed and left the slot on a mirror of the
      // memory (zeros, so a black face): point every slot bound to this header at the target.
      {
        // SvR07: device + (3072 + slot) * 4 = +0x3000 (SetTexture sub_821D5058; svr07_d3d.h kTexture;
        // RR 0x31A0, UFC 0x31F8). 26 slots: +0x3068 is the viewport (as RR-NR-7 found in RR).
        constexpr u32 kBoundTextureTable = svr07::d3d::kTexture;
        auto &vs = bd::gpu::state();
        for (u32 slot = 0; slot < svr07::d3d::kTextureSlots; ++slot) {
          if (bd::mem::load<u32>(device_guest + kBoundTextureTable + slot * 4u) == dest_va &&
              vs.textures[slot] != dst) {
            bd::gpu::Video::SetTexture(slot, dst);
            static std::atomic<u32> s_logs{0};
            if (s_logs.fetch_add(1, std::memory_order_relaxed) < 16)
              BD_INFO("native: slot {} was bound to 0x{:08X} before its resolve target existed: "
                      "rebound to the target",
                      slot, dest_va);
          }
        }
      }
      i32 dst_x = 0, dst_y = 0;
      if (const auto *pt = bd::mem::try_at<const be_i32>(dest_point_va)) {
        dst_x = bd::gpu::SvrScalePx(i32(pt[0]), dst);
        dst_y = bd::gpu::SvrScalePx(i32(pt[1]), dst);
      }
      if (!have_rect && full)
        rect = plume::RenderRect(0, 0, i32(full->width), i32(full->height));
      // The whole surface into a whole one-level 2D texture keeps the original path (it can alias).
      const bool whole = full && (src || !have_rect) && level == 0 && face == 0 &&
                         dst->mipLevels == 1 && rect.left == 0 && rect.top == 0 &&
                         rect.right == i32(full->width) && rect.bottom == i32(full->height) &&
                         dst_x == 0 && dst_y == 0 && dst->width == full->width &&
                         dst->height == full->height;
      if (!whole)
        NoteResolveEntry({u32(bd::gpu::SvrFrameIndex()), flags, full ? full->width : 0,
                          full ? full->height : 0, full ? u32(full->format) : 0,
                          full ? u32(full->sampleCount) : 0, dest_va, dst->width, dst->height,
                          u32(dst->format), dst->mipLevels, level, face, rect.left, rect.top,
                          rect.right, rect.bottom, dst_x, dst_y});
      // A 1x target and a scaled one: a 1:1 region copy would take the wrong part of the source.
      const bool mixed_scale = full && full->keep1x != dst->keep1x;
      if (mixed_scale) {
        static std::atomic<u32> s_mixed{0};
        if (s_mixed.fetch_add(1, std::memory_order_relaxed) < 8)
          BD_WARN("native: resolve between a 1x and a scaled target: src {}x{} -> 0x{:08X} {}x{}, "
                  "whole surface stretched", full->width, full->height, dest_va, dst->width,
                  dst->height);
      }
      // NR-12a: the host rect this resolve writes, when it is known exactly (the region path's
      // clamping in ResolveRegionToTexture), so a target's owed fill from guest memory leaves it out.
      // Anything else (stretched, depth, cube, the whole-copy bisect bit) gets the fill first.
      bool exact = false;
      plume::RenderRect written{};
      if (!mixed_scale && full && face == 0 && !(flags & bd::gpu::kResolveDepthStencil)) {
        const i32 lw = i32(std::max(1u, dst->width >> level));
        const i32 lh = i32(std::max(1u, dst->height >> level));
        if (whole) {
          exact = true;
          written = plume::RenderRect(0, 0, lw, lh);
        } else if (!(REXCVAR_GET(svr07_native_off) & 1)) {
          i32 sl = rect.left, st = rect.top, dx = dst_x, dy = dst_y;
          if (sl < 0) { dx -= sl; sl = 0; }
          if (st < 0) { dy -= st; st = 0; }
          if (dx < 0) { sl -= dx; dx = 0; }
          if (dy < 0) { st -= dy; dy = 0; }
          const i32 sr = std::min<i32>({rect.right, i32(full->width), sl + lw - dx});
          const i32 sb = std::min<i32>({rect.bottom, i32(full->height), st + lh - dy});
          exact = true;
          written = plume::RenderRect(dx, dy, dx + std::max(0, sr - sl), dy + std::max(0, sb - st));
        }
      }
      bd::gpu::SvrPrepareTargetResolve(dest_va, exact);
      // NR-CE1: with D3D9 pixel centres (PA_SU_VTX_CNTL.pix_center clear, SvR07 device+0x2940 bit 0)
      // geometry is shifted half a guest pixel right and down (constant_buffers.cpp). A full-target
      // quad then leaves the host pixels whose centres sit in the first half guest pixel uncovered:
      // ceil((scale - 1) / 2) of them, 1 at 2x and 3x. Their stale contents spread through the
      // bloom / luster chain into a bright band along the top and left of cutscenes at 4K.
      if (src && !(flags & bd::gpu::kResolveDepthStencil) && !src->keep1x &&
          !(REXCVAR_GET(svr07_native_off2) & 1) &&
          (bd::mem::load<u32>(device_guest + svr07::d3d::kPaSuVtxCntl) & 1u) == 0) {
        const float scale = bd::gpu::SvrScaleEdge(1024.0f, src) / 1024.0f;
        const u32 gap = scale > 1.0f ? u32(std::ceil((scale - 1.0f) * 0.5f - 1e-4f)) : 0u;
        if (gap)
          bd::gpu::Video::FillHalfPixelGap(rect, gap);
      }
      bool resolved = true;
      if (whole || mixed_scale || (REXCVAR_GET(svr07_native_off) & 1)) {
        bd::gpu::Video::TrackResolveSource(flags, dst, level, face);
        bd::gpu::Video::ResolveRtToTexture(dst);
      } else if (!bd::gpu::Video::ResolveRegionToTexture(flags, dst, rect, dst_x, dst_y, level,
                                                         face)) {
        resolved = false;
        static std::atomic<u32> s_fail{0};
        if (s_fail.fetch_add(1, std::memory_order_relaxed) < 8)
          BD_WARN("native: partial resolve {},{}-{},{} -> 0x{:08X} at {},{} not done", rect.left,
                  rect.top, rect.right, rect.bottom, dest_va, dst_x, dst_y);
      }
      if (resolved && exact)
        bd::gpu::SvrNoteTargetResolved(dest_va, level, written.left, written.top, written.right,
                                       written.bottom);
      if (resolved && !(flags & bd::gpu::kResolveDepthStencil)) {
        const auto *r = bd::mem::try_at<const bd::gpu::D3DRect>(src_rect_va);
        const auto *pt = bd::mem::try_at<const be_i32>(dest_point_va);
        bd::gpu::UfcWritebackNoteResolve(dest_va, dst, full, level, face, r != nullptr,
                                         r ? i32(r->left) : 0, r ? i32(r->top) : 0,
                                         r ? i32(r->right) : 0, r ? i32(r->bottom) : 0,
                                         pt ? i32(pt[0]) : 0, pt ? i32(pt[1]) : 0);
      }
      static const u32 s_probe_va =
          u32(std::strtoul(std::string(REXCVAR_GET(svr07_native_probe_clear)).c_str(), nullptr, 16));
      if (s_probe_va && dest_va == s_probe_va) {
        bd::gpu::SvrPrepareTargetResolve(dest_va, /*exact=*/false);
        bd::gpu::Video::ClearTexture(dst, 0xFF808080u);
      }
    } else {
      static std::atomic<u32> s_miss{0};
      if (s_miss.fetch_add(1, std::memory_order_relaxed) < 8)
        BD_WARN("D3DDevice_Resolve: destination guest VA 0x{:08X} not a host texture", dest_va);
    }
  }
  if ((flags & 0x300u) && !(REXCVAR_GET(svr07_native_off) & 2)) {
    u32 color_argb = 0;
    if (const auto *c = bd::mem::try_at<const be_f32>(clear_color_va)) {
      const auto pack = [](float v) -> u32 {
        return static_cast<u32>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
      };
      color_argb = (pack(c[3]) << 24) | (pack(c[0]) << 16) | (pack(c[1]) << 8) | pack(c[2]);
    }
    const u32 clear_flags = ((flags & 0x100u) ? bd::gpu::kClearTarget : 0u) |
                            ((flags & 0x200u) ? (bd::gpu::kClearZBuffer | bd::gpu::kClearStencil)
                                              : 0u);
    // ClearStencil: in the caller's frame (SvR07: sub_82444708 / Present stw +92(r1), as UFC).
    const u32 stencil = bd::mem::try_load<u32>(args.r1.u32 + svr07::d3d::kResolveStackStencil);
    if (have_rect)
      bd::gpu::Video::Clear(clear_flags, color_argb, float(args.f1.f64), stencil, &rect, 1);
    else
      bd::gpu::Video::Clear(clear_flags, color_argb, float(args.f1.f64), stencil);
  }
}

// SvR: the game's BeginVertices hands out space in its own command buffer; the vertices it writes
// there are drawn when EndVertices is reached, before the game's EndVertices submits them.
void SvrBeginVerticesObserved(PPCContext &args, u32 data_va, u8 *) {
  g_begin_vertices_pending = {};
  if (!data_va)
    return;
  g_begin_vertices_pending.device = args.r3.u32;
  g_begin_vertices_pending.primitive_type = args.r4.u32;
  g_begin_vertices_pending.vertex_count = args.r5.u32;
  g_begin_vertices_pending.stride = args.r6.u32;
  g_begin_vertices_pending.data_va = data_va;
}

// UFC links D3DDevice_BeginIndexedVertices (sub_823837A8; SvR 2010 doesn't): (dev, prim,
// BaseVertexIndex, NumVertices, IndexCount, IndexFormat, Stride, ppIndexData in r10,
// ppVertexData = the first stack argument, caller frame + 0x54). It allocates both blocks, stores
// their addresses through the two out pointers and returns 0; the game fills them and calls
// EndVertices like after BeginVertices. IndexFormat bit 2 = INDEX32 (D3DFMT_INDEX32 6, INDEX16 1).
void UfcBeginIndexedVerticesObserved(PPCContext &args, u32 result, u8 *) {
  g_begin_vertices_pending = {};
  if (result != 0)
    return;
  const u32 index_va = bd::mem::try_load<u32>(args.r10.u32);
  const u32 vertex_va = bd::mem::try_load<u32>(bd::mem::try_load<u32>(args.r1.u32 + 0x54));
  if (!index_va || !vertex_va || !args.r7.u32)
    return;
  g_begin_vertices_pending.device = args.r3.u32;
  g_begin_vertices_pending.primitive_type = args.r4.u32;
  g_begin_vertices_pending.base_vertex = static_cast<i32>(args.r5.u32);
  g_begin_vertices_pending.vertex_count = args.r6.u32;
  g_begin_vertices_pending.index_count = args.r7.u32;
  g_begin_vertices_pending.index32 = (args.r8.u32 & 4u) != 0;
  g_begin_vertices_pending.stride = args.r9.u32;
  g_begin_vertices_pending.data_va = vertex_va;
  g_begin_vertices_pending.index_va = index_va;
}
#endif

} // namespace

#if defined(SVR_NATIVE_RENDERER)
void UfcDrawRecorded(uint32_t dev, bool indexed, uint32_t prim, uint32_t base_vertex, uint32_t start,
                     uint32_t count) {
  if (indexed)
    D3DDevice_DrawIndexedVertices_hook(dev, prim, base_vertex, start, count);
  else
    D3DDevice_DrawVertices_hook(dev, prim, start, count);
}
#endif

#if defined(SVR_NATIVE_RENDERER)
namespace bd::gpu {
// The partial resolves leading up to a device loss, oldest first (gpu/device_lost.cpp).
void SvrLogRecentResolves() {
  std::lock_guard lock(g_resolve_log_mutex);
  const u32 size = u32(g_resolve_log_recent.size());
  const u32 kept = std::min(g_resolve_log_count, size);
  for (u32 i = g_resolve_log_count - kept; i != g_resolve_log_count; ++i)
    LogResolveEntry("recent resolve", g_resolve_log_recent[i % size]);
}
} // namespace bd::gpu

// SvR: the renderer draws from the state the game's draw call is about to consume, then the game's
// own D3D function runs (see src/native/ufc_hook.h).
namespace {
// NR-P6: the game's draw starts by copying everything marked in the device's five dirty masks
// (+0x00..+0x27) into the ring: constants, registers, fetch constants, shader binds. Only the null
// GPU reads that; the hooks read the device's own copies, which the setters write. Clearing the
// masks first is what the flush itself ends with, so the rest of the function runs as usual. Not
// while recording a command buffer: there the ring is the recording.
void UfcDropGuestFlush(u32 device_guest, u32 bit) {
  static const u32 s_skip = [] {
    u32 skip = u32(REXCVAR_GET(svr07_native_guest_skip));
    const i32 up = REXCVAR_GET(svr07_native_guest_skip_up);
    if (up > 0 || (up < 0 && REXCVAR_GET(svr07_high_fps_hz) > 60.0))
      skip |= 4u;
    BD_INFO("native: guest D3D state flush skipped for{}{}{} (svr07_native_guest_skip {}, "
            "svr07_native_guest_skip_up {})",
            (skip & 1u) ? " DrawIndexedVertices" : "", (skip & 2u) ? " DrawVertices" : "",
            (skip & 4u) ? " BeginVertices" : "", REXCVAR_GET(svr07_native_guest_skip), up);
    return skip;
  }();
#if defined(SVR07_NATIVE_RENDERER)
  // SvR07 (XDK 2.0.3215.1): the five dirty masks at +0x00..+0x27, as UFC (the draws ld +0/+8/+16/
  // +24/+32 and clear each after its flush: DrawIndexedVertices sub_821D9D78), the ring at +0x28.
  // SVR07-CENSUS-1 (2026-11-11, laptop, emulated, title/menus/1v1 match): 0 INDIRECT_BUFFER packets in hooked ranges and in the gaps between them, share 0.00%: confirmed. Was: no command-buffer recording state found (NR-1: none expected), so the skip is not gated on one | census: command-buffer share stays 0.00% (no INDIRECT_BUFFER in draw ranges)
  if (!(s_skip & bit))
    return;
  if (auto *masks = bd::mem::at<u8>(device_guest + svr07::d3d::kDirtyMasks))
    std::memset(masks, 0, svr07::d3d::kDirtyMaskCount * sizeof(uint64_t));
#else
  if (!(s_skip & bit) || bd::mem::load<u32>(device_guest + UFC3_D3D_DEVICE_RECORDING_CB))
    return;
  if (auto *masks = bd::mem::at<u8>(device_guest))
    std::memset(masks, 0, 5 * sizeof(uint64_t));
#endif
}
u32 UfcDrawVerticesObserved(u32 device_guest, u32 primitiveType, u32 startVertex, u32 vertexCount) {
  const u32 result =
      D3DDevice_DrawVertices_hook(device_guest, primitiveType, startVertex, vertexCount);
  UfcDropGuestFlush(device_guest, 2);
  return result;
}
u32 UfcDrawIndexedVerticesObserved(u32 device_guest, u32 primitiveType, u32 baseVertexIndex,
                                   u32 startIndex, u32 indexCount) {
  UfcNr10NoteDraw(device_guest, primitiveType, indexCount, /*after=*/false);
  const u32 result = D3DDevice_DrawIndexedVertices_hook(device_guest, primitiveType,
                                                        baseVertexIndex, startIndex, indexCount);
  UfcNr10NoteDraw(device_guest, primitiveType, indexCount, /*after=*/true);
  UfcDropGuestFlush(device_guest, 1);
  return result;
}
}  // namespace
#if !defined(SVR_NO_DRAW_VERTICES)  // SvR 2009 links no DrawVertices (svr_d3d_names.h)
SVR_HOOK_BEFORE(D3DDevice_DrawVertices, UfcDrawVerticesObserved);
#endif
SVR_HOOK_BEFORE(D3DDevice_DrawIndexedVertices, UfcDrawIndexedVerticesObserved);
// SVR_HOOK_RESULT plus L14's flush skip before the game's function (bit 4, see
// svr07_native_guest_skip_up): the draw itself is made at EndVertices from the device's own copies
// of the state, which the setters write; the masks only steer the copy into the null GPU's ring.
REX_EXTERN(__imp__D3DDevice_BeginVertices);
extern "C" REX_FUNC(D3DDevice_BeginVertices) {
  SVR_HOOK_EMULATED(D3DDevice_BeginVertices)
  PPCContext svr_args;
  ::svr::CopyArgs(svr_args, ctx);
  UfcDropGuestFlush(ctx.r3.u32, 4);
  {
    bd::gpu::SvrZone svr_zone(bd::gpu::SvrZoneId::kGuestD3D);
    __imp__D3DDevice_BeginVertices(ctx, base);
  }
  bd::gpu::SvrZone svr_zone(bd::gpu::SvrZoneId::kHookState);
  SvrBeginVerticesObserved(svr_args, ctx.r3.u32, base);
}
#if !defined(SVR07_NATIVE_RENDERER)  // SvR07 XDK 2.0.3215.1 has no BeginIndexedVertices (svr07_d3d_names.h)
SVR_HOOK_RESULT(D3DDevice_BeginIndexedVertices, UfcBeginIndexedVerticesObserved);
#endif
SVR_HOOK_BEFORE(D3DDevice_EndVertices, D3DDevice_EndVertices_hook);
SVR_HOOK_RESULT(D3DDevice_Resolve, UfcResolveObserved);
#else
REX_HOOK(D3DDevice_DrawVertices, D3DDevice_DrawVertices_hook);
REX_HOOK(D3DDevice_DrawVerticesUP, D3DDevice_DrawVerticesUP_hook);
REX_HOOK(D3DDevice_DrawIndexedVertices, D3DDevice_DrawIndexedVertices_hook);
REX_HOOK(D3DDevice_BeginVertices, D3DDevice_BeginVertices_hook);
REX_HOOK(D3DDevice_EndVertices, D3DDevice_EndVertices_hook);
REX_HOOK(D3DDevice_Resolve, D3DDevice_Resolve_hook);
REX_HOOK(D3DDevice_BeginTiling, D3DDevice_BeginTiling_hook);
REX_HOOK(D3DDevice_EndTiling, D3DDevice_EndTiling_hook);
#endif
