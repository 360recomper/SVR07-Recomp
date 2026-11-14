// svr07 - game-code hooks.
//
// Each hook replaces a weak recompiled function by name; the original stays
// callable as __imp__<name>.

#include "generated/default/svr07_init.h"
#include "svr07_fps.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

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
  svr07_fps::OnEntranceStart();  // [fpslog] entrance timing (svr07_fps_log, off by default) + L17 scene gate
  // L17: in the high-fps mode the entrance runs at the console's 30 (svr07_hfps_entrance_console); at 60 Hz as before.
  if (REXCVAR_GET(entrance_60fps) && svr07_fps::KeepEntrance60()) {
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
namespace {
constexpr uint32_t kHeapTable = 0x8295B9F4;

struct HeapStats {
  uint32_t start = 0, end = 0, blocks = 0;
  uint64_t free_bytes = 0, largest = 0;
};

bool GetHeapStats(uint8_t* base, uint32_t h, HeapStats& s) {
  uint32_t table = REX_LOAD_U32(kHeapTable);
  if (!table) return false;
  s = HeapStats{REX_LOAD_U32(table + h * 24), REX_LOAD_U32(table + h * 24 + 4)};
  if (!s.start || s.end <= s.start) return false;
  uint32_t units = (s.end - s.start) >> 4;
  for (uint32_t idx = REX_LOAD_U32(s.start + 8); idx && idx < units && s.blocks < 1000000; ++s.blocks) {
    uint32_t block = s.start + idx * 16;
    uint32_t len = (REX_LOAD_U32(block) & 0x0FFFFFFF) - idx - 1;
    s.free_bytes += uint64_t(len) * 16;
    s.largest = std::max<uint64_t>(s.largest, uint64_t(len) * 16);
    idx = REX_LOAD_U32(block + 8);
  }
  return true;
}

void LogHeaps(uint8_t* base) {
  HeapStats s;
  for (uint32_t h = 0; h < 8 && GetHeapStats(base, h, s); ++h) {
    REXKRNL_WARN("  heap {}: {:#x}-{:#x} ({} MB), free {} KB in {} blocks, largest {} KB", h, s.start,
                 s.end, (s.end - s.start) >> 20, s.free_bytes / 1024, s.blocks, s.largest / 1024);
  }
}
}  // namespace

REX_HOOK_RAW(sub_82414850) {
  uint32_t size = ctx.r3.u32;
  uint32_t tag = ctx.r4.u32;
  __imp__sub_82414850(ctx, base);
  if (ctx.r3.u32 != 0 || size == 0 || !REXCVAR_GET(log_heap_failures)) {
    return;
  }
  REXKRNL_WARN("game heap: alloc of {:#x} bytes (tag {}) failed", size, tag);
  LogHeaps(base);
}

REXCVAR_DEFINE_INT32(heap_arena_extra_mb, 40, "Game",
                     "Ask for a heap arena this many MB bigger than 200 MB, placed lower in physical "
                     "memory (the 250 MB texture pool above it stays)");

// Boot memory setup sub_821C5730(cfg, out, ...) (from sub_821B8560): cfg+20 = 250 MB, cfg+24 =
// 200 MB. It allocates a cfg+20 placeholder from 0x1FC00000 down, then the heap arena (cfg+24,
// shrunk by 64 KB until it fits) right below it, then frees the placeholder and keeps its range
// (stored at 0x8293CEC8) as the pool textures, movies and audio are allocated in. Shrinking that
// pool (cfg+20) crashes the match load (sub_821D3E78, failed physical alloc with an inverted range),
// so only the arena request grows: its range [pool start - cfg+24, pool start] reaches lower.
REX_HOOK_RAW(sub_821C5730) {
  const uint32_t cfg = ctx.r3.u32;
  const int32_t extra = REXCVAR_GET(heap_arena_extra_mb);
  if (extra > 0 && extra <= 128 && REX_LOAD_U32(cfg + 20) == 250u * 1024 * 1024 &&
      REX_LOAD_U32(cfg + 24) == 200u * 1024 * 1024) {
    REX_STORE_U32(cfg + 24, REX_LOAD_U32(cfg + 24) + uint32_t(extra) * 1024 * 1024);
    REXKRNL_INFO("heap_arena_extra_mb: heap arena request 200 MB -> {} MB", 200 + extra);
  }
  const uint32_t out = ctx.r4.u32;
  __imp__sub_821C5730(ctx, base);
  REXKRNL_INFO("heap arena: {:#x}, {} KB", REX_LOAD_U32(out), REX_LOAD_U32(out + 4) >> 10);
}

REXCVAR_DEFINE_INT32(file_cache_mb, 72, "Game",
                     "Size of the game's file-cache heap (heap 5) in MB; retail 16. Ported motion banks "
                     "in m.pac need more (FINDINGS-tipbox-hang.md)");

// Heap setup at boot: sub_82414D38(table, top, sizes_kb, count) carves heaps from the top of the
// arena down, heap i = sizes_kb[i] KB (list ends at a 0), the last heap gets the rest. The list is
// static data at 0x82007090: 6144, 32768, 12800, 16384, 2048, 16384 KB, so heap 6 (the general
// heap) gets the remainder. Heap 5 is the file cache (sub_8241D310 config, manager at
// [0x8295C64C] +68 = 5): pac banks are loaded into it, and sub_824286B0 retries an allocation that
// does not fit forever (evicts released entries, sleeps, tries again). The pack's MOT/GAME bank 3
// is 19.5 MB (retail 7 MB): it never fits the 16 MB heap, and the match load hangs on the tip box.
// Heap 5 at boot stays retail: every mode re-carves it (below).
// Each mode then re-carves heaps 5.. with its own list: sub_824150D0(first, sizes_kb) from the
// sub_821B86B8..sub_821B89E0 setters (0x820070B0 / 0x82007128: heap 5 = 20 MB + 27 small heaps;
// 0x8200719C / 0x820073A0: 16 MB), the last heap again gets the rest of the 200 MB arena. Guest
// memory is full (the game takes 250 + 200 MB of 512), so the room can only come from that last
// heap: heap 5 grows to file_cache_mb but leaves the last heap at least kMinRestKb.
// The lists are read-only xex data: the call gets a patched copy on the guest stack.
namespace {
constexpr uint32_t kMinRestKb = 16 * 1024;
constexpr uint32_t kMatchLayout = 0x820070B0;

uint32_t ArenaBottom(uint8_t* base) {
  uint32_t table = REX_LOAD_U32(kHeapTable), bottom = 0xFFFFFFFF;
  for (uint32_t h = 0; table && h < 48; ++h) {
    uint32_t start = REX_LOAD_U32(table + h * 24);
    if (start) bottom = std::min(bottom, start);
  }
  return bottom;
}

void LogLayout(uint8_t* base, const char* what) {
  HeapStats s, last;
  uint32_t n = 0;
  for (uint32_t h = 0; h < 48 && GetHeapStats(base, h, s); ++h, ++n) last = s;
  GetHeapStats(base, 5, s);
  REXKRNL_INFO("heaps {}: {} heaps, heap 5 {} KB, last heap {} {} KB", what, n,
               (s.end - s.start) >> 10, n - 1, (last.end - last.start) >> 10);
}
}  // namespace

REX_HOOK_RAW(sub_824150D0) {
  const uint32_t first = ctx.r3.u32, list = ctx.r4.u32, sp = ctx.r1.u32;
  if (first == 5) svr07_fps::OnHeapLayout(list);  // L17 scene gate: match layout = the match scene
  const uint32_t table = REX_LOAD_U32(kHeapTable);
  const int32_t mb = REXCVAR_GET(file_cache_mb);
  // Only the match layout: it loads /MOT/GAME whole into heap 5. Growing heap 5 past 48 MB in
  // the 0x82007128 layout (before the entrances) crashes in sub_821D3E78 on a failed physical alloc.
  if (first == 5 && list == kMatchLayout && table && mb > 0 && mb <= 256) {
    uint32_t n = 0, sum = 0;
    while (n < 43 && REX_LOAD_U32(list + n * 4)) sum += REX_LOAD_U32(list + n * 4), ++n;
    const uint32_t top = REX_LOAD_U32(table + 4 * 24);  // heap 4 start = top of heaps 5..
    const uint32_t avail = (top - ArenaBottom(base)) >> 10;
    const uint32_t want = uint32_t(mb) * 1024, have = n ? REX_LOAD_U32(list) : 0;
    uint32_t grow = want > have ? want - have : 0;
    if (avail > sum + kMinRestKb) grow = std::min(grow, avail - sum - kMinRestKb); else grow = 0;
    if (grow) {
      const uint32_t copy = (sp - 512) & ~15u;
      for (uint32_t i = 0; i <= n; ++i) REX_STORE_U32(copy + i * 4, REX_LOAD_U32(list + i * 4));
      REX_STORE_U32(copy, have + grow);
      ctx.r4.u64 = copy;
      ctx.r1.u64 = copy - 128;  // the callee's frame goes below the copy
    }
    REXKRNL_INFO("file_cache_mb: layout {:#x}: heap 5 {} -> {} KB (arena below heap 4 {} KB, list {} KB)",
                 list, have, have + grow, avail, sum);
  }
  __imp__sub_824150D0(ctx, base);
  ctx.r1.u64 = sp;
  LogLayout(base, "re-carved");
}

REX_HOOK_RAW(sub_82414D38) {
  __imp__sub_82414D38(ctx, base);
  HeapStats s;
  for (uint32_t h = 0; h < 8 && GetHeapStats(base, h, s); ++h) {
    REXKRNL_INFO("heap {}: {:#x}-{:#x} ({} KB)", h, s.start, s.end, (s.end - s.start) >> 10);
  }
}

// Physical regions: the texture/movie/audio pool (header 0x8293CEC8: +0 low, +4 high, +12 region
// count, physical addresses) is split into regions, table 0x8293F6C8, 60 bytes each {+0 used,
// +4 low, +8 high}. sub_821C5A08(sizes_kb, first) re-carves regions first.. from the same mode
// setters as the heaps: region i = sizes_kb[i] KB after the previous one, a negative size = the
// rest of the pool, 0 = unused. XMemAlloc of a physical block (sub_821C5B98) asks
// sub_821D0558(size, low, high, align, protect) for room inside its region only; a full region
// returns 0 and D3D resource creation (sub_821D3020 index buffers) passes the 0 on.
namespace {
constexpr uint32_t kPoolHeader = 0x8293CEC8;
constexpr uint32_t kRegionTable = 0x8293F6C8;

void LogRegions(uint8_t* base) {
  const uint32_t n = std::min<uint32_t>(REX_LOAD_U32(kPoolHeader + 12), 64);
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t e = kRegionTable + i * 60, lo = REX_LOAD_U32(e + 4), hi = REX_LOAD_U32(e + 8);
    if (lo || hi) REXKRNL_INFO("  region {}: {:#x}-{:#x} ({} KB)", i, lo, hi, (hi - lo) >> 10);
  }
}
}  // namespace

REXCVAR_DEFINE_INT32(match_wrestler_region_kb, 1024, "Game",
                     "Size of the match layout's wrestler regions 18-24 in KB; retail 768 (the entrance "
                     "layout has 1024). Pack models overflow 768 -> crash at the match load");

// Match layout list 0x82007238 (from region 6, set together with heap layout 0x820070B0): regions
// 18-24 are 768 KB, the same regions get 1024 KB in the entrance layout 0x820072A8. A pack wrestler
// fills its 768 KB region on the entrance -> match load (rc1c-christy: region 19 full, the next
// index buffer gets 0 and sub_821D30D0 reads [0+24]). The growth comes out of the last region
// (the rest of the pool, 71 MB in-match). The list is read-only xex data: patched copy on the stack.
REX_HOOK_RAW(sub_821C5A08) {
  const uint32_t list = ctx.r3.u32, first = ctx.r4.u32, sp = ctx.r1.u32;
  const int32_t kb = REXCVAR_GET(match_wrestler_region_kb);
  if (list == 0x82007238 && first == 6 && kb > 768 && kb <= 4096) {
    const uint32_t count = std::min<uint32_t>(REX_LOAD_U32(kPoolHeader + 12), 64);
    const uint32_t copy = (sp - 512) & ~15u;
    uint32_t n = 0;
    while (first + n < count) {
      const int32_t v = int32_t(REX_LOAD_U32(list + n * 4));
      const uint32_t r = first + n;
      REX_STORE_U32(copy + n * 4, (r >= 18 && r <= 24 && v == 768) ? uint32_t(kb) : uint32_t(v));
      ++n;
      if (v < 0) break;
    }
    ctx.r3.u64 = copy;
    ctx.r1.u64 = copy - 128;  // the callee's frame goes below the copy
  }
  __imp__sub_821C5A08(ctx, base);
  ctx.r1.u64 = sp;
  if (!REXCVAR_GET(log_heap_failures)) return;
  REXKRNL_INFO("regions re-carved: list {:#x} from {} (pool {:#x}-{:#x})", list, first,
               REX_LOAD_U32(kPoolHeader), REX_LOAD_U32(kPoolHeader + 4));
  LogRegions(base);
}

REX_HOOK_RAW(sub_821D0558) {
  const uint32_t size = ctx.r3.u32, lo = ctx.r4.u32, hi = ctx.r5.u32;
  __imp__sub_821D0558(ctx, base);
  if (ctx.r3.u32 || !REXCVAR_GET(log_heap_failures)) return;
  int32_t region = -1;
  const uint32_t n = std::min<uint32_t>(REX_LOAD_U32(kPoolHeader + 12), 64);
  for (uint32_t i = 0; i < n && region < 0; ++i) {
    if (REX_LOAD_U32(kRegionTable + i * 60 + 4) == lo) region = int32_t(i);
  }
  REXKRNL_WARN("physical region {}: alloc of {} KB in {:#x}-{:#x} ({} KB) failed", region,
               size >> 10, lo, hi, (hi - lo) >> 10);
}

// File loader buffer: sub_8241C158(size, heap) (from sub_8241D5A8: size = request+36 << 8, heap =
// request+96, file name at request+42; the request is still in r31 here); heap 5 goes through
// the cache below. Log big ones.
REX_HOOK_RAW(sub_8241C158) {
  const uint32_t size = ctx.r3.u32, heap = ctx.r4.u32, req = ctx.r31.u32;
  std::string name;
  if (req >= 0x40000000 && req < 0xC0000000) {
    const char* p = reinterpret_cast<const char*>(REX_RAW_ADDR(req + 42));
    name.assign(p, strnlen(p, 32));
  }
  const bool log = REXCVAR_GET(log_heap_failures) && size >= (1u << 20);
  if (log) REXKRNL_INFO("file buffer: {} KB heap {} for '{}'", size >> 10, int32_t(heap), name);
  __imp__sub_8241C158(ctx, base);
  if (log) REXKRNL_INFO("file buffer: '{}' -> {:#x}", name, ctx.r3.u32);
}

// File cache allocation (manager, size, align), retried until it fits. Log a request that is
// larger than any free block of the cache heap, once per size.
REX_HOOK_RAW(sub_824286B0) {
  if (REXCVAR_GET(log_heap_failures)) {
    static std::mutex m;
    static uint32_t last = 0;
    const uint32_t size = ctx.r4.u32;
    const uint32_t heap = REX_LOAD_U32(ctx.r3.u32 + 68);
    HeapStats s;
    if (heap < 8 && GetHeapStats(base, heap, s) && size > s.largest) {
      std::lock_guard lock(m);
      if (size != last) {
        last = size;
        REXKRNL_WARN("file cache: alloc of {} KB in heap {} ({} MB) must wait: free {} KB, largest {} KB",
                     size >> 10, heap, (s.end - s.start) >> 20, s.free_bytes / 1024, s.largest / 1024);
      }
    }
  }
  __imp__sub_824286B0(ctx, base);
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

REXCVAR_DEFINE_BOOL(rctl_guard, true, "Game",
                    "Skip a prop physics component whose node array pointer got corrupted");

// Prop physics component 'RCTL' (vtable 0x82003300, FINDINGS-rc1-crash.md): the game hangs
// one on ring-side props (bg0.pac STG/WPON 40001). sub_8211FEA8(this, list, owner, count, tag)
// builds it: owner at +28, +32 = +36 = count, +40 = array of count node pointers. The owner's
// component walker (sub_82415B58) calls vtable slot 3 = sub_82120088 every frame while the
// enabled word +8 is non-zero. RC1 crashed there with +40 = 0x00009602, the rest intact:
// something overwrote it. The guard logs the component and disables it instead of crashing,
// and every update compares +40 with the value the constructor left, which dates the write.
namespace {
std::mutex g_rctl_mutex;
std::unordered_map<uint32_t, uint32_t> g_rctl_nodes;  // component -> +40 after construction

bool GuestPointer(uint32_t p) { return p >= 0x40000000 && p < 0xC0000000 && !(p & 3); }

// The node array and its first (up to 64) node pointers.
bool NodesValid(uint8_t* base, uint32_t c) {
  uint32_t nodes = REX_LOAD_U32(c + 40);
  if (!GuestPointer(nodes)) return false;
  uint32_t count = std::min<uint32_t>(REX_LOAD_U32(c + 32), 64);
  for (uint32_t i = 0; i < count; ++i)
    if (!GuestPointer(REX_LOAD_U32(nodes + i * 4))) return false;
  return true;
}

void LogRctl(uint8_t* base, const char* what, uint32_t c, uint32_t before) {
  uint32_t owner = REX_LOAD_U32(c + 28);
  uint32_t kind = GuestPointer(owner) ? REX_LOAD_U32(owner + 64) : 0;
  REXKRNL_WARN("rctl: {} component {:#x} owner {:#x} (owner+64 {:#x}) +40 was {:#x} | +28..+44: "
               "{:08x} {:08x} {:08x} {:08x} {:08x}",
               what, c, owner, kind, before, owner, REX_LOAD_U32(c + 32), REX_LOAD_U32(c + 36),
               REX_LOAD_U32(c + 40), REX_LOAD_U32(c + 44));
}
}  // namespace

REX_HOOK_RAW(sub_8211FEA8) {
  uint32_t c = ctx.r3.u32;
  __imp__sub_8211FEA8(ctx, base);
  std::lock_guard lock(g_rctl_mutex);
  g_rctl_nodes[c] = REX_LOAD_U32(c + 40);
}

REX_HOOK_RAW(sub_82120010) {
  {
    std::lock_guard lock(g_rctl_mutex);
    g_rctl_nodes.erase(ctx.r3.u32);
  }
  __imp__sub_82120010(ctx, base);
}

REX_HOOK_RAW(sub_82120088) {
  uint32_t c = ctx.r3.u32;
  uint32_t nodes = REX_LOAD_U32(c + 40);
  {
    std::lock_guard lock(g_rctl_mutex);
    auto it = g_rctl_nodes.find(c);
    if (it != g_rctl_nodes.end() && it->second != nodes) {
      LogRctl(base, "+40 changed in", c, it->second);
      it->second = nodes;  // log each change once
    }
  }
  if (REXCVAR_GET(rctl_guard) && !NodesValid(base, c)) {
    LogRctl(base, "bad node array, disabled", c, nodes);
    REX_STORE_U32(c + 8, 0);  // the walker skips it from now on
    return;
  }
  __imp__sub_82120088(ctx, base);
}

// Self-tests (FINDINGS-selftest-cvars.md): force the match's wrestlers / arena and make P1 start a
// move on P2 on a timer, so a scripted run can reach any match-up and show any move. All off by default.
REXCVAR_DEFINE_INT32(selftest_p1, -1, "Game", "Self-test: force entrant 0 (P1) to this wrestler id (-1 off)");
REXCVAR_DEFINE_INT32(selftest_p2, -1, "Game", "Self-test: force entrant 1 (P2) to this wrestler id (-1 off)");
REXCVAR_DEFINE_INT32(selftest_attire1, 0, "Game", "Self-test: P1 attire 1-4 (0 = the one picked)");
REXCVAR_DEFINE_INT32(selftest_attire2, 0, "Game", "Self-test: P2 attire 1-4 (0 = the one picked)");
REXCVAR_DEFINE_INT32(selftest_arena, -1, "Game", "Self-test: force the arena (stage id, -1 off)");
REXCVAR_DEFINE_STRING(selftest_move, "", "Game",
                      "Self-test: P1 starts this move id on P2, or a list \"3579,5101,5970\" fired in turn (empty off)");
REXCVAR_DEFINE_DOUBLE(selftest_delay_s, 20.0, "Game", "Self-test: seconds from the match start to the first move");
REXCVAR_DEFINE_DOUBLE(selftest_repeat_s, 15.0, "Game", "Self-test: seconds between moves (0 = once per match)");
REXCVAR_DEFINE_DOUBLE(selftest_link_wait_s, 5.0, "Game",
                      "Self-test: a repeat start waits this long past due for the +80 links to clear, then starts");
REXCVAR_DEFINE_INT32(selftest_move2, 0, "Game", "Self-test: every second start uses this move id (0 off)");
// One-person moves (strikes, taunts: one y0 track, no defender track): the two-person start puts P2 into the move
// as role 1 with no track, P2 stays in it (aonly-3048: def act 3048 still 20 s later) and gets no hit reaction.
REXCVAR_DEFINE_STRING(selftest_solo, "", "Game",
                      "Self-test: these move ids (list like selftest_move, or \"all\") start one-person on P1 "
                      "(sub_82251C00): P2 stays free, the engine plays its own hit reaction (empty off)");
REXCVAR_DEFINE_DOUBLE(selftest_solo_near, 0.0, "Game",
                      "Self-test: a solo start waits until P1-P2 (x/z +256/+264) is this close, at most "
                      "selftest_link_wait_s past due (0 off)");
REXCVAR_DEFINE_INT32(selftest_weapon, 0, "Game",
                     "Self-test: before each start P1 picks up a weapon of this type (3 = steel chair) and the move "
                     "starts as a weapon grapple (0 off)");
REXCVAR_DEFINE_INT32(selftest_weapon_act, 15084, "Game", "Self-test: P1's pick-up action for selftest_weapon");
REXCVAR_DEFINE_DOUBLE(selftest_weapon_wait_s, 3.0, "Game", "Self-test: seconds from the pick-up to the move");
REXCVAR_DEFINE_INT32(selftest_weapon_spawn, 11, "Game",
                     "Self-test: no weapon of selftest_weapon's type in the ring -> spawn this object id into P1's "
                     "hand once per match (sub_820F4480; 11 = the kind the floor pick-up looks for; 0 off)");
// One boot, many matches (svr_selftest --session): the tool rewrites this file before each Rematch; each change is
// applied as runtime cvars (they outrank the command line) and starts a new "live gen".
REXCVAR_DEFINE_STRING(selftest_live, "", "Game",
                      "Self-test: file of name=value cvar lines, applied again whenever it changes (empty off)");
// Match end on demand: P2's body-part HP (CHP +40..+46) set to 0, then P1 pins; again every _every_s, max _tries.
REXCVAR_DEFINE_DOUBLE(selftest_end_s, 0.0, "Game",
                      "Self-test: end the match this many s after the last move of the list started (no list: after "
                      "the match start) with a pin on a P2 with no HP left (0 off)");
REXCVAR_DEFINE_INT32(selftest_end_move, 5296, "Game",
                     "Self-test: the pin for selftest_end_s (5296 = Jackhammer Pin 2, from a standing front grapple)");
REXCVAR_DEFINE_DOUBLE(selftest_end_every_s, 10.0, "Game", "Self-test: seconds between pin tries");
REXCVAR_DEFINE_INT32(selftest_end_tries, 0, "Game", "Self-test: pin tries per match (0 = until the match ends)");
// Move tests: entrant byte +76 = COM flag (sub_8218AEA0 counts humans as +76 == 0; the match init sub_821A4BD8 sets
// wrestler +760 = 1 for +76 == 0, the tag swap in sub_820FA3C0 hands +760 over = "player-controlled"), +77 = user /
// pad index (P1 0, COM -1; 255 -> default user, sub_824B6788). P2 becomes a human on pad 2, which nothing presses.
REXCVAR_DEFINE_BOOL(selftest_p2_idle, false, "Game",
                    "Self-test / move tests: P2 (entrant 1) is a human on pad 2 with no input: no AI, no reversals");

// Match setup (sub_821B8CE0 returns it): entrant i's u16 code at +80 + 36*i = wrestler id * 100 +
// attire digits (exactly 25500 = empty: the game tests it with beq; extras 167-632 use their own
// id, so 25600-63299 is a real pick and the roster hooks handle >= 10000), stage
// id = signed byte +1148. Every reader goes through the getter, so the forced values are put back
// on each call while a pick is there; the select screen's own cursor state is left alone.
namespace {
constexpr uint32_t kMatchSetup = 0x82933AA8;
constexpr uint32_t kWrestlers = 0x829488BC;  // 8 ring people (sub_820C6EB0), [0] P1, [1] P2

void ForceEntrant(uint8_t* base, uint32_t i, int32_t id, int32_t attire) {
  static std::mutex m;
  static uint32_t logged[2] = {0, 0};
  const uint32_t at = kMatchSetup + 80 + 36 * i;
  const uint32_t code = REX_LOAD_U16(at);
  if (id < 0 || id == 255 || id > 632 || code == 25500) return;
  uint32_t want = uint32_t(id) * 100 + (attire > 0 ? uint32_t(attire) * 10 : code % 100);
  if (code == want) return;
  REX_STORE_U16(at, uint16_t(want));
  std::lock_guard lock(m);
  if (logged[i] != code) {
    logged[i] = code;
    REXKRNL_INFO("selftest: entrant {} code {} -> {} (id {}, attire {})", i, code, want, id, attire);
  }
}

// selftest_p2_idle: written from P2's per-frame update (wrestlers exist = the select screen is over, so its COM pick
// stays), entrant 1's own values put back once no P2 update came for 3 s (results, menus).
struct P2Idle {
  std::mutex m;
  bool on = false;
  uint8_t com = 0, pad = 0;
  std::chrono::steady_clock::time_point seen{};
} g_idle;
constexpr uint32_t kEntrant1 = kMatchSetup + 72 + 36;

void P2IdleFrame(uint8_t* base, uint32_t p2) {
  std::lock_guard lock(g_idle.m);
  if (!g_idle.on) {
    g_idle.on = true;
    g_idle.com = REX_LOAD_U8(kEntrant1 + 4);
    g_idle.pad = REX_LOAD_U8(kEntrant1 + 5);
    REXKRNL_INFO("selftest: p2 idle on, entrant 1 com {} pad {} -> 0 / 1, P2 {:#x} +760 {}", g_idle.com,
                 int8_t(g_idle.pad), p2, REX_LOAD_U32(p2 + 760));
  }
  g_idle.seen = std::chrono::steady_clock::now();
  if (REX_LOAD_U8(kEntrant1 + 4)) REX_STORE_U8(kEntrant1 + 4, 0);
  if (REX_LOAD_U8(kEntrant1 + 5) != 1) REX_STORE_U8(kEntrant1 + 5, 1);
  if (REX_LOAD_U32(p2 + 760) != 1) REX_STORE_U32(p2 + 760, 1);
}

void P2IdleRestore(uint8_t* base) {
  std::unique_lock lock(g_idle.m, std::try_to_lock);
  if (!lock || !g_idle.on || std::chrono::steady_clock::now() - g_idle.seen < std::chrono::seconds(3)) return;
  g_idle.on = false;
  REX_STORE_U8(kEntrant1 + 4, g_idle.com);
  REX_STORE_U8(kEntrant1 + 5, g_idle.pad);
  REXKRNL_INFO("selftest: p2 idle off, entrant 1 com {} pad {} back", g_idle.com, int8_t(g_idle.pad));
}

// selftest_live: checked every 0.5 s from the getter (it runs at each match load and in menus). The tool replaces the
// file in one step (tmp + rename), so a changed text is a whole spec. g_live_mutex also guards readers of the string
// cvar selftest_move against the set.
std::mutex g_live_mutex;
std::atomic<uint32_t> g_live_gen{0};

void ReloadLive() {
  static std::mutex m;
  static std::chrono::steady_clock::time_point next{};
  static std::string text;
  std::unique_lock busy(m, std::try_to_lock);
  const auto now = std::chrono::steady_clock::now();
  if (!busy || now < next) return;
  next = now + std::chrono::milliseconds(500);
  const std::string path = REXCVAR_GET(selftest_live);
  if (path.empty()) return;
  std::ifstream f(path, std::ios::binary);
  if (!f) return;
  std::string now_text{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
  if (now_text == text) return;
  text = now_text;
  std::string applied, rejected;
  std::lock_guard lock(g_live_mutex);
  size_t at = 0;
  while (at < text.size()) {
    size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    std::string l = text.substr(at, end - at);
    at = end + 1;
    l.erase(0, l.find_first_not_of(" \t\r"));
    l.erase(l.find_last_not_of(" \t\r") + 1);
    const size_t eq = l.find('=');
    if (l.empty() || l[0] == '#' || eq == std::string::npos) continue;
    std::string k = l.substr(0, eq), v = l.substr(eq + 1);
    k.erase(k.find_last_not_of(" \t") + 1);
    v.erase(0, v.find_first_not_of(" \t"));
    if (k == "selftest_live") continue;
    (rex::cvar::SetFlagByName(k, v) ? applied : rejected) += " " + k + "=" + v;
  }
  REXKRNL_INFO("selftest: live gen {} from {}:{}{}{}", ++g_live_gen, path, applied, rejected.empty() ? "" : " REJECTED:",
               rejected);
}
}  // namespace

REX_HOOK_RAW(sub_821B8CE0) {
  __imp__sub_821B8CE0(ctx, base);
  ReloadLive();
  ForceEntrant(base, 0, REXCVAR_GET(selftest_p1), REXCVAR_GET(selftest_attire1));
  ForceEntrant(base, 1, REXCVAR_GET(selftest_p2), REXCVAR_GET(selftest_attire2));
  P2IdleRestore(base);
  const int32_t arena = REXCVAR_GET(selftest_arena);
  if (arena >= 0 && arena < 128 && REX_LOAD_U8(kMatchSetup + 1148) != uint32_t(arena)) {
    REXKRNL_INFO("selftest: arena {} -> {}", int8_t(REX_LOAD_U8(kMatchSetup + 1148)), arena);
    REX_STORE_U8(kMatchSetup + 1148, uint8_t(arena));
  }
}

// The select screen's picks (R5a): the select object (sub_82307370 / sub_82308C00, r31) keeps a picks block at +884:
// 40-byte entries, entry i = entrant i, u32 wrestler id at +16, count at +288. Each frame those functions copy the
// grid's hovered id (vcall +88) into the entry when it differs (then sub_82301818 redraws), and the panels and the VS
// screen's models (sub_8246A958) read the entry. Entry = +984 == 1 ? the last one : r26 / r28 (= 40 * player).
// The hook sits after `clrlwi r10,r3,16` (the hovered id) at the compares and the stores and swaps in the forced
// id, so the entry holds it, the compare sees no change and nothing redraws again. Attire digits are not in the entry
// (+24 is a table index): selftest_attire1/2 still apply from the match setup on.
void SelftestSelectPick(PPCRegister& id, PPCRegister& sel, PPCRegister& off) {
  const int32_t p1 = REXCVAR_GET(selftest_p1), p2 = REXCVAR_GET(selftest_p2);
  if (p1 < 0 && p2 < 0) return;
  uint8_t* base = REX_KERNEL_MEMORY()->virtual_membase();
  const uint32_t picks = REX_LOAD_U32(sel.u32 + 884);
  const uint32_t i = !picks ? 0xFFFFFFFF
                     : REX_LOAD_U32(sel.u32 + 984) == 1 ? REX_LOAD_U32(picks + 288) - 1 : off.u32 / 40;
  static std::atomic<int> entry_logs{0};
  if (entry_logs.fetch_add(1) < 4)  // R5a: proves the site runs (r5-vs2 had no select pick line)
    REXKRNL_INFO("selftest: select hook sel {:08X} picks {:08X} mode {} off {} -> entry {} id {}", sel.u32, picks,
                 REX_LOAD_U32(sel.u32 + 984), off.u32, int32_t(i), id.u32);
  if (!picks) return;
  const int32_t want = i == 0 ? p1 : i == 1 ? p2 : -1;
  if (want < 0 || want == 255 || want > 632 || id.u32 == uint32_t(want)) return;
  static std::mutex m;
  static uint32_t logged[2] = {0xFFFFFFFF, 0xFFFFFFFF};
  {
    std::lock_guard lock(m);
    if (logged[i] != id.u32) {
      logged[i] = id.u32;
      REXKRNL_INFO("selftest: select pick {} id {} -> {}", i, id.u32, want);
    }
  }
  id.u64 = uint32_t(want);
}

// The VS screen's models (sub_8246A958): per entrant r30, entry r29 = [r23+584] + 40*r30, id at +16, read right
// after `add r29,r22,r11`. The forced id is written into the entry there, so the VS models (and the size lookup
// further down, which re-reads +16) use it even when the select screen's own path missed SelftestSelectPick.
void SelftestVsPick(PPCRegister& entry, PPCRegister& idx) {
  const int32_t want = idx.u32 == 0 ? REXCVAR_GET(selftest_p1) : idx.u32 == 1 ? REXCVAR_GET(selftest_p2) : -1;
  if (want < 0 || want == 255 || want > 632) return;
  uint8_t* base = REX_KERNEL_MEMORY()->virtual_membase();
  const uint32_t have = REX_LOAD_U32(entry.u32 + 16);
  if (have == uint32_t(want)) return;
  REXKRNL_INFO("selftest: vs pick {} id {} -> {}", idx.u32, have, want);
  REX_STORE_U32(entry.u32 + 16, uint32_t(want));
}

// The move: sub_82102550 is a wrestler's per-frame action update (this = component, wrestler at
// +28). After P1's (entrant 0) update, a due move starts through the game's own two-person start
// sub_820C9CF0(attacker, defender, move, variant 0 = random): it links both (+80), sets each
// one's action (+192, attacker role 0 / defender role 1) and syncs their positions. A start waits
// while either wrestler is linked to someone. A new match = P1 not updated for 3 s, or a new P1.
namespace {
using Clock = std::chrono::steady_clock;
struct SelfTestMove {
  uint32_t p1 = 0;
  Clock::time_point seen{}, start{}, check{}, armed_at{}, last_start{}, end_next{}, wait_log{};
  int count = 0;
  int end_tries = 0;      // selftest_end_s pins started
  uint32_t gen = 0;       // g_live_gen at the match start
  int32_t last_id = 0;  // the move of the last start, for its +1 s line
  bool pending_check = false;
  bool armed = false;  // selftest_weapon: picked up, the move is next
  bool no_weapon_logged = false;
  bool spawn_tried = false;  // selftest_weapon_spawn: one spawn per match
  int32_t spawn_slot = -1;
} g_st;
// A live gen whose match already got its end pins: the post-match scenes count as new matches, nothing fires there
// until the tool writes the next spec.
uint32_t g_done_gen = UINT32_MAX;

// "3579" or "3579,5101,5970" (',' ';' or ' ' between ids) -> the ids > 0.
std::vector<int32_t> ParseIds(const std::string& text) {
  std::vector<int32_t> ids;
  int32_t v = 0;
  bool any = false;
  for (const char c : text + ",") {
    if (c >= '0' && c <= '9') {
      v = v * 10 + (c - '0');
      any = true;
    } else {
      if (any && v > 0) ids.push_back(v);
      v = 0;
      any = false;
    }
  }
  return ids;
}

// selftest_move parsed once per text.
const std::vector<int32_t>& SelfTestMoves() {
  static std::string text;
  static std::vector<int32_t> ids;
  std::string now;
  {
    std::lock_guard lock(g_live_mutex);
    now = REXCVAR_GET(selftest_move);
  }
  if (now == text) return ids;
  text = now;
  ids = ParseIds(text);
  return ids;
}

// selftest_solo: is this move a one-person start ("all" = every move).
bool SoloMove(int32_t id) {
  std::string now;
  {
    std::lock_guard lock(g_live_mutex);
    now = REXCVAR_GET(selftest_solo);
  }
  if (now == "all") return true;
  const auto ids = ParseIds(now);
  return std::find(ids.begin(), ids.end(), id) != ids.end();
}

// Weapons (FINDINGS-selftest-cvars.md "Weapons"): ring objects in the 72-slot table 0x827A8230 with kind
// +64 = 11, own slot +508, weapon type +512 (= the MOT/WEAP child / key x, 3 = steel chair). A wrestler
// holds slot +668 (-1 none). sub_820CA160(w, act, type) = wrestler + held weapon action (the game's own
// pick-up passes 15084); sub_820CA0F0(att, def, move, type) = weapon grapple: the two-person start plus
// the held weapon as role 2 (the y2 prop track).
constexpr uint32_t kObjects = 0x827A8230;

float LoadF32(uint8_t* base, uint32_t at) {
  const uint32_t u = REX_LOAD_U32(at);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

uint32_t HeldWeapon(uint8_t* base, uint32_t w) {
  const uint32_t slot = REX_LOAD_U32(w + 668);
  return slot < 72 ? REX_LOAD_U32(kObjects + 4 * slot) : 0;
}

// The nearest weapon of this type (x / z at +256 / +264), logging every weapon once per match.
int32_t FindWeapon(uint8_t* base, uint32_t w, int32_t type, bool log) {
  int32_t best = -1;
  float best_d = 0;
  for (uint32_t i = 0; i < 72; ++i) {
    const uint32_t o = REX_LOAD_U32(kObjects + 4 * i);
    if (!o || REX_LOAD_U32(o + 64) != 11) continue;
    const float dx = LoadF32(base, o + 256) - LoadF32(base, w + 256);
    const float dz = LoadF32(base, o + 264) - LoadF32(base, w + 264);
    const float d = dx * dx + dz * dz;
    if (log)
      REXKRNL_INFO("selftest: weapon slot {} obj {:#x} type {} act {} own {} holder {:#x} at ({:.0f} {:.0f} {:.0f})", i,
                   o, REX_LOAD_U32(o + 512), REX_LOAD_U32(o + 192), REX_LOAD_U32(o + 508), REX_LOAD_U32(o + 80),
                   LoadF32(base, o + 256), LoadF32(base, o + 260), LoadF32(base, o + 264));
    if (int32_t(REX_LOAD_U32(o + 512)) == type && (best < 0 || d < best_d)) {
      best = int32_t(i);
      best_d = d;
    }
  }
  return best;
}

// No weapon of the type in the ring (normal matches start with none; the game spawns them when someone takes one):
// sub_820F4480(w, id) = the game's spawn into a wrestler's hand (object factory sub_82195520(-1, id, 0, 0), free
// table slot -> +508, w +668 = slot, w's position, +192 = -1; nothing if an id object with +406 = 11 exists).
// The type +512 comes from the object def table *0x827A820C (sub_8218EEE0(id) = record [def[id + 8]] byte 2), so
// the ids with a type are logged first. Returns P1's held slot after the spawn, -1 none.
int32_t SpawnWeapon(PPCContext& ctx, uint8_t* base, uint32_t w, int32_t id) {
  const PPCContext saved = ctx;
  if (REX_LOAD_U32(0x827A820C)) {
    std::string types;
    for (int32_t i = 0; i < 200; ++i) {
      ctx.r3.u64 = uint32_t(i);
      ctx.r4.u64 = 0;
      sub_8218EEE0(ctx, base);
      if (const uint32_t t = ctx.r3.u32 & 0xFF) types += fmt::format(" {}:{}", i, t);
    }
    ctx = saved;
    REXKRNL_INFO("selftest: object def types (id:type){}", types);
  }
  ctx.r3.u64 = w;
  ctx.r4.u64 = uint32_t(id);
  sub_820F4480(ctx, base);
  ctx = saved;
  const uint32_t slot = REX_LOAD_U32(w + 668);
  const uint32_t o = HeldWeapon(base, w);
  REXKRNL_INFO("selftest: spawn id {} -> P1 held slot {} obj {:#x} kind {} type {}", id, int32_t(slot), o,
               o ? REX_LOAD_U32(o + 64) : 0, o ? REX_LOAD_U32(o + 512) : 0);
  return o ? int32_t(slot) : -1;
}

// The table is in creation order: [0] = P1, [1] = P2, then the referee(s) (proof run
// selftest-cvars4). +405 is not the entrant index (it changes in-match) and +432 bit 58 is never set.
// P2 = P1's target (+1248, what the game's own starts pass as defender) when that is [1], else [1].
uint32_t FindOpponent(uint8_t* base, uint32_t p1) {
  const uint32_t p2 = REX_LOAD_U32(kWrestlers + 4);
  const uint32_t target = REX_LOAD_U32(p1 + 1248);
  return target && target != p1 && target == p2 ? target : p2;
}

// A person's component by 4cc: the list head is at +480, sub_82415908(head, tag) walks it (children +16, then the
// +12 chain, tag at +4) and callers take the component at +20. Same walk, capped.
uint32_t FindComponent(uint8_t* base, uint32_t w, uint32_t tag) {
  const uint32_t head = w + 480;
  uint32_t c = REX_LOAD_U32(head + 16);
  for (int n = 0; c && n < 256; ++n, c = REX_LOAD_U32(c + 16))
    if (REX_LOAD_U32(c + 4) == tag) return REX_LOAD_U32(c + 20);
  for (uint32_t r = head, n = 0; r && n < 256; ++n, r = REX_LOAD_U32(r + 12))
    if (REX_LOAD_U32(r + 4) == tag) return REX_LOAD_U32(r + 20);
  return 0;
}

// CHP (FINDINGS-movedata byte 5): 4 body-part HP s16 at +40..+46, lower = more hurt (sub_8210CF48 rolls the daze
// against the lowest). -> the old values, all set to 0.
std::string ZeroHp(uint8_t* base, uint32_t w) {
  const uint32_t chp = FindComponent(base, w, 0x00434850);  // 'CHP'
  if (!chp) return " no CHP";
  std::string old = fmt::format(" CHP {:#x} hp", chp);
  for (uint32_t k = 0; k < 4; ++k) {
    old += fmt::format(" {}", int16_t(REX_LOAD_U16(chp + 40 + 2 * k)));
    REX_STORE_U16(chp + 40 + 2 * k, 0);
  }
  return old;
}

void StopEntranceSkip(const char* why);  // below, with the entrance skip
}  // namespace

// Entrance probe (FINDINGS-wk-arenas 6.2): every N updates of P1 / P2, the position (+256..+264) before and
// after the update, the action and the component, to see whether the entrance root lands there and who writes it.
// selftest_probe_dz moves P1's z after each update (0 off), to see whether the next frame keeps or rewrites it.
REXCVAR_DEFINE_INT32(selftest_probe, 0, "Game", "Self-test: log P1/P2 position every N updates (0 off)");
REXCVAR_DEFINE_DOUBLE(selftest_probe_dz, 0.0, "Game", "Self-test: add this to P1's z after each update (0 off)");

// Field watch (move-test mode research): at the first in-ring frame, the entrant records (match setup +72 + 36*i)
// and the small-int words where P1 and P2 differ (controller type candidates); then every N s the P1 / P2 / match
// setup words that changed (pointers skipped), to find the momentum meter next to the HUD captures.
// movetest-probe (11-04): no meter word in P1 +0..+2600 (+1152 = the hit wrestler's 0-3600 recovery timer, it
// decays ~36/s), so P1's components (the +480 list, "c<tag>") are watched too.
REXCVAR_DEFINE_DOUBLE(selftest_watch_s, 0.0, "Game", "Self-test: log changed P1/P2 words every N s (0 off)");

namespace {
constexpr uint32_t kWatchBytes = 2600, kSetupBytes = 1400, kCompBytes = 1024;
struct Watched {
  std::string name;
  uint32_t at = 0, bytes = 0;
  std::vector<uint32_t> prev;
};
struct Watch {
  uint32_t p1 = 0;
  Clock::time_point seen{}, next{};
  bool started = false;
  std::vector<Watched> objs;
} g_watch;

// P1's components, both walks of FindComponent, each object once, at most 64.
void AddComponents(uint8_t* base, uint32_t w, std::vector<Watched>& out) {
  const uint32_t head = w + 480;
  std::vector<uint32_t> links;
  for (uint32_t c = REX_LOAD_U32(head + 16), n = 0; c && n < 256; ++n, c = REX_LOAD_U32(c + 16)) links.push_back(c);
  for (uint32_t r = REX_LOAD_U32(head + 12), n = 0; r && n < 256; ++n, r = REX_LOAD_U32(r + 12)) links.push_back(r);
  std::string list;
  for (const uint32_t l : links) {
    const uint32_t o = REX_LOAD_U32(l + 20), tag = REX_LOAD_U32(l + 4);
    if (o < 0x40000000 || out.size() >= 67 ||
        std::any_of(out.begin(), out.end(), [&](const Watched& x) { return x.at == o; }))
      continue;
    std::string t;
    for (int s = 24; s >= 0; s -= 8)
      if (const char ch = char((tag >> s) & 0xFF); ch > ' ' && ch < 127) t += ch;
    out.push_back({"c" + t, o, kCompBytes, {}});
    list += fmt::format(" {}={:#x}", t, o);
  }
  REXKRNL_INFO("selftest: watch P1 components{}", list);
}

bool Plain(uint32_t v) {
  if (int32_t(v) >= -100000 && int32_t(v) <= 100000) return true;
  float f;
  std::memcpy(&f, &v, 4);
  return std::isfinite(f) && std::fabs(f) >= 0.001f && std::fabs(f) <= 100000.f;
}

void WatchFields(uint8_t* base, uint32_t w) {
  const double every = REXCVAR_GET(selftest_watch_s);
  if (every <= 0) return;
  const auto now = Clock::now();
  if (w != g_watch.p1 || now - g_watch.seen > std::chrono::seconds(3)) g_watch = Watch{w, now, now};
  g_watch.seen = now;
  const uint32_t p2 = REX_LOAD_U32(kWrestlers + 4);
  if (!REX_LOAD_U32(kWrestlers + 8) || !p2 || now < g_watch.next) return;
  g_watch.next = now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(every));
  if (!g_watch.started) {
    g_watch.started = true;
    for (uint32_t i = 0; i < 4; ++i) {
      std::string hex;
      for (uint32_t k = 0; k < 36; ++k) hex += fmt::format("{:02x}{}", REX_LOAD_U8(kMatchSetup + 72 + 36 * i + k), k % 4 == 3 ? " " : "");
      REXKRNL_INFO("selftest: watch entrant {} [{}]", i, hex);
    }
    std::string diff;
    for (uint32_t o = 0; o < kWatchBytes; o += 4) {
      const uint32_t a = REX_LOAD_U32(w + o), b = REX_LOAD_U32(p2 + o);
      if (a != b && a <= 20 && b <= 20) diff += fmt::format(" +{}={}/{}", o, a, b);
    }
    REXKRNL_INFO("selftest: watch P1 {:#x} P2 {:#x} small-int diffs{}", w, p2, diff);
    g_watch.objs = {{"p1", w, kWatchBytes, {}}, {"p2", p2, kWatchBytes, {}}, {"ms", kMatchSetup, kSetupBytes, {}}};
    AddComponents(base, w, g_watch.objs);
  }
  for (Watched& obj : g_watch.objs) {
    const uint32_t n = obj.bytes / 4;
    std::vector<uint32_t> cur(n);
    for (uint32_t k = 0; k < n; ++k) cur[k] = REX_LOAD_U32(obj.at + 4 * k);
    if (obj.prev.size() == n) {
      std::string line;
      int count = 0;
      for (uint32_t k = 0; k < n && count < 48; ++k) {
        const uint32_t a = obj.prev[k], b = cur[k];
        if (a == b || !Plain(a) || !Plain(b)) continue;
        float fa, fb;
        std::memcpy(&fa, &a, 4);
        std::memcpy(&fb, &b, 4);
        line += (int32_t(b) >= -100000 && int32_t(b) <= 100000) ? fmt::format(" +{}:{}>{}", 4 * k, int32_t(a), int32_t(b))
                                                                  : fmt::format(" +{}:{:.2f}>{:.2f}", 4 * k, fa, fb);
        ++count;
      }
      if (count)
        REXKRNL_INFO("selftest: watch {} act {}{}", obj.name, obj.name[0] == 'p' ? REX_LOAD_U32(obj.at + 192) : 0,
                     line);
    }
    obj.prev = std::move(cur);
  }
}
}  // namespace

REX_HOOK_RAW(sub_82102550) {
  const uint32_t comp = ctx.r3.u32;
  const uint32_t w = REX_LOAD_U32(comp + 28);
  const int32_t probe = REXCVAR_GET(selftest_probe);
  const int who = !w ? -1 : w == REX_LOAD_U32(kWrestlers) ? 0 : w == REX_LOAD_U32(kWrestlers + 4) ? 1 : -1;
  float before[3] = {};
  if (probe > 0 && who >= 0)
    for (int k = 0; k < 3; ++k) before[k] = LoadF32(base, w + 256 + 4 * k);
  __imp__sub_82102550(ctx, base);
  if (probe > 0 && who >= 0) {
    static uint32_t n[2];
    static auto t0 = Clock::now();
    const double dz = REXCVAR_GET(selftest_probe_dz);
    if (who == 0 && dz != 0) {
      const float z = LoadF32(base, w + 264) + float(dz);
      uint32_t u;
      std::memcpy(&u, &z, 4);
      REX_STORE_U32(w + 264, u);
    }
    if (n[who]++ % uint32_t(probe) == 0)
      REXKRNL_INFO("selftest: probe {:.2f} s p{} w {:#x} comp {:#x} act {} {} ref {:#x} pos ({:.1f} {:.1f} {:.1f}) -> "
                   "({:.1f} {:.1f} {:.1f})",
                   std::chrono::duration<double>(Clock::now() - t0).count(), who, w, comp, REX_LOAD_U32(w + 192),
                   REX_LOAD_U32(w + 196), REX_LOAD_U32(kWrestlers + 8), before[0], before[1], before[2],
                   LoadF32(base, w + 256), LoadF32(base, w + 260), LoadF32(base, w + 264));
    if (n[who] % (uint32_t(probe) * 8) == 1) {
      const uint32_t root = REX_LOAD_U32(w + 272), m = REX_LOAD_U32(comp + 28);
      REXKRNL_INFO("selftest: probe p{} vt {:#x} root {:#x} ({:.1f} {:.1f} {:.1f}) m {:#x}", who, REX_LOAD_U32(comp),
                   root, root ? LoadF32(base, root) : 0.f, root ? LoadF32(base, root + 4) : 0.f,
                   root ? LoadF32(base, root + 8) : 0.f, m);
    }
  }
  if (w && w == REX_LOAD_U32(kWrestlers)) WatchFields(base, w);
  if (w && w == REX_LOAD_U32(kWrestlers + 4) && REXCVAR_GET(selftest_p2_idle)) P2IdleFrame(base, w);
  const auto& moves = SelfTestMoves();
  const double end_s = REXCVAR_GET(selftest_end_s);
  if ((moves.empty() && end_s <= 0) || !w || w != REX_LOAD_U32(kWrestlers)) return;
  const auto now = Clock::now();
  if (w != g_st.p1 || now - g_st.seen > std::chrono::seconds(3)) {
    if (g_st.end_tries > 0) g_done_gen = g_st.gen;
    g_st = SelfTestMove{w, now, now, now};
    g_st.gen = g_live_gen;
    std::string text;
    {
      std::lock_guard lock(g_live_mutex);
      text = REXCVAR_GET(selftest_move);
    }
    REXKRNL_INFO("selftest: match, P1 obj {:#x} P2 obj {:#x} (P1 target {:#x}), moves {} ({}) in {} s, end {} s, "
                 "live gen {}{}", w, FindOpponent(base, w), REX_LOAD_U32(w + 1248), text, moves.size(),
                 REXCVAR_GET(selftest_delay_s), end_s, g_st.gen, g_st.gen == g_done_gen ? " (already ended: idle)" : "");
  }
  g_st.seen = now;
  const double repeat = REXCVAR_GET(selftest_repeat_s);
  const double due = REXCVAR_GET(selftest_delay_s) + g_st.count * std::max(repeat, 0.0);
  const double since = std::chrono::duration<double>(now - g_st.start).count();
  // A list start that is due but held back: say which gate, once per 5 s.
  const bool list_due = !moves.empty() && g_st.count < int(moves.size()) && (g_st.count == 0 || repeat > 0) &&
                        since >= due;
  const auto waiting = [&](const char* why, uint32_t a, uint32_t b) {
    if (!list_due || now < g_st.wait_log) return;
    g_st.wait_log = now + std::chrono::seconds(5);
    REXKRNL_INFO("selftest: waiting #{} ({:.0f} s past due): {} ({:#x} / {:#x})", g_st.count + 1, since - due, why, a,
                 b);
  };
  if (g_st.gen == g_done_gen) return waiting("match already ended", g_st.gen, g_done_gen);
  // The VS preview has only the two wrestlers in the table; the ring adds the referee.
  if (!REX_LOAD_U32(kWrestlers + 8)) return waiting("no referee in the table", 0, 0);
  // speed3-session: the last entrance scene's exit never came, its A presses ran through the match, the replay and
  // the results (= Rematch with the old spec). The fight is on by the first start: no more presses.
  if (since >= REXCVAR_GET(selftest_delay_s)) StopEntranceSkip("match under way");
  const uint32_t p2 = FindOpponent(base, w);
  if (g_st.pending_check && now >= g_st.check && p2) {
    g_st.pending_check = false;
    const uint32_t held = HeldWeapon(base, w);
    REXKRNL_INFO("selftest: +1 s move {} act {} / {} (link {:#x} / {:#x}), weapon {:#x} act {}", g_st.last_id,
                 REX_LOAD_U32(w + 192),
                 REX_LOAD_U32(p2 + 192), REX_LOAD_U32(w + 80), REX_LOAD_U32(p2 + 80), held,
                 held ? REX_LOAD_U32(held + 192) : 0);
  }
  // The end: once the list is done (each move once; no list = from the match start), end_s later, P1 pins P2.
  if (end_s > 0 && (moves.empty() || g_st.count >= int(moves.size()) || (repeat <= 0 && g_st.count > 0))) {
    if (!p2) return;
    // Keep P2's HP at 0 while the pin runs (the kick-out comes during the count).
    if (g_st.end_tries > 0 && REX_LOAD_U32(p2 + 80) == w) ZeroHp(base, p2);
    const auto from = g_st.count ? g_st.last_start : g_st.start;
    const int32_t max_tries = REXCVAR_GET(selftest_end_tries);
    if (std::chrono::duration<double>(now - from).count() < end_s || now < g_st.end_next ||
        (max_tries > 0 && g_st.end_tries >= max_tries) || REX_LOAD_U32(w + 80) || REX_LOAD_U32(p2 + 80))
      return;
    const int32_t id = REXCVAR_GET(selftest_end_move);
    ++g_st.end_tries;
    g_st.end_next = now + std::chrono::duration_cast<Clock::duration>(
                              std::chrono::duration<double>(std::max(REXCVAR_GET(selftest_end_every_s), 1.0)));
    const std::string hp = ZeroHp(base, p2);
    REXKRNL_INFO("selftest: end #{} pin {} att {:#x} (act {}) def {:#x} (act {}),{} -> 0, meter {:.0f}", g_st.end_tries,
                 id, w, REX_LOAD_U32(w + 192), p2, REX_LOAD_U32(p2 + 192), hp, LoadF32(base, p2 + 1152));
    g_st.last_id = id;
    PPCContext saved = ctx;
    ctx.r3.u64 = w;
    ctx.r4.u64 = p2;
    ctx.r5.u64 = uint32_t(id);
    ctx.r6.u64 = 0;
    sub_820C9CF0(ctx, base);
    ctx = saved;
    g_st.check = now + std::chrono::seconds(1);
    g_st.pending_check = true;
    return;
  }
  if (moves.empty()) return;
  if (g_st.count > 0 && repeat <= 0) return;
  if (since < due) return;
  if (!p2) return waiting("no P2", 0, 0);
  // A link (+80) can stay set while P2 lies down after the last move (moves3-1: #2 never started). The first start
  // waits for it; a repeat start waits selftest_link_wait_s at most, then starts anyway (the start relinks both).
  const uint32_t link1 = REX_LOAD_U32(w + 80), link2 = REX_LOAD_U32(p2 + 80);
  if ((link1 || link2) && (g_st.count == 0 || since - due < REXCVAR_GET(selftest_link_wait_s)))
    return waiting("linked", link1, link2);
  if (link1 || link2)
    REXKRNL_INFO("selftest: link wait over for #{}, starting with links {:#x} / {:#x}", g_st.count + 1, link1, link2);
  const int32_t weapon = REXCVAR_GET(selftest_weapon);
  PPCContext saved = ctx;
  if (weapon > 0 && !g_st.armed) {
    int32_t slot = FindWeapon(base, w, weapon, g_st.count == 0);
    if (slot < 0 && REXCVAR_GET(selftest_weapon_spawn) > 0 && !g_st.spawn_tried) {
      g_st.spawn_tried = true;
      g_st.spawn_slot = SpawnWeapon(ctx, base, w, REXCVAR_GET(selftest_weapon_spawn));
    }
    // The spawned weapon is P1's for the whole match, held or dropped (its type may not be selftest_weapon's).
    if (slot < 0 && g_st.spawn_slot >= 0 && REX_LOAD_U32(kObjects + 4 * uint32_t(g_st.spawn_slot)))
      slot = g_st.spawn_slot;
    if (slot < 0) {
      if (!g_st.no_weapon_logged) REXKRNL_INFO("selftest: no weapon of type {} in the ring objects", weapon);
      g_st.no_weapon_logged = true;
      return;
    }
    const int32_t act = REXCVAR_GET(selftest_weapon_act);
    REXKRNL_INFO("selftest: pick-up act {} weapon slot {} (P1 held slot was {})", act, slot,
                 int32_t(REX_LOAD_U32(w + 668)));
    REX_STORE_U32(w + 668, uint32_t(slot));
    ctx.r3.u64 = w;
    ctx.r4.u64 = uint32_t(act);
    ctx.r5.u64 = REX_LOAD_U32(REX_LOAD_U32(kObjects + 4 * uint32_t(slot)) + 512);  // as the floor pick-up: its +512
    sub_820CA160(ctx, base);
    ctx = saved;
    g_st.armed = true;
    g_st.armed_at = now;
    return;
  }
  if (weapon > 0 && now - g_st.armed_at < std::chrono::duration<double>(REXCVAR_GET(selftest_weapon_wait_s))) return;
  const int32_t move2 = REXCVAR_GET(selftest_move2);
  const int32_t id = move2 > 0 && g_st.count % 2 ? move2 : moves[size_t(g_st.count) % moves.size()];
  const bool solo = SoloMove(id);
  const float dx = LoadF32(base, p2 + 256) - LoadF32(base, w + 256);
  const float dz = LoadF32(base, p2 + 264) - LoadF32(base, w + 264);
  const double dist = std::sqrt(double(dx) * dx + double(dz) * dz);
  const double near_d = REXCVAR_GET(selftest_solo_near);  // not 'near': a Windows macro
  if (solo && near_d > 0 && dist > near_d && since - due < REXCVAR_GET(selftest_link_wait_s))
    return waiting("solo: P2 far", uint32_t(dist), uint32_t(near_d));
  g_st.armed = false;
  ++g_st.count;
  g_st.last_start = now;
  const uint32_t held = HeldWeapon(base, w);
  // "selftest: start <id> " first, so --on-log "selftest: start 5101 " catches one move of a list.
  REXKRNL_INFO("selftest: start {} #{} move {} att {:#x} (act {}) def {:#x} (act {}), weapon {:#x} act {}", id,
               g_st.count, id, w, REX_LOAD_U32(w + 192), p2, REX_LOAD_U32(p2 + 192), held,
               held ? REX_LOAD_U32(held + 192) : 0);
  g_st.last_id = id;
  if (solo) {  // +244 = facing candidate (the game's strike path reads it before its sub_82251C00)
    REXKRNL_INFO("selftest: solo {} P1 ({:.1f} {:.1f}) f244 {:.3f}, P2 ({:.1f} {:.1f}) f244 {:.3f}, dist {:.1f}", id,
                 LoadF32(base, w + 256), LoadF32(base, w + 264), LoadF32(base, w + 244), LoadF32(base, p2 + 256),
                 LoadF32(base, p2 + 264), LoadF32(base, p2 + 244), dist);
    ctx.r3.u64 = w;
    ctx.r4.u64 = uint32_t(id);
    ctx.r7.u64 = 0;
    sub_82251C00(ctx, base);
    ctx = saved;
    g_st.check = now + std::chrono::seconds(1);
    g_st.pending_check = true;
    return;
  }
  ctx.r3.u64 = w;
  ctx.r4.u64 = p2;
  ctx.r5.u64 = uint32_t(id);
  if (weapon > 0 && held) {
    ctx.r6.u64 = REX_LOAD_U32(held + 512);
    sub_820CA0F0(ctx, base);
  } else {
    ctx.r6.u64 = 0;
    sub_820C9CF0(ctx, base);
  }
  ctx = saved;
  g_st.check = now + std::chrono::seconds(1);
  g_st.pending_check = true;
}

// Entrance skip (self-test speed): one entrance scene per wrestler, init sub_824BB848 (calls the 60 -> 30 switch)
// and exit sub_824BB910 (ids255-1: inits at ~164 s and ~241 s, tip box ~330 s). While one is open, pad 0 gets
// the game's own skip input: the button pressed for selftest_skip_hold_s every selftest_skip_every_s, added to
// what XInputGetState (sub_82572C78) returns. Nothing changes outside the scene (menus, tip box, match).
REXCVAR_DEFINE_BOOL(selftest_skip_entrance, false, "Game", "Self-test: press the skip button during entrances");
REXCVAR_DEFINE_INT32(selftest_skip_button, 0x1000, "Game", "Self-test: XInput button mask for the skip (0x1000 = A)");
REXCVAR_DEFINE_DOUBLE(selftest_skip_wait_s, 2.0, "Game", "Self-test: seconds from the scene start to the first press");
REXCVAR_DEFINE_DOUBLE(selftest_skip_every_s, 1.0, "Game", "Self-test: seconds between skip presses");
REXCVAR_DEFINE_DOUBLE(selftest_skip_hold_s, 0.15, "Game", "Self-test: seconds each skip press is held");

namespace {
struct EntranceSkip {
  std::mutex m;
  bool open = false;
  bool down = false;
  Clock::time_point start{};
  uint32_t packet = 0;
  int presses = 0;
} g_skip;

void StopEntranceSkip(const char* why) {
  std::lock_guard lock(g_skip.m);
  if (!g_skip.open) return;
  g_skip.open = false;
  REXKRNL_INFO("selftest: entrance skip off ({}) after {:.1f} s, {} skip presses", why,
               std::chrono::duration<double>(Clock::now() - g_skip.start).count(), g_skip.presses);
}
}  // namespace

REX_HOOK_RAW(sub_824BB848) {
  __imp__sub_824BB848(ctx, base);
  if (!REXCVAR_GET(selftest_skip_entrance)) return;
  // The post-match scenes use the same init: once this spec's match got its end pins, no presses until the tool
  // writes the next spec (they would pick Rematch on the results menu).
  if ((g_st.end_tries > 0 && g_st.gen == g_live_gen) || g_done_gen == g_live_gen) {
    REXKRNL_INFO("selftest: scene start after the match end (live gen {}): no skip presses", uint32_t(g_live_gen));
    return;
  }
  std::lock_guard lock(g_skip.m);
  g_skip.open = true;
  g_skip.down = false;
  g_skip.start = Clock::now();
  g_skip.presses = 0;
  REXKRNL_INFO("selftest: entrance scene start, skip button {:#x} from +{} s", REXCVAR_GET(selftest_skip_button),
               REXCVAR_GET(selftest_skip_wait_s));
}

REX_HOOK_RAW(sub_824BB910) {
  {
    std::lock_guard lock(g_skip.m);
    if (g_skip.open)
      REXKRNL_INFO("selftest: entrance scene end after {:.1f} s, {} skip presses",
                   std::chrono::duration<double>(Clock::now() - g_skip.start).count(), g_skip.presses);
    g_skip.open = false;
  }
  __imp__sub_824BB910(ctx, base);
}

REX_HOOK_RAW(sub_82572C78) {  // (user, XINPUT_STATE*) -> error
  const uint32_t user = ctx.r3.u32, state = ctx.r4.u32;
  __imp__sub_82572C78(ctx, base);
  if (user != 0 || ctx.r3.u32 != 0 || !state || !REXCVAR_GET(selftest_skip_entrance)) return;
  std::lock_guard lock(g_skip.m);
  if (!g_skip.open) return;
  const double t = std::chrono::duration<double>(Clock::now() - g_skip.start).count() - REXCVAR_GET(selftest_skip_wait_s);
  const double every = std::max(REXCVAR_GET(selftest_skip_every_s), 0.1);
  const bool down = t >= 0 && std::fmod(t, every) < REXCVAR_GET(selftest_skip_hold_s);
  if (down && !g_skip.down) ++g_skip.presses;
  if (down != g_skip.down) ++g_skip.packet;  // a new packet number on each edge, like a real pad
  g_skip.down = down;
  if (down) REX_STORE_U16(state + 4, uint16_t(REX_LOAD_U16(state + 4) | REXCVAR_GET(selftest_skip_button)));
  REX_STORE_U32(state, REX_LOAD_U32(state) + g_skip.packet);
}

// Entrance segment objects (FINDINGS-wk-arenas 6.2): sub_8234B3D0 = start of a placed object (vtable only):
// +32 position copied to +160 (sub_8234B7E0), +84 its record; on a raised stage, record +32 set -> y +164 -= the
// 18-unit lift. Probe: log each one; selftest_seg_dx / _dz move +160 / +168 of the ones with record +32 set.
REXCVAR_DEFINE_DOUBLE(selftest_seg_dx, 0.0, "Game", "Self-test: add to x of lifted entrance segments (0 off)");
REXCVAR_DEFINE_DOUBLE(selftest_seg_dz, 0.0, "Game", "Self-test: add to z of lifted entrance segments (0 off)");

REX_HOOK_RAW(sub_8234B3D0) {
  const uint32_t o = ctx.r3.u32;
  __imp__sub_8234B3D0(ctx, base);
  if (REXCVAR_GET(selftest_probe) <= 0) return;
  const uint32_t rec = REX_LOAD_U32(o + 84), hdr = REX_LOAD_U32(o + 80);
  const int32_t flag = rec ? int32_t(LoadF32(base, rec + 32)) : 0;
  const double dx = REXCVAR_GET(selftest_seg_dx), dz = REXCVAR_GET(selftest_seg_dz);
  if (flag && (dx != 0 || dz != 0))
    for (const auto& [at, d] : {std::pair{o + 160, dx}, std::pair{o + 168, dz}}) {
      const float v = LoadF32(base, at) + float(d);
      uint32_t u;
      std::memcpy(&u, &v, 4);
      REX_STORE_U32(at, u);
    }
  float r[12] = {};
  if (rec)
    for (int k = 0; k < 12; ++k) r[k] = LoadF32(base, rec + 4 * k);
  REXKRNL_INFO("selftest: seg {:#x} vt {:#x} hdr {:#x} ver {} pos ({:.1f} {:.1f} {:.1f}) rec {:#x} [{:.2f} {:.2f} {:.2f} "
               "{:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f}]",
               o, REX_LOAD_U32(o), hdr, hdr ? int16_t(REX_LOAD_U16(hdr + 4)) : 0, LoadF32(base, o + 160),
               LoadF32(base, o + 164), LoadF32(base, o + 168), rec, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8],
               r[9], r[10], r[11]);
}

REXCVAR_DEFINE_BOOL(svr07_no_blur, true, "Game",
                    "Turn off the game's depth-of-field blur (crowd, near ropes, halo around wrestlers)");
REXCVAR_DEFINE_BOOL(svr07_no_post_aa, false, "Game",
                    "Also skip the game's post-process antialiasing pass (T_Antialiasing)");

// Post-effect settings: [0x82964A68] -> struct, +4 depth of field on, +8..+32
// its focus/range floats, +60 HDR on (bloom/star/tone map, final pass
// sub_8244AB90 picks T_FinalScenePass*UseDOF when +4 is set), +64 blue shift,
// +68 tone map, +188 monotone, +204 after-image, +292 antialiasing. The scene
// code sets +4 per camera (sub_821BCD78 and others). The post effects that
// read +4: HDR render sub_8244B6E8 (-> final pass), Poisson DOF filter
// sub_8244C308 (only when HDR is off), 2x2 down-scale + 5x5 blur
// sub_8244CDD0, monotone sub_8244BA78 and antialiasing sub_8244CFD0 (the last
// two skip themselves when the combined DOF final pass did their work).
// With svr07_no_blur each sees +4 = 0 for its call; the scene code's value is
// put back afterwards, so only the blur goes, not bloom/tone map/monotone/AA.
namespace {
constexpr uint32_t kPostFxSettings = 0x82964A68;

struct NoDofScope {
  uint8_t* base;
  uint32_t s = 0, saved = 0;
  explicit NoDofScope(uint8_t* b) : base(b) {
    if (!REXCVAR_GET(svr07_no_blur)) return;
    s = REX_LOAD_U32(kPostFxSettings);
    if (!s) return;
    saved = REX_LOAD_U32(s + 4);
    REX_STORE_U32(s + 4, 0);
  }
  ~NoDofScope() {
    if (s) REX_STORE_U32(s + 4, saved);
  }
};
}  // namespace

REX_HOOK_RAW(sub_8244B6E8) {
  NoDofScope scope(base);
  __imp__sub_8244B6E8(ctx, base);
}

REX_HOOK_RAW(sub_8244AB90) {
  NoDofScope scope(base);
  __imp__sub_8244AB90(ctx, base);
}

REX_HOOK_RAW(sub_8244C308) {
  NoDofScope scope(base);
  __imp__sub_8244C308(ctx, base);
}

REX_HOOK_RAW(sub_8244CDD0) {
  NoDofScope scope(base);
  __imp__sub_8244CDD0(ctx, base);
}

REX_HOOK_RAW(sub_8244BA78) {
  NoDofScope scope(base);
  __imp__sub_8244BA78(ctx, base);
}

REX_HOOK_RAW(sub_8244CFD0) {
  if (REXCVAR_GET(svr07_no_post_aa)) return;
  NoDofScope scope(base);
  __imp__sub_8244CFD0(ctx, base);
}
