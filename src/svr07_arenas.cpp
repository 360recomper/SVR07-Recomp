// svr07 - extra arenas (see RESEARCH-arenas.md and model-port/tools/arena_port.py).
//
// The arena list is data: misc.pac ARE/0000 (68-byte records: name string id, stage id, base arena,
// brand, list order, preview clip path, mode flags), loaded by sub_821CA3B0 into 80-byte records
// at [0x829414E8] (count [0x829414E4]): +0 stage id, +44 selectable, +76 list order.
// A stage id N loads "/STG%d/%04d" (N/10, N); the path parser keeps 4 characters per part, so
// ids 100-127 resolve to STG1/0100-0127. The stage id is a signed byte (match setup +1148), so
// 127 is the top.
//
// Arena Select (sub_82309AD0) walks stage ids 0-78 by list order and keeps the selectable ones
// (id 25 excluded), so arenas with ids 79+ never show. This hook appends them, by list order,
// up to the screen's 79 rows. Ids 4-19 are shop unlocks (sub_8253C2A0); every other id is open.

#include "generated/default/svr07_init.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/rex_app.h>

REXCVAR_DEFINE_BOOL(extra_arenas, true, "Game",
                    "Show arenas with stage ids 79+ (mods: misc.pac ARE/0000) on Arena Select");

namespace {
constexpr uint32_t kAreCount = 0x829414E4;
constexpr uint32_t kAreTable = 0x829414E8;
constexpr uint32_t kAreSize = 80;
constexpr uint32_t kRows = 79;        // stage ids per row at +220, locked flags at +536
constexpr uint32_t kRowStage = 220;
constexpr uint32_t kRowLocked = 536;
constexpr uint32_t kLastRow = 852;    // row count - 1
constexpr uint32_t kCursor = 856;     // row of the current stage
constexpr uint32_t kSetup = 128;      // -> +300 current stage id
}  // namespace

// Current stage id for game logic: sub_821B8EC0 returns the signed byte at 0x82933AA8+1148, and
// its 76 callers index per-arena tables sized for the retail ids (lighting: sub_82147AD8 read
// garbage for stage 91 and crashed while the match loaded). For an extra arena (79+) they get its
// base arena instead (ARE record +3, the donor the arena was built from), so lighting, crowd and
// sound setups are that arena's. The stage group itself is loaded through the byte directly
// (sub_8212F670 -> "/STG%d/%04d"), so the extra arena's own scenery still loads.
// Stage ids below 79 that retail leaves unused (no ARE record, no group) and the mods use for extra arenas
// (model-port/tools/arena_port.py LOW_IDS): the retail list already shows them (list orders 68-78), but their
// per-arena table entries are empty, so they get the same base-arena mapping as 79+.
constexpr int32_t kLowIds[] = {27, 28, 29, 47, 52, 53, 54, 58, 59, 69, 75};

REX_HOOK_RAW(sub_821B8EC0) {
  __imp__sub_821B8EC0(ctx, base);
  const int32_t stage = ctx.r3.s32;
  if (!REXCVAR_GET(extra_arenas)) return;
  if (stage < int32_t(kRows) && std::find(std::begin(kLowIds), std::end(kLowIds), stage) == std::end(kLowIds))
    return;
  const uint32_t n = REX_LOAD_U32(kAreCount);
  const uint32_t table = REX_LOAD_U32(kAreTable);
  for (uint32_t i = 0; table && i < n && i < 1000; ++i) {
    const uint32_t rec = table + i * kAreSize;
    if (int32_t(REX_LOAD_U32(rec)) != stage) continue;
    const uint32_t base_arena = REX_LOAD_U32(rec + 68);
    if (base_arena < kRows) ctx.r3.u64 = base_arena;
    return;
  }
}

// Entrance backdrop clip: sub_82464A20(stage) returns "/ARMV/<name>" (menu.pac ARMV, picked through the stage's base
// arena), which the entrance plays on every titantron screen. Extra arenas have their donor as base, so they showed the
// donor's clip (WM22 graphics in WrestleMania XXIV, user test 2026-10-20). mods/arenas/armv.txt ("<stage> /ARMV/<name>"
// per line, written by arena_port.py) names each extra arena's own clip; the string lives in guest memory.
namespace {
std::map<int32_t, uint32_t> g_armv;  // stage -> guest string
std::once_flag g_armv_once;

void LoadArmv(uint8_t* base) {
  auto mods = rex::Runtime::ResolveModsRoot();
  if (mods.empty()) return;
  std::ifstream f(mods / "arenas" / "armv.txt");
  int32_t stage = 0;
  std::string path;
  while (f >> stage >> path) {
    if (path.size() > 30 || path.rfind("/ARMV/", 0) != 0) continue;
    uint32_t g = REX_KERNEL_MEMORY()->SystemHeapAlloc(32, 16);
    if (!g) break;
    auto* p = reinterpret_cast<char*>(base + g);
    std::memset(p, 0, 32);
    std::memcpy(p, path.data(), path.size());
    g_armv[stage] = g;
  }
  if (!g_armv.empty()) REXLOG_INFO("Arenas: {} entrance backdrop entries (mods/arenas/armv.txt)", g_armv.size());
}

const uint32_t* FindArmv(uint8_t* base, int32_t stage) {
  if (!REXCVAR_GET(extra_arenas)) return nullptr;
  std::call_once(g_armv_once, LoadArmv, base);
  auto it = g_armv.find(stage);
  return it == g_armv.end() ? nullptr : &it->second;
}
}  // namespace

REX_HOOK_RAW(sub_82464A20) {
  const uint32_t* g = FindArmv(base, ctx.r3.s32);
  if (g && *g) {
    ctx.r3.u64 = *g;
    return;
  }
  __imp__sub_82464A20(ctx, base);
}

// Match-up screen: sub_8232EC78(kind, stage) returns "/MACH/<name>" (kind 0 logo, 1 background, 2 font, 3 layout table)
// from a 25-row table in the exe keyed by stage id; an id it doesn't list gets SmackDown's row (so every extra arena
// showed SmackDown's match-up screen). mods/arenas/mach.txt ("<stage> <logo> <bg> <font> <table>", written by
// arena_port.py / matchup07.py, entries in menuHD.pac + menu.pac MACH) names each extra arena's own.
namespace {
std::map<int32_t, std::array<uint32_t, 4>> g_mach;  // stage -> guest strings
std::once_flag g_mach_once;

uint32_t GuestString(uint8_t* base, const std::string& s) {
  if (s.size() > 30) return 0;
  uint32_t g = REX_KERNEL_MEMORY()->SystemHeapAlloc(32, 16);
  if (!g) return 0;
  auto* p = reinterpret_cast<char*>(base + g);
  std::memset(p, 0, 32);
  std::memcpy(p, s.data(), s.size());
  return g;
}

void LoadMach(uint8_t* base) {
  auto mods = rex::Runtime::ResolveModsRoot();
  if (mods.empty()) return;
  std::ifstream f(mods / "arenas" / "mach.txt");
  int32_t stage = 0;
  std::string parts[4];
  while (f >> stage >> parts[0] >> parts[1] >> parts[2] >> parts[3]) {
    std::array<uint32_t, 4> g{};
    bool ok = true;
    for (int k = 0; k < 4; ++k) {
      ok = ok && parts[k].rfind("/MACH/", 0) == 0 && (g[k] = GuestString(base, parts[k])) != 0;
    }
    if (ok) g_mach[stage] = g;
  }
  if (!g_mach.empty()) REXLOG_INFO("Arenas: {} match-up screens (mods/arenas/mach.txt)", g_mach.size());
}
}  // namespace

REX_HOOK_RAW(sub_8232EC78) {
  const uint32_t kind = ctx.r3.u32;
  const int32_t stage = ctx.r4.s32;
  if (REXCVAR_GET(extra_arenas) && kind < 4) {
    std::call_once(g_mach_once, LoadMach, base);
    auto it = g_mach.find(stage);
    if (it != g_mach.end()) {
      ctx.r3.u64 = it->second[kind];
      return;
    }
  }
  __imp__sub_8232EC78(ctx, base);
}

// Arena Select preview picture (found 2026-10-21): the box on the left is a still, not a clip. sub_8230AA20 (the screen's
// update, this = widget) sets the "moviedummy" texture to the 64-bit handle at [bank + 76][stage id * 8], bank =
// sub_822F4CA8() = menuHD.pac MENU/STTX (one 512x512 picture per retail stage id; children 0-25), and sub_820D21A0
// rejects the value when it isn't a valid handle. A stage id past the bank (every extra arena) reads garbage, so the
// last picture stayed up. arena_port.py appends the extras' pictures as children 26+ and writes mods/arenas/preview.txt
// "<stage> <child>"; while the update runs for such a stage, the value that sub_820D21A0 checks (a stack copy) is
// replaced by the bank's handle for that child. The bank itself is left alone (replacing its array froze the match load).
namespace {
std::map<int32_t, uint32_t> g_preview;  // stage -> bank child
std::once_flag g_preview_once;
thread_local uint32_t g_preview_handle[2] = {0, 0};  // handle words (big-endian order) for the stage being drawn
thread_local bool g_preview_active = false;

void LoadPreview(uint8_t*) {
  auto mods = rex::Runtime::ResolveModsRoot();
  if (mods.empty()) return;
  std::ifstream f(mods / "arenas" / "preview.txt");
  int32_t stage = 0;
  uint32_t child = 0;
  while (f >> stage >> child) g_preview[stage] = child;
  if (!g_preview.empty()) REXLOG_INFO("Arenas: {} Arena Select pictures (mods/arenas/preview.txt)", g_preview.size());
}
}  // namespace

REX_HOOK_RAW(sub_8230AA20) {
  g_preview_active = false;
  if (REXCVAR_GET(extra_arenas)) {
    std::call_once(g_preview_once, LoadPreview, base);
    const uint32_t widget = ctx.r3.u32;
    const uint32_t cursor = REX_LOAD_U32(widget + 796);
    const int32_t stage = int32_t(REX_LOAD_U32(widget + 4 * (cursor + 40)));
    auto it = g_preview.find(stage);
    if (widget && cursor < 128 && it != g_preview.end()) {
      const uint64_t saved_r3 = ctx.r3.u64, saved_lr = ctx.lr;
      __imp__sub_822F4CA8(ctx, base);
      const uint32_t bank = ctx.r3.u32;
      ctx.r3.u64 = saved_r3;
      ctx.lr = saved_lr;
      const uint32_t arr = bank ? REX_LOAD_U32(bank + 76) : 0;
      if (arr) {
        g_preview_handle[0] = REX_LOAD_U32(arr + 8 * it->second);
        g_preview_handle[1] = REX_LOAD_U32(arr + 8 * it->second + 4);
        g_preview_active = g_preview_handle[0] || g_preview_handle[1];
        static std::map<int32_t, bool> logged;
        if (!logged[stage]) {
          logged[stage] = true;
          REXLOG_INFO("Arenas: preview for stage {} = bank child {} handle {:08X}{:08X}", stage, it->second,
                      g_preview_handle[0], g_preview_handle[1]);
        }
      }
    }
  }
  __imp__sub_8230AA20(ctx, base);
  g_preview_active = false;
}

REX_HOOK_RAW(sub_820D21A0) {
  if (g_preview_active && ctx.r3.u32) {
    REX_STORE_U32(ctx.r3.u32, g_preview_handle[0]);
    REX_STORE_U32(ctx.r3.u32 + 4, g_preview_handle[1]);
  }
  __imp__sub_820D21A0(ctx, base);
}

// Finding the Arena Select preview player (2026-10-21; the select screen shows the arena's flythrough clip, and
// SmackDown's on extra arenas): with arena_trace on, log each distinct pac path once with its caller. The resolver
// sub_8241C370 is hooked in svr07_roster.cpp, so its wrappers are logged here: sub_8241C660 (exists) and sub_8241C690
// (size) take the path in r3, sub_8241C7E8 / sub_8241C830 (read) in r4; sub_821BAD38 opens a Bink clip (path in r4).
REXCVAR_DEFINE_BOOL(arena_trace, false, "Game", "Log every distinct pac path looked up (Arena Select preview hunt)");

namespace {
void TracePath(uint8_t* base, uint32_t path, const char* what, uint32_t lr) {
  if (!REXCVAR_GET(arena_trace) || !path) return;
  static std::mutex m;
  static std::map<std::string, int> seen;
  const char* p = reinterpret_cast<const char*>(base + path);
  if (p[0] != '/') return;
  std::string s(p, strnlen(p, 64));
  std::lock_guard<std::mutex> lock(m);
  if (seen.size() < 2000 && seen.emplace(s, 1).second) {
    REXLOG_INFO("Arenas trace: {} {} (caller {:08X})", what, s, lr);
  }
}
}  // namespace

REX_HOOK_RAW(sub_821BAD38) {
  TracePath(base, ctx.r4.u32, "clip", uint32_t(ctx.lr));
  __imp__sub_821BAD38(ctx, base);
}
REX_HOOK_RAW(sub_8241C660) {
  TracePath(base, ctx.r3.u32, "exists", uint32_t(ctx.lr));
  __imp__sub_8241C660(ctx, base);
}
REX_HOOK_RAW(sub_8241C690) {
  TracePath(base, ctx.r3.u32, "size", uint32_t(ctx.lr));
  __imp__sub_8241C690(ctx, base);
}
REX_HOOK_RAW(sub_8241C7E8) {
  TracePath(base, ctx.r4.u32, "read", uint32_t(ctx.lr));
  __imp__sub_8241C7E8(ctx, base);
}
REX_HOOK_RAW(sub_8241C830) {
  TracePath(base, ctx.r4.u32, "read2", uint32_t(ctx.lr));
  __imp__sub_8241C830(ctx, base);
}

// Arena Select preview hunt: list every string in the exe image that mentions MOV/movie/Bink so the code that builds
// the preview clip name can be found by address (lis/addi pair) in the generated sources.
namespace {
void DumpMovieStrings(uint8_t* base) {
  if (!REXCVAR_GET(arena_trace)) return;
  static bool done = false;
  if (done) return;
  done = true;
  int n = 0;
  for (uint32_t a = 0x82000000; a < 0x829A0000 && n < 400; ++a) {
    const char* p = reinterpret_cast<const char*>(base + a);
    if (p[0] != '/' && p[0] != 'm' && p[0] != 'M') continue;
    if (a > 0x82000000 && base[a - 1] >= 0x20) continue;  // start of a string only
    std::string s(p, strnlen(p, 48));
    if (s.size() < 4) continue;
    bool ok = s.rfind("/MOV", 0) == 0 || s.rfind("MOV/", 0) == 0 || s.rfind("movie", 0) == 0 ||
              s.find("/MOV/") != std::string::npos || s.find("MOV/M") != std::string::npos ||
              s.find(".bik") != std::string::npos || s.find("/BNK") != std::string::npos;
    if (!ok) continue;
    for (char c : s) ok = ok && c >= 0x20 && c < 0x7F;
    if (!ok) continue;
    REXLOG_INFO("Arenas strings: {:08X} \"{}\"", a, s);
    ++n;
  }
}
}  // namespace

REX_HOOK_RAW(sub_82309AD0) {
  const uint32_t self = ctx.r3.u32;
  DumpMovieStrings(base);
  __imp__sub_82309AD0(ctx, base);
  if (!REXCVAR_GET(extra_arenas)) return;
  const uint32_t n = REX_LOAD_U32(kAreCount);
  const uint32_t table = REX_LOAD_U32(kAreTable);
  if (!table || !n || n > 1000) return;
  struct Row {
    int32_t order, stage;
  };
  std::vector<Row> extra;
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t rec = table + i * kAreSize;
    const int32_t stage = int32_t(REX_LOAD_U32(rec));
    if (stage < int32_t(kRows) || stage > 127 || REX_LOAD_U32(rec + 44) == 0) continue;
    extra.push_back({int32_t(REX_LOAD_U32(rec + 76)), stage});
  }
  if (extra.empty()) return;
  std::stable_sort(extra.begin(), extra.end(),
                   [](const Row& a, const Row& b) { return a.order < b.order; });
  uint32_t rows = REX_LOAD_U32(self + kLastRow) + 1;
  const uint32_t setup = REX_LOAD_U32(self + kSetup);
  const int32_t current = setup ? int32_t(REX_LOAD_U32(setup + 300)) : -1;
  uint32_t added = 0;
  for (const Row& r : extra) {
    if (rows >= kRows) {
      REXLOG_WARN("Arenas: Arena Select full ({} rows), stage {} and later left out", kRows,
                  r.stage);
      break;
    }
    REX_STORE_U32(self + kRowStage + 4 * rows, uint32_t(r.stage));
    REX_STORE_U32(self + kRowLocked + 4 * rows, 0);
    if (r.stage == current) REX_STORE_U32(self + kCursor, rows);
    ++rows;
    ++added;
  }
  REX_STORE_U32(self + kLastRow, rows - 1);
  static bool logged = false;
  if (!logged) {
    REXLOG_INFO("Arenas: {} extra arenas on Arena Select ({} rows)", added, rows);
    logged = true;
  }
}
