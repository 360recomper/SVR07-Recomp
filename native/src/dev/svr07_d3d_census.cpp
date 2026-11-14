// SvR07 native port, Stop 1 (lane L3, 2026-11-10): observe-only D3D call census for WWE SmackDown vs.
// Raw 2007. Port of RR's src/dev/rr_d3d_census.cpp (RR-NR-5), itself UFC-native's ufc_d3d_census.cpp
// (NR-6 / NR-6b), to SvR07's XDK 2.0.3215.1 (src/svr07_d3d.h, src/svr07_d3d_names.h; the offline proof
// of every offset is reports/L3-d3d-verification.md).
//
// --svr07_d3d_census=true counts calls to the game's XDK D3D functions per Swap and logs a [census]
// report every --svr07_d3d_census_every frames. The emulated GPU still draws: every hook runs the
// game's own function unchanged, so with the cvar off (default) the exe behaves like the emulated one
// (one flag test per call). Built only while SVR07_NATIVE_HOOKS is off (cmake/native_renderer.cmake),
// because the native hooks override the same functions.
//
// What it answers (Stop 1):
//   - draws per frame the hooks see: BeginVertices / DrawVertices / DrawIndexedVertices, plus the draw
//     packets inside the ring range each Clear, Resolve and SetShaderGPRAllocation writes (the GPR call
//     emits 24 point-list "pipe drain" draws through sub_821D8578: UFC's hidden 72 a frame). Compare with
//     the emulated GPU's own count (--trace_draw_ps=0 --trace_draw_every=N: "Draw trace c.f (frame F):
//     end, X of Y draws logged"; UFC tools/nr_census_report.py reads both).
//   - VS vs PS BY TYPE (the RR all-black bug): at every draw the objects at +0x3094 / +0x3090 must have
//     Common type 6 / 7 (CreateVertexShader / CreatePixelShader write them), and each Create*Shader
//     result is checked the same way.
//   - device offsets (svr07_d3d.h), each against the arguments of the setter that writes it: VS
//     +0x3094, PS +0x3090, the public VS slot +0x3364, decl +0x2D20 at every draw; after each setter:
//     IB +0x2F94, RT +0x2F98 + 4i, DS +0x2FA8, stream vb +0x2FAC + 4s + stride/4 +0x2FF0 + s + vertex
//     fetch 95 - s (offset folded in), texture +0x3000 + 4s (26 slots), viewport +0x3068 (ints; W/H
//     clamped), scissor +0x3080 and its shadows +0x2844/+0x2848, PA_CL_VPORT +0x2888...
//   - the register shadow blocks: every (register, device offset) pair the block writers sub_821E1878
//     (registers) and sub_821E1C18 (float constants) are handed, logged once each.
//   - the "SVR07-UNVERIFIED" values: ring +0x28/+0x2C/+0x30, code pointers in +0x34..+0x3FC (the
//     render-state call tables), +0x2FEC, +0x30A8, display size +0x3274/+0x3278, sampler indices >= 26,
//     texture Common types, index-buffer format bits, level surfaces bound as targets, Resolve's
//     stack arguments, prim types per draw function.
//   - resolve targets (keep-1x list): every distinct (source RT/depth, dest texture size, format,
//     caller) a Resolve gets, logged once.
//   - the 0x2080 block: sub_821D51D0 writes it with its own header (no block writer); its header,
//     values and the shadows +0x2844/+0x2848 are compared per caller.
//   - command buffers: INDIRECT_BUFFER(_PFD) packets in every scanned ring range: the hooked calls'
//     own ranges and the gaps between hooked calls (what unhooked code such as RunCommandBuffer
//     writes). Expected 0 for SvR07; the "command-buffer share" line is computed, not a constant.

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <tuple>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "svr07_d3d.h"
#include "svr07_d3d_names.h"

REXCVAR_DEFINE_BOOL(svr07_d3d_census, false, "svr07",
                    "SvR07 Stop 1: count the game's D3D calls per frame (draws, clears, resolves), check "
                    "the device offsets against the setters' arguments and log a [census] report "
                    "every svr07_d3d_census_every frames. Observe only; the emulated GPU still draws");
REXCVAR_DEFINE_INT32(svr07_d3d_census_every, 300, "svr07",
                     "svr07_d3d_census: frames (Swaps) per report")
    .range(1, 100000);

namespace svr07_census {

namespace d = svr07::d3d;

enum Fn : int {
  kSwap, kPresent, kResolve, kClear, kClearF, kSetRenderTarget, kSetDepthStencil, kSetViewport,
  kSetScissor, kBeginVertices, kEndVertices, kDrawVertices, kDrawIndexedVertices, kSetVertexShader,
  kSetPixelShader, kSetVertexDeclaration, kSetTexture, kSetStreamSource, kSetIndices, kSetVsConstF,
  kSetPsConstF, kVbUnlock, kIbUnlock, kCreateVs, kCreatePs, kCreateDecl, kCommit, kBlockRegs,
  kBlockConsts, kBlockFetch, kGprAlloc, kScissorBlock, kFnCount
};
constexpr const char* kFnName[kFnCount] = {
    "Swap", "Present", "Resolve", "Clear", "ClearF", "SetRenderTarget", "SetDepthStencil",
    "SetViewport", "SetScissor", "BeginVertices", "EndVertices", "DrawVertices",
    "DrawIndexedVertices", "SetVS", "SetPS", "SetDecl", "SetTexture", "SetStreamSource",
    "SetIndices", "SetVSConstF", "SetPSConstF", "VBUnlock", "IBUnlock", "CreateVS", "CreatePS",
    "CreateDecl", "Commit_821E2760", "Blk_821E1878", "Blk_821E1C18", "Blk_821E1AB8", "GprAlloc_821DAF80",
    "Scissor_821D51D0"};

enum Tally : int {
  kDirect,        // draw calls (BeginVertices, DrawVertices, DrawIndexedVertices)
  kClearDraws,    // draw packets in the ring range each Clear wrote
  kResolveDraws,  // draw packets in the ring range each Resolve wrote (its copy + any clear)
  kResolveClearDraws, // ... of those, in Resolves with clear flags
  kGprDraws,      // draw packets in the ring range each SetShaderGPRAllocation wrote (pipe drain)
  kScanSkipped,   // Clear / Resolve / GPR calls whose ring range couldn't be scanned (wrapped / kicked off)
  kDrawsInScan,   // draw packets in the ring ranges the draw calls themselves wrote
  kVsMatch, kVsMismatch, kPsMatch, kPsMismatch, kVsPubMatch, kVsPubMismatch, kDeclMatch,
  kDeclMismatch, kIbMatch, kIbMismatch, kRtMatch, kRtMismatch, kDsMatch, kDsMismatch,
  kStreamMatch, kStreamMismatch, kTexMatch, kTexMismatch, kVpMatch, kVpMismatch,
  kDrawStreamMatch, kDrawStreamMismatch, kDrawTexMatch, kDrawTexMismatch,
  kVsTypeMatch, kVsTypeMismatch, kPsTypeMatch, kPsTypeMismatch,      // object type at +0x3094 / +0x3090
  kCreateTypeMatch, kCreateTypeMismatch,                             // Create*Shader result types
  kLevelTargets,                                                     // RT / DS set to a level surface
  // Command buffers: INDIRECT_BUFFER(_PFD) packets (0x3F / 0x37) found by any ring scan (the hooked
  // calls' own ranges and the gaps between hooked calls), the draws inside them, empty ones, and
  // ones nested in another indirect buffer.
  kIndirect, kIndirectDraws, kIndirectEmpty, kIndirectNested,
  // Gaps: ring ranges written by unhooked code between the end of one hooked call and the start of
  // the next (BeginVertices..EndVertices is not a gap: inline vertices). Skipped = wrapped / kicked off.
  kGapScans, kGapSkipped, kGapDwords, kGapDraws, kGapIndirect,
  // 0x2080 block (sub_821D51D0 writes its own type-0 header 0x00022080 = 0x2080..0x2082 from its
  // arguments): simple vs tiled (predicated) path, and whether the TL/BR it sends equal the device
  // shadows +0x2844 / +0x2848 (the native renderer reads those).
  kScissorSimple, kScissorTiled, kScissorSkipped, kScissorShadowMatch, kScissorShadowMismatch,
  kTallyCount
};

inline std::atomic<uint64_t> g_calls[kFnCount];
inline std::atomic<uint64_t> g_tally[kTallyCount];
inline std::atomic<uint32_t> g_frame_draws{0};
inline std::atomic<uint32_t> g_last_vs{0}, g_last_ps{0}, g_last_decl{0}, g_last_ib{0};
inline std::atomic<uint32_t> g_last_stream_vb[16], g_last_stream_off[16];
inline std::atomic<uint32_t> g_last_tex[32];
inline std::atomic<bool> g_dumped_device{false};

struct Window {
  std::mutex lock;
  uint64_t frames = 0;
  uint32_t min = UINT32_MAX, max = 0, last = 0;
  uint64_t frame_index = 0;
};
inline Window g_window;

// Logged-once sets.
inline std::mutex g_once_lock;
inline std::set<std::tuple<uint32_t, uint32_t, uint32_t>> g_blocks;   // (writer, reg, offset)
inline std::set<uint32_t> g_stream_pad;                               // +0x2FEC values
inline std::set<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>> g_resolves;  // (flags, w<<16|h, fmt, lr)
inline std::set<std::pair<uint32_t, uint32_t>> g_prims;               // (fn, prim)
inline std::set<uint32_t> g_tex_types;                                // (Common & 0xF) << 8 | dim
inline std::set<uint32_t> g_ib_formats;                               // Common >> 29
inline std::set<std::tuple<uint32_t, uint32_t, uint32_t>> g_resolve_args;  // (flags, stencil, params != 0)
inline uint32_t g_mismatch_logs = 0;

inline uint32_t Load32(uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, base + addr, 4);
  return _byteswap_ulong(v);
}
inline uint8_t Load8(uint8_t* base, uint32_t addr) { return base[addr]; }
inline bool Plausible(uint32_t addr) { return addr >= 0x10000; }
inline bool On() { return REXCVAR_GET(svr07_d3d_census); }
inline void Count(Fn f) { g_calls[f].fetch_add(1, std::memory_order_relaxed); }
inline void Add(Tally t, uint64_t n = 1) { g_tally[t].fetch_add(n, std::memory_order_relaxed); }
inline void Check(bool ok, Tally match) { Add(ok ? match : Tally(match + 1)); }

template <typename... Args>
inline void Mismatch(fmt::format_string<Args...> f, Args&&... args) {
  std::lock_guard<std::mutex> g(g_once_lock);
  if (g_mismatch_logs >= 64) return;
  ++g_mismatch_logs;
  REXLOG_INFO("[census] mismatch: {}", fmt::format(f, std::forward<Args>(args)...));
}

template <typename... Args>
inline void Once(std::set<uint32_t>& set, uint32_t key, size_t cap, fmt::format_string<Args...> f, Args&&... args) {
  std::lock_guard<std::mutex> g(g_once_lock);
  if (set.size() < cap && set.insert(key).second)
    REXLOG_INFO("[census] {}", fmt::format(f, std::forward<Args>(args)...));
}

inline uint32_t PhysToVirtual(uint32_t addr) {
  return addr >= 0x80000000u ? addr : 0xA0000000u + (addr & 0x1FFFFFFFu);
}
// GPU_CONVERT_CPU_TO_GPU_ADDRESS as SetStreamSource does it (rlwinm 12,20,31 / addi 512 / & 0x1000).
inline uint32_t GpuAddress(uint32_t a) { return (a & 0x1FFFFFFFu) + ((((a >> 20) & 0xFFFu) + 0x200u) & 0x1000u); }

struct Pm4Count {
  uint32_t draws = 0, other3 = 0;
  uint32_t indirect = 0;      // INDIRECT_BUFFER(_PFD) packets at any depth
  uint32_t indirect_draws = 0;  // draw packets inside them
  uint32_t indirect_empty = 0;  // ... with no draw inside
  uint32_t indirect_nested = 0; // ... found inside another indirect buffer
};

// Walks one PM4 range for DRAW_INDX (0x22) / DRAW_INDX_2 (0x36), following INDIRECT_BUFFER(_PFD)
// (0x3F / 0x37: dword1 = physical address, dword2 low 20 bits = size in dwords) and counting them.
inline void ScanPm4Va(uint8_t* base, uint32_t va, uint32_t dwords, int depth, Pm4Count& out) {
  if (!va || dwords > 0x100000 || depth > 4) return;
  for (uint32_t i = 0; i < dwords;) {
    const uint32_t h = Load32(base, va + 4 * i);
    const uint32_t type = h >> 30;
    if (type == 0) {
      i += 2 + ((h >> 16) & 0x3FFF);
    } else if (type == 1) {
      i += 3;
    } else if (type == 2) {
      i += 1;
    } else {
      const uint32_t op = (h >> 8) & 0x7F, count = ((h >> 16) & 0x3FFF) + 1;
      if (op == 0x22 || op == 0x36) {
        ++out.draws;
        if (depth > 0) ++out.indirect_draws;
      } else if ((op == 0x3F || op == 0x37) && i + 2 < dwords) {
        ++out.indirect;
        if (depth > 0) ++out.indirect_nested;
        const uint32_t draws_before = out.draws;
        ScanPm4Va(base, PhysToVirtual(Load32(base, va + 4 * (i + 1))),
                  Load32(base, va + 4 * (i + 2)) & 0xFFFFF, depth + 1, out);
        if (out.draws == draws_before) ++out.indirect_empty;
      } else {
        ++out.other3;
      }
      i += 1 + count;
    }
  }
}

inline void AddIndirect(const Pm4Count& c) {
  if (!c.indirect) return;
  Add(kIndirect, c.indirect);
  Add(kIndirectDraws, c.indirect_draws);
  Add(kIndirectEmpty, c.indirect_empty);
  Add(kIndirectNested, c.indirect_nested);
}

// Draw packets in the ring range a call wrote: the ring pointer (+0x28) is the last dword written
// (stwu pre-increment), so new data starts at before + 4. A KickOff in the call resets it: skipped.
// Every scanned range also feeds the command-buffer (INDIRECT_BUFFER) tallies.
inline bool ScanRing(uint8_t* base, uint32_t before, uint32_t after, Pm4Count& c) {
  if (after <= before || after - before >= 0x40000) return false;
  ScanPm4Va(base, before + 4, (after - before) / 4, 0, c);
  AddIndirect(c);
  return true;
}

// Gap scans: the ring the game (or an unhooked XDK call such as RunCommandBuffer) writes between
// two hooked calls is scanned at the start of the second one, so an INDIRECT_BUFFER that is not in
// any hooked call's own range is still counted. Only the outermost hooked call per thread takes
// part (a Clear inside a Resolve is not a new gap).
inline thread_local int t_depth = 0;
inline std::atomic<uint32_t> g_gap_dev{0}, g_gap_end{0};

inline void ScanGap(uint8_t* base, uint32_t dev) {
  if (!Plausible(dev) || g_gap_dev.load(std::memory_order_relaxed) != dev) return;
  const uint32_t end = g_gap_end.load(std::memory_order_relaxed);
  const uint32_t now = Load32(base, dev + d::kRing);
  if (now == end) return;
  if (now < end || now - end >= 0x40000) {
    Add(kGapSkipped);
    return;
  }
  Pm4Count c;
  ScanPm4Va(base, end + 4, (now - end) / 4, 0, c);
  Add(kGapScans);
  Add(kGapDwords, (now - end) / 4);
  Add(kGapDraws, c.draws);
  Add(kGapIndirect, c.indirect);
  AddIndirect(c);
}

// RAII: scan the gap before the outermost hooked call, remember where the ring ends after it.
// end_valid = false for BeginVertices (the inline vertices follow, EndVertices closes the range).
struct CallScope {
  uint8_t* base;
  uint32_t dev;
  bool end_valid;
  CallScope(uint8_t* b, uint32_t dv, bool valid = true) : base(b), dev(dv), end_valid(valid) {
    if (t_depth++ == 0) ScanGap(base, dev);
  }
  ~CallScope() {
    if (--t_depth != 0) return;
    if (!end_valid || !Plausible(dev)) {
      g_gap_dev.store(0, std::memory_order_relaxed);
      return;
    }
    g_gap_end.store(Load32(base, dev + d::kRing), std::memory_order_relaxed);
    g_gap_dev.store(dev, std::memory_order_relaxed);
  }
};

// Texture fetch constant (object + 0x1C): dword2 = size_2d (width-1 [12:0], height-1 [25:13]),
// dword1 format [5:0], dword5 dimension [10:9] (GetType sub_821D27E8: rlwinm r10,r10,23,30,31).
inline void TextureSize(uint8_t* base, uint32_t tex, uint32_t& w, uint32_t& h, uint32_t& fmt) {
  w = h = fmt = 0;
  if (!Plausible(tex)) return;
  const uint32_t d1 = Load32(base, tex + d::kTextureFetch + 4);
  const uint32_t d2 = Load32(base, tex + d::kTextureFetch + 8);
  w = (d2 & 0x1FFF) + 1;
  h = ((d2 >> 13) & 0x1FFF) + 1;
  fmt = d1 & 0x3F;
}
inline uint32_t CommonType(uint8_t* base, uint32_t obj) {
  return Plausible(obj) ? (Load32(base, obj) & d::kCommonTypeMask) : 0;
}

inline void DumpDeviceOnce(uint8_t* base, uint32_t dev) {
  bool expected = false;
  if (!g_dumped_device.compare_exchange_strong(expected, true)) return;
  // Code pointers in +0x34..+0x3FC (Blue Dragon: render-state table at +0x38; UFC: +0x40).
  uint32_t code = 0, first = 0, last = 0;
  for (uint32_t off = d::kStateCallScanBegin; off < d::kStateCallScanEnd; off += 4) {
    const uint32_t v = Load32(base, dev + off);
    if (v >= 0x82000000u && v < 0x83400000u) {
      if (!code) first = off;
      last = off;
      ++code;
    }
  }
  REXLOG_INFO("[census] device {:08X}: {} code pointers in +0x{:X}..+0x{:X}, first +0x{:X} ({:08X}), last +0x{:X}",
              dev, code, d::kStateCallScanBegin, d::kStateCallScanEnd - 4, first,
              first ? Load32(base, dev + first) : 0u, last);
  std::string head;
  for (uint32_t off = 0; off < 0x40; off += 4) head += fmt::format(" {:08X}", Load32(base, dev + off));
  REXLOG_INFO("[census] device {:08X}: +0x00..+0x3C{} (ring +0x{:X}, limit? +0x{:X}, guarantee +0x{:X})", dev, head,
              d::kRing, d::kRingLimit, d::kRingGuarantee);
  REXLOG_INFO("[census] device {:08X}: VS +0x{:X} {:08X} (type {}), PS +0x{:X} {:08X} (type {}), VS public +0x{:X} {:08X}, "
              "decl +0x{:X} {:08X}, IB +0x{:X} {:08X}, RT0 +0x{:X} {:08X}, DS +0x{:X} {:08X}, stream pad +0x{:X} {:08X}, "
              "+0x{:X} {:08X}, display? {}x{}, front +0x{:X} {:08X}, back +0x{:X} {:08X}, create flags +0x{:X} {:08X}, "
              "GPR +0x{:X} {:08X}",
              dev, d::kVertexShader, Load32(base, dev + d::kVertexShader),
              CommonType(base, Load32(base, dev + d::kVertexShader)), d::kPixelShader,
              Load32(base, dev + d::kPixelShader), CommonType(base, Load32(base, dev + d::kPixelShader)),
              d::kVertexShaderPublic, Load32(base, dev + d::kVertexShaderPublic), d::kVertexDeclaration,
              Load32(base, dev + d::kVertexDeclaration), d::kIndexBuffer, Load32(base, dev + d::kIndexBuffer),
              d::kRenderTarget0, Load32(base, dev + d::kRenderTarget0), d::kDepthStencil,
              Load32(base, dev + d::kDepthStencil), d::kStreamPad, Load32(base, dev + d::kStreamPad),
              d::kRtCompareGate, Load32(base, dev + d::kRtCompareGate), Load32(base, dev + d::kDisplayWidth),
              Load32(base, dev + d::kDisplayHeight), d::kFrontBuffer, Load32(base, dev + d::kFrontBuffer),
              d::kBackBuffer, Load32(base, dev + d::kBackBuffer), d::kCreateFlags,
              Load32(base, dev + d::kCreateFlags), d::kGprAllocation, Load32(base, dev + d::kGprAllocation));
}

inline void NoteStreamPad(uint8_t* base, uint32_t dev) {
  const uint32_t v = Load32(base, dev + d::kStreamPad);
  Once(g_stream_pad, v, 16, "+0x{:X} (between streams and strides) new value {:08X}", d::kStreamPad, v);
}

inline void NotePrim(uint32_t fn, uint32_t prim) {
  std::lock_guard<std::mutex> g(g_once_lock);
  if (g_prims.size() < 64 && g_prims.emplace(fn, prim).second)
    REXLOG_INFO("[census] {} prim type {} (first seen, frame {})", kFnName[fn], prim, g_window.frame_index);
}

// Every draw call: count it and check the device's shader / declaration / stream / texture slots
// against the last setter arguments, and the shader objects by their own type.
inline void OnDraw(uint8_t* base, uint32_t dev, Fn fn, uint32_t prim) {
  DumpDeviceOnce(base, dev);
  NotePrim(fn, prim);
  Add(kDirect);
  g_frame_draws.fetch_add(1, std::memory_order_relaxed);
  const uint32_t vs = Load32(base, dev + d::kVertexShader), ps = Load32(base, dev + d::kPixelShader),
                 vs_pub = Load32(base, dev + d::kVertexShaderPublic),
                 decl = Load32(base, dev + d::kVertexDeclaration);
  const uint32_t lvs = g_last_vs.load(std::memory_order_relaxed),
                 lps = g_last_ps.load(std::memory_order_relaxed),
                 ldecl = g_last_decl.load(std::memory_order_relaxed);
  Check(vs == lvs, kVsMatch);
  Check(ps == lps, kPsMatch);
  Check(vs_pub == lvs, kVsPubMatch);
  Check(decl == ldecl, kDeclMatch);
  if (vs != lvs) Mismatch("draw VS +0x3094 {:08X} last SetVS {:08X}", vs, lvs);
  if (ps != lps) Mismatch("draw PS +0x3090 {:08X} last SetPS {:08X}", ps, lps);
  if (decl != ldecl) Mismatch("draw decl +0x2D20 {:08X} last SetDecl {:08X}", decl, ldecl);
  // VS vs PS by the objects' own type (6 = vertex, 7 = pixel), not by the setter names.
  if (Plausible(vs)) {
    const uint32_t t = CommonType(base, vs);
    Check(t == d::kTypeVertexShader, kVsTypeMatch);
    if (t != d::kTypeVertexShader) Mismatch("draw VS +0x3094 {:08X} has Common type {} (want 6)", vs, t);
  }
  if (Plausible(ps)) {
    const uint32_t t = CommonType(base, ps);
    Check(t == d::kTypePixelShader, kPsTypeMatch);
    if (t != d::kTypePixelShader) Mismatch("draw PS +0x3090 {:08X} has Common type {} (want 7)", ps, t);
  }
  for (uint32_t s = 0; s < d::kStreamCount; ++s) {
    const uint32_t vb = Load32(base, dev + d::kStreamSource + d::kStreamSourceStride * s);
    if (!vb && !g_last_stream_vb[s].load(std::memory_order_relaxed)) continue;
    bool ok = vb == g_last_stream_vb[s].load(std::memory_order_relaxed);
    if (ok && Plausible(vb)) {  // the offset lives in vertex fetch 95 - s only
      const uint32_t fetch0 = Load32(base, dev + d::StreamFetchOffset(s));
      const uint32_t want = GpuAddress(Load32(base, vb + d::kBufferAddress) +
                                       g_last_stream_off[s].load(std::memory_order_relaxed));
      ok = (fetch0 & ~3u) == (want & ~3u);
    }
    Check(ok, kDrawStreamMatch);
  }
  for (uint32_t s = 0; s < d::kTextureSlots; ++s) {
    const uint32_t t = Load32(base, dev + d::kTexture + 4 * s);
    if (!t && !g_last_tex[s].load(std::memory_order_relaxed)) continue;
    Check(t == g_last_tex[s].load(std::memory_order_relaxed), kDrawTexMatch);
  }
}

inline uint64_t Take(std::atomic<uint64_t>& a) { return a.exchange(0, std::memory_order_relaxed); }

inline void OnSwap(uint8_t* base, uint32_t dev) {
  if (Plausible(dev)) NoteStreamPad(base, dev);
  const uint32_t draws = g_frame_draws.exchange(0, std::memory_order_relaxed);
  Window& w = g_window;
  std::lock_guard<std::mutex> guard(w.lock);
  ++w.frame_index;
  ++w.frames;
  w.last = draws;
  if (draws < w.min) w.min = draws;
  if (draws > w.max) w.max = draws;
  if (w.frames < uint64_t(REXCVAR_GET(svr07_d3d_census_every))) return;

  const double f = double(w.frames);
  uint64_t calls[kFnCount], t[kTallyCount];
  for (int i = 0; i < kFnCount; ++i) calls[i] = Take(g_calls[i]);
  for (int i = 0; i < kTallyCount; ++i) t[i] = Take(g_tally[i]);
  // gpu-like = what the GPU runs as draws: the draw calls + the clear quads (Clear, and Resolves
  // with clear flags) + the GPR pipe-drain points. A Resolve's own copy packet is a copy, not a draw.
  const uint64_t resolve_extra =
      t[kResolveDraws] > calls[kResolve] ? t[kResolveDraws] - calls[kResolve] : 0;
  const uint64_t gpu_like = t[kDirect] + t[kClearDraws] + t[kGprDraws];
  // Same line shape as UFC's census so tools/nr_census_report.py (UFC) reads it. "runs" = the
  // INDIRECT_BUFFER packets the ring scans found (call ranges + gaps); "replayed" = draws inside
  // them; share = replayed / (gpu-like + replayed). "recorded" stays 0.0: recording
  // (BeginCommandBuffer) is not hooked, and 0 predicated draws is not measured.
  const uint64_t replayed = t[kIndirectDraws];
  const double share = (gpu_like + replayed) ? 100.0 * double(replayed) / double(gpu_like + replayed) : 0.0;
  REXLOG_INFO("[census] frame {} ({} frames): gpu-like {:.1f}/frame (min {} max {} last {}) = direct {:.1f} "
              "+ internal 0.0 + replayed {:.1f} + clears {:.1f} + gpr flush {:.1f}; recorded 0.0; runs {} ({:.1f}/frame, "
              "0 predicated draws, {} empty, {} nested; {:.1f} draws/frame in the buffers themselves); "
              "command-buffer share {:.2f}%",
              w.frame_index, w.frames, double(gpu_like) / f, w.min, w.max, w.last, double(t[kDirect]) / f,
              double(replayed) / f, double(t[kClearDraws]) / f, double(t[kGprDraws]) / f, t[kIndirect],
              double(t[kIndirect]) / f, t[kIndirectEmpty], t[kIndirectNested], double(replayed) / f, share);
  REXLOG_INFO("[census] frame {}: command buffers: {} INDIRECT_BUFFER packets ({} in gaps between hooked calls), "
              "{} draws inside; gaps scanned {} ({:.0f} dwords/frame, {} draw packets outside hooked calls), "
              "gaps skipped {} (wrapped / kicked off)",
              w.frame_index, t[kIndirect], t[kGapIndirect], replayed, t[kGapScans], double(t[kGapDwords]) / f,
              t[kGapDraws], t[kGapSkipped]);
  REXLOG_INFO("[census] frame {}: 0x2080 block (sub_821D51D0): simple path {} (header 0x00022080), tiled path {}, "
              "skipped {}; sent TL/BR == shadows +0x2844/+0x2848 {}/{} (Resolve's own rect is expected to differ)",
              w.frame_index, t[kScissorSimple], t[kScissorTiled], t[kScissorSkipped], t[kScissorShadowMatch],
              t[kScissorShadowMatch] + t[kScissorShadowMismatch]);
  REXLOG_INFO("[census] frame {}: resolves {:.1f}/frame, draw packets in them {:.1f}/frame (extra over one "
              "per resolve {:.1f}, in clearing resolves {:.1f}); gpu-like + extra {:.1f}/frame; ring scans skipped {}; "
              "draw packets the draw calls wrote {:.1f}/frame; GPR allocations {:.1f}/frame",
              w.frame_index, double(calls[kResolve]) / f, double(t[kResolveDraws]) / f,
              double(resolve_extra) / f, double(t[kResolveClearDraws]) / f,
              (double(gpu_like) + double(resolve_extra)) / f, t[kScanSkipped], double(t[kDrawsInScan]) / f,
              double(calls[kGprAlloc]) / f);
  REXLOG_INFO("[census] frame {}: device check (at draws) VS +0x3094 {}/{} match, PS +0x3090 {}/{}, VS public "
              "+0x3364 {}/{}, decl +0x2D20 {}/{}, streams +0x2FAC+fetch {}/{}, textures +0x3000 {}/{}; "
              "by type: VS objects type 6 {}/{}, PS objects type 7 {}/{}, Create*Shader results {}/{}",
              w.frame_index, t[kVsMatch], t[kVsMatch] + t[kVsMismatch], t[kPsMatch], t[kPsMatch] + t[kPsMismatch],
              t[kVsPubMatch], t[kVsPubMatch] + t[kVsPubMismatch], t[kDeclMatch], t[kDeclMatch] + t[kDeclMismatch],
              t[kDrawStreamMatch], t[kDrawStreamMatch] + t[kDrawStreamMismatch], t[kDrawTexMatch],
              t[kDrawTexMatch] + t[kDrawTexMismatch], t[kVsTypeMatch], t[kVsTypeMatch] + t[kVsTypeMismatch],
              t[kPsTypeMatch], t[kPsTypeMatch] + t[kPsTypeMismatch], t[kCreateTypeMatch],
              t[kCreateTypeMatch] + t[kCreateTypeMismatch]);
  REXLOG_INFO("[census] frame {}: setter check IB +0x2F94 {}/{}, RT +0x2F98 {}/{}, DS +0x2FA8 {}/{}, "
              "stream vb +0x2FAC / stride +0x2FF0 / vfetch 95-s {}/{}, texture +0x3000 {}/{}, viewport +0x3068 {}/{}; "
              "level surfaces set as targets {}",
              w.frame_index, t[kIbMatch], t[kIbMatch] + t[kIbMismatch], t[kRtMatch], t[kRtMatch] + t[kRtMismatch],
              t[kDsMatch], t[kDsMatch] + t[kDsMismatch], t[kStreamMatch], t[kStreamMatch] + t[kStreamMismatch],
              t[kTexMatch], t[kTexMatch] + t[kTexMismatch], t[kVpMatch], t[kVpMatch] + t[kVpMismatch],
              t[kLevelTargets]);
  std::string line;
  for (int i = 0; i < kFnCount; ++i) {
    if (!calls[i]) continue;
    line += fmt::format(" {}={:.1f}", kFnName[i], double(calls[i]) / f);
  }
  REXLOG_INFO("[census] frame {}: calls/frame{}", w.frame_index, line);
  w.frames = 0;
  w.min = UINT32_MAX;
  w.max = 0;
}

inline void NoteBlock(uint32_t writer, uint32_t dev, uint32_t reg, uint32_t src) {
  const uint32_t off = src - dev;
  std::lock_guard<std::mutex> g(g_once_lock);
  if (g_blocks.size() < 64 && g_blocks.emplace(writer, reg, off).second)
    REXLOG_INFO("[census] block writer {:08X}: register 0x{:04X} from device +0x{:X} (svr07_d3d.h: +0x{:X})", writer,
                reg, off, d::RegisterOffset(reg));
}

// sub_821D51D0(dev, l, t, r, b) writes the 0x2080 block itself (no block writer, no device source):
// simple path = type-0 header 0x00022080 (3 registers from 0x2080), 0 (PA_SC_WINDOW_OFFSET), TL =
// (t << 16 & 0x3FFF0000) | (l & 0x3FFF), BR the same from r / b. Tiled path (+0x2A38 bit 4 / 5) =
// type-3 packets per tile. Callers: SetScissorRect sub_821D76A0, sub_821D7DC0, the state flush
// sub_821D8578 (all three pass the shadows +0x2844 / +0x2848) and Resolve sub_821DB9C0 (its own rect,
// then the shadows again). Proves kRegs2080Count = 3 and that the shadows are what the GPU gets.
inline std::set<std::tuple<uint32_t, uint32_t, uint32_t>> g_scissor_logs;  // (lr, path, match)

inline void NoteScissorBlock(uint8_t* base, uint32_t dev, uint32_t before, uint32_t after, uint32_t lr,
                             const uint32_t arg[4], uint32_t tl_shadow, uint32_t br_shadow) {
  if (after <= before || after - before >= 0x40000) {
    Add(kScissorSkipped);
    return;
  }
  const uint32_t h = Load32(base, before + 4);
  constexpr uint32_t kMask = 0x3FFF3FFFu;
  uint32_t path, match = 2, v0 = 0, tl = 0, br = 0;
  if (h == 0x00022080u && after - before >= 16) {
    path = 0;
    Add(kScissorSimple);
    v0 = Load32(base, before + 8);
    tl = Load32(base, before + 12);
    br = Load32(base, before + 16);
    match = ((tl & kMask) == (tl_shadow & kMask) && (br & kMask) == (br_shadow & kMask)) ? 1 : 0;
    Add(match ? kScissorShadowMatch : kScissorShadowMismatch);
  } else {
    path = 1;
    Add(kScissorTiled);
  }
  const uint32_t want_tl = ((arg[1] << 16) & 0x3FFF0000u) | (arg[0] & 0x3FFFu);
  const uint32_t want_br = ((arg[3] << 16) & 0x3FFF0000u) | (arg[2] & 0x3FFFu);
  std::lock_guard<std::mutex> g(g_once_lock);
  if (g_scissor_logs.size() < 32 && g_scissor_logs.emplace(lr, path, match).second)
    REXLOG_INFO("[census] 0x2080 block from lr {:08X}: {} path, first dword {:08X} (want 00022080 = 3 registers "
                "from 0x2080), 0x2080 {:08X} TL {:08X} BR {:08X}; from args {} {} {} {} -> TL {:08X} BR {:08X}; "
                "shadows +0x2844 {:08X} +0x2848 {:08X}: {}",
                lr, path ? "tiled" : "simple", h, v0, tl, br, int32_t(arg[0]), int32_t(arg[1]), int32_t(arg[2]),
                int32_t(arg[3]), want_tl, want_br, tl_shadow, br_shadow,
                match == 2 ? "n/a" : (match ? "match" : "DIFFER"));
}

}  // namespace svr07_census

namespace rc = svr07_census;

#define SVR07_CENSUS_COUNT(fn, idx)                       \
  REX_EXTERN(__imp__##fn);                               \
  extern "C" REX_FUNC(fn) {                              \
    if (rc::On()) rc::Count(rc::idx);                    \
    __imp__##fn(ctx, base);                              \
  }

SVR07_CENSUS_COUNT(D3DDevice_ClearF, kClearF)
SVR07_CENSUS_COUNT(D3DDevice_SetVertexShaderConstantF, kSetVsConstF)
SVR07_CENSUS_COUNT(D3DDevice_SetPixelShaderConstantF, kSetPsConstF)
SVR07_CENSUS_COUNT(D3DVertexBuffer_Unlock, kVbUnlock)
SVR07_CENSUS_COUNT(D3DIndexBuffer_Unlock, kIbUnlock)
SVR07_CENSUS_COUNT(D3DDevice_CreateVertexDeclaration, kCreateDecl)
SVR07_CENSUS_COUNT(sub_821E2760, kCommit)

// Present / EndVertices take part in the gap scans (CallScope); EndVertices closes the
// BeginVertices range (its exit is the first valid gap start after the inline vertices).
#define SVR07_CENSUS_SCOPED(fn, idx)          \
  REX_EXTERN(__imp__##fn);                   \
  extern "C" REX_FUNC(fn) {                  \
    if (!rc::On()) {                         \
      __imp__##fn(ctx, base);                \
      return;                                \
    }                                        \
    rc::CallScope scope(base, ctx.r3.u32);   \
    rc::Count(rc::idx);                      \
    __imp__##fn(ctx, base);                  \
  }

SVR07_CENSUS_SCOPED(D3DDevice_Present, kPresent)
SVR07_CENSUS_SCOPED(D3DDevice_EndVertices, kEndVertices)

// Create*Shader: the object they return must carry their own type (6 vertex, 7 pixel).
#define SVR07_CENSUS_CREATE_SHADER(fn, idx, want)                                              \
  REX_EXTERN(__imp__##fn);                                                                    \
  extern "C" REX_FUNC(fn) {                                                                   \
    const bool on = rc::On();                                                                 \
    const uint32_t container = ctx.r3.u32;                                                    \
    const uint32_t flags = (on && rc::Plausible(container)) ? rc::Load32(base, container) : 0; \
    __imp__##fn(ctx, base);                                                                   \
    if (!on) return;                                                                          \
    rc::Count(rc::idx);                                                                       \
    const uint32_t t = rc::CommonType(base, ctx.r3.u32);                                      \
    rc::Check(t == (want), rc::kCreateTypeMatch);                                             \
    if (t != (want))                                                                          \
      rc::Mismatch(#fn "({:08X}, container flags {:08X}) -> {:08X} Common type {} (want {})", \
                   container, flags, ctx.r3.u32, t, uint32_t(want));                          \
  }

SVR07_CENSUS_CREATE_SHADER(D3DDevice_CreateVertexShader, kCreateVs, svr07::d3d::kTypeVertexShader)
SVR07_CENSUS_CREATE_SHADER(D3DDevice_CreatePixelShader, kCreatePs, svr07::d3d::kTypePixelShader)

// Draws (dev, prim, ...): counted and checked before the call; the ring range the call writes is
// scanned too (one packet per call expected; DrawIndexedVertices splits counts over 65535).
#define SVR07_CENSUS_DRAW(fn, idx)                                       \
  REX_EXTERN(__imp__##fn);                                              \
  extern "C" REX_FUNC(fn) {                                             \
    if (!rc::On()) {                                                    \
      __imp__##fn(ctx, base);                                           \
      return;                                                           \
    }                                                                   \
    const uint32_t dev = ctx.r3.u32;                                    \
    rc::CallScope scope(base, dev);                                     \
    rc::Count(rc::idx);                                                 \
    rc::OnDraw(base, dev, rc::idx, ctx.r4.u32);                         \
    const uint32_t before = rc::Load32(base, dev + rc::d::kRing);       \
    __imp__##fn(ctx, base);                                             \
    rc::Pm4Count c;                                                     \
    if (rc::ScanRing(base, before, rc::Load32(base, dev + rc::d::kRing), c)) \
      rc::Add(rc::kDrawsInScan, c.draws);                               \
  }

SVR07_CENSUS_DRAW(D3DDevice_DrawVertices, kDrawVertices)

// DrawIndexedVertices also logs the index buffer's format bits (Common >> 29) once each.
REX_EXTERN(__imp__D3DDevice_DrawIndexedVertices);
extern "C" REX_FUNC(D3DDevice_DrawIndexedVertices) {
  if (!rc::On()) {
    __imp__D3DDevice_DrawIndexedVertices(ctx, base);
    return;
  }
  const uint32_t dev = ctx.r3.u32;
  rc::CallScope scope(base, dev);
  rc::Count(rc::kDrawIndexedVertices);
  rc::OnDraw(base, dev, rc::kDrawIndexedVertices, ctx.r4.u32);
  const uint32_t ib = rc::Load32(base, dev + rc::d::kIndexBuffer);
  if (rc::Plausible(ib)) {
    const uint32_t bits = rc::Load32(base, ib) >> 29;
    rc::Once(rc::g_ib_formats, bits, 8, "index buffer Common >> 29 = {} (bit 2 = 32-bit, 1-0 = endian), e.g. IB {:08X}",
             bits, ib);
  }
  const uint32_t before = rc::Load32(base, dev + rc::d::kRing);
  __imp__D3DDevice_DrawIndexedVertices(ctx, base);
  rc::Pm4Count c;
  if (rc::ScanRing(base, before, rc::Load32(base, dev + rc::d::kRing), c))
    rc::Add(rc::kDrawsInScan, c.draws);
}

// BeginVertices writes its DRAW_INDX header; the vertices follow until EndVertices. Not scanned,
// and no gap starts at its end (the inline vertices are not PM4).
REX_EXTERN(__imp__D3DDevice_BeginVertices);
extern "C" REX_FUNC(D3DDevice_BeginVertices) {
  if (!rc::On()) {
    __imp__D3DDevice_BeginVertices(ctx, base);
    return;
  }
  rc::CallScope scope(base, ctx.r3.u32, /*end_valid=*/false);
  rc::Count(rc::kBeginVertices);
  rc::OnDraw(base, ctx.r3.u32, rc::kBeginVertices, ctx.r4.u32);
  rc::Add(rc::kDrawsInScan);
  __imp__D3DDevice_BeginVertices(ctx, base);
}

#define SVR07_CENSUS_SET(fn, idx, last)                                \
  REX_EXTERN(__imp__##fn);                                            \
  extern "C" REX_FUNC(fn) {                                           \
    if (rc::On()) {                                                   \
      rc::Count(rc::idx);                                             \
      rc::last.store(ctx.r4.u32, std::memory_order_relaxed);          \
    }                                                                 \
    __imp__##fn(ctx, base);                                           \
  }

SVR07_CENSUS_SET(D3DDevice_SetVertexShader, kSetVertexShader, g_last_vs)
SVR07_CENSUS_SET(D3DDevice_SetPixelShader, kSetPixelShader, g_last_ps)
SVR07_CENSUS_SET(D3DDevice_SetVertexDeclaration, kSetVertexDeclaration, g_last_decl)

// SetIndices(dev, ib).
REX_EXTERN(__imp__D3DDevice_SetIndices);
extern "C" REX_FUNC(D3DDevice_SetIndices) {
  const bool on = rc::On();
  const uint32_t dev = ctx.r3.u32, ib = ctx.r4.u32;
  __imp__D3DDevice_SetIndices(ctx, base);
  if (!on) return;
  rc::Count(rc::kSetIndices);
  rc::g_last_ib.store(ib, std::memory_order_relaxed);
  const uint32_t v = rc::Load32(base, dev + rc::d::kIndexBuffer);
  rc::Check(v == ib, rc::kIbMatch);
  if (v != ib) rc::Mismatch("SetIndices({:08X}) -> +0x2F94 {:08X}", ib, v);
}

// SetRenderTarget(dev, index, surface).
REX_EXTERN(__imp__D3DDevice_SetRenderTarget);
extern "C" REX_FUNC(D3DDevice_SetRenderTarget) {
  const bool on = rc::On();
  const uint32_t dev = ctx.r3.u32, idx = ctx.r4.u32, surf = ctx.r5.u32;
  __imp__D3DDevice_SetRenderTarget(ctx, base);
  if (!on) return;
  rc::Count(rc::kSetRenderTarget);
  if (rc::Plausible(surf) && (rc::Load32(base, surf) & rc::d::kCommonLevelSurface)) rc::Add(rc::kLevelTargets);
  if (idx < rc::d::kRenderTargetCount) {
    const uint32_t v = rc::Load32(base, dev + rc::d::kRenderTarget0 + 4 * idx);
    rc::Check(v == surf, rc::kRtMatch);
    if (v != surf) rc::Mismatch("SetRenderTarget({}, {:08X}) -> +0x{:X} {:08X}", idx, surf,
                                rc::d::kRenderTarget0 + 4 * idx, v);
  }
}

// SetDepthStencilSurface(dev, surface).
REX_EXTERN(__imp__D3DDevice_SetDepthStencilSurface);
extern "C" REX_FUNC(D3DDevice_SetDepthStencilSurface) {
  const bool on = rc::On();
  const uint32_t dev = ctx.r3.u32, surf = ctx.r4.u32;
  __imp__D3DDevice_SetDepthStencilSurface(ctx, base);
  if (!on) return;
  rc::Count(rc::kSetDepthStencil);
  if (rc::Plausible(surf) && (rc::Load32(base, surf) & rc::d::kCommonLevelSurface)) rc::Add(rc::kLevelTargets);
  const uint32_t v = rc::Load32(base, dev + rc::d::kDepthStencil);
  rc::Check(v == surf, rc::kDsMatch);
  if (v != surf) rc::Mismatch("SetDepthStencilSurface({:08X}) -> +0x2FA8 {:08X}", surf, v);
}

// SetStreamSource(dev, stream, vb, offset, stride, mask): bare vb at +0x2FAC + 4s, stride/4 at
// +0x2FF0 + s, and vertex fetch 95 - s = GPU(vb.FetchLo + offset) / vb.FetchHi - offset.
REX_EXTERN(__imp__D3DDevice_SetStreamSource);
extern "C" REX_FUNC(D3DDevice_SetStreamSource) {
  const bool on = rc::On();
  const uint32_t dev = ctx.r3.u32, s = ctx.r4.u32, vb = ctx.r5.u32, off = ctx.r6.u32, stride = ctx.r7.u32;
  __imp__D3DDevice_SetStreamSource(ctx, base);
  if (!on) return;
  rc::Count(rc::kSetStreamSource);
  if (s < rc::d::kStreamCount) {
    rc::g_last_stream_vb[s].store(vb, std::memory_order_relaxed);
    rc::g_last_stream_off[s].store(off, std::memory_order_relaxed);
    const uint32_t e_vb = rc::Load32(base, dev + rc::d::kStreamSource + rc::d::kStreamSourceStride * s);
    const uint32_t e_stride = rc::Load8(base, dev + rc::d::kStreamStrideDiv4 + s);
    bool ok = e_vb == vb && e_stride == ((stride >> 2) & 0xFF);
    uint32_t f0 = 0, f1 = 0;
    if (rc::Plausible(vb)) {
      f0 = rc::Load32(base, dev + rc::d::StreamFetchOffset(s));
      f1 = rc::Load32(base, dev + rc::d::StreamFetchOffset(s) + 4);
      ok = ok && f0 == rc::GpuAddress(rc::Load32(base, vb + rc::d::kBufferAddress) + off) &&
           f1 == rc::Load32(base, vb + rc::d::kBufferSize) - off;
    }
    rc::Check(ok, rc::kStreamMatch);
    if (!ok) rc::Mismatch("SetStreamSource({}, vb {:08X}, off {}, stride {}) -> vb {:08X} stride/4 {} vfetch {:08X} {:08X}",
                          s, vb, off, stride, e_vb, e_stride, f0, f1);
  } else {
    rc::Mismatch("SetStreamSource stream {} (>= 16)", s);
  }
  rc::NoteStreamPad(base, dev);
}

// SetTexture(dev, sampler, texture, mask).
REX_EXTERN(__imp__D3DDevice_SetTexture);
extern "C" REX_FUNC(D3DDevice_SetTexture) {
  const bool on = rc::On();
  const uint32_t dev = ctx.r3.u32, s = ctx.r4.u32, tex = ctx.r5.u32;
  __imp__D3DDevice_SetTexture(ctx, base);
  if (!on) return;
  rc::Count(rc::kSetTexture);
  if (s < 32) rc::g_last_tex[s].store(tex, std::memory_order_relaxed);
  if (rc::Plausible(tex)) {
    const uint32_t type = rc::CommonType(base, tex);
    const uint32_t dim = (rc::Load32(base, tex + rc::d::kTextureFetch + 20) >> 9) & 3;
    rc::Once(rc::g_tex_types, (type << 8) | dim, 16, "SetTexture object type {} fetch dim {} (e.g. {:08X}, sampler {})",
             type, dim, tex, s);
  }
  if (s < rc::d::kTextureSlots) {  // +0x3000 + 4 * 26 = +0x3068 (viewport)
    const uint32_t v = rc::Load32(base, dev + rc::d::kTexture + 4 * s);
    rc::Check(v == tex, rc::kTexMatch);
    if (v != tex) rc::Mismatch("SetTexture({}, {:08X}) -> +0x{:X} {:08X}", s, tex, rc::d::kTexture + 4 * s, v);
  } else {
    rc::Mismatch("SetTexture sampler {} (past the 26 slots before the viewport)", s);
  }
}

// SetViewport(dev, pViewport): integer X, Y, W, H, float MinZ, MaxZ at +0x3068; W/H are clamped to
// the bound target (or the display size), so W/H only have to be <= the argument.
REX_EXTERN(__imp__D3DDevice_SetViewport);
extern "C" REX_FUNC(D3DDevice_SetViewport) {
  const bool on = rc::On();
  const uint32_t dev = ctx.r3.u32, vp = ctx.r4.u32;
  uint32_t arg[6] = {};
  if (on && rc::Plausible(vp))
    for (int i = 0; i < 6; ++i) arg[i] = rc::Load32(base, vp + 4 * i);
  __imp__D3DDevice_SetViewport(ctx, base);
  if (!on || !rc::Plausible(vp)) return;
  rc::Count(rc::kSetViewport);
  uint32_t v[6];
  for (int i = 0; i < 6; ++i) v[i] = rc::Load32(base, dev + rc::d::kViewport + 4 * i);
  const bool ok = v[0] == arg[0] && v[1] == arg[1] && v[2] <= arg[2] && v[3] <= arg[3] && v[4] == arg[4] &&
                  v[5] == arg[5];
  rc::Check(ok, rc::kVpMatch);
  if (!ok) rc::Mismatch("SetViewport({} {} {} {} {:08X} {:08X}) -> +0x3068 {} {} {} {} {:08X} {:08X}", arg[0], arg[1],
                        arg[2], arg[3], arg[4], arg[5], v[0], v[1], v[2], v[3], v[4], v[5]);
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 4)
    rc::Mismatch("(info) SetViewport {} {} {}x{} -> {}x{}: window scissor +0x2844 {:08X} +0x2848 {:08X}, PA_CL_VPORT "
                 "+0x2888.. {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
                 arg[0], arg[1], arg[2], arg[3], v[2], v[3], rc::Load32(base, dev + rc::d::kPaScWindowScissorTL),
                 rc::Load32(base, dev + rc::d::kPaScWindowScissorBR), rc::Load32(base, dev + rc::d::kPaClVportXScale),
                 rc::Load32(base, dev + rc::d::kPaClVportXScale + 4), rc::Load32(base, dev + rc::d::kPaClVportXScale + 8),
                 rc::Load32(base, dev + rc::d::kPaClVportXScale + 12), rc::Load32(base, dev + rc::d::kPaClVportXScale + 16),
                 rc::Load32(base, dev + rc::d::kPaClVportXScale + 20));
}

// SetScissorRect(dev, pRect): +0x3080 = the rect; the window scissor shadows (+0x2844 / +0x2848)
// get it clamped to the viewport.
REX_EXTERN(__imp__D3DDevice_SetScissorRect);
extern "C" REX_FUNC(D3DDevice_SetScissorRect) {
  const bool on = rc::On();
  const uint32_t dev = ctx.r3.u32, r = ctx.r4.u32;
  __imp__D3DDevice_SetScissorRect(ctx, base);
  if (!on) return;
  rc::Count(rc::kSetScissor);
  static std::atomic<int> logged{0};
  if (rc::Plausible(r) && logged.fetch_add(1) < 4)
    rc::Mismatch("(info) SetScissorRect {} {} {} {} -> +0x3080 {} {} {} {}, +0x2844 {:08X} +0x2848 {:08X}",
                 int32_t(rc::Load32(base, r)), int32_t(rc::Load32(base, r + 4)), int32_t(rc::Load32(base, r + 8)),
                 int32_t(rc::Load32(base, r + 12)), int32_t(rc::Load32(base, dev + rc::d::kScissorRect)),
                 int32_t(rc::Load32(base, dev + rc::d::kScissorRect + 4)),
                 int32_t(rc::Load32(base, dev + rc::d::kScissorRect + 8)),
                 int32_t(rc::Load32(base, dev + rc::d::kScissorRect + 12)),
                 rc::Load32(base, dev + rc::d::kPaScWindowScissorTL), rc::Load32(base, dev + rc::d::kPaScWindowScissorBR));
}

REX_EXTERN(__imp__D3DDevice_Swap);
extern "C" REX_FUNC(D3DDevice_Swap) {
  const uint32_t dev = ctx.r3.u32;
  if (!rc::On()) {
    __imp__D3DDevice_Swap(ctx, base);
    return;
  }
  rc::CallScope scope(base, dev);
  __imp__D3DDevice_Swap(ctx, base);
  {
    rc::Count(rc::kSwap);
    rc::OnSwap(base, dev);
  }
}

// Clear(dev, Count, pRects, Flags, Color, Z, Stencil): its draws are the packets in the ring range
// it writes (one rect-list draw per rect, through ClearF).
REX_EXTERN(__imp__D3DDevice_Clear);
extern "C" REX_FUNC(D3DDevice_Clear) {
  if (!rc::On()) {
    __imp__D3DDevice_Clear(ctx, base);
    return;
  }
  const uint32_t dev = ctx.r3.u32;
  rc::CallScope scope(base, dev);
  const uint32_t before = rc::Load32(base, dev + rc::d::kRing);
  __imp__D3DDevice_Clear(ctx, base);
  rc::Count(rc::kClear);
  rc::Pm4Count c;
  if (rc::ScanRing(base, before, rc::Load32(base, dev + rc::d::kRing), c)) {
    rc::Add(rc::kClearDraws, c.draws);
    rc::g_frame_draws.fetch_add(c.draws, std::memory_order_relaxed);
  } else {
    rc::Add(rc::kScanSkipped);
    rc::Add(rc::kClearDraws);  // at least one rect
    rc::g_frame_draws.fetch_add(1, std::memory_order_relaxed);
  }
}

// SetShaderGPRAllocation(dev, flags, vs GPRs, ps GPRs): writes SQ_GPR_MANAGEMENT and, through the
// state flush sub_821D8578, 2 x 12 point-list DRAW_INDX_2 packets (the pipe drain). Counted as
// "gpr flush" draws: the native renderer never sees them (no hook), the emulated GPU does.
REX_EXTERN(__imp__sub_821DAF80);
extern "C" REX_FUNC(sub_821DAF80) {
  if (!rc::On()) {
    __imp__sub_821DAF80(ctx, base);
    return;
  }
  const uint32_t dev = ctx.r3.u32;
  rc::CallScope scope(base, dev);
  const uint32_t before = rc::Load32(base, dev + rc::d::kRing);
  __imp__sub_821DAF80(ctx, base);
  rc::Count(rc::kGprAlloc);
  rc::Pm4Count c;
  if (rc::ScanRing(base, before, rc::Load32(base, dev + rc::d::kRing), c)) {
    rc::Add(rc::kGprDraws, c.draws);
    rc::g_frame_draws.fetch_add(c.draws, std::memory_order_relaxed);
  } else {
    rc::Add(rc::kScanSkipped);
  }
}

// Resolve(dev, flags, pSourceRect, pDestTexture, pDestPoint, level, slice, pClearColor, f1 Z, then
// ClearStencil / pParameters in the caller's frame at r1 + 0x5C / + 0x64): scan its ring range; log
// each distinct target (keep-1x list) and each distinct (flags, stencil, params) argument set.
REX_EXTERN(__imp__D3DDevice_Resolve);
extern "C" REX_FUNC(D3DDevice_Resolve) {
  if (!rc::On()) {
    __imp__D3DDevice_Resolve(ctx, base);
    return;
  }
  const uint32_t dev = ctx.r3.u32, flags = ctx.r4.u32, rect = ctx.r5.u32, tex = ctx.r6.u32, point = ctx.r7.u32;
  const uint32_t level = ctx.r8.u32, slice = ctx.r9.u32, color = ctx.r10.u32;
  const uint32_t stencil = rc::Load32(base, ctx.r1.u32 + rc::d::kResolveStackStencil);
  const uint32_t params = rc::Load32(base, ctx.r1.u32 + rc::d::kResolveStackParams);
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  const uint32_t rt = (flags & rc::d::kResolveDepth)
                          ? rc::Load32(base, dev + rc::d::kDepthStencil)
                          : rc::Load32(base, dev + rc::d::kRenderTarget0 + 4 * (flags & 3));
  rc::CallScope scope(base, dev);
  const uint32_t before = rc::Load32(base, dev + rc::d::kRing);
  __imp__D3DDevice_Resolve(ctx, base);
  rc::Count(rc::kResolve);
  rc::Pm4Count c;
  if (rc::ScanRing(base, before, rc::Load32(base, dev + rc::d::kRing), c)) {
    rc::Add(rc::kResolveDraws, c.draws);
    if (flags & 0x300) rc::Add(rc::kResolveClearDraws, c.draws);
  } else {
    rc::Add(rc::kScanSkipped);
  }
  uint32_t w, h, fmt;
  rc::TextureSize(base, tex, w, h, fmt);
  std::lock_guard<std::mutex> g(rc::g_once_lock);
  if (rc::g_resolves.size() < 96 && rc::g_resolves.emplace(flags & 0x3FF, (w << 16) | h, fmt, lr).second)
    REXLOG_INFO("[census] resolve target: flags {:08X} ({} {}) src surface {:08X} -> tex {:08X} {}x{} fmt {} "
                "level {} slice {} rect {} point {} from lr {:08X} (frame {})",
                flags, (flags & rc::d::kResolveDepth) ? "depth" : "rt", flags & 3, rt, tex, w, h, fmt, level, slice,
                rc::Plausible(rect) ? fmt::format("{},{}-{},{}", int32_t(rc::Load32(base, rect)),
                                                  int32_t(rc::Load32(base, rect + 4)), int32_t(rc::Load32(base, rect + 8)),
                                                  int32_t(rc::Load32(base, rect + 12)))
                                    : std::string("full"),
                rc::Plausible(point) ? fmt::format("{},{}", int32_t(rc::Load32(base, point)),
                                                   int32_t(rc::Load32(base, point + 4)))
                                     : std::string("0,0"),
                lr, rc::g_window.frame_index);
  if (rc::g_resolve_args.size() < 32 && rc::g_resolve_args.emplace(flags, stencil, params != 0).second)
    REXLOG_INFO("[census] resolve args: flags {:08X} pClearColor {:08X} Z {} stencil(r1+0x5C) {:08X} "
                "params(r1+0x64) {:08X} from lr {:08X}",
                flags, color, ctx.f1.f64, stencil, params, lr);
}

// Block writers (dev, mask, first register, source in the device): sub_821E1878 = registers,
// sub_821E1C18 = float constants (0x4000 from +0x700, 0x4400 from +0x1700), sub_821E1AB8 = (dev,
// mask) fetch-constant flush (counted only).
REX_EXTERN(__imp__sub_821E1878);
extern "C" REX_FUNC(sub_821E1878) {
  if (rc::On()) {
    rc::Count(rc::kBlockRegs);
    rc::NoteBlock(0x821E1878u, ctx.r3.u32, ctx.r5.u32, ctx.r6.u32);
  }
  __imp__sub_821E1878(ctx, base);
}
REX_EXTERN(__imp__sub_821E1C18);
extern "C" REX_FUNC(sub_821E1C18) {
  if (rc::On()) {
    rc::Count(rc::kBlockConsts);
    rc::NoteBlock(0x821E1C18u, ctx.r3.u32, ctx.r5.u32, ctx.r6.u32);
  }
  __imp__sub_821E1C18(ctx, base);
}
REX_EXTERN(__imp__sub_821E1AB8);
extern "C" REX_FUNC(sub_821E1AB8) {
  if (rc::On()) rc::Count(rc::kBlockFetch);
  __imp__sub_821E1AB8(ctx, base);
}

// sub_821D51D0(dev, l, t, r, b): the 0x2080 block writer (see NoteScissorBlock).
REX_EXTERN(__imp__sub_821D51D0);
extern "C" REX_FUNC(sub_821D51D0) {
  if (!rc::On()) {
    __imp__sub_821D51D0(ctx, base);
    return;
  }
  const uint32_t dev = ctx.r3.u32, lr = static_cast<uint32_t>(ctx.lr);
  const uint32_t arg[4] = {ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32};
  rc::CallScope scope(base, dev);
  const uint32_t tl = rc::Load32(base, dev + rc::d::kPaScWindowScissorTL);
  const uint32_t br = rc::Load32(base, dev + rc::d::kPaScWindowScissorBR);
  const uint32_t before = rc::Load32(base, dev + rc::d::kRing);
  __imp__sub_821D51D0(ctx, base);
  rc::Count(rc::kScissorBlock);
  rc::NoteScissorBlock(base, dev, before, rc::Load32(base, dev + rc::d::kRing), lr, arg, tl, br);
}