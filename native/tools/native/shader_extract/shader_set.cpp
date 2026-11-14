#include "shader_set.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "win_crypto.h"
#include "xex_image.h"

namespace svr07_shader_cache {

namespace {

constexpr size_t kContainerHeader = 36;  // the scan needs this much past a magic (as the Python tool)
constexpr uint32_t kMaxSection = 0x100000;
constexpr int kMaxPachDepth = 4;
constexpr size_t kMaxBpeOut = size_t{256} << 20;

uint32_t Be32(const char* p) {
  const auto* u = reinterpret_cast<const uint8_t*>(p);
  return uint32_t{u[0]} << 24 | uint32_t{u[1]} << 16 | uint32_t{u[2]} << 8 | u[3];
}

uint32_t Le32(std::string_view d, size_t o) {
  if (o + 4 > d.size()) throw std::out_of_range("le32");
  const auto* u = reinterpret_cast<const uint8_t*>(d.data()) + o;
  return uint32_t{u[0]} | uint32_t{u[1]} << 8 | uint32_t{u[2]} << 16 | uint32_t{u[3]} << 24;
}

std::string Hex(size_t value) {
  char text[24];
  std::snprintf(text, sizeof(text), "%zx", value);
  return text;
}

// Every shader container in d, old (10 2A 0E) and new (10 2A 11) magic, in that order:
// 4-aligned, 4th byte 0 (PS) or 1 (VS), virtual + physical size over 36 and inside d, each section
// under 1 MiB; a 0E header's three offsets (defTab, ctab, shader) must also lie inside the virtual
// section. Same rule as NR-3's scan_pacs.py containers().
template <typename Fn>
void ForEachContainer(std::string_view d, Fn&& fn) {
  for (const std::string_view magic : {std::string_view("\x10\x2a\x0e", 3), std::string_view("\x10\x2a\x11", 3)}) {
    const bool old_format = magic[2] == '\x0e';
    size_t i = 0;
    for (;;) {
      i = d.find(magic, i);
      if (i == std::string_view::npos || i + kContainerHeader > d.size()) break;
      if (i % 4 == 0 && static_cast<uint8_t>(d[i + 3]) <= 1) {
        const char* p = d.data() + i;
        const uint32_t vsize = Be32(p + 4), psize = Be32(p + 8);
        const uint64_t total = uint64_t{vsize} + psize;
        bool ok = total > kContainerHeader && total <= d.size() - i && vsize < kMaxSection && psize < kMaxSection;
        if (ok && old_format) {
          const uint32_t def_tab = Be32(p + 12), ctab = Be32(p + 16), shader = Be32(p + 20);
          ok = def_tab < vsize && ctab > 0 && ctab < vsize && shader > 0 && shader < vsize;
        }
        if (ok) {
          fn(i, d.substr(i, static_cast<size_t>(total)));
          i += static_cast<size_t>(total);
          continue;
        }
      }
      ++i;
    }
  }
}

// Yuke's "BPE " byte-pair packer (format from dfxyz/rr-mod-tool; same as the textures.bpe_decompress
// NR-3's scan_pacs.py uses). Throws std::out_of_range on a damaged blob.
std::string BpeDecompress(std::string_view d) {
  auto at = [&](size_t p) -> uint8_t {
    if (p >= d.size()) throw std::out_of_range("bpe");
    return static_cast<uint8_t>(d[p]);
  };
  const uint32_t clen = Le32(d, 8), dlen = Le32(d, 12);
  if (dlen > kMaxBpeOut) throw std::out_of_range("bpe size");
  size_t p = 16;
  const uint64_t end = uint64_t{16} + clen;
  std::string out;
  out.reserve(dlen);
  uint8_t left[256], right[256];
  std::vector<uint8_t> stack;
  while (p < end) {
    for (int c = 0; c < 256; ++c) {
      left[c] = static_cast<uint8_t>(c);
      right[c] = 0;
    }
    int c = 0;
    while (c < 256) {
      int cnt = at(p++);
      if (cnt > 127) {
        c += cnt - 127;
        cnt = 0;
      }
      if (c == 256) break;
      for (int k = 0; k < cnt + 1; ++k) {
        if (c >= 256) throw std::out_of_range("bpe table");
        left[c] = at(p++);
        if (c != left[c]) right[c] = at(p++);
        ++c;
      }
    }
    const size_t size = at(p) | size_t{at(p + 1)} << 8;
    p += 2;
    const size_t blk = p;
    const size_t blk_end = std::min(p + size, d.size());  // Python slices short at the end
    p += size;
    size_t i = blk;
    stack.clear();
    for (;;) {
      uint8_t ch;
      if (!stack.empty()) {
        ch = stack.back();
        stack.pop_back();
      } else if (i < blk_end) {
        ch = static_cast<uint8_t>(d[i++]);
      } else {
        break;
      }
      if (ch == left[ch]) {
        out.push_back(static_cast<char>(ch));
        if (out.size() > kMaxBpeOut) throw std::out_of_range("bpe output");
      } else {
        stack.push_back(right[ch]);
        stack.push_back(left[ch]);
      }
    }
  }
  if (out.size() < dlen) out.append(dlen - out.size(), '\0');
  return out;
}

class Finder {
 public:
  struct Entry {
    const std::string* data;  // key of index_, stable
    std::string source;
  };

  void Scan(std::string_view d, const std::string& source) {
    ForEachContainer(d, [&](size_t offset, std::string_view container) {
      auto [it, added] = index_.try_emplace(std::string(container), entries_.size());
      if (added) entries_.push_back({&it->first, source + "@" + Hex(offset)});
    });
  }

  // Unpack BPE, scan every node that is not a PACH, recurse into PACH children (scan_pacs.py walk()).
  void Walk(std::string_view d, const std::string& path, int depth = 0) {
    std::string unpacked;
    if (d.substr(0, 4) == "BPE ") {
      try {
        unpacked = BpeDecompress(d);
        ++bpe_blobs;
        bpe_bytes += unpacked.size();
        d = unpacked;
      } catch (const std::out_of_range&) {
        Scan(d, path);  // "BPE-ERR": scanned as is
        return;
      }
    }
    if (d.substr(0, 4) != "PACH") {
      Scan(d, path);
      return;
    }
    if (depth >= kMaxPachDepth) return;
    uint32_t n;
    try {
      n = Le32(d, 4);
    } catch (const std::out_of_range&) {
      return;
    }
    const uint64_t base = 8 + uint64_t{n} * 12;
    if (base > d.size()) return;
    for (uint32_t i = 0; i < n; ++i) {
      const uint32_t no = Le32(d, 8 + size_t{i} * 12);
      const uint64_t off = base + Le32(d, 12 + size_t{i} * 12);
      const uint32_t len = Le32(d, 16 + size_t{i} * 12);
      if (off + len <= d.size())
        Walk(d.substr(static_cast<size_t>(off), len), path + "/" + std::to_string(no), depth + 1);
    }
  }

  const std::vector<Entry>& entries() const { return entries_; }
  size_t bpe_blobs = 0;
  uint64_t bpe_bytes = 0;

 private:
  std::unordered_map<std::string, size_t> index_;
  std::vector<Entry> entries_;  // first-seen order
};

std::filesystem::path FindAssets(const std::filesystem::path& root) {
  std::error_code ec;
  for (const auto& dir : {root, root / "assets", root / "game" / "assets"}) {
    if (std::filesystem::is_regular_file(dir / "default.xex", ec) &&
        std::filesystem::is_regular_file(dir / "pac" / "_startup.pac", ec))
      return dir;
  }
  throw std::runtime_error(PathText(root) + " does not hold default.xex and pac\\_startup.pac " +
                           "(also looked in assets\\ and game\\assets\\)");
}

}  // namespace

std::string PathText(const std::filesystem::path& path) {
  const auto u8 = path.u8string();
  return {reinterpret_cast<const char*>(u8.data()), u8.size()};
}

std::string ReadFileBytes(const std::filesystem::path& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) throw std::runtime_error("cannot open " + PathText(path));
  const std::streamoff size = f.tellg();
  if (size < 0) throw std::runtime_error("cannot read " + PathText(path));
  std::string data(static_cast<size_t>(size), '\0');
  f.seekg(0);
  f.read(data.data(), size);
  if (f.gcount() != size) throw std::runtime_error("cannot read " + PathText(path));
  return data;
}

void WriteFileBytes(const std::filesystem::path& path, std::string_view data) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(data.data(), static_cast<std::streamsize>(data.size()));
  f.close();
  if (!f) throw std::runtime_error("cannot write " + PathText(path));
}

ShaderSet ExtractShaderSet(const std::filesystem::path& game_root) {
  const auto assets = FindAssets(game_root);
  const auto pac_path = assets / "pac" / "_startup.pac";
  const auto xex_path = assets / "default.xex";

  ShaderSet set;
  Finder finder;
  const std::string pac = ReadFileBytes(pac_path);
  // SvR07's _startup.pac is a PACH at the top (5 children; the shaders are in child 0, BPE-packed).
  const std::string_view top = std::string_view(pac).substr(0, 4);
  if (top != "PACH" && top != "BPE ")
    throw std::runtime_error(PathText(pac_path) + " is not a PACH archive (is it SvR07's?)");
  finder.Walk(pac, "_startup.pac");
  set.from_pac = finder.entries().size();
  finder.Scan(LoadXexImage(ReadFileBytes(xex_path)), "xex");
  set.containers = finder.entries().size();
  set.from_xex = set.containers - set.from_pac;
  set.bpe_blobs = finder.bpe_blobs;
  set.bpe_bytes = finder.bpe_bytes;
  if (set.from_pac == 0) throw std::runtime_error("no shaders found in " + PathText(pac_path));

  for (const auto& entry : finder.entries()) {
    if (entry.data->compare(0, 3, "\x10\x2a\x0e", 3) == 0) ++set.old_format;
    set.files.push_back({Sha1Hex(entry.data->data(), entry.data->size(), 16) + ".bin", *entry.data, entry.source});
  }
  std::sort(set.files.begin(), set.files.end(),
            [](const ShaderFile& a, const ShaderFile& b) { return a.name < b.name; });

  Sha1 total;
  for (const auto& f : set.files) {
    total.Update(f.name.data(), f.name.size());
    total.Update(f.data.data(), f.data.size());
  }
  const auto digest = total.Finish();
  set.set_hash = ToHex(digest.data(), digest.size()).substr(0, 16);
  return set;
}

void WriteShaderSet(const ShaderSet& set, const std::filesystem::path& dir) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (!std::filesystem::is_directory(dir, ec)) throw std::runtime_error("cannot create folder " + PathText(dir));
  // _sources.txt as extract_shaders.py writes it: "<name> <first 4 bytes hex> <size> <source>",
  // sorted by source.
  std::vector<const ShaderFile*> by_source;
  for (const auto& f : set.files) {
    WriteFileBytes(dir / f.name, f.data);
    by_source.push_back(&f);
  }
  std::sort(by_source.begin(), by_source.end(),
            [](const ShaderFile* a, const ShaderFile* b) { return a->source < b->source; });
  std::string sources;
  for (const ShaderFile* f : by_source) {
    sources += f->name + " " + ToHex(reinterpret_cast<const uint8_t*>(f->data.data()), 4) + " " +
               std::to_string(f->data.size()) + " " + f->source + "\n";
  }
  WriteFileBytes(dir / "_sources.txt", sources);
}

}  // namespace svr07_shader_cache
