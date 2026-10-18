// svr07 - roster expansion: superstars beyond the game's 93 roster slots.
#pragma once

#include <cstdint>
#include <filesystem>

#include <rex/ppc/context.h>

namespace svr07::roster {

// The game's ids: 0-92 roster, 93-122 CAWs, 123-127 CAW templates, 128-152 CAW move
// sets, 153-166 blank. Extra superstars get ids from kFirstExtra up.
constexpr uint32_t kRosterCount = 93;
constexpr uint32_t kFirstExtra = 167;
constexpr uint32_t kMaxExtras = 32;

bool IsExtra(uint32_t id);

// Called after a `cmpwi/cmplwi crN,rM,93` (tools/roster_sites.py generates the call
// sites). kRoster: an extra id compares like a roster id (less than 93), so the game
// takes its roster branch. kObserve: only logged.
enum class Site { kRoster, kObserve };
void OnIdCompare(uint32_t addr, Site policy, PPCCRRegister& cr, PPCRegister& id);

// Model numbers: (model*10 + attire)*10 + kind, 10000 and up = a CAW's (built from
// edit parts). An extra's model number is its id (16700 and up), so the CAW tests
// get hooks too (tools/roster_sites.py):
// - after `addi rT,rS,-10000` (followed by a sign test): an extra's number gives -1,
//   so the game takes its below-10000 branch;
// - after `cmp n,10000` in the model loaders: an extra's number compares as less.
bool IsExtraModel(uint32_t n);
void OnModelSub(uint32_t addr, PPCRegister& diff);
void OnModelCompare(uint32_t addr, PPCCRRegister& cr, PPCRegister& n);

// Startup, before the game files are mounted: reads mods\roster, assigns ids
// (mods\roster\slots.txt), writes the extras' model pac and the plist360.h files
// that list it to out_root (served over the game's files).
void BuildRosterFiles(const std::filesystem::path& game_root, const std::filesystem::path& mods,
                      const std::filesystem::path& out_root);

}  // namespace svr07::roster
