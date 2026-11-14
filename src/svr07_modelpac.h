// svr07 - model pacs (pac\ch\chNN.pac): one EPAC group "EMD " of model entries.
//
// EPAC: 0x800 header, TOC at 0x800 (12-byte records: name, block, length in 256-byte
// units; a group record is {name, id << 16 | entries * 3, first block}), data at
// 0x4000 + block * 2048, 0x800-byte footer. Entry names are the model number "%04d"
// ((model*10 + attire)*10 + kind), or "X" + 3 base-36 digits for extras (svr07_roster).
#pragma once

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <rex/logging.h>

namespace svr07::modelpac {

using Bytes = std::vector<uint8_t>;

constexpr size_t kTocStart = 0x800, kDataStart = 0x4000, kFooter = 0x800;
constexpr uint32_t kDefaultGroupId = 867;  // ch92.pac's

struct PacEntry {
  std::string name;
  Bytes data;
};

inline uint32_t Rd32le(const uint8_t* p) {
  return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24;
}
inline void Wr32le(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i));
}

// The EMD group's entries of an EPAC model pac (group_id: the group record's id).
inline std::vector<PacEntry> ReadModelPac(const std::filesystem::path& p, Bytes& header,
                                          Bytes& footer, uint32_t* group_id = nullptr) {
  std::ifstream f(p, std::ios::binary);
  Bytes d(std::istreambuf_iterator<char>(f), {});
  if (d.size() < kDataStart + kFooter || std::memcmp(d.data(), "EPAC", 4) != 0)
    throw std::runtime_error("not an EPAC pac");
  header.assign(d.begin(), d.begin() + kTocStart);
  footer.assign(d.end() - kFooter, d.end());
  std::vector<PacEntry> out;
  std::string group;
  uint32_t left = 0;
  for (size_t pos = kTocStart; pos + 12 <= kDataStart; pos += 12) {
    std::string name(reinterpret_cast<const char*>(&d[pos]), 4);
    uint32_t a = Rd32le(&d[pos + 4]), b = Rd32le(&d[pos + 8]);
    if (name == std::string(4, '\0')) break;
    if (left == 0) {
      left = (a & 0xFFFF) / 3;
      group = name;
      if (group == "EMD " && group_id) *group_id = a >> 16;
      continue;
    }
    --left;
    if (group != "EMD ") {
      REXLOG_WARN("Model pac {}: skipping {}/{} (only EMD entries are used)", p.string(), group,
                  name);
      continue;
    }
    size_t at = kDataStart + size_t(a) * 2048, len = size_t(b) * 256;
    if (at + len > d.size()) throw std::runtime_error("entry out of range");
    out.push_back({name, Bytes(d.begin() + at, d.begin() + at + len)});
  }
  return out;
}

inline Bytes WriteModelPac(const std::vector<PacEntry>& entries, Bytes header, const Bytes& footer,
                           uint32_t group_id = kDefaultGroupId) {
  Bytes toc, data;
  auto rec = [&](const std::string& name, uint32_t a, uint32_t b) {
    toc.insert(toc.end(), name.begin(), name.end());
    toc.resize(toc.size() + 8);
    Wr32le(&toc[toc.size() - 8], a);
    Wr32le(&toc[toc.size() - 4], b);
  };
  rec("EMD ", (group_id << 16) | uint32_t(entries.size() * 3), 0);
  for (auto& e : entries) {
    rec(e.name, uint32_t(data.size() / 2048), uint32_t((e.data.size() + 255) / 256));
    data.insert(data.end(), e.data.begin(), e.data.end());
    data.resize((data.size() + 2047) & ~size_t(2047), 0);
  }
  if (toc.size() > kDataStart - kTocStart) throw std::runtime_error("too many model entries");
  Wr32le(&header[4], uint32_t(toc.size()));
  Wr32le(&header[8], uint32_t(data.size()));
  Bytes out = header;
  out.insert(out.end(), toc.begin(), toc.end());
  out.resize(kDataStart, 0);
  out.insert(out.end(), data.begin(), data.end());
  out.insert(out.end(), footer.begin(), footer.end());
  return out;
}

}  // namespace svr07::modelpac
