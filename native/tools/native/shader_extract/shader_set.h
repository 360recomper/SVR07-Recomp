// SvR07's shader containers, pulled from the player's own disc files. Same set as NR-3's
// NR-3 scan_pacs.py (internal research script) (335 containers: 332 in _startup.pac child 0, BPE-packed,
// plus 3 in default.xex); file names are the same (sha1[:16].bin of the container bytes).
//
// SvR07 keeps its shaders as old XDK containers (0x102A0E0x). They are written out unchanged; the
// patched XenosRecomp (native/patches/xenosrecomp-0e.patch) upgrades them in memory and keys the
// cache on these original bytes (what the game passes to CreateVertexShader / CreatePixelShader).

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace svr07_shader_cache {

struct ShaderFile {
  std::string name;    // <sha1[:16]>.bin
  std::string data;    // the XDK shader container, as on disc
  std::string source;  // where it was first seen, e.g. _startup.pac/0@1010a8 or xex@6fd40
};

struct ShaderSet {
  std::vector<ShaderFile> files;  // sorted by name
  size_t containers = 0;
  size_t from_pac = 0;
  size_t from_xex = 0;
  size_t old_format = 0;  // 0x102A0E containers (all of SvR07's)
  size_t bpe_blobs = 0;   // BPE blobs unpacked on the way
  uint64_t bpe_bytes = 0;
  std::string set_hash;   // sha1 over name + data of every file, first 16 hex digits
};

// Finds default.xex and pac\_startup.pac in game_root, game_root\assets or game_root\game\assets.
// Throws std::runtime_error.
ShaderSet ExtractShaderSet(const std::filesystem::path& game_root);

// Writes every file plus _sources.txt into dir (created if missing).
void WriteShaderSet(const ShaderSet& set, const std::filesystem::path& dir);

std::string ReadFileBytes(const std::filesystem::path& path);
void WriteFileBytes(const std::filesystem::path& path, std::string_view data);
std::string PathText(const std::filesystem::path& path);  // UTF-8, for messages

}  // namespace svr07_shader_cache
