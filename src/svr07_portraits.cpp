// svr07 - select-screen portraits from the mods folder.
//
// The select screen draws three pictures per superstar, each from a bank in
// pac/menu/menuHD.pac (menuSD.pac in 4:3), keyed by the superstar's id (the NN
// of pac/ch/chNN.pac):
//   MENU/SF2A  face icon, 256x256 DXT5
//   MENU/SF2B  body render, 512x512 DXT5
//   MENU/SF2D  bust render, 256x256 DXT5
// The game loads the banks by path (table at 0x8275D090). Slots a mod adds (the
// UE mod's Jericho, Punk, MVP, ...) have no entry, so they show no picture.
//
// Mods give a slot pictures with mods\portraits\<id>\body.png (any size; a
// transparent background works best), and optionally face.png / bust.png to
// override the crops made from the body. A .dds (DXT5, the bank's size) is used
// as-is instead of a .png. mods\portraits\<id>.png is short for <id>\body.png.
//
// Layout of the files involved:
//   EPAC: 0x800 header, TOC at 0x800 (12-byte records: name, block, length in
//         256-byte units; a group record's block differs from the running one),
//         data at 0x4000 + block * 2048, 0x800-byte footer.
//   PACH: "PACH", count, then {id, offset, length} records; data follows them.
//   BPE:  "BPE ", 0x100, compressed length, decompressed length, then
//         byte-pair blocks.
// Each bank entry is PACH{0: BPE(PACH{id: DDS})}.

#include "svr07_portraits.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <future>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/ui/image_decode.h>

namespace svr07 {
namespace {

using Bytes = std::vector<uint8_t>;
namespace fs = std::filesystem;

uint32_t Rd32(const uint8_t* p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24);
}
void Put32(Bytes& b, uint32_t v) {
  for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i)));
}
void Set32(Bytes& b, size_t at, uint32_t v) {
  for (int i = 0; i < 4; ++i) b[at + i] = uint8_t(v >> (8 * i));
}
void Check(bool ok, const char* what) {
  if (!ok) throw std::runtime_error(what);
}

Bytes ReadFile(const fs::path& path) {
  std::ifstream f(path, std::ios::binary);
  Check(f.good(), "can't open file");
  return Bytes(std::istreambuf_iterator<char>(f), {});
}
void WriteFile(const fs::path& path, const Bytes& data) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(data.data()), data.size());
  Check(f.good(), "can't write file");
}

// ---- BPE ------------------------------------------------------------------

Bytes BpeDecompress(const Bytes& d) {
  Check(d.size() >= 16 && std::memcmp(d.data(), "BPE ", 4) == 0, "not BPE");
  size_t clen = Rd32(&d[8]), dlen = Rd32(&d[12]);
  Check(16 + clen <= d.size(), "BPE truncated");
  Bytes out;
  out.reserve(dlen);
  size_t p = 16, end = 16 + clen;
  std::array<uint8_t, 256> left, right;
  std::vector<uint8_t> stack;
  while (p < end) {
    for (int c = 0; c < 256; ++c) left[c] = uint8_t(c), right[c] = 0;
    int c = 0;
    while (c < 256) {
      int cnt = d[p++];
      if (cnt > 127) {
        c += cnt - 127;
        cnt = 0;
      }
      if (c == 256) break;
      for (int i = 0; i <= cnt; ++i, ++c) {
        left[c] = d[p++];
        if (c != left[c]) right[c] = d[p++];
      }
    }
    size_t size = d[p] | (d[p + 1] << 8);
    p += 2;
    for (size_t i = 0; i < size; ++i) {
      stack.push_back(d[p + i]);
      while (!stack.empty()) {
        uint8_t v = stack.back();
        stack.pop_back();
        if (v == left[v]) {
          out.push_back(v);
        } else {
          stack.push_back(right[v]);
          stack.push_back(left[v]);
        }
      }
    }
    p += size;
  }
  out.resize(std::max(out.size(), dlen));
  return out;
}

// One block: repeatedly replace the most frequent byte pair with a byte value
// the block doesn't use, then write the pair table the way BpeDecompress reads it.
Bytes BpeCompressBlock(const uint8_t* src, size_t n) {
  std::vector<uint8_t> d(src, src + n);
  std::array<uint8_t, 256> left, right{};
  std::array<bool, 256> present{};
  for (int c = 0; c < 256; ++c) left[c] = uint8_t(c);
  for (uint8_t v : d) present[v] = true;
  std::vector<uint16_t> count(65536, 0);
  for (;;) {
    if (d.size() < 2) break;
    for (size_t i = 0; i + 1 < d.size(); ++i) {
      uint16_t& k = count[(d[i] << 8) | d[i + 1]];
      if (k < 65535) ++k;
    }
    int best = 0, best_key = 0;
    for (size_t i = 0; i + 1 < d.size(); ++i) {
      int key = (d[i] << 8) | d[i + 1];
      if (count[key] > best) best = count[key], best_key = key;
    }
    for (size_t i = 0; i + 1 < d.size(); ++i) count[(d[i] << 8) | d[i + 1]] = 0;
    if (best < 4) break;
    int code = -1;
    for (int c = 0; c < 256 && code < 0; ++c) {
      if (!present[c] && left[c] == c) code = c;
    }
    if (code < 0) break;
    uint8_t a = uint8_t(best_key >> 8), b = uint8_t(best_key);
    size_t j = 0;
    for (size_t i = 0; i < d.size();) {
      if (i + 1 < d.size() && d[i] == a && d[i + 1] == b) {
        d[j++] = uint8_t(code);
        i += 2;
      } else {
        d[j++] = d[i++];
      }
    }
    d.resize(j);
    left[code] = a;
    right[code] = b;
    present[code] = true;
  }

  Bytes out;
  auto entry = [&](int c) {
    out.push_back(left[c]);
    if (left[c] != c) out.push_back(right[c]);
  };
  for (int c = 0; c < 256;) {
    int run = 0;
    while (c + run < 256 && run < 128 && left[c + run] == c + run) ++run;
    if (run > 0) {
      out.push_back(uint8_t(run + 127));
      c += run;
      if (c == 256) break;
      entry(c++);  // after a skip the reader takes exactly one entry
      continue;
    }
    int k = 1;
    while (c + k < 256 && k < 128 && left[c + k] != c + k) ++k;
    out.push_back(uint8_t(k - 1));
    for (int i = 0; i < k; ++i) entry(c + i);
    c += k;
  }
  out.push_back(uint8_t(d.size()));
  out.push_back(uint8_t(d.size() >> 8));
  out.insert(out.end(), d.begin(), d.end());
  return out;
}

Bytes BpeCompress(const Bytes& raw) {
  constexpr size_t kBlock = 8192;
  size_t blocks = (raw.size() + kBlock - 1) / kBlock;
  std::vector<Bytes> parts(blocks);
  unsigned workers = std::max(1u, std::thread::hardware_concurrency());
  std::vector<std::future<void>> jobs;
  for (unsigned w = 0; w < workers; ++w) {
    jobs.push_back(std::async(std::launch::async, [&, w] {
      for (size_t b = w; b < blocks; b += workers) {
        size_t at = b * kBlock;
        parts[b] = BpeCompressBlock(&raw[at], std::min(kBlock, raw.size() - at));
      }
    }));
  }
  for (auto& job : jobs) job.get();
  Bytes out = {'B', 'P', 'E', ' '};
  Put32(out, 0x100);
  Put32(out, 0);
  Put32(out, uint32_t(raw.size()));
  for (auto& part : parts) out.insert(out.end(), part.begin(), part.end());
  Set32(out, 8, uint32_t(out.size() - 16));
  Check(BpeDecompress(out) == raw, "BPE round trip failed");
  return out;
}

// ---- PACH -----------------------------------------------------------------

struct PachChild {
  uint32_t id;
  Bytes data;
};

std::vector<PachChild> PachSplit(const Bytes& b) {
  Check(b.size() >= 8 && std::memcmp(b.data(), "PACH", 4) == 0, "not PACH");
  uint32_t n = Rd32(&b[4]);
  size_t base = 8 + 12 * size_t(n);
  Check(base <= b.size(), "PACH truncated");
  std::vector<PachChild> out;
  for (uint32_t i = 0; i < n; ++i) {
    const uint8_t* r = &b[8 + 12 * i];
    size_t off = base + Rd32(r + 4), len = Rd32(r + 8);
    Check(off + len <= b.size(), "PACH child out of range");
    out.push_back({Rd32(r), Bytes(b.begin() + off, b.begin() + off + len)});
  }
  return out;
}

// align16 keeps each child's data 16-byte aligned in the file, like the banks.
Bytes PachJoin(const std::vector<PachChild>& children, bool align16) {
  size_t base = 8 + 12 * children.size();
  size_t pad = align16 ? (16 - base % 16) % 16 : 0;
  Bytes out = {'P', 'A', 'C', 'H'};
  Put32(out, uint32_t(children.size()));
  size_t pos = base + pad;  // where the child's data goes in the file
  for (auto& c : children) {
    Put32(out, c.id);
    Put32(out, uint32_t(pos - base));
    Put32(out, uint32_t(c.data.size()));
    pos += c.data.size();
    if (align16) pos = (pos + 15) & ~size_t(15);
  }
  out.resize(base + pad, 0);
  for (auto& c : children) {
    out.insert(out.end(), c.data.begin(), c.data.end());
    if (align16) out.resize((out.size() + 15) & ~size_t(15), 0);
  }
  return out;
}

// ---- EPAC -----------------------------------------------------------------

constexpr size_t kTocStart = 0x800, kDataStart = 0x4000, kFooter = 0x800;

struct EpacRecord {
  bool group;
  std::string key;  // "GROUP/NAME" for entries (raw 4-char names)
  size_t at;        // TOC position
  uint32_t block, length;
};

std::vector<EpacRecord> EpacParse(const Bytes& d) {
  Check(d.size() > kDataStart + kFooter && std::memcmp(d.data(), "EPAC", 4) == 0, "not EPAC");
  // A group record is {name, file id << 16 | entries * 3, first block}; its entries follow.
  // (Mods don't always keep entries in file order, so block numbers can't tell them apart.)
  std::vector<EpacRecord> out;
  std::string group;
  uint32_t left = 0;
  for (size_t pos = kTocStart; pos + 12 <= kDataStart; pos += 12) {
    std::string name(reinterpret_cast<const char*>(&d[pos]), 4);
    uint32_t off = Rd32(&d[pos + 4]), len = Rd32(&d[pos + 8]);
    if (name == std::string(4, '\0')) break;
    if (left == 0) {
      Check((off & 0xFFFF) % 3 == 0, "bad EPAC group record");
      left = (off & 0xFFFF) / 3;
      group = name;
      out.push_back({true, name, pos, off, len});
    } else {
      --left;
      out.push_back({false, group + "/" + name, pos, off, len});
    }
  }
  return out;
}

Bytes EpacEntry(const Bytes& d, const std::vector<EpacRecord>& recs, const std::string& key) {
  for (auto& r : recs) {
    if (!r.group && r.key == key) {
      size_t at = kDataStart + size_t(r.block) * 2048, len = size_t(r.length) * 256;
      Check(at + len <= d.size(), "EPAC entry out of range");
      return Bytes(d.begin() + at, d.begin() + at + len);
    }
  }
  throw std::runtime_error("EPAC entry not found: " + key);
}

// Same as RRXX-tools epac_rebuild.py: untouched entries keep their bytes and
// slot padding, replaced ones are padded to 2048 and everything after moves.
Bytes EpacRebuild(const Bytes& d, const std::vector<EpacRecord>& recs,
                  const std::map<std::string, Bytes>& repl) {
  std::vector<size_t> starts;
  for (auto& r : recs)
    if (!r.group) starts.push_back(kDataStart + size_t(r.block) * 2048);
  std::sort(starts.begin(), starts.end());
  size_t data_end = d.size() - kFooter;

  Bytes toc, data;
  uint32_t blk = 0;
  for (auto& r : recs) {
    const uint8_t* raw = &d[r.at];
    if (r.group) {
      toc.insert(toc.end(), raw, raw + 8);
      Put32(toc, blk);
      continue;
    }
    uint32_t len = r.length;
    if (auto it = repl.find(r.key); it != repl.end()) {
      len = uint32_t((it->second.size() + 255) / 256);
      data.insert(data.end(), it->second.begin(), it->second.end());
      data.resize((data.size() + 2047) & ~size_t(2047), 0);
    } else {
      size_t at = kDataStart + size_t(r.block) * 2048;
      auto next = std::upper_bound(starts.begin(), starts.end(), at);
      size_t end = next == starts.end() ? data_end : *next;
      data.insert(data.end(), d.begin() + at, d.begin() + end);
    }
    toc.insert(toc.end(), raw, raw + 4);
    Put32(toc, blk);
    Put32(toc, len);
    blk = uint32_t(data.size() / 2048);
  }
  Check(toc.size() <= kDataStart - kTocStart, "EPAC TOC too big");
  Bytes out(d.begin(), d.begin() + kTocStart);
  Set32(out, 8, uint32_t(data.size()));
  out.insert(out.end(), toc.begin(), toc.end());
  out.resize(kDataStart, 0);
  out.insert(out.end(), data.begin(), data.end());
  out.insert(out.end(), d.end() - kFooter, d.end());
  return out;
}

// ---- Pictures -------------------------------------------------------------

struct Image {
  int w = 0, h = 0;
  std::vector<float> px;  // premultiplied RGBA, 0..1
  const float* at(int x, int y) const { return &px[(size_t(y) * w + x) * 4]; }
};

Image LoadPng(const fs::path& path) {
  Bytes file = ReadFile(path);
  Image img;
  auto rgba = rex::ui::DecodeImageRGBA(file.data(), file.size(), img.w, img.h);
  Check(!rgba.empty(), "can't decode PNG");
  img.px.resize(rgba.size());
  for (size_t i = 0; i < rgba.size(); i += 4) {
    float a = rgba[i + 3] / 255.0f;
    for (int c = 0; c < 3; ++c) img.px[i + c] = rgba[i + c] / 255.0f * a;
    img.px[i + 3] = a;
  }
  return img;
}

// Bilinear sample with transparent outside the picture.
void Sample(const Image& s, float x, float y, float* out) {
  x -= 0.5f, y -= 0.5f;
  int x0 = int(std::floor(x)), y0 = int(std::floor(y));
  float fx = x - x0, fy = y - y0;
  for (int c = 0; c < 4; ++c) out[c] = 0;
  for (int j = 0; j < 2; ++j) {
    for (int i = 0; i < 2; ++i) {
      int xx = x0 + i, yy = y0 + j;
      if (xx < 0 || yy < 0 || xx >= s.w || yy >= s.h) continue;
      float wgt = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
      const float* p = s.at(xx, yy);
      for (int c = 0; c < 4; ++c) out[c] += p[c] * wgt;
    }
  }
}

// The square (x, y, size) of s, scaled to dim x dim (4x4 samples per pixel).
Image Crop(const Image& s, float x, float y, float size, int dim) {
  Image out;
  out.w = out.h = dim;
  out.px.assign(size_t(dim) * dim * 4, 0.0f);
  float step = size / dim;
  for (int py = 0; py < dim; ++py) {
    for (int px = 0; px < dim; ++px) {
      float acc[4] = {}, smp[4];
      for (int j = 0; j < 4; ++j) {
        for (int i = 0; i < 4; ++i) {
          Sample(s, x + (px + (i + 0.5f) / 4) * step, y + (py + (j + 0.5f) / 4) * step, smp);
          for (int c = 0; c < 4; ++c) acc[c] += smp[c] / 16;
        }
      }
      std::copy(acc, acc + 4, &out.px[(size_t(py) * dim + px) * 4]);
    }
  }
  return out;
}

// A body picture of any size, fitted into 512x512: centred, standing on the bottom.
Image FitBody(const Image& s) {
  float side = float(std::max(s.w, s.h));
  return Crop(s, (s.w - side) / 2, s.h - side, side, 512);
}

// Top of the head and its centre, from the alpha of a 512 body picture.
// Calibrated against the retail pictures (face: 200 px square from 6 px above
// the head; bust: 348 px square from the top of the head).
void FindHead(const Image& body, float& top, float& centre) {
  top = 0, centre = body.w / 2.0f;
  for (int y = 0; y < body.h; ++y) {
    int lo = -1, hi = -1, solid = 0;
    for (int yy = y; yy < std::min(body.h, y + 40); ++yy) {
      for (int x = 0; x < body.w; ++x) {
        if (body.at(x, yy)[3] > 0.5f) {
          if (yy == y) ++solid;
          if (lo < 0 || x < lo) lo = x;
          if (x > hi) hi = x;
        }
      }
    }
    if (solid >= 3) {
      top = float(y), centre = (lo + hi) / 2.0f;
      return;
    }
  }
}

Image CropAroundHead(const Image& body, float size, float dx, float dy, int dim) {
  float top, centre;
  FindHead(body, top, centre);
  float x = std::clamp(centre + dx - size / 2, 0.0f, body.w - size);
  float y = std::clamp(top + dy, 0.0f, body.h - size);
  return Crop(body, x, y, size, dim);
}

uint16_t To565(const float* c) {
  auto q = [](float v, int bits) { return int(std::lround(std::clamp(v, 0.0f, 1.0f) * ((1 << bits) - 1))); };
  return uint16_t((q(c[0], 5) << 11) | (q(c[1], 6) << 5) | q(c[2], 5));
}
void From565(uint16_t v, float* c) {
  c[0] = ((v >> 11) & 31) / 31.0f, c[1] = ((v >> 5) & 63) / 63.0f, c[2] = (v & 31) / 31.0f;
}

// DXT5 with the header the retail banks use (no mipmaps).
Bytes EncodeDds(const Image& img) {
  Bytes out(128, 0);
  std::memcpy(out.data(), "DDS ", 4);
  Set32(out, 4, 124);
  Set32(out, 8, 0x81007);
  Set32(out, 12, img.h);
  Set32(out, 16, img.w);
  Set32(out, 20, uint32_t(img.w * img.h));
  Set32(out, 76, 32);
  Set32(out, 80, 4);
  std::memcpy(&out[84], "DXT5", 4);
  Set32(out, 108, 0x1000);
  for (int by = 0; by < img.h; by += 4) {
    for (int bx = 0; bx < img.w; bx += 4) {
      float rgb[16][3], alpha[16];
      float lo[3] = {1, 1, 1}, hi[3] = {0, 0, 0};
      float amin = 1, amax = 0;
      bool any = false;
      for (int i = 0; i < 16; ++i) {
        const float* p = img.at(bx + i % 4, by + i / 4);
        alpha[i] = p[3];
        for (int c = 0; c < 3; ++c) rgb[i][c] = p[3] > 0 ? p[c] / p[3] : 0;
        amin = std::min(amin, p[3]), amax = std::max(amax, p[3]);
        if (p[3] < 0.03f) continue;
        any = true;
        for (int c = 0; c < 3; ++c) lo[c] = std::min(lo[c], rgb[i][c]), hi[c] = std::max(hi[c], rgb[i][c]);
      }
      // Alpha: 8-value mode, a0 > a1.
      int a0 = int(std::lround(amax * 255)), a1 = int(std::lround(amin * 255));
      float apal[8];
      apal[0] = float(a0), apal[1] = float(a1);
      for (int k = 1; k < 7; ++k) apal[k + 1] = ((7 - k) * a0 + k * a1) / 7.0f;
      uint64_t abits = 0;
      for (int i = 0; i < 16 && a0 != a1; ++i) {
        int best = 0;
        for (int k = 1; k < 8; ++k)
          if (std::abs(apal[k] - alpha[i] * 255) < std::abs(apal[best] - alpha[i] * 255)) best = k;
        abits |= uint64_t(best) << (3 * i);
      }
      out.push_back(uint8_t(a0));
      out.push_back(uint8_t(a1));
      for (int k = 0; k < 6; ++k) out.push_back(uint8_t(abits >> (8 * k)));
      // Colour: bounding box inset by 1/16, 4-colour mode (c0 > c1).
      if (!any) lo[0] = lo[1] = lo[2] = hi[0] = hi[1] = hi[2] = 0;
      float inset[3];
      for (int c = 0; c < 3; ++c) inset[c] = (hi[c] - lo[c]) / 16;
      float e0[3], e1[3];
      for (int c = 0; c < 3; ++c) e0[c] = hi[c] - inset[c], e1[c] = lo[c] + inset[c];
      uint16_t c0 = To565(e0), c1 = To565(e1);
      if (c0 < c1) std::swap(c0, c1);
      float pal[4][3];
      From565(c0, pal[0]);
      From565(c1, pal[1]);
      for (int c = 0; c < 3; ++c) {
        pal[2][c] = (2 * pal[0][c] + pal[1][c]) / 3;
        pal[3][c] = (pal[0][c] + 2 * pal[1][c]) / 3;
      }
      uint32_t cbits = 0;
      for (int i = 0; i < 16 && c0 != c1; ++i) {
        int best = 0;
        float best_d = 1e9f;
        for (int k = 0; k < 4; ++k) {
          float dist = 0;
          for (int c = 0; c < 3; ++c) dist += (pal[k][c] - rgb[i][c]) * (pal[k][c] - rgb[i][c]);
          if (dist < best_d) best_d = dist, best = k;
        }
        cbits |= uint32_t(best) << (2 * i);
      }
      out.push_back(uint8_t(c0));
      out.push_back(uint8_t(c0 >> 8));
      out.push_back(uint8_t(c1));
      out.push_back(uint8_t(c1 >> 8));
      Put32(out, cbits);
    }
  }
  return out;
}

// ---- Mods folder ----------------------------------------------------------

struct Bank {
  const char* key;
  const char* file;  // picture name in mods\portraits\<id>
  int dim;
};
constexpr Bank kBanks[] = {
    {"MENU/SF2A", "face", 256},
    {"MENU/SF2B", "body", 512},
    {"MENU/SF2D", "bust", 256},
};

struct Slot {
  uint32_t id;
  fs::path dir;        // mods\portraits\<id>, may not exist
  fs::path body_png;   // body picture
  std::vector<fs::path> inputs;
};

std::optional<fs::path> Existing(const fs::path& p) {
  std::error_code ec;
  if (fs::is_regular_file(p, ec)) return p;
  return std::nullopt;
}

bool AllDigits(const std::string& s) {
  return !s.empty() && s.size() <= 4 && std::all_of(s.begin(), s.end(), ::isdigit);
}

std::vector<Slot> FindSlots(const fs::path& portraits) {
  std::map<uint32_t, Slot> slots;
  std::error_code ec;
  for (auto& e : fs::directory_iterator(portraits, ec)) {
    auto stem = e.path().stem().string();
    if (e.is_directory() && AllDigits(e.path().filename().string())) {
      auto& s = slots[std::stoul(e.path().filename().string())];
      s.dir = e.path();
    } else if (e.is_regular_file() && AllDigits(stem) && e.path().extension() == ".png") {
      slots[std::stoul(stem)].body_png = e.path();
    }
  }
  std::vector<Slot> out;
  for (auto& [id, s] : slots) {
    s.id = id;
    if (!s.dir.empty()) {
      if (auto p = Existing(s.dir / "body.png")) s.body_png = *p;
      for (auto& b : kBanks) {
        for (auto ext : {".png", ".dds"})
          if (auto p = Existing(s.dir / (std::string(b.file) + ext))) s.inputs.push_back(*p);
      }
    }
    if (!s.body_png.empty() &&
        std::find(s.inputs.begin(), s.inputs.end(), s.body_png) == s.inputs.end())
      s.inputs.push_back(s.body_png);
    if (!s.inputs.empty()) out.push_back(std::move(s));
  }
  return out;
}

// The DDS for one bank of one slot, or nothing if the slot can't make it.
std::optional<Bytes> SlotPicture(const Slot& s, const Bank& b, std::optional<Image>& body) {
  if (!s.dir.empty()) {
    if (auto p = Existing(s.dir / (std::string(b.file) + ".dds"))) {
      Bytes dds = ReadFile(*p);
      Check(dds.size() >= 128 && std::memcmp(dds.data(), "DDS ", 4) == 0, "bad DDS");
      return dds;
    }
    // A face/bust picture of its own: fitted into the square, centred.
    if (std::string(b.file) != "body") {
      if (auto p = Existing(s.dir / (std::string(b.file) + ".png"))) {
        Image img = LoadPng(*p);
        float side = float(std::max(img.w, img.h));
        return EncodeDds(Crop(img, (img.w - side) / 2, (img.h - side) / 2, side, b.dim));
      }
    }
  }
  if (s.body_png.empty()) return std::nullopt;
  if (!body) body = FitBody(LoadPng(s.body_png));
  if (b.dim == 512) return EncodeDds(*body);
  if (std::string(b.file) == "face") return EncodeDds(CropAroundHead(*body, 200, 6, -6, b.dim));
  return EncodeDds(CropAroundHead(*body, 348, 8, 0, b.dim));
}

std::string Stamp(const fs::path& base, const std::vector<Slot>& slots) {
  std::ostringstream st;
  auto add = [&](const fs::path& p) {
    std::error_code ec;
    st << p.string() << '|' << fs::file_size(p, ec) << '|'
       << fs::last_write_time(p, ec).time_since_epoch().count() << '\n';
  };
  st << "svr07-portraits 3\n";
  add(base);
  for (auto& s : slots) {
    st << s.id << '\n';
    for (auto& p : s.inputs) add(p);
  }
  return st.str();
}

}  // namespace

void BuildPortraitPacs(const fs::path& game_root, const fs::path& mods, const fs::path& out_root) {
  std::vector<Slot> slots;
  if (!mods.empty()) slots = FindSlots(mods / "portraits");

  // Pictures are made once and shared by menuHD and menuSD.
  std::map<std::pair<uint32_t, int>, Bytes> pictures;
  bool pictures_ready = false;

  for (const char* name : {"menuHD.pac", "menuSD.pac"}) {
    fs::path out = out_root / "pac" / "menu" / name;
    fs::path stamp_file = out;
    stamp_file += ".stamp";
    std::error_code ec;
    if (slots.empty()) {
      fs::remove(out, ec);
      fs::remove(stamp_file, ec);
      continue;
    }
    fs::path base = mods / "pac" / "menu" / name;
    if (!Existing(base)) base = game_root / "pac" / "menu" / name;
    if (!Existing(base)) continue;

    std::string stamp = Stamp(base, slots);
    if (Existing(out) && Existing(stamp_file)) {
      Bytes old = ReadFile(stamp_file);
      if (std::string(old.begin(), old.end()) == stamp) {
        REXLOG_INFO("Portraits: {} is up to date ({} slots)", name, slots.size());
        continue;
      }
    }

    try {
      if (!pictures_ready) {
        for (auto& s : slots) {
          std::optional<Image> body;
          for (int b = 0; b < 3; ++b) {
            try {
              if (auto dds = SlotPicture(s, kBanks[b], body)) pictures[{s.id, b}] = std::move(*dds);
            } catch (const std::exception& e) {
              REXLOG_WARN("Portraits: slot {} {}: {}", s.id, kBanks[b].file, e.what());
            }
          }
        }
        pictures_ready = true;
      }

      REXLOG_INFO("Portraits: building {} from {}", name, base.string());
      Bytes pac = ReadFile(base);
      auto recs = EpacParse(pac);
      std::map<std::string, Bytes> repl;
      for (int b = 0; b < 3; ++b) {
        auto outer = PachSplit(EpacEntry(pac, recs, kBanks[b].key));
        Check(outer.size() == 1, "unexpected bank layout");
        auto inner = PachSplit(BpeDecompress(outer[0].data));
        int added = 0, replaced = 0;
        for (auto& [key, dds] : pictures) {
          if (key.second != b) continue;
          auto it = std::find_if(inner.begin(), inner.end(),
                                 [&](const PachChild& c) { return c.id == key.first; });
          if (it != inner.end()) {
            it->data = dds;
            ++replaced;
          } else {
            inner.push_back({key.first, dds});
            ++added;
          }
        }
        std::sort(inner.begin(), inner.end(),
                  [](const PachChild& x, const PachChild& y) { return x.id < y.id; });
        outer[0].data = BpeCompress(PachJoin(inner, true));
        repl[kBanks[b].key] = PachJoin(outer, false);
        REXLOG_INFO("Portraits: {} {}: {} added, {} replaced", name, kBanks[b].key, added,
                    replaced);
      }

      fs::create_directories(out.parent_path());
      fs::path tmp = out;
      tmp += ".tmp";
      WriteFile(tmp, EpacRebuild(pac, recs, repl));
      fs::rename(tmp, out);
      WriteFile(stamp_file, Bytes(stamp.begin(), stamp.end()));
    } catch (const std::exception& e) {
      REXLOG_WARN("Portraits: couldn't build {}: {} (using the game's)", name, e.what());
      fs::remove(out, ec);
      fs::remove(stamp_file, ec);
    }
  }
}

// ---- plist360.arc ---------------------------------------------------------
//
// The game finds pac entries through plist360.arc (and plist360_4x3.arc), not through each
// pac's own table: for each pac in plist360.h, in order, a marker (ff 00 00 00 for the
// first, ff <index> ff ff after) and a big-endian copy of the pac's EPAC table, group
// records stored as (entries * 3) << 20 | file id; then ff ff ff ff. A mod that moves
// entries without updating it makes the game read the wrong bytes: the UE 2.5 mod grew four
// loading screens in menuHD.pac, so LOAD/S006..S031 were read from stale offsets (empty
// picture set -> null track array -> crash on the first NOW LOADING screen). Merged pacs
// (above) move entries too. So the index is rebuilt from the pacs the game will really
// read, and laid over the game's when it differs. Rebuilding the retail index from retail
// files reproduces it byte for byte.

namespace {

std::optional<fs::path> FirstExisting(const std::vector<fs::path>& roots, const fs::path& rel) {
  for (auto& r : roots)
    if (auto p = Existing(r / rel)) return p;
  return std::nullopt;
}

Bytes IndexBlock(const fs::path& pac) {
  Bytes head(kDataStart);
  {
    std::ifstream f(pac, std::ios::binary);
    f.read(reinterpret_cast<char*>(head.data()), head.size());
    Check(f.gcount() == std::streamsize(head.size()), "pac too small");
  }
  Check(std::memcmp(head.data(), "EPAC", 4) == 0, "not EPAC");
  Bytes out;
  auto be = [&](uint32_t v) {
    for (int i = 3; i >= 0; --i) out.push_back(uint8_t(v >> (8 * i)));
  };
  uint32_t left = 0;
  for (size_t pos = kTocStart; pos + 12 <= kDataStart; pos += 12) {
    if (Rd32(&head[pos]) == 0) break;
    uint32_t a = Rd32(&head[pos + 4]), b = Rd32(&head[pos + 8]);
    out.insert(out.end(), &head[pos], &head[pos + 4]);
    if (left == 0) {
      Check((a & 0xFFFF) % 3 == 0, "bad EPAC group record");
      left = (a & 0xFFFF) / 3;
      be(((a & 0xFFFF) << 20) | (a >> 16));
    } else {
      --left;
      be(a);
    }
    be(b);
  }
  return out;
}

void RebuildPacIndex(const std::vector<fs::path>& roots, const fs::path& out_root,
                     const fs::path& list_rel, const fs::path& arc_rel) {
  fs::path out = out_root / arc_rel;
  std::error_code ec;
  try {
    // The game's own index (mods folder or game folder), not one we wrote earlier.
    std::vector<fs::path> base_roots(roots.begin() + 1, roots.end());
    auto base_arc = FirstExisting(base_roots, arc_rel);
    auto list = FirstExisting(roots, list_rel);
    if (!base_arc || !list) return;

    Bytes text = ReadFile(*list);
    std::vector<std::string> names;
    std::istringstream lines(std::string(text.begin(), text.end()));
    for (std::string line; std::getline(lines, line);) {
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
      if (!line.empty()) names.push_back(line);
    }
    Bytes arc;
    for (size_t i = 0; i < names.size(); ++i) {
      std::string rel = names[i];
      std::replace(rel.begin(), rel.end(), '\\', '/');
      auto pac = FirstExisting(roots, rel);
      Check(pac.has_value(), ("missing " + names[i]).c_str());
      const uint8_t marker[4] = {0xFF, uint8_t(i ? i : 0), uint8_t(i ? 0xFF : 0),
                                 uint8_t(i ? 0xFF : 0)};
      arc.insert(arc.end(), marker, marker + 4);
      Bytes block = IndexBlock(*pac);
      arc.insert(arc.end(), block.begin(), block.end());
    }
    const uint8_t end_marker[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    arc.insert(arc.end(), end_marker, end_marker + 4);

    if (arc == ReadFile(*base_arc)) {
      fs::remove(out, ec);
      REXLOG_INFO("Pac index: {} matches the pac files", arc_rel.string());
      return;
    }
    if (Existing(out) && ReadFile(out) == arc) {
      REXLOG_INFO("Pac index: {} rebuilt earlier, up to date", arc_rel.string());
      return;
    }
    fs::create_directories(out.parent_path());
    WriteFile(out, arc);
    REXLOG_INFO("Pac index: {} was stale for these files, rebuilt ({} -> {} bytes)",
                arc_rel.string(), fs::file_size(*base_arc, ec), arc.size());
  } catch (const std::exception& e) {
    REXLOG_WARN("Pac index: couldn't rebuild {}: {} (using the game's)", arc_rel.string(),
                e.what());
    fs::remove(out, ec);
  }
}

}  // namespace

void FixPacIndexes(const fs::path& game_root, const fs::path& mods, const fs::path& out_root) {
  std::vector<fs::path> roots = {out_root};
  if (!mods.empty()) roots.push_back(mods);
  roots.push_back(game_root);
  RebuildPacIndex(roots, out_root, "pac/plist360.h", "plist360.arc");
  RebuildPacIndex(roots, out_root, "pac/plist360_4x3.h", "plist360_4x3.arc");
}

void PreparePortraits(const fs::path& game_root, const fs::path& cache_root) {
  const fs::path out_root = cache_root / "mods_generated";
  rex::Runtime::AddModsOverlay(out_root);
  const fs::path mods = rex::Runtime::ResolveModsRoot();
  BuildPortraitPacs(game_root, mods, out_root);
  FixPacIndexes(game_root, mods, out_root);
}

}  // namespace svr07
