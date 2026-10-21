// svr07 - select-screen portraits from the mods folder.
#pragma once

#include <cstdint>
#include <filesystem>
#include <utility>
#include <vector>

namespace svr07 {

// Builds menuHD.pac / menuSD.pac with the pictures in <mods>\portraits merged into
// the select-screen banks (MENU/SF2A face, SF2B body, SF2D bust) and lays the
// result over the game files. The merged files are cached under cache_root and
// only rebuilt when an input changes.
// extra_folders: roster extras' (id, mods\roster\<folder>) that have pictures there.
using PictureFolders = std::vector<std::pair<uint32_t, std::filesystem::path>>;
void PreparePortraits(const std::filesystem::path& game_root,
                      const std::filesystem::path& cache_root,
                      const PictureFolders& extra_folders = {});

// Part of PreparePortraits, without the runtime: merges <mods>\portraits into
// out_root\pac\menu\menu{HD,SD}.pac (removes them when there are no portraits).
void BuildPortraitPacs(const std::filesystem::path& game_root, const std::filesystem::path& mods,
                       const std::filesystem::path& out_root,
                       const PictureFolders& extra_folders = {});

// Rebuilds plist360.arc / plist360_4x3.arc (the game's index of every pac's table) from
// the pacs the game will read (out_root, then mods, then the game folder) and writes them
// to out_root when the game's index is stale. Removes them when it isn't.
void FixPacIndexes(const std::filesystem::path& game_root, const std::filesystem::path& mods,
                   const std::filesystem::path& out_root);

}  // namespace svr07
