/**
 * @file    gpu/constant_buffers.cpp
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/constant_buffers.h"

#include "ufc_frame_diag.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <iterator>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <plume_render_interface_builders.h>
#include <rex/cvar.h>
#include <rex/graphics/xenos.h>
#include <rex/runtime.h>
#include <rex/types.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "core/profiling.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/hooks/tweaks.h"
#include "gpu/sampler_cache.h"
#include "gpu/settings.h"
#include "gpu/shaders/shader_cache.h"

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

REXCVAR_DECLARE(i32, svr07_native_off);
REXCVAR_DECLARE(i32, svr07_native_off2);
REXCVAR_DEFINE_BOOL(svr07_native_check_constants, false, "Native",
                    "Native renderer dev check: fill the constant registers a shader doesn't declare "
                    "with 1e9 (a shader reading one shows garbage), and rebuild all texture slots "
                    "after the partial rebuild, logging any difference");
REXCVAR_DEFINE_INT32(svr07_native_skin_black_slots, 0, "Native",
                     "Native renderer dev switch: mask of texture slots that read black in the fighter "
                     "skin pixel shaders that use the liquid map (16384 = t14), for bisecting");
REXCVAR_DEFINE_BOOL(svr07_native_skin_no_hdlig, false, "Native",
                    "Native renderer dev switch: fighter skin pixel shaders read g_f4HdLig (c156) as "
                    "30,30, which turns the overbright term off, for bisecting");

namespace bd::gpu {

namespace {

constexpr u32 kCBVAlignment = 256;

constexpr u32 kUploadChunkSize = 16 * 1024 * 1024;

// 256 vector4f registers = 4 KiB. Shaders reference the full window, so the
// upload spans the whole range every flush.
constexpr u32 kConstantRegisterCount = 256;
constexpr u32 kConstantBlockBytes = kConstantRegisterCount * 16;

struct UploadChunk {
  std::unique_ptr<plume::RenderBuffer> buffer;
  u8 *mapped = nullptr;
  u64 gpuBase = 0;
};

// One upload chunk list per in-flight frame slot, rewound only after that
// slot's GPU fence is awaited, so the GPU never reads bytes the CPU has
// overwritten.
struct FrameUpload {
  std::vector<UploadChunk> chunks;
  u32 chunkIndex = 0;
  u32 chunkOffset = 0;
  u32 peakChunkCount = 0;
};

// DecodeFromFetch + ResolveSlotLocked (mutex + hash lookup) run per bound slot
// on EVERY draw, so the slot remembers the sampler it resolved for the fetch
// constant's sampler fields. Keyed on just those fields (DecodeFromFetch reads
// nothing else): the full constant also holds the texture's address and size,
// which change with every texture (a crowd frame binds hundreds) while the
// sampler state stays the same. Sampler heap slots are never reclaimed, so a
// cached index stays valid until device teardown.
struct SamplerSlotCache {
  u32 key = 0;
  u32 sampler = 0;
  i32 aniso = -1;
  bool clamp3d = false;
  bool valid = false;
};

// The fetch constant fields DecodeFromFetch reads, packed.
u32 SamplerFieldsKey(const u32 fc[6]) {
  rex::graphics::xenos::xe_gpu_texture_fetch_t f;
  std::memcpy(&f, fc, sizeof(f));
  return u32(f.clamp_x) | u32(f.clamp_y) << 3 | u32(f.clamp_z) << 6 |
         u32(f.mag_filter) << 9 | u32(f.min_filter) << 11 |
         u32(f.mip_filter) << 13 | u32(f.border_color) << 15;
}

// The last vertex/pixel constants uploaded on this command list, as the guest
// stored them. SvR uploads both stages every draw (inline constant writes have
// no hook), but consecutive draws mostly agree on what they read, so unchanged
// registers keep the binding they have. Only the registers in boundMask are in
// the bound upload; raw is stale outside them.
struct StageConstantCache {
  alignas(64) u8 raw[256 * 16];
  u64 boundMask[4] = {};
  bool valid = false;
  bool pinned = false;
};

// Set by whoever changes what a texture slot resolves to, taken by UploadSharedConstants.
std::atomic<u32> g_texture_slots_dirty{~u32{0}};
constexpr u32 kAllTextureSlots =
    kTextureSlots >= 32 ? ~u32{0} : (u32{1} << (kTextureSlots & 31)) - 1;

struct UploadState {
  FrameUpload frames[kNumFrames];
  u32 cursor = 0;
  u64 epoch = 1;  // bumped by every ResetFrame
  SharedConstants shared{};
  SharedConstants lastUploaded{};
  SamplerSlotCache samplerSlots[kTextureSlots];
  StageConstantCache vsConstants, psConstants;
  // What the texture slots of 'shared' were built against.
  DeviceFetchConstant slotFetch[32]{};
  const GuestTexture *slotRenderTarget = nullptr;
  const GuestTexture *slotDepthStencil = nullptr;
  i32 slotAniso = -1;
  u64 slotChecks = 0;
  u64 slotCheckMisses = 0;
  float shadowPcfScale = 1.0f;
  bool sharedBound = false;
  bool ready = false;
  bool vfetchTableClear = false;  // L14: shared.vfetchElements all zero (FillComputedFetchTable)
};

UploadState &upload_state() {
  static UploadState s;
  return s;
}

void RecomputeShadowPcfScale(UploadState &s) {
  const f64 dist = std::clamp(ShadowCoverageScale(), 1.0, 4.0);
  const f64 dim = std::max(512, Settings::Get().ShadowDimension());
  s.shadowPcfScale = static_cast<float>(std::max(1.0 / dist, 1024.0 / dim));
}

bool CreateChunkNow(UploadChunk &chunk) {
  auto *device = bd::gpu::Video::HostDevice();
  if (!device)
    return false;
  chunk.buffer = bd::gpu::CreateHostBuffer(
      device,
      plume::RenderBufferDesc::UploadBuffer(
          kUploadChunkSize, plume::RenderBufferFlag::CONSTANT |
                                plume::RenderBufferFlag::VERTEX |
                                plume::RenderBufferFlag::INDEX |
                                plume::RenderBufferFlag::DEVICE_ADDRESSABLE),
      "cb-upload-chunk");
  if (!chunk.buffer) {
    BD_ERROR("constant_buffers: createBuffer({} MiB chunk) failed",
             kUploadChunkSize / (1024 * 1024));
    return false;
  }
  chunk.mapped = reinterpret_cast<u8 *>(chunk.buffer->map());
  if (!chunk.mapped) {
    BD_ERROR("constant_buffers: RenderBuffer::map() returned null");
    chunk.buffer.reset();
    return false;
  }
  chunk.gpuBase = chunk.buffer->getDeviceAddress();
  return true;
}

// NR-14d: a load frame that outgrows its slot's chunks made each new 16 MB upload buffer on the
// frame thread (4K career load, 286 new textures: 40.5 ms of mirror uploads, nr14d-c3). A few
// chunks are kept made ahead by a background thread; svr07_native_off bit 268435456 = inline again.
// Never destroyed: the spares outlive the device's teardown order, like the chunks in use.
struct ChunkSpares {
  std::mutex mutex;
  std::condition_variable cv;
  std::vector<UploadChunk> ready;
  bool started = false;
};
constexpr size_t kChunkSpares = 4;

ChunkSpares &chunk_spares() {
  static auto *spares = new ChunkSpares;
  return *spares;
}

void ChunkSpareThread() {
  auto &sp = chunk_spares();
  for (;;) {
    {
      std::unique_lock lock(sp.mutex);
      sp.cv.wait(lock, [&sp] { return sp.ready.size() < kChunkSpares; });
    }
    UploadChunk chunk;
    if (!CreateChunkNow(chunk)) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      continue;
    }
    // Touch every page here, so the frame thread's first copy into it doesn't take the faults.
    std::memset(chunk.mapped, 0, kUploadChunkSize);
    std::lock_guard lock(sp.mutex);
    sp.ready.push_back(std::move(chunk));
  }
}

bool CreateChunk(UploadChunk &chunk) {
  if (!(REXCVAR_GET(svr07_native_off) & 268435456)) {
    auto &sp = chunk_spares();
    std::unique_lock lock(sp.mutex);
    if (!sp.started) {
      sp.started = true;
      std::thread(ChunkSpareThread).detach();
    }
    if (!sp.ready.empty()) {
      chunk = std::move(sp.ready.back());
      sp.ready.pop_back();
      lock.unlock();
      sp.cv.notify_one();
      return true;
    }
  }
  SvrDiagTimer timer(SvrDiag::kUploadChunk);
  return CreateChunkNow(chunk);
}

ConstantAllocation Allocate(UploadState &s, u32 size, u32 alignment) {
  if (!s.ready)
    return {};
  if (size > kUploadChunkSize) {
    BD_ERROR("constant_buffers: single allocation {} exceeds chunk size {}",
             size, kUploadChunkSize);
    return {};
  }
  FrameUpload &up = s.frames[s.cursor];
  u32 off = (up.chunkOffset + alignment - 1) & ~(alignment - 1);
  if (off + size > kUploadChunkSize) {
    ++up.chunkIndex;
    off = 0;
  }
  if (up.chunks.size() <= up.chunkIndex) {
    up.chunks.resize(up.chunkIndex + 1);
  }
  auto &chunk = up.chunks[up.chunkIndex];
  if (!chunk.buffer) {
    if (!CreateChunk(chunk))
      return {};
    const u32 total = up.chunkIndex + 1;
    if (total > up.peakChunkCount) {
      up.peakChunkCount = total;
      BD_DEBUG("constant_buffers: slot {} grew to {} chunk(s) ({} MiB total)",
               s.cursor, total, total * (kUploadChunkSize / (1024 * 1024)));
    }
  }
  up.chunkOffset = off + size;
  ConstantAllocation a;
  a.memory = chunk.mapped + off;
  a.ref = plume::RenderBufferReference(chunk.buffer.get(), off);
  a.gpuAddress = chunk.gpuBase + off;
  a.size = size;
  return a;
}

// kFlushNaN=true also flushes NaN -> +0 in the same pass. Xenos float ALU obeys
// the X360/D3D9 "multiply by zero yields zero" rule (0*NaN=0), so BD's
// degenerate constants (e.g. the bloom/glare 0/0 weight normalization when
// intensity is zero) are harmless on hardware. Our recompiled D3D12 shaders use
// strict IEEE (NaN*0=NaN), so a NaN constant propagates and blackens the
// post-fx composite. No BD shader reinterprets a float constant register as
// int, so the flush cannot corrupt int-encoded data. Branchless (cmov) and
// fused into the byte swap so it adds no extra memory pass.
template <bool kFlushNaN>
void CopyByteSwap32Impl(u8 *dst, u32 guest_va, u32 size) {
  const auto *src = bd::mem::at<const u32>(guest_va);
  if (!src) {
    std::memset(dst, 0, size);
    return;
  }
  const u32 count = size / sizeof(u32);
  auto *out = reinterpret_cast<u32 *>(dst);
  u32 i = 0;
#if defined(__AVX2__)
  // L12: x86-64-v3 builds (build-native-v3.bat): 8 registers' words per step, one byte shuffle.
  {
    const __m256i swap = _mm256_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12,
                                          3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
    const __m256i abs8 = _mm256_set1_epi32(0x7FFFFFFF);
    const __m256i inf8 = _mm256_set1_epi32(0x7F800000);
    for (; i + 8 <= count; i += 8) {
      __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(src + i));
      v = _mm256_shuffle_epi8(v, swap);
      if constexpr (kFlushNaN) {
        const __m256i is_nan = _mm256_cmpgt_epi32(_mm256_and_si256(v, abs8), inf8);
        v = _mm256_andnot_si256(is_nan, v);
      }
      _mm256_storeu_si256(reinterpret_cast<__m256i *>(out + i), v);
    }
  }
#endif
#if defined(__x86_64__) || defined(_M_X64)
  const __m128i abs_mask = _mm_set1_epi32(0x7FFFFFFF);
  const __m128i inf_bits = _mm_set1_epi32(0x7F800000);
  for (; i + 4 <= count; i += 4) {
    __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i *>(src + i));
    v = _mm_or_si128(_mm_slli_epi16(v, 8), _mm_srli_epi16(v, 8));
    v = _mm_shufflelo_epi16(v, _MM_SHUFFLE(2, 3, 0, 1));
    v = _mm_shufflehi_epi16(v, _MM_SHUFFLE(2, 3, 0, 1));
    if constexpr (kFlushNaN) {
      const __m128i is_nan =
          _mm_cmpgt_epi32(_mm_and_si128(v, abs_mask), inf_bits);
      v = _mm_andnot_si128(is_nan, v);
    }
    _mm_storeu_si128(reinterpret_cast<__m128i *>(out + i), v);
  }
#endif
  for (; i < count; ++i) {
#if defined(_MSC_VER)
    const u32 v = _byteswap_ulong(src[i]);
#else
    const u32 v = __builtin_bswap32(src[i]);
#endif
    if constexpr (kFlushNaN) {
      // NaN iff exponent all-1 and mantissa != 0, i.e. |bits| > +Inf bits.
      out[i] = (v & 0x7FFFFFFFu) > 0x7F800000u ? 0u : v;
    } else {
      out[i] = v;
    }
  }
}

void CopyByteSwap32(u8 *dst, u32 guest_va, u32 size) {
  CopyByteSwap32Impl<false>(dst, guest_va, size);
}

void CopyByteSwap32FlushNaN(u8 *dst, u32 guest_va, u32 size) {
  CopyByteSwap32Impl<true>(dst, guest_va, size);
}

// L14: a shader's register range in one pass: the guest's words as they are into the compare cache
// (`raw`), byte-swapped with NaN flushed into the upload (`dst`). Same bytes as the memcpy +
// CopyByteSwap32FlushNaN pair it replaces, with one read of the guest block instead of two. In
// the heavy entrance scenes nearly every draw uploads one stage of ~200 registers (cbregs/cbblocks
// 209 at 3233 blocks a frame). `bytes` is a whole number of registers (16 bytes each).
#if (defined(__x86_64__) || defined(_M_X64)) && (defined(__clang__) || defined(__GNUC__))
#define SVR_CB_AVX2_DISPATCH 1
__attribute__((target("avx2"))) void CopyRawSwapFlushNaNAvx2(u8 *dst, u8 *raw, const u8 *src,
                                                              u32 bytes) {
  const __m256i swap = _mm256_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12,
                                        3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
  const __m256i abs8 = _mm256_set1_epi32(0x7FFFFFFF);
  const __m256i inf8 = _mm256_set1_epi32(0x7F800000);
  u32 i = 0;
  for (; i + 32 <= bytes; i += 32) {
    __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(src + i));
    _mm256_storeu_si256(reinterpret_cast<__m256i *>(raw + i), v);
    v = _mm256_shuffle_epi8(v, swap);
    const __m256i is_nan = _mm256_cmpgt_epi32(_mm256_and_si256(v, abs8), inf8);
    _mm256_storeu_si256(reinterpret_cast<__m256i *>(dst + i), _mm256_andnot_si256(is_nan, v));
  }
  if (i < bytes) {  // one register left (bytes is a multiple of 16)
    const __m128i swap4 = _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
    __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i *>(src + i));
    _mm_storeu_si128(reinterpret_cast<__m128i *>(raw + i), v);
    v = _mm_shuffle_epi8(v, swap4);
    const __m128i is_nan = _mm_cmpgt_epi32(_mm_and_si128(v, _mm256_castsi256_si128(abs8)),
                                           _mm256_castsi256_si128(inf8));
    _mm_storeu_si128(reinterpret_cast<__m128i *>(dst + i), _mm_andnot_si128(is_nan, v));
  }
}

// CPUID: AVX2 and the OS saving the YMM registers (XCR0 bits 1-2).
bool CpuHasAvx2() {
  unsigned a, b, c, d;
  __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0u), "c"(0u));
  if (a < 7)
    return false;
  __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1u), "c"(0u));
  if (!(c & (1u << 27)) || !(c & (1u << 28)))  // OSXSAVE, AVX
    return false;
  unsigned xlo, xhi;
  __asm__ volatile("xgetbv" : "=a"(xlo), "=d"(xhi) : "c"(0u));
  if ((xlo & 6u) != 6u)
    return false;
  __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7u), "c"(0u));
  return (b & (1u << 5)) != 0;  // AVX2
}
#endif

void CopyRawSwapFlushNaN(u8 *dst, u8 *raw, const u8 *src, u32 bytes) {
#if defined(SVR_CB_AVX2_DISPATCH)
  static const bool s_avx2 = CpuHasAvx2();
  if (s_avx2) {
    CopyRawSwapFlushNaNAvx2(dst, raw, src, bytes);
    return;
  }
#endif
  u32 i = 0;
#if defined(__x86_64__) || defined(_M_X64)
  const __m128i abs_mask = _mm_set1_epi32(0x7FFFFFFF);
  const __m128i inf_bits = _mm_set1_epi32(0x7F800000);
  for (; i + 16 <= bytes; i += 16) {
    __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i *>(src + i));
    _mm_storeu_si128(reinterpret_cast<__m128i *>(raw + i), v);
    v = _mm_or_si128(_mm_slli_epi16(v, 8), _mm_srli_epi16(v, 8));
    v = _mm_shufflelo_epi16(v, _MM_SHUFFLE(2, 3, 0, 1));
    v = _mm_shufflehi_epi16(v, _MM_SHUFFLE(2, 3, 0, 1));
    const __m128i is_nan = _mm_cmpgt_epi32(_mm_and_si128(v, abs_mask), inf_bits);
    _mm_storeu_si128(reinterpret_cast<__m128i *>(dst + i), _mm_andnot_si128(is_nan, v));
  }
#endif
  for (; i + 4 <= bytes; i += 4) {
    u32 w;
    std::memcpy(&w, src + i, 4);
    std::memcpy(raw + i, &w, 4);
#if defined(_MSC_VER)
    const u32 v = _byteswap_ulong(w);
#else
    const u32 v = __builtin_bswap32(w);
#endif
    const u32 out = (v & 0x7FFFFFFFu) > 0x7F800000u ? 0u : v;
    std::memcpy(dst + i, &out, 4);
  }
}

} // namespace

bool TryInit() {
  auto &s = upload_state();
  if (s.ready)
    return true;
  s.cursor = 0;
  for (auto &up : s.frames) {
    up.chunkIndex = 0;
    up.chunkOffset = 0;
    up.peakChunkCount = 0;
  }
  for (auto &slot : s.samplerSlots)
    slot.valid = false;
  s.sharedBound = false;
  MarkAllTextureSlotsDirty();
  RecomputeShadowPcfScale(s);
  s.ready = true;
  return true;
}

void ResetFrame(u32 slot) {
  auto &s = upload_state();
  if (!s.ready)
    return;
  s.cursor = slot;
  ++s.epoch;
  FrameUpload &up = s.frames[slot];
  up.chunkIndex = 0;
  up.chunkOffset = 0;
  RecomputeShadowPcfScale(s);
}

void InvalidateSharedBinding() {
  auto &s = upload_state();
  s.sharedBound = false;
  s.vsConstants.valid = false;
  s.psConstants.valid = false;
}

// c50.xy is BD's NDC->UV half-scale (0.5 on hw). bd_blur_ps reconstructs its
// sample UV as uv = c50.xy*(ndc+1), so it MUST be 0.5. The guest derives it
// from sceneDim/1280x720*0.5, letting output res and supersampling leak in, and
// the pass oversamples until the source collapses into the top-left. bd_blur_ps
// is the ONLY pixel shader reading c50 as this screen->UV scale (verified
// across all 17 c50-using PS). The rest own c50 as material data, so a blanket
// pin would corrupt them (bd_lightshaft_ps's g_vLightShaftDiffuse -> gray
// god rays). bdCameraRefractionUvScaleHook pins device reg50 at the
// bdCameraRender writers, and this catches blur draws those writers miss.
constexpr u32 kScreenUVScaleRegByteOffset = 50 * 16;
constexpr u64 kBDBlurPSHash = 0xD94E164866C3B9BCull;
void PinScreenUVScaleReg(u8 *block) {
  auto *ps = bd::gpu::state().pipelineState.pixelShader;
  const u64 h = (ps && ps->shaderCacheEntry) ? ps->shaderCacheEntry->hash : 0;
  if (h == kBDBlurPSHash) {
    auto *reg = reinterpret_cast<float *>(block + kScreenUVScaleRegByteOffset);
    reg[0] = 0.5f;
    reg[1] = 0.5f;
  }
}

namespace {
const GuestConstantUse &AllRegisters() {
  static const GuestConstantUse use = [] {
    GuestConstantUse u;
    u.ranges[0] = {0, kConstantRegisterCount};
    u.rangeCount = 1;
    u.registerEnd = kConstantRegisterCount;
    for (auto &word : u.mask)
      word = ~u64{0};
    u.known = true;
    return u;
  }();
  return use;
}

const GuestConstantUse &NoRegisters() {
  static const GuestConstantUse use = [] {
    GuestConstantUse u;
    u.known = true;
    return u;
  }();
  return use;
}

const GuestConstantUse &ShaderRegisters(const GuestShader *shader) {
  if (!shader)
    return NoRegisters();
  // Bit 16 = both halves of the old switch; 33554432 = only this half (every register uploaded).
  if ((REXCVAR_GET(svr07_native_off) & (16 | 33554432)) || !shader->constantUse.known)
    return AllRegisters();
  return shader->constantUse;
}

// True when every register the shader reads is in the bound upload with the value the guest
// holds now, so the caller keeps that binding.
// (The D3D runtime's own dirty masks, m_Mask[0]/[1], can't replace this compare: over a match
// about 1.3% of draws had new constants with the mask clean.)
bool BoundUploadStillGood(const StageConstantCache &cache, const u8 *src,
                          const GuestConstantUse &use, bool pinned) {
  if (!cache.valid || cache.pinned != pinned)
    return false;
  for (u32 i = 0; i < 4; ++i) {
    if (use.mask[i] & ~cache.boundMask[i])
      return false;
  }
  for (u32 i = 0; i < use.rangeCount; ++i) {
    const u32 offset = u32(use.ranges[i].first) * 16;
    if (std::memcmp(cache.raw + offset, src + offset, u32(use.ranges[i].count) * 16) != 0)
      return false;
  }
  return true;
}

// Stores only the registers in use. The bound range still spans registers 0..registerEnd: the
// shader's cbuffer has them at those offsets, it just never reads the ones left out.
ConstantAllocation UploadStageConstants(UploadState &s, StageConstantCache &cache, u32 va,
                                        const GuestConstantUse &use, bool pinned) {
  const auto *src = bd::mem::at<const u8>(va);
  if (!src) {
    cache.valid = false;
    return {};
  }
  if (BoundUploadStillGood(cache, src, use, pinned))
    return {};
  const bool poison = REXCVAR_GET(svr07_native_check_constants);
  const u32 bytes = poison ? kConstantBlockBytes : std::max<u32>(use.registerEnd, 1) * 16;
  auto alloc = Allocate(s, bytes, kCBVAlignment);
  if (!alloc.memory) {
    cache.valid = false;
    return {};
  }
  if (poison)
    std::fill_n(reinterpret_cast<float *>(alloc.memory), kConstantBlockBytes / 4, 1e9f);
  u32 registers = 0;
  for (u32 i = 0; i < use.rangeCount; ++i) {
    const u32 offset = u32(use.ranges[i].first) * 16;
    const u32 size = u32(use.ranges[i].count) * 16;
    // svr07_native_off2 bit 2 = the two-pass copy (L14 A/B; same bytes).
    if (REXCVAR_GET(svr07_native_off2) & 2) {
      std::memcpy(cache.raw + offset, src + offset, size);
      CopyByteSwap32FlushNaN(alloc.memory + offset, va + offset, size);
    } else {
      CopyRawSwapFlushNaN(alloc.memory + offset, cache.raw + offset, src + offset, size);
    }
    registers += use.ranges[i].count;
  }
  std::memcpy(cache.boundMask, use.mask, sizeof(cache.boundMask));
  cache.pinned = pinned;
  cache.valid = true;
  SvrPerfCount(SvrCount::kCbBlocks, 1);
  SvrPerfCount(SvrCount::kCbRegisters, registers);
  return alloc;
}

bool PinsScreenUVScale() {
  auto *ps = bd::gpu::state().pipelineState.pixelShader;
  return ps && ps->shaderCacheEntry && ps->shaderCacheEntry->hash == kBDBlurPSHash;
}
}  // namespace

ConstantAllocation UploadVertexShaderConstants(u32 device_guest) {
  BD_CPU_ZONE("UploadVSConstants");
  auto &s = upload_state();
  if (!device_guest)
    return {};
  const u32 va = device_guest + offsetof(D3DDevice, vsFloatConstants);
  return UploadStageConstants(s, s.vsConstants, va,
                              ShaderRegisters(bd::gpu::state().pipelineState.vertexShader), false);
}

ConstantAllocation UploadPixelShaderConstants(u32 device_guest) {
  BD_CPU_ZONE("UploadPSConstants");
  auto &s = upload_state();
  if (!device_guest)
    return {};
  const u32 va = device_guest + offsetof(D3DDevice, psFloatConstants);
  const bool pinned = PinsScreenUVScale();
  // The occlusion counter replaces the guest shader at pipeline build, a null one is depth only.
  const auto &vstate = bd::gpu::state();
  const GuestConstantUse &use = (pinned || vstate.occlusion_counting)
                                    ? AllRegisters()
                                    : ShaderRegisters(vstate.pipelineState.pixelShader);
  // Dev bisection switch: svr07_native_skin_no_hdlig. Pinned, so the next shader gets a fresh block.
  const auto *skin_ps = vstate.pipelineState.pixelShader;
  const u64 skin_hash = (skin_ps && skin_ps->shaderCacheEntry) ? skin_ps->shaderCacheEntry->hash : 0;
  const bool no_hdlig = REXCVAR_GET(svr07_native_skin_no_hdlig) &&
                        (skin_hash == 0xE3E6BFE1E3027967ull || skin_hash == 0x9AAE805CC82A2634ull ||
                         skin_hash == 0x8F1C444E64B0346Aull || skin_hash == 0x3C773B6F2734378Full ||
                         skin_hash == 0x2F2A0C6C63D67480ull);
  auto alloc = UploadStageConstants(s, s.psConstants, va, use, pinned || no_hdlig);
  if (alloc.memory)
    PinScreenUVScaleReg(alloc.memory);
  if (alloc.memory && no_hdlig) {
    auto *hd_lig = reinterpret_cast<float *>(alloc.memory + 156 * 16);
    hd_lig[0] = 30.0f;
    hd_lig[1] = 30.0f;
  }
  return alloc;
}

void MarkTextureSlotDirty(u32 slot) {
  g_texture_slots_dirty.fetch_or(u32{1} << (slot & 31), std::memory_order_relaxed);
}

void MarkAllTextureSlotsDirty() {
  g_texture_slots_dirty.store(~u32{0}, std::memory_order_relaxed);
}

// Per-slot entries of the shared block: slots 0-15 in the base arrays, SvR's 16-31 after them.
#if defined(SVR_NATIVE_RENDERER)
static u32 &Tex2DIndex(SharedConstants &c, u32 i) {
  return i < 16 ? c.texture2DIndices[i] : c.texture2DIndicesHi[i - 16];
}
static u32 &Tex3DIndex(SharedConstants &c, u32 i) {
  return i < 16 ? c.texture3DIndices[i] : c.texture3DIndicesHi[i - 16];
}
static u32 &TexCubeIndex(SharedConstants &c, u32 i) {
  return i < 16 ? c.textureCubeIndices[i] : c.textureCubeIndicesHi[i - 16];
}
static u32 &SamplerIndex(SharedConstants &c, u32 i) {
  return i < 16 ? c.samplerIndices[i] : c.samplerIndicesHi[i - 16];
}
#else
static u32 &Tex2DIndex(SharedConstants &c, u32 i) { return c.texture2DIndices[i]; }
static u32 &Tex3DIndex(SharedConstants &c, u32 i) { return c.texture3DIndices[i]; }
static u32 &TexCubeIndex(SharedConstants &c, u32 i) { return c.textureCubeIndices[i]; }
static u32 &SamplerIndex(SharedConstants &c, u32 i) { return c.samplerIndices[i]; }
#endif

#if defined(SVR_NATIVE_RENDERER)
// svrVfetch's format codes (XenosRecomp's VFETCH_HELPER), 0 for a format it doesn't read.
static u32 VfetchFormatCode(plume::RenderFormat format) {
  switch (format) {
  case plume::RenderFormat::R32_FLOAT:
  case plume::RenderFormat::R32_UINT:
    return 1;
  case plume::RenderFormat::R32G32_FLOAT:
    return 2;
  case plume::RenderFormat::R32G32B32_FLOAT:
  case plume::RenderFormat::R32G32B32_UINT:
    return 3;
  case plume::RenderFormat::R32G32B32A32_FLOAT:
    return 4;
  case plume::RenderFormat::R8G8B8A8_UNORM:
    return 5;
  case plume::RenderFormat::B8G8R8A8_UNORM:
    return 6;
  case plume::RenderFormat::R8G8B8A8_UINT:
    return 7;
  case plume::RenderFormat::R16G16_SNORM:
    return 8;
  case plume::RenderFormat::R16G16_UNORM:
    return 9;
  case plume::RenderFormat::R16G16_FLOAT:
    return 10;
  case plume::RenderFormat::R16G16_UINT:
    return 11;
  case plume::RenderFormat::R16G16_SINT:
    return 12;
  case plume::RenderFormat::R16G16B16A16_SNORM:
    return 13;
  case plume::RenderFormat::R16G16B16A16_UNORM:
    return 14;
  case plume::RenderFormat::R16G16B16A16_FLOAT:
    return 15;
  case plume::RenderFormat::R16G16B16A16_UINT:
    return 16;
  case plume::RenderFormat::R16G16B16A16_SINT:
    return 17;
  default:
    return 0;
  }
}

// What a vertex shader with computed fetches reads instead of its inputs (NR-8b-2g).
// L14: `table_clear` = c.vfetchElements is known to be all zero (most draws: no computed fetch),
// so the 384-byte clear is left out until a shader fills it again.
static void FillComputedFetchTable(SharedConstants &c, const VideoState &vs, bool &table_clear) {
  c.vfetchStreamBytes[0] = c.vfetchStreamBytes[1] = 0;
  const auto *shader = vs.vertex_shader;
  const auto *decl = vs.vertex_declaration;
  if (!table_clear || (REXCVAR_GET(svr07_native_off2) & 4))
    std::memset(c.vfetchElements, 0, sizeof(c.vfetchElements));
  table_clear = true;
  if (!decl || !shader || !shader->shaderCacheEntry ||
      !(shader->shaderCacheEntry->flags & kShaderFlagComputedFetch))
    return;
  table_clear = false;
  c.vfetchStreamBytes[0] = vs.vertex_views[0].size;
  c.vfetchStreamBytes[1] = vs.vertex_views[1].size;
  for (u32 i = 0; i < decl->inputElementCount; ++i) {
    const auto &e = decl->inputElements[i];
    if (e.slotIndex == 15 || e.location >= std::size(c.vfetchElements))
      continue;  // a filler for a semantic the declaration lacks: reads zero
    const u32 code = VfetchFormatCode(e.format);
    if (e.slotIndex > 1 || !code) {
      static std::atomic<u32> s_warned{0};
      if (s_warned.fetch_add(1, std::memory_order_relaxed) < 8)
        BD_WARN("native: computed fetch can't read {}{} (stream {}, format {}) of vs {:016X}",
                e.semanticName ? e.semanticName : "?", e.semanticIndex, e.slotIndex,
                u32(e.format), shader->shaderCacheEntry->hash);
      continue;
    }
    u32 *out = c.vfetchElements[e.location];
    out[0] = e.alignedByteOffset;
    out[1] = vs.input_slots[e.slotIndex].stride;
    out[2] = code;
    out[3] = e.slotIndex;
  }
}
#endif

static void BuildTextureSlot(UploadState &s, SharedConstants &c, u32 i, const VideoState &vs,
                             const D3DDevice *device_p, i32 aniso_now) {
  SamplerIndex(c, i) = 0;
  Tex2DIndex(c, i) = bd::gpu::kNullTexture2DDescriptorIndex;
  Tex3DIndex(c, i) = bd::gpu::kNullTexture3DDescriptorIndex;
  TexCubeIndex(c, i) = bd::gpu::kNullTextureCubeDescriptorIndex;

  bd::gpu::GuestTexture *tex = vs.textures[i];
  if (tex && tex->sourceSurface && tex->sourceSurface->texture &&
      tex->sourceSurface->sampleCount == plume::RenderSampleCount::COUNT_1 &&
      tex->sourceSurface != vs.render_target &&
      tex->sourceSurface != vs.depth_stencil &&
      tex->sourceSurface->descriptorIndex !=
          bd::gpu::kInvalidDescriptorIndex) {
    tex = tex->sourceSurface;
  }
  if (tex && tex->descriptorIndex != bd::gpu::kInvalidDescriptorIndex) {
    switch (tex->viewDimension) {
    case plume::RenderTextureViewDimension::TEXTURE_3D:
      Tex3DIndex(c, i) = tex->descriptorIndex;
      // X360: a 2D fetch on a 3D resource reads slice 0, so publish the
      // volume as its slice-0 2D view too so tfetch2D samples the base layer.
      if (tex->companion2D && tex->companion2D->descriptorIndex !=
                                  bd::gpu::kInvalidDescriptorIndex) {
        Tex2DIndex(c, i) = tex->companion2D->descriptorIndex;
      }
      break;
    case plume::RenderTextureViewDimension::TEXTURE_CUBE:
      TexCubeIndex(c, i) = tex->descriptorIndex;
      break;
    case plume::RenderTextureViewDimension::TEXTURE_2D:
    case plume::RenderTextureViewDimension::UNKNOWN:
    default:
      Tex2DIndex(c, i) = tex->descriptorIndex;
      // BD static reflection cubes bind as a 2D atlas yet the water/glass
      // shader cube-fetches the slot, so publish the sliced TextureCube
      // companion so tfetchCube resolves a real cube, not the null cube.
      if (tex->companionCube && tex->companionCube->descriptorIndex !=
                                    bd::gpu::kInvalidDescriptorIndex) {
        TexCubeIndex(c, i) = tex->companionCube->descriptorIndex;
      }
      break;
    }

    // X360 stores sampler state in fetchConstants[N].dword[*]. The
    // SetSamplerState_* setters are unhooked and run their recompiled bodies,
    // so the address mode bits there are valid.
    if (device_p) {
      const auto &fc_be = device_p->fetchConstants[i];
      const bool clamp3d =
          tex->viewDimension == plume::RenderTextureViewDimension::TEXTURE_3D;
      auto &sc = s.samplerSlots[i];
      const u32 fc[6] = {
          u32(fc_be.dword[0]), u32(fc_be.dword[1]), u32(fc_be.dword[2]),
          u32(fc_be.dword[3]), u32(fc_be.dword[4]), u32(fc_be.dword[5]),
      };
      const u32 key = SamplerFieldsKey(fc);
      if (sc.valid && sc.clamp3d == clamp3d && sc.aniso == aniso_now &&
          sc.key == key) {
        SamplerIndex(c, i) = sc.sampler;
      } else {
        auto desc = DecodeFromFetch(fc);

        // Shell fur volumes encode shell depth in W, and X360-default WRAP
        // wraps a z=0 fetch's second tap to the tip slice and halves density,
        // so force CLAMP to keep both taps on the dense base slice.
        if (clamp3d) {
          desc.addressW = plume::RenderTextureAddressMode::CLAMP;
        }
        const u32 resolved = ResolveSlotLocked(desc);
        sc.key = key;
        sc.sampler = resolved;
        sc.aniso = aniso_now;
        sc.clamp3d = clamp3d;
        sc.valid = true;
        SamplerIndex(c, i) = resolved;
      }
    }
  }
}

// The shared block's fields other than the texture slots and the computed-fetch table, from the
// live state (L16: shared by UploadSharedConstants and SvrSharedBlockWouldMatch).
static void FillSharedScalars(const UploadState &s, SharedConstants &c, u32 device_guest,
                              const D3DDevice *device_p, const VideoState &vs) {
  // Shader bool constants: VS at device+0x2700, PS at device+0x2710, 4 BE
  // dwords each. Shaders branch on BOOL_BIT(n) of a 256-bit register file
  // (VS 0..127, PS 128..255).
  if (device_guest) {
    const auto *device = bd::mem::at<const D3DDevice>(device_guest);
    for (u32 i = 0; i < 4; ++i) {
      c.booleansArr[i] = device ? u32(device->vsBoolConstants[i]) : 0u;
      c.booleansArr[4 + i] =
          device ? u32(device->psBoolConstants[i]) : 0u;
    }
  }

  c.alphaThreshold = bd::gpu::Video::AlphaThreshold();
  c.halfPixelOffsetX = 0.0f;
  c.halfPixelOffsetY = 0.0f;
#if defined(SVR_NATIVE_RENDERER)
  // Host clip space is Vulkan's (pixel centres at +0.5; re:Blue's viewport puts NDC +1 at the top).
  c.screenSpaceScaleX = 1.0f;
  c.screenSpaceScaleY = 1.0f;
  c.screenSpaceOffsetX = 0.0f;
  c.screenSpaceOffsetY = 0.0f;
  // In guest pixels: the game's own viewport (host targets may be scaled up, the conversion to
  // clip space is the same).
  const float gx = device_p ? float(device_p->viewport.X) : 0.0f;
  const float gy = device_p ? float(device_p->viewport.Y) : 0.0f;
  const float gw = device_p ? float(device_p->viewport.Width) : 0.0f;
  const float gh = device_p ? float(device_p->viewport.Height) : 0.0f;
  if (device_guest && gw > 0.0f && gh > 0.0f) {
    const float w = gw;
    const float h = gh;
    // PA_CL_VTE_CNTL (SvR07 device+0x28CC, RR +0x2D8C, SvR 2008 +0x294C; SvR07's
    // SetRenderState_ViewportEnable sub_821D6680: stw 0x43F on / 0x400 off, 10444(r3)). With the
    // viewport transform off the vertex shader outputs window pixels.
    const u32 vte = bd::mem::load<u32>(device_guest + svr07::d3d::kPaClVteCntl);
    // PA_SU_VTX_CNTL.pix_center (SvR07 device+0x2940, RR +0x2E00, SvR 2008 +0x29C0, bit 0;
    // SvR07's SetRenderState_HalfPixelOffset sub_821D6A38: rlwimi r4 into bit 0 of 10560(r3)):
    // clear means Direct3D 9 pixel centres, on integer coordinates.
    const bool d3d9_centres = (bd::mem::load<u32>(device_guest + svr07::d3d::kPaSuVtxCntl) & 1u) == 0;
    const float centre = d3d9_centres ? 0.5f : 0.0f;
    if ((vte & 0x3Fu) == 0) {
      c.screenSpaceScaleX = 2.0f / w;
      c.screenSpaceScaleY = -2.0f / h;
      c.screenSpaceOffsetX = (centre - gx) * 2.0f / w - 1.0f;
      c.screenSpaceOffsetY = 1.0f - (centre - gy) * 2.0f / h;
    } else if (d3d9_centres) {
      // Half a pixel right and down, so D3D9-centred geometry samples and covers as on Xenos.
      c.halfPixelOffsetX = 1.0f / w;
      c.halfPixelOffsetY = -1.0f / h;
    }
  }
#endif
  c.swappedTexcoords =
      vs.vertex_declaration ? vs.vertex_declaration->swappedTexcoords : 0u;
  c.swappedNormals =
      vs.vertex_declaration ? vs.vertex_declaration->swappedNormals : 0u;
  c.swappedBinormals =
      vs.vertex_declaration ? vs.vertex_declaration->swappedBinormals : 0u;
  c.swappedTangents =
      vs.vertex_declaration ? vs.vertex_declaration->swappedTangents : 0u;
  c.swappedBlendWeights =
      vs.vertex_declaration ? vs.vertex_declaration->swappedBlendWeights : 0u;
  c.swappedPositions =
      vs.vertex_declaration ? vs.vertex_declaration->swappedPositions : 0u;
  c.sintTexcoords =
      vs.vertex_declaration ? vs.vertex_declaration->sintTexcoords : 0u;

  c.shadowPcfScale = s.shadowPcfScale;

  // Viewport extent, not the render target's: the NDC->pixel mapping this
  // cancels is the viewport's. +x/-y = half a pixel right and down.
  c.blitHalfPixelOffsetX =
      vs.viewport.width > 0.0f ? 1.0f / vs.viewport.width : 0.0f;
  c.blitHalfPixelOffsetY =
      vs.viewport.height > 0.0f ? -1.0f / vs.viewport.height : 0.0f;

#if defined(SVR_NATIVE_RENDERER)
  c.vertexIdBase = vs.vertex_id_base;
#endif
}

ConstantAllocation UploadSharedConstants(u32 device_guest) {
  BD_CPU_ZONE("UploadSharedConstants");
  auto &s = upload_state();
  if (!s.ready)
    return {};
  // bd_anisotropy participates in DecodeFromFetch's output, so a live change
  // must miss the per-slot cache so stale sampler indices are re-resolved.
  const i32 aniso_now = Settings::Get().Anisotropy();

  // vs.textures is authoritative: our SetTexture hook replaces BD's recompiled
  // body, so the engine's per-slot bound-texture shadow (BD device+0x2FF0+slot*4; SvR07 +0x3000)
  // and GPU texture fetch constants (device+0x400+slot*0x18) are never written.
  // Video::SetTexture mirrors Xenos semantics (a null bind is ignored, since on
  // hardware it does not rebuild the fetch constant), so vs.textures holds the
  // last real texture per slot, exactly what the GPU would still be sampling.
  auto &vs = bd::gpu::state();
  const auto *device_p = bd::mem::at<const D3DDevice>(device_guest);
  u32 dirty = g_texture_slots_dirty.exchange(0, std::memory_order_relaxed);
  if (aniso_now != s.slotAniso || vs.render_target != s.slotRenderTarget ||
      vs.depth_stencil != s.slotDepthStencil ||
      (REXCVAR_GET(svr07_native_off) & (16 | 67108864))) {   // 67108864 = only this half
    dirty = ~u32{0};
    s.slotAniso = aniso_now;
    s.slotRenderTarget = vs.render_target;
    s.slotDepthStencil = vs.depth_stencil;
  }
  if (device_p && std::memcmp(s.slotFetch, device_p->fetchConstants, sizeof(s.slotFetch)) != 0) {
    for (u32 i = 0; i < bd::gpu::kTextureSlots; ++i) {
      if (std::memcmp(&s.slotFetch[i], &device_p->fetchConstants[i], sizeof(s.slotFetch[i])) != 0)
        dirty |= u32{1} << i;
    }
    std::memcpy(s.slotFetch, device_p->fetchConstants, sizeof(s.slotFetch));
  }
  dirty &= kAllTextureSlots;
  SvrPerfCount(SvrCount::kSharedSlots, u32(std::popcount(dirty)));
  for (u32 left = dirty; left; left &= left - 1)
    BuildTextureSlot(s, s.shared, u32(std::countr_zero(left)), vs, device_p, aniso_now);

  // Dev bisection switch: svr07_native_skin_black_slots. The slots are rebuilt on every draw while
  // it's on, so a blacked-out slot doesn't leak into the next shader.
  if (const u32 black = u32(REXCVAR_GET(svr07_native_skin_black_slots)) & kAllTextureSlots) {
    const auto *ps = vs.pipelineState.pixelShader;
    const u64 ps_hash = (ps && ps->shaderCacheEntry) ? ps->shaderCacheEntry->hash : 0;
    const bool skin = ps_hash == 0xE3E6BFE1E3027967ull || ps_hash == 0x9AAE805CC82A2634ull ||
                      ps_hash == 0x8F1C444E64B0346Aull || ps_hash == 0x3C773B6F2734378Full;
    for (u32 left = black; left; left &= left - 1) {
      const u32 i = u32(std::countr_zero(left));
      BuildTextureSlot(s, s.shared, i, vs, device_p, aniso_now);
      if (skin) {
        Tex2DIndex(s.shared, i) = bd::gpu::kNullTexture2DDescriptorIndex;
        Tex3DIndex(s.shared, i) = bd::gpu::kNullTexture3DDescriptorIndex;
        TexCubeIndex(s.shared, i) = bd::gpu::kNullTextureCubeDescriptorIndex;
      }
    }
  }

  if (REXCVAR_GET(svr07_native_check_constants)) {
    SharedConstants all = s.shared;
    for (u32 i = 0; i < bd::gpu::kTextureSlots; ++i)
      BuildTextureSlot(s, all, i, vs, device_p, aniso_now);
    ++s.slotChecks;
    if (std::memcmp(&all, &s.shared, sizeof(all)) != 0) {
      if (s.slotCheckMisses++ < 32) {
        for (u32 i = 0; i < bd::gpu::kTextureSlots; ++i) {
          if (Tex2DIndex(all, i) != Tex2DIndex(s.shared, i) ||
              Tex3DIndex(all, i) != Tex3DIndex(s.shared, i) ||
              TexCubeIndex(all, i) != TexCubeIndex(s.shared, i) ||
              SamplerIndex(all, i) != SamplerIndex(s.shared, i))
            BD_WARN("native: stale texture slot {} (2D {} wanted {}, sampler {} wanted {})", i,
                    Tex2DIndex(s.shared, i), Tex2DIndex(all, i), SamplerIndex(s.shared, i),
                    SamplerIndex(all, i));
        }
      }
      s.shared = all;
    }
    if ((s.slotChecks & 0x3FFFF) == 0)
      BD_INFO("native: texture slot check: {} draws, {} with a stale slot", s.slotChecks,
              s.slotCheckMisses);
  }

  FillSharedScalars(s, s.shared, device_guest, device_p, vs);
#if defined(SVR_NATIVE_RENDERER)
  FillComputedFetchTable(s.shared, vs, s.vfetchTableClear);
#endif

  // Byte-identical to the block already bound on this command list: the live
  // CBV is still correct, skip the upload and let the caller skip the rebind.
  // SharedConstants padding is zero-initialized and never written, so memcmp
  // is deterministic.
  if (s.sharedBound &&
      std::memcmp(&s.shared, &s.lastUploaded, sizeof(SharedConstants)) == 0) {
    return {};
  }

  auto alloc = Allocate(s, sizeof(SharedConstants), kCBVAlignment);
  if (!alloc.memory)
    return {};
  std::memcpy(alloc.memory, &s.shared, sizeof(SharedConstants));
  s.lastUploaded = s.shared;
  s.sharedBound = true;
  SvrPerfCount(SvrCount::kSharedUploads, 1);
  return alloc;
}

// L16: the first register of `use` whose guest value differs from the bound copy, and how many do.
static void CountConstantDiff(const StageConstantCache &cache, const u8 *src,
                              const GuestConstantUse &use, int *first, int *regs) {
  int f = -1, n = 0;
  for (u32 i = 0; i < use.rangeCount; ++i) {
    for (u32 r = use.ranges[i].first; r < u32(use.ranges[i].first) + use.ranges[i].count; ++r) {
      if (std::memcmp(cache.raw + r * 16, src + r * 16, 16) != 0) {
        if (f < 0)
          f = int(r);
        ++n;
      }
    }
  }
  if (first)
    *first = f;
  if (regs)
    *regs = n;
}

// L16: true when UploadVertexShaderConstants and UploadPixelShaderConstants would both return
// size 0 (keep the bound blocks) for the current shaders. Same register choice as they make;
// anything they treat specially (pinned blur PS, occlusion counting, the skin switch, the poison
// check) answers false.
bool SvrStageConstantsWouldMatch(u32 device_guest, int *diff_stage, int *diff_first,
                                 int *diff_regs) {
  auto &s = upload_state();
  if (!device_guest || REXCVAR_GET(svr07_native_check_constants) ||
      REXCVAR_GET(svr07_native_skin_no_hdlig))
    return false;
  const auto &vstate = bd::gpu::state();
  if (vstate.occlusion_counting || PinsScreenUVScale())
    return false;
  const auto *vs_src = bd::mem::at<const u8>(device_guest + offsetof(D3DDevice, vsFloatConstants));
  const auto *ps_src = bd::mem::at<const u8>(device_guest + offsetof(D3DDevice, psFloatConstants));
  if (!vs_src || !ps_src)
    return false;
  const GuestConstantUse &vuse = ShaderRegisters(vstate.pipelineState.vertexShader);
  if (!BoundUploadStillGood(s.vsConstants, vs_src, vuse, false)) {
    if (diff_stage) {
      *diff_stage = 0;
      CountConstantDiff(s.vsConstants, vs_src, vuse, diff_first, diff_regs);
    }
    return false;
  }
  const GuestConstantUse &puse = ShaderRegisters(vstate.pipelineState.pixelShader);
  if (!BoundUploadStillGood(s.psConstants, ps_src, puse, false)) {
    if (diff_stage) {
      *diff_stage = 1;
      CountConstantDiff(s.psConstants, ps_src, puse, diff_first, diff_regs);
    }
    return false;
  }
  return true;
}

// L16: true when UploadSharedConstants would find the block it builds byte-identical to the one
// bound on this list. Conservative: any texture slot marked or any fetch constant changed answers
// false without rebuilding the slot; so does a computed-fetch table in use.
bool SvrSharedBlockWouldMatch(u32 device_guest) {
  auto &s = upload_state();
  if (!s.ready || !s.sharedBound || !device_guest)
    return false;
  if ((g_texture_slots_dirty.load(std::memory_order_relaxed) & kAllTextureSlots) != 0 ||
      (REXCVAR_GET(svr07_native_off) & (16 | 67108864)) ||
      (u32(REXCVAR_GET(svr07_native_skin_black_slots)) & kAllTextureSlots) ||
      REXCVAR_GET(svr07_native_check_constants))
    return false;
  const auto &vs = bd::gpu::state();
  if (Settings::Get().Anisotropy() != s.slotAniso || vs.render_target != s.slotRenderTarget ||
      vs.depth_stencil != s.slotDepthStencil)
    return false;
  const auto *device_p = bd::mem::at<const D3DDevice>(device_guest);
  if (!device_p || std::memcmp(s.slotFetch, device_p->fetchConstants, sizeof(s.slotFetch)) != 0)
    return false;
#if defined(SVR_NATIVE_RENDERER)
  if (!s.vfetchTableClear || (REXCVAR_GET(svr07_native_off2) & 4))
    return false;
  if (vs.vertex_shader && vs.vertex_shader->shaderCacheEntry &&
      (vs.vertex_shader->shaderCacheEntry->flags & kShaderFlagComputedFetch))
    return false;
#endif
  // fps15: only the scalar part is rebuilt and compared. The texture slots are what s.shared holds
  // (no slot marked, fetch constants equal: UploadSharedConstants would rebuild none), the
  // computed-fetch table is clear, and every UploadSharedConstants leaves s.shared equal to
  // lastUploaded (it either finds them equal or uploads s.shared).
  constexpr size_t kScalarsAt = offsetof(SharedConstants, booleansArr);
#if defined(SVR_NATIVE_RENDERER)
  constexpr size_t kScalarsEnd = offsetof(SharedConstants, texture2DIndicesHi);
#else
  constexpr size_t kScalarsEnd = sizeof(SharedConstants);
#endif
  static_assert(offsetof(SharedConstants, blitHalfPixelOffsetY) < kScalarsEnd);
  SharedConstants probe;  // only [kScalarsAt, kScalarsEnd) is read below
  std::memcpy(reinterpret_cast<u8 *>(&probe) + kScalarsAt,
              reinterpret_cast<const u8 *>(&s.shared) + kScalarsAt, kScalarsEnd - kScalarsAt);
  FillSharedScalars(s, probe, device_guest, device_p, vs);
#if defined(SVR_NATIVE_RENDERER)
  // What FillComputedFetchTable leaves for a shader without computed fetches (table already clear).
  probe.vfetchStreamBytes[0] = probe.vfetchStreamBytes[1] = 0;
#endif
  return std::memcmp(reinterpret_cast<const u8 *>(&probe) + kScalarsAt,
                     reinterpret_cast<const u8 *>(&s.lastUploaded) + kScalarsAt,
                     kScalarsEnd - kScalarsAt) == 0 &&
         std::memcmp(&s.shared, &s.lastUploaded, kScalarsAt) == 0 &&
         std::memcmp(reinterpret_cast<const u8 *>(&s.shared) + kScalarsEnd,
                     reinterpret_cast<const u8 *>(&s.lastUploaded) + kScalarsEnd,
                     sizeof(SharedConstants) - kScalarsEnd) == 0;
}

ConstantAllocation UploadGuestBytesByteSwap32(u32 guest_va, u32 size,
                                              u32 alignment) {
  BD_CPU_ZONE("UploadGuestBytesByteSwap32");
  auto &s = upload_state();
  if (!s.ready || !guest_va || !size)
    return {};
  auto alloc = Allocate(s, size, alignment);
  if (!alloc.memory)
    return {};
  CopyByteSwap32(alloc.memory, guest_va, size);
  return alloc;
}

ConstantAllocation UploadGuestBytes(u32 guest_va, u32 size, u32 alignment) {
  auto &s = upload_state();
  if (!s.ready || !guest_va || !size)
    return {};
  auto alloc = Allocate(s, size, alignment);
  if (!alloc.memory)
    return {};
  const auto *src = bd::mem::at<const u8>(guest_va);
  if (!src) {
    BD_WARN("constant_buffers: UploadGuestBytes translate failed for "
            "guest_va={:#x}",
            guest_va);
    return {};
  }
  std::memcpy(alloc.memory, src, size);
  return alloc;
}

ConstantAllocation UploadHostBytes(const void *host_data, u32 size,
                                   u32 alignment) {
  auto &s = upload_state();
  if (!s.ready || !host_data || !size)
    return {};
  auto alloc = Allocate(s, size, alignment);
  if (!alloc.memory)
    return {};
  std::memcpy(alloc.memory, host_data, size);
  return alloc;
}

ConstantAllocation AllocateUploadBytes(u32 size, u32 alignment) {
  auto &s = upload_state();
  if (!s.ready || !size)
    return {};
  return Allocate(s, size, alignment);
}

u64 UploadEpoch() { return upload_state().epoch; }

} // namespace bd::gpu
