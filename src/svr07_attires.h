// svr07 - attire picks: which look fills a superstar's attire 1-4.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "svr07_modelpac.h"

namespace svr07::attires {

// Reads the attire_picks cvar and the looks it names (mods\attire\<name>\ch.pac). Once per
// run; later calls return at once.
void LoadPicks(const std::filesystem::path& mods);

// The picks of the ids `want` accepts and their inputs, for a cache stamp ("" = none).
std::string Stamp(const std::function<bool(uint32_t)>& want);

// Puts the picked looks of `id` into a model pac's entries: ring model (kind 2) and
// entrance model (kind 4; removed when the look has none, so the ring model is used).
// name(n) is the entry name of model number n. Returns how many picks were applied.
int Apply(std::vector<modelpac::PacEntry>& entries, uint32_t id,
          const std::function<std::string(uint32_t)>& name);

// Picks for roster ids 0-92: writes out_root\pac\ch\chNN.pac (the mods' or the game's
// with the picks in) and removes the ones it wrote earlier that no pick needs now.
// ch92.pac is left to the roster merge when that wrote it (it applies the picks of 92
// and the extras itself, BuildRosterFiles).
void BuildAttirePacs(const std::filesystem::path& game_root, const std::filesystem::path& mods,
                     const std::filesystem::path& out_root);

}  // namespace svr07::attires
