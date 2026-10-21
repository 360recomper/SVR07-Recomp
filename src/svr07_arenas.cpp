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
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

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

REX_HOOK_RAW(sub_82309AD0) {
  const uint32_t self = ctx.r3.u32;
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
