/**
 * @file    gpu/shaders/guest_shaders.cpp
 * @brief   Turning a guest shader into a host one: cache lookup (UFC 3: from
 *          shader_cache.bin, read at run time) and spec constant linking.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/device.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h>

#include <plume_render_interface.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>

REXCVAR_DECLARE(i32, svr07_native_off);
REXCVAR_DECLARE(double, svr07_high_fps_hz);
REXCVAR_DEFINE_INT32(svr07_native_cb_exact_arrays, -1, "Native",
                     "Native renderer (fps, L16): upload a shader's constant arrays at their declared "
                     "size when its microcode has no a0/aL constant read (otherwise every array runs "
                     "to register 255). -1 = auto (on in the high-fps mode), 0 = off, 1 = on");
#include <rex/hash.h>

#include "core/logging.h"
#include "gpu/backend.h"
#include "gpu/host_resource_heap.h"
#include "gpu/pipeline/pso_recorder.h"
#include "gpu/shaders/shader_cache_file.h"
#include "gpu/shaders/shader_linker.h"

REXCVAR_DEFINE_STRING(svr07_shader_cache, "shader_cache.bin", "Native",
                      "Native renderer: the shader cache built from the game files (relative = "
                      "beside the exe). The prelinked variants are read from "
                      "<name>_linked.bin next to it.");

namespace bd::gpu {

namespace {
namespace scf = shader_cache_file;

// UFC 3: the cache is two files built on the player's machine (shader_cache.h),
// read once on the first shader create. A missing or bad file leaves the cache
// empty, so every shader logs a miss and its draws are dropped.
std::once_flag g_cache_once;
scf::Cache g_cache;
scf::LinkedCache g_linked_cache;

void LoadShaderCache() {
  std::call_once(g_cache_once, [] {
    std::filesystem::path path = std::filesystem::u8path(REXCVAR_GET(svr07_shader_cache));
    if (path.is_relative())
      path = rex::filesystem::GetExecutableFolder() / path;
    std::string error;
    if (!scf::Load(path, g_cache, error)) {
      BD_ERROR("native: shader cache {}: {}", rex::path_to_utf8(path), error);
      g_cache = {};
      return;
    }
    std::filesystem::path linked = path;
    linked.replace_filename(path.stem().wstring() + L"_linked" + path.extension().wstring());
    if (!scf::LoadLinked(linked, g_cache.fileHash, g_linked_cache, error)) {
      BD_WARN("native: prelinked shaders {}: {} (spec-constant shaders link at run time)",
              rex::path_to_utf8(linked), error);
      g_linked_cache = {};
    }
    BD_INFO("native: shader cache {} ({} shaders, {} prelinked variants, xxh3 {:016X})",
            rex::path_to_utf8(path), g_cache.entries.size(), g_linked_cache.entries.size(),
            g_cache.fileHash);
  });
}

ShaderCacheEntry *FindShaderCacheEntry(u64 hash) {
  LoadShaderCache();
  return scf::Find(g_cache, hash);
}

// Guards GuestShader::shader / linkedShaders lookups+inserts only. DXC link
// and createShader run unlocked so precache workers compile in parallel.
std::mutex g_shader_link_mutex;

// X360 exports BD's bloom bright mask into an LDR EDRAM tile, saturating it at
// 1.0 before the gaussian blur, but reblue's FP16 posteff chain keeps the raw
// mask (wc01 authors BLOOM BriMulti=50 -> exports ~59) and the blur spreads it
// into giant white blobs. These two masks are substituted by copies whose
// export is clamped to [0,1].
// SvR 2008: no shader substitutions yet (re:Blue swaps in clamped copies of Blue Dragon's
// bloom bright-pass shaders here, because FP16 render targets don't saturate like EDRAM).
bool BloomMaskClampBlob(u64, const void *&, size_t &) { return false; }

// Publishes the first shader built for gs, a concurrent builder's copy is
// dropped before any caller can observe it.
plume::RenderShader *PublishShader(GuestShader *gs,
                                   std::unique_ptr<plume::RenderShader> sh) {
  std::lock_guard lock(g_shader_link_mutex);
  if (!gs->shader)
    gs->shader = std::move(sh);
  return gs->shader.get();
}

// XenosRecomp's constant_table.h, the parts ReadConstantUse needs.
struct GuestConstantInfo {
  be_u32 name;
  be_u16 registerSet;
  be_u16 registerIndex;
  be_u16 registerCount;
  be_u16 reserved;
  be_u32 typeInfo;
  be_u32 defaultValue;
};
static_assert(sizeof(GuestConstantInfo) == 20);
constexpr u32 kRegisterSetFloat4 = 2;
constexpr u32 kConstantTableBase = 4;  // past the container's size word; constantInfo counts from here
constexpr u32 kConstantTableCountOffset = kConstantTableBase + 12;
constexpr u32 kConstantTableInfoOffset = kConstantTableBase + 16;

// L16: what the microcode says about the two things the upload path has to be conservative about.
// The walk is XenosRecomp's analyzeVertexIndexUse (shader_recompiler.cpp), on the same bytes it
// read: control flow pairs at the start of the microcode, each exec naming `count` instructions
// at `address` (3 dwords each), the sequence bits telling fetch (1) from ALU (0). Bit fields are
// decoded by hand with the layouts of shader_code.h.
//  - relative: some ALU instruction has const0Relative / const1Relative set (dword 1 bits 31/30),
//    i.e. may read a constant at a0/aL + n. A superset of what the recompiler indexes by a0/aL.
//  - vertex_id: the recompiler's usesVertexId (r0.x read before written, or a computed fetch).
// Returns false when the microcode can't be located or a field points outside it.
bool ScanMicrocode(const be_u32 *function, bool &relative, bool &vertex_id) {
  relative = true;
  vertex_id = true;
  const auto *bytes = reinterpret_cast<const u8 *>(function);
  const auto *container = reinterpret_cast<const ShaderContainer *>(function);
  const u32 magic = u32(container->flags) & 0xFFFFFF00u;
  const u32 virt = container->virtualSize;
  const u32 phys = container->physicalSize;
  u32 code_at = 0, size = 0;
  if (magic == 0x102A0E00u) {
    // 0E (SvR07, RR): the physical section is the microcode (main.cpp upgradeContainer0E keeps
    // it whole as physicalOffset 0, size physicalSize).
    code_at = virt;
    size = phys;
  } else if (magic == 0x102A1100u) {
    const u32 shader_at = container->shaderOffset;
    if (u64(shader_at) + 8 > virt)
      return false;
    const u32 phys_off = *reinterpret_cast<const be_u32 *>(bytes + shader_at);
    size = *reinterpret_cast<const be_u32 *>(bytes + shader_at + 4);
    if (u64(phys_off) + size > phys)
      return false;
    code_at = virt + phys_off;
  } else {
    return false;
  }
  if (size < 12 || (size % 4) != 0)
    return false;
  const auto *code = reinterpret_cast<const be_u32 *>(bytes + code_at);
  const u32 words = size / 4;

  bool any_relative = false;
  bool uses_vertex_id = false;
  bool computed_fetch = false;
  bool index_written = false;
  const auto reads_r0x = [](u32 reg, u32 select, u32 swizzle) {
    if (select == 0 || (reg & 0x3F) != 0)
      return false;
    for (u32 k = 0; k < 4; ++k) {
      if ((((swizzle >> (k * 2)) + k) & 0x3) == 0)
        return true;
    }
    return false;
  };
  u32 cf_size = size;
  for (u32 at = 0; at < cf_size; at += 12) {
    const u32 w0 = code[at / 4], w1 = code[at / 4 + 1], w2 = code[at / 4 + 2];
    const u32 cf[2][2] = {{w0, w1 & 0xFFFFu}, {(w1 >> 16) | (w2 << 16), w2 >> 16}};
    for (const auto &c : cf) {
      const u32 opcode = (c[1] >> 12) & 0xFu;
      switch (opcode) {
      case 1: case 2: case 3: case 4: case 5: case 6: case 13: case 14:
        break;  // Exec, ExecEnd, CondExec(End), CondExecPred(End), CondExecPredClean(End)
      default:
        continue;
      }
      const u32 address = c[0] & 0xFFFu;
      const u32 count = (c[0] >> 12) & 0x7u;
      u32 sequence = (c[0] >> 16) & 0xFFFu;
      if (address != 0)
        cf_size = std::min<u32>(cf_size, address * 12);
      if (u64(address + count) * 3 > words)
        return false;
      for (u32 i = 0; i < count; ++i, sequence >>= 2) {
        const u32 f0 = code[(address + i) * 3], f1 = code[(address + i) * 3 + 1],
                  f2 = code[(address + i) * 3 + 2];
        if (sequence & 0x1u) {
          const u32 fetch_op = f0 & 0x1Fu;
          const u32 dst = (f0 >> 12) & 0x3Fu;
          const u32 dst_swizzle = f1 & 0xFFFu;
          if (fetch_op == 0) {  // VertexFetch
            const u32 src = (f0 >> 5) & 0x3Fu;
            const u32 src_swizzle = (f0 >> 30) & 0x3u;
            if (src != 0 || src_swizzle != 0 || index_written)
              computed_fetch = true;
          }
          if (dst == 0 && (dst_swizzle & 0x7u) != 0x7u)
            index_written = true;
        } else {
          if ((f1 >> 30) & 0x3u)
            any_relative = true;
          if (!index_written) {
            const u32 src3_sw = f1 & 0xFFu, src2_sw = (f1 >> 8) & 0xFFu, src1_sw = (f1 >> 16) & 0xFFu;
            const u32 src3_reg = f2 & 0xFFu, src2_reg = (f2 >> 8) & 0xFFu, src1_reg = (f2 >> 16) & 0xFFu;
            const u32 src3_sel = (f2 >> 29) & 1u, src2_sel = (f2 >> 30) & 1u, src1_sel = (f2 >> 31) & 1u;
            if (reads_r0x(src1_reg, src1_sel, src1_sw) || reads_r0x(src2_reg, src2_sel, src2_sw) ||
                reads_r0x(src3_reg, src3_sel, src3_sw))
              uses_vertex_id = true;
          }
          const u32 vector_dest = f0 & 0x3Fu;
          const u32 scalar_dest = (f0 >> 8) & 0x3Fu;
          const bool export_data = ((f0 >> 15) & 1u) != 0;
          const u32 vector_mask = (f0 >> 16) & 0xFu;
          const u32 scalar_mask = (f0 >> 20) & 0xFu;
          if (!export_data && ((vector_dest == 0 && (vector_mask & 1u)) ||
                               (scalar_dest == 0 && (scalar_mask & 1u))))
            index_written = true;
        }
      }
    }
  }
  relative = any_relative;
  vertex_id = uses_vertex_id || computed_fetch;
  return true;
}

// L16: constant arrays end at their declared size when the shader can't index past it.
// -1 = auto (on while svr07_high_fps_hz is above 60, so stock 60 keeps the old upload), 0 off, 1 on.
bool ExactArraysOn() {
  static const bool on = [] {
    const i32 v = REXCVAR_GET(svr07_native_cb_exact_arrays);
    const bool r = v > 0 || (v < 0 && REXCVAR_GET(svr07_high_fps_hz) > 60.0);
    BD_INFO("native: constant arrays without a0/aL reads uploaded at their declared size: {} "
            "(svr07_native_cb_exact_arrays {})",
            r ? "on" : "off", v);
    return r;
  }();
  return on;
}

GuestConstantUse ReadConstantUse(const be_u32 *function) {
  GuestConstantUse use;
  {
    bool relative = true, vertex_id = true;
    const bool scanned = ScanMicrocode(function, relative, vertex_id);
    if (scanned) {
      use.relativeConstants = relative;
      use.readsVertexId = vertex_id;
    }
    // How many shaders the scan read, how many index constants, how many read the vertex index.
    static std::atomic<u32> s_total{0}, s_unread{0}, s_rel{0}, s_vid{0};
    const u32 n = s_total.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!scanned)
      s_unread.fetch_add(1, std::memory_order_relaxed);
    if (use.relativeConstants)
      s_rel.fetch_add(1, std::memory_order_relaxed);
    if (use.readsVertexId)
      s_vid.fetch_add(1, std::memory_order_relaxed);
    if ((n & (n - 1)) == 0 || (n % 512) == 0)
      BD_INFO("native: microcode scan: {} shaders, {} unread, {} with a0/aL constant reads, {} "
              "may read the vertex index",
              n, s_unread.load(std::memory_order_relaxed), s_rel.load(std::memory_order_relaxed),
              s_vid.load(std::memory_order_relaxed));
  }
  const auto *container = reinterpret_cast<const ShaderContainer *>(function);
  const u32 table_at = container->constantTableOffset;
  const u32 limit = container->virtualSize;
  if (!table_at || table_at + kConstantTableInfoOffset + 4 > limit)
    return use;
  const auto *bytes = reinterpret_cast<const u8 *>(function);
  const u32 count = *reinterpret_cast<const be_u32 *>(bytes + table_at + kConstantTableCountOffset);
  const u32 info_at = u32(*reinterpret_cast<const be_u32 *>(bytes + table_at + kConstantTableInfoOffset)) +
                      table_at + kConstantTableBase;
  if (count > limit / sizeof(GuestConstantInfo) ||
      info_at + count * u32(sizeof(GuestConstantInfo)) > limit)
    return use;
  const auto *infos = reinterpret_cast<const GuestConstantInfo *>(bytes + info_at);

  bool declared[256] = {};
  for (u32 i = 0; i < count; ++i) {
    if (u32(infos[i].registerSet) != kRegisterSetFloat4)
      continue;
    const u32 first = infos[i].registerIndex;
    u32 end = std::min(first + u32(infos[i].registerCount), 256u);
    // An array can be read with a0/aL past its declared count: XenosRecomp lets NAME(INDEX) run
    // to register 255, as the hardware's constant file does. UFC's skinning shaders read
    // g_fBruiseLv[a0] with indices past the table; left out, those registers held whatever the
    // upload ring had (select screen 2026-11-07: a dark torn patch on a fighter's cheek and a
    // streak on a shin, native only, gone with every register uploaded). svr07_native_off bit
    // 134217728: declared counts only, as before.
    // L16: a shader with no a0/aL constant read can only name registers inside the table (the
    // recompiler indexes arrays with literals there), so the tail is left out in the high-fps mode.
    if (infos[i].registerCount > 1 && !(REXCVAR_GET(svr07_native_off) & 134217728) &&
        (use.relativeConstants || !ExactArraysOn()))
      end = 256u;
    for (u32 r = first; r < end; ++r)
      declared[r] = true;
  }

  // A gap of a few registers costs less than another compare and copy.
  for (u32 gap = 2;; gap *= 2) {
    use.rangeCount = 0;
    bool fits = true;
    for (u32 r = 0; r < 256 && fits; ++r) {
      if (!declared[r])
        continue;
      if (use.rangeCount) {
        auto &last = use.ranges[use.rangeCount - 1];
        if (r <= u32(last.first) + last.count + gap) {
          last.count = u16(r + 1 - last.first);
          continue;
        }
      }
      if (use.rangeCount == GuestConstantUse::kMaxRanges)
        fits = false;
      else
        use.ranges[use.rangeCount++] = {u16(r), 1};
    }
    if (fits)
      break;
  }
  for (u32 i = 0; i < use.rangeCount; ++i) {
    const auto &range = use.ranges[i];
    for (u32 r = range.first; r < u32(range.first) + range.count; ++r)
      use.mask[r / 64] |= u64(1) << (r % 64);
    use.registerEnd = u16(range.first + range.count);
  }
  use.known = true;
  return use;
}

} // namespace

GuestShader *CreateShader(const be_u32 *function, ResourceType type) {
  // The cache key hashes the whole container, matching how XenosRecomp keyed
  // the entries it generated.
  const auto *container = reinterpret_cast<const ShaderContainer *>(function);
  // SvR07 (L2 request): only 0x102A0E0x (SvR07 / RR) and 0x102A110x containers are keyed. A bare
  // XDK microcode blob (SvR07's 108-byte VS at 0x820169B8: "virtualSize" 0x1200, "physicalSize"
  // 0xC4000000) would hash gigabytes of guest memory: give it an entry-less shader (its draws drop,
  // as for any miss).
  const u32 magic = u32(container->flags) & 0xFFFFFF00u;
  const u64 container_len = u64(u32(container->virtualSize)) + u32(container->physicalSize);
  if ((magic != 0x102A0E00u && magic != 0x102A1100u) || container_len > (1u << 20)) {
    BD_WARN("native: shader miss (not a container: flags {:08X} len {}) type {}",
            u32(container->flags), container_len, static_cast<u32>(type));
    return HostResourceHeap::Alloc<GuestShader>(type);
  }
  const u32 hash_len = u32(container_len);
  const u64 hash = XXH3_64bits(function, hash_len);
  ShaderCacheEntry *entry = FindShaderCacheEntry(hash);
  auto *shader = HostResourceHeap::Alloc<GuestShader>(type);
  if (entry) {
    if (entry->guestShader == nullptr) {
      entry->guestShader = shader;
    }
    shader->shaderCacheEntry = entry;
    shader->constantUse = ReadConstantUse(function);
    if (!shader->constantUse.known)
      BD_WARN("native: shader {:016X} has no readable constant table, uploads all registers", hash);
  } else {
    BD_WARN("native: shader miss {:016X} (len {} type {})", hash, hash_len,
            static_cast<u32>(type));
  }
  // Let boot cache replay enqueue any pending PSO that was waiting on this
  // shader's microcode hash now that its host object exists.
  OnShaderCreated(hash);
  return shader;
}

plume::RenderShader *GetOrLinkShader(GuestShader *gs, u32 specConstants) {
  if (!gs)
    return nullptr;
  // hcg*ShaderCreateByHlsl path: shader populated directly, no cache entry.
  if (gs->shader)
    return gs->shader.get();
  const ShaderCacheEntry *entry = gs->shaderCacheEntry;
  if (!entry)
    return nullptr;
  auto *device = Video::HostDevice();
  if (!device)
    return nullptr;

  const void *clampBlob = nullptr;
  size_t clampSize = 0;
  if (BloomMaskClampBlob(entry->hash, clampBlob, clampSize)) {
    return PublishShader(gs, device->createShader(clampBlob, clampSize, "main",
                                                  kHostShaderFormat));
  }

  const u8 *cache = g_cache.dxil.data();

  const u32 masked = specConstants & entry->specConstantsMask;
  if (entry->specConstantsMask != 0) {
    std::lock_guard lock(g_shader_link_mutex);
    auto it = gs->linkedShaders.find(masked);
    if (it != gs->linkedShaders.end())
      return it->second.get();
  }

  // Miss: build outside the lock. DXC (per-call instances) and D3D12 shader
  // creation are free-threaded, so precache workers link in parallel and a
  // render thread miss waits only on its own build.
  if (entry->specConstantsMask == 0) {
    // The cache DXIL is already a complete vs/ps.
    return PublishShader(
        gs, device->createShader(cache + entry->dxilOffset, entry->dxilSize,
                                 "main", plume::RenderShaderFormat::DXIL));
  }

  // Spec constant shaders are DXIL libraries with an unresolved
  // g_SpecConstants() export. svr07_prelink bakes every mask subset of every
  // cached shader into shader_cache_linked.bin, so the pre-linked lookup is
  // the norm. The runtime DXC link below only covers a missing or stale
  // linked file.
  std::unique_ptr<plume::RenderShader> sh;
  const LinkedCacheFileEntry *pre =
      scf::FindLinked(g_linked_cache, entry->hash, masked);
  if (pre) {
    sh = device->createShader(g_linked_cache.dxil.data() + pre->dxilOffset,
                              pre->dxilSize, "main",
                              plume::RenderShaderFormat::DXIL);
  } else {
    BD_WARN("GetOrLinkShader: no prelinked variant, runtime DXC link "
            "(hash=0x{:016X} mask=0x{:X})",
            entry->hash, masked);
    std::vector<u8> linked =
        LinkSpecConstant(cache + entry->dxilOffset, entry->dxilSize,
                         gs->type == ResourceType::PixelShader, masked);
    if (linked.empty())
      return nullptr;
    sh = device->createShader(linked.data(), linked.size(), "main",
                              plume::RenderShaderFormat::DXIL);
  }
  std::lock_guard lock(g_shader_link_mutex);
  auto [it, inserted] = gs->linkedShaders.try_emplace(masked, std::move(sh));
  return it->second.get();
}

GuestShader *FindGuestShaderByHash(u64 hash) {
  ShaderCacheEntry *entry = FindShaderCacheEntry(hash);
  if (!entry || !entry->guestShader)
    return nullptr;
  return entry->guestShader;
}

} // namespace bd::gpu
