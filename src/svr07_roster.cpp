// svr07 - roster expansion: superstars beyond the game's 93 roster slots.
//
// The game numbers every character in one space: 0-92 roster, 93-122 CAWs, 123-127
// CAW templates, 128-152 CAW move sets, 153-166 blank. "id >= 93 means CAW" is coded
// all over (docs/ROSTER_EXPANSION.md), and the roster's tables are fixed arrays of 93
// right in front of the CAWs' (and saved as a raw memory dump), so there is no room
// after 92. Extra superstars get ids from 167 up instead, with their own storage:
//
// - record (178 bytes, like 0x828139BC + id*178), moves profile (412 bytes per profile
//   set, like 0x827A8558 + set*0x11E10 + 88 + id*412) and GM-mode record (76 bytes, like
//   sub_821B8E30() + 29648 + id*76), in guest memory allocated here;
// - the getters (sub_821B9A70 record, sub_821B9AF0 profile, sub_8234BDA8 GM record)
//   answer for them;
// - every `cmp id,93` gets a hook (tools/roster_sites.py) that makes an extra id
//   compare like a roster id, so the game takes its roster branch;
// - their model number is their id: entries (id*10 + attire)*10 + kind, 16700 and up.
//   EPAC names are 4 characters, so those get names "X" + 3 base-36 digits (sprintf
//   hook) and go into a copy of pac\ch\ch92.pac laid over the game's;
// - the select list (sub_82536E20) and the select pictures (sub_82466220 builds a
//   bank's 108-entry table, sub_82465DC8 looks one up) include them.
//
// mods\roster\<folder>\manifest.txt + ch.pac make one (see docs/ROSTER_EXPANSION.md).

#include "svr07_roster.h"

#include "generated/default/svr07_init.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(roster_extras, true, "Game",
                    "Load extra superstars from mods\\roster (ids 167 and up)");

REXCVAR_DEFINE_BOOL(roster_trace_sites, false, "Game",
                    "Log the first time each roster id-compare site runs (any id)");

namespace svr07::roster {
namespace {

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

// ---- game layout -----------------------------------------------------------

constexpr uint32_t kRecordSize = 178;
constexpr uint32_t kProfileSize = 412;
constexpr uint32_t kGmSize = 76;
constexpr uint32_t kProfileSets = 8;  // set index from sub_821B9AF0's r5, or [0x8293464C]
constexpr uint32_t kCurrentProfileSet = 0x8293464C;
constexpr uint32_t kProfileBlocks = 0x827A8558;  // set s: + s * kProfileSetSize (sub_821B8E98)
constexpr uint32_t kProfileSetSize = 0x11E10;
constexpr uint32_t kSelectListMax = 123;  // the select screen keeps its list in 123 slots
constexpr uint32_t kEmdFormat = 0x82001E48;  // "/EMD/%04d"
constexpr uint32_t kFirstExtraModel = kFirstExtra * 100;

// Record fields (big-endian in memory).
constexpr uint32_t kRecBrand = 16;   // 0 RAW, 1 SmackDown, 2 other
constexpr uint32_t kRecModel = 26;   // s16 model number (entries (model*10 + attire)*10 + kind)
constexpr uint32_t kRecName = 28;    // 46 chars
constexpr uint32_t kRecNick = 74;    // 46 chars
constexpr uint32_t kRecShort = 120;  // 26 chars
constexpr uint32_t kRecFemale = 146;
constexpr uint32_t kRecId = 148;     // byte
constexpr uint32_t kRecAttires = 150;  // 4 x u16, id*10 + 1..4
constexpr uint32_t kRecPlayable = 160;  // 1 = on the select screen

// ---- extras ------------------------------------------------------------------

// File sizes of the per-extra overrides (as in misc.pac CHAR/DAT, CHAR/PRO and
// gm_mode.pac GMMD/DATA: one decompressed child / record, little-endian).
constexpr uint32_t kDatSize = 176;

struct Extra {
  uint32_t id = 0;
  std::string folder, name, nick, short_name;
  fs::path dir;
  uint32_t base = 0;  // roster superstar it starts from (stats, moves, entrance)
  int brand = -1, female = -1;
  Bytes dat, pro, gmd;  // own records (dat=, pro=, gmd=), empty = the base's
  bool pictures = false;
  std::string group;  // select-screen section (group=), empty = from the record
};

std::vector<Extra> g_extras;  // filled at startup, read-only afterwards

// Select-screen sections: mods\roster\select_order.txt ("order = A | B | ...", "<id> = <group>"
// for roster ids, "<folder> = <group>" for extras) and the manifests' group=. Read at startup.
bool g_sections = false;
std::vector<std::string> g_group_order;
std::map<uint32_t, std::string> g_id_group;
std::map<std::string, std::string> g_folder_group;
std::array<int, kMaxExtras> g_slot_of{};  // id - kFirstExtra -> index in g_extras, or -1

std::mutex g_mutex;  // guards the storage setup below
uint32_t g_records = 0, g_profiles = 0, g_gm = 0;  // guest addresses
uint8_t* g_base = nullptr;  // guest memory, for the register-only site hooks
std::array<bool, kMaxExtras> g_ready{};

int SlotOf(uint32_t id) {
  if (id < kFirstExtra || id >= kFirstExtra + kMaxExtras) return -1;
  return g_slot_of[id - kFirstExtra];
}

std::string Trim(std::string s) {
  while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
  return s.substr(i);
}

Bytes ReadFile(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return Bytes(std::istreambuf_iterator<char>(f), {});
}

void WriteFileIfChanged(const fs::path& p, const Bytes& data) {
  std::error_code ec;
  if (fs::exists(p, ec) && ReadFile(p) == data) return;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream f(p, std::ios::binary);
  f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
}

uint32_t Rd32le(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }
void Wr32le(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i));
}

// "X" + 3 base-36 digits for model entry numbers from 16700 (ids 167+).
std::string ExtraEntryName(uint32_t n) {
  static const char kDigits[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  uint32_t v = n - kFirstExtraModel;
  std::string s = "X";
  s += kDigits[(v / 1296) % 36];
  s += kDigits[(v / 36) % 36];
  s += kDigits[v % 36];
  return s;
}

// ---- guest helpers -----------------------------------------------------------

void Copy(uint8_t* base, uint32_t dst, uint32_t src, uint32_t n) {
  std::memmove(base + dst, base + src, n);
}

void PutString(uint8_t* base, uint32_t at, const std::string& s, uint32_t field) {
  std::memset(base + at, 0, field);
  std::memcpy(base + at, s.data(), std::min<size_t>(s.size(), field - 1));
}

using GuestFn = void (*)(PPCContext&, uint8_t*);

// Calls a game function with up to 4 arguments; the caller's registers are kept.
uint32_t Call(PPCContext& ctx, uint8_t* base, GuestFn fn, uint32_t a = 0, uint32_t b = 0,
              uint32_t c = 0, uint32_t d = 0) {
  PPCContext saved = ctx;
  ctx.r3.u64 = a;
  ctx.r4.u64 = b;
  ctx.r5.u64 = c;
  ctx.r6.u64 = d;
  fn(ctx, base);
  uint32_t r = ctx.r3.u32;
  ctx = saved;
  return r;
}

uint32_t RecordAddr(uint32_t slot) { return g_records + slot * kRecordSize; }
uint32_t ProfileAddr(uint32_t slot, uint32_t set) {
  return g_profiles + (set * uint32_t(g_extras.size()) + slot) * kProfileSize;
}
uint32_t GmAddr(uint32_t slot) { return g_gm + slot * kGmSize; }

uint32_t ProfileSet(uint8_t* base, int32_t set) {
  uint32_t s = set > -1 ? uint32_t(set) : REX_LOAD_U32(kCurrentProfileSet);
  return s < kProfileSets ? s : 0;
}

// Sets an extra up from its base the first time the game asks for it (after the
// game's own tables exist).
void EnsureReady(PPCContext& ctx, uint8_t* base, int slot) {
  if (g_ready[slot]) return;
  std::lock_guard lock(g_mutex);
  if (g_ready[slot]) return;
  g_base = base;
  if (!g_records) {
    auto* mem = REX_KERNEL_MEMORY();
    const uint32_t n = uint32_t(g_extras.size());  // fixed after startup
    g_records = mem->SystemHeapAlloc(n * kRecordSize);
    g_profiles = mem->SystemHeapAlloc(kProfileSets * n * kProfileSize);
    g_gm = mem->SystemHeapAlloc(n * kGmSize);
    std::memset(base + g_records, 0, n * kRecordSize);
    std::memset(base + g_profiles, 0, kProfileSets * n * kProfileSize);
    std::memset(base + g_gm, 0, n * kGmSize);
    REXLOG_INFO("Roster: extra storage at {:08X} (records) {:08X} (profiles) {:08X} (GM)",
                g_records, g_profiles, g_gm);
  }
  const Extra& e = g_extras[slot];
  // Record: the base's (or dat=, converted like sub_82122470 does: its u16 fields
  // swapped), renamed, with its own id and model number.
  uint32_t rec = RecordAddr(slot);
  Copy(base, rec, Call(ctx, base, __imp__sub_821B9A70, e.base, uint32_t(-1)), kRecordSize);
  if (!e.dat.empty()) {
    std::memcpy(base + rec, e.dat.data(), kDatSize);
    for (uint32_t at : {22u, 24u, 26u, 150u, 152u, 154u, 156u}) std::swap(base[rec + at], base[rec + at + 1]);
  }
  if (!e.name.empty()) PutString(base, rec + kRecName, e.name, 46);
  if (!e.nick.empty() || e.dat.empty())
    PutString(base, rec + kRecNick, e.nick.empty() ? e.short_name : e.nick, 46);
  if (!e.short_name.empty()) PutString(base, rec + kRecShort, e.short_name, 26);
  REX_STORE_U16(rec + kRecModel, uint16_t(e.id));
  base[rec + kRecId] = uint8_t(e.id);
  for (uint32_t a = 0; a < 4; ++a) REX_STORE_U16(rec + kRecAttires + 2 * a, uint16_t(e.id * 10 + 1 + a));
  base[rec + kRecPlayable] = 1;
  if (e.brand >= 0) base[rec + kRecBrand] = uint8_t(e.brand);
  if (e.female >= 0) base[rec + kRecFemale] = uint8_t(e.female);
  // Profiles: the base's (or pro=, converted by the game's sub_821B99D8 like the
  // CHAR/PRO loader sub_821B9BA0 does) in every set.
  for (uint32_t s = 0; s < kProfileSets; ++s) {
    uint32_t dst = ProfileAddr(slot, s);
    if (!e.pro.empty()) {
      std::memcpy(base + dst, e.pro.data(), kProfileSize);
      Call(ctx, base, sub_821B99D8, dst);
    } else {
      Copy(base, dst, Call(ctx, base, __imp__sub_821B9AF0, e.base, uint32_t(-1), s), kProfileSize);
    }
  }
  // GM-mode record: the base's (or gmd=, converted by sub_82354528 like the
  // gm_mode.pac loader sub_82354710 does), own id, not in the draft pool.
  uint32_t gm = GmAddr(slot);
  if (!e.gmd.empty()) {
    std::memcpy(base + gm, e.gmd.data(), kGmSize);
    Call(ctx, base, sub_82354528, gm);
  } else {
    Copy(base, gm, Call(ctx, base, sub_821B8E30) + 29648 + e.base * kGmSize, kGmSize);
  }
  base[gm + 0] = uint8_t(e.id);
  base[gm + 1] = 0;
  g_ready[slot] = true;
  REXLOG_INFO("Roster: {} ready as id {} (base {}{}{}{})", e.name, e.id, e.base,
              e.dat.empty() ? "" : ", own DAT", e.pro.empty() ? "" : ", own PRO",
              e.gmd.empty() ? "" : ", own GMD");
}

}  // namespace

bool IsExtra(uint32_t id) { return SlotOf(id) >= 0; }

std::vector<std::pair<uint32_t, fs::path>> ExtraPictureFolders() {
  std::vector<std::pair<uint32_t, fs::path>> out;
  for (auto& e : g_extras)
    if (e.pictures) out.emplace_back(e.id, e.dir);
  return out;
}

void OnIdCompare(uint32_t addr, Site policy, PPCCRRegister& cr, PPCRegister& id) {
  if (REXCVAR_GET(roster_trace_sites)) {
    static std::mutex tm;
    static std::set<uint32_t> traced;
    std::lock_guard lock(tm);
    if (traced.insert(addr).second)
      REXLOG_INFO("Roster trace: site {:08X} first run, value {}", addr, id.u32);
  }
  if (!IsExtra(id.u32)) return;
  static std::mutex m;
  static std::set<uint32_t> seen;
  {
    std::lock_guard lock(m);
    if (seen.insert(addr).second)
      REXLOG_INFO("Roster: site {:08X} saw extra id {} ({})", addr, id.u32,
                  policy == Site::kRoster ? "as roster" : "observed");
  }
  if (policy == Site::kRoster) {
    cr.lt = 1;
    cr.gt = 0;
    cr.eq = 0;
  }
}

bool IsExtraModel(uint32_t n) { return n >= kFirstExtraModel && IsExtra(n / 100); }

namespace {
void LogModelSite(uint32_t addr, uint32_t n) {
  static std::mutex m;
  static std::set<uint32_t> seen;
  std::lock_guard lock(m);
  if (seen.insert(addr).second)
    REXLOG_INFO("Roster: model site {:08X} saw extra model {} (as roster)", addr, n);
}
}  // namespace

void OnModelSub(uint32_t addr, PPCRegister& diff) {
  uint32_t n = diff.u32 + 10000;
  if (!IsExtraModel(n)) return;
  LogModelSite(addr, n);
  diff.u64 = uint64_t(-1);
}

namespace {
// id*412 for an extra: the offset from the current set's block to its profile - 88.
bool ProfileOffset(uint32_t addr, uint32_t id, uint32_t& offset) {
  int slot = SlotOf(id);
  if (slot < 0) return false;
  uint8_t* base = g_base;
  if (!base || !g_ready[slot]) {
    REXLOG_WARN("Roster: profile site {:08X}: extra id {} not set up yet", addr, id);
    return false;
  }
  uint32_t set = ProfileSet(base, -1);
  offset = ProfileAddr(slot, set) - 88 - (kProfileBlocks + set * kProfileSetSize);
  static std::mutex m;
  static std::set<uint32_t> seen;
  std::lock_guard lock(m);
  if (seen.insert(addr).second)
    REXLOG_INFO("Roster: profile site {:08X} saw extra id {} (own profile)", addr, id);
  return true;
}
}  // namespace

void OnProfileIndexBefore(uint32_t addr, PPCRegister& id) {
  uint32_t offset;
  if (!ProfileOffset(addr, id.u32, offset)) return;
  // v * 412 == offset (mod 2^32): 412 = 4 * 103, offset is a multiple of 4, 103 is odd.
  uint32_t inv = 103;
  for (int i = 0; i < 5; ++i) inv *= 2 - 103 * inv;
  id.u64 = uint32_t((offset >> 2) * inv);
}

void OnProfileIndexAfter(uint32_t addr, PPCRegister& product, PPCRegister& id) {
  uint32_t offset;
  if (ProfileOffset(addr, id.u32, offset)) product.u64 = offset;
}

void OnModelCompare(uint32_t addr, PPCCRRegister& cr, PPCRegister& n) {
  if (!IsExtraModel(n.u32)) return;
  LogModelSite(addr, n.u32);
  cr.lt = 1;
  cr.gt = 0;
  cr.eq = 0;
}

// ---- startup: mods\roster ------------------------------------------------------

namespace {

constexpr size_t kTocStart = 0x800, kDataStart = 0x4000, kFooter = 0x800;

struct PacEntry {
  std::string name;
  Bytes data;
};

// The EMD group's entries of an EPAC model pac.
std::vector<PacEntry> ReadModelPac(const fs::path& p, Bytes& header, Bytes& footer) {
  Bytes d = ReadFile(p);
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
      continue;
    }
    --left;
    if (group != "EMD ") {
      REXLOG_WARN("Roster: {}: skipping {}/{} (only EMD entries are used)", p.string(), group,
                  name);
      continue;
    }
    size_t at = kDataStart + size_t(a) * 2048, len = size_t(b) * 256;
    if (at + len > d.size()) throw std::runtime_error("entry out of range");
    out.push_back({name, Bytes(d.begin() + at, d.begin() + at + len)});
  }
  return out;
}

Bytes WriteModelPac(const std::vector<PacEntry>& entries, Bytes header, const Bytes& footer) {
  Bytes toc, data;
  auto rec = [&](const std::string& name, uint32_t a, uint32_t b) {
    toc.insert(toc.end(), name.begin(), name.end());
    toc.resize(toc.size() + 8);
    Wr32le(&toc[toc.size() - 8], a);
    Wr32le(&toc[toc.size() - 4], b);
  };
  rec("EMD ", (867u << 16) | uint32_t(entries.size() * 3), 0);
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

std::map<std::string, std::string> ReadManifest(const fs::path& p) {
  std::map<std::string, std::string> kv;
  const Bytes raw = ReadFile(p);
  std::istringstream in(std::string(raw.begin(), raw.end()));
  for (std::string line; std::getline(in, line);) {
    line = Trim(line);
    if (line.empty() || line[0] == '#') continue;
    auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    kv[Trim(line.substr(0, eq))] = Trim(line.substr(eq + 1));
  }
  return kv;
}

void ReadSelectOrder(const fs::path& p) {
  std::error_code ec;
  if (!fs::is_regular_file(p, ec)) return;
  g_sections = true;
  for (auto& [key, value] : ReadManifest(p)) {
    if (key == "order") {
      std::string rest = value;
      for (size_t at; !rest.empty();) {
        at = rest.find('|');
        std::string g = Trim(rest.substr(0, at));
        if (!g.empty()) g_group_order.push_back(g);
        if (at == std::string::npos) break;
        rest = rest.substr(at + 1);
      }
    } else if (!key.empty() && std::all_of(key.begin(), key.end(), ::isdigit)) {
      g_id_group[uint32_t(std::stoul(key))] = value;
    } else {
      g_folder_group[key] = value;
    }
  }
  REXLOG_INFO("Roster: select sections from {}: {} groups, {} ids, {} folders", p.string(),
              g_group_order.size(), g_id_group.size(), g_folder_group.size());
}

// mods\roster\slots.txt: "<folder>\t<id>" per line. A mod keeps its id, so adding or
// removing one never moves another's data.
std::map<std::string, uint32_t> ReadSlots(const fs::path& p) {
  std::map<std::string, uint32_t> out;
  const Bytes raw = ReadFile(p);
  std::istringstream in(std::string(raw.begin(), raw.end()));
  for (std::string line; std::getline(in, line);) {
    auto tab = line.find('\t');
    if (tab == std::string::npos) continue;
    try {
      out[Trim(line.substr(0, tab))] = uint32_t(std::stoul(Trim(line.substr(tab + 1))));
    } catch (...) {
    }
  }
  return out;
}

}  // namespace

// The extras' model entries go into a copy of pac\ch\ch92.pac (with its own entries)
// laid over the game's. A pac of their own would need a plist360.h line, and UE 2.5
// already lists the 192 pac files the game's file system is set up for (sub_821C4D18 ->
// sub_8241D310); entries are found by name through plist360.arc, so any listed pac can
// hold them (FixPacIndexes rebuilds the index for the merged file).
void BuildRosterFiles(const fs::path& game_root, const fs::path& mods, const fs::path& out_root) {
  g_slot_of.fill(-1);
  const fs::path host_rel = fs::path("pac") / "ch" / "ch92.pac";
  const fs::path pac_out = out_root / host_rel;
  std::error_code ec;
  const fs::path dir = mods.empty() ? fs::path() : mods / "roster";
  if (!REXCVAR_GET(roster_extras) || dir.empty() || !fs::is_directory(dir, ec)) {
    fs::remove(pac_out, ec);
    return;
  }

  auto slots = ReadSlots(dir / "slots.txt");
  ReadSelectOrder(dir / "select_order.txt");
  std::vector<PacEntry> models;
  Bytes unused_header, unused_footer;
  std::vector<fs::path> folders;
  for (auto& it : fs::directory_iterator(dir, ec))
    if (it.is_directory() && !fs::exists(it.path() / "disabled", ec)) folders.push_back(it.path());
  std::sort(folders.begin(), folders.end());

  // A file named by a manifest key, inside the folder, of exactly `size` bytes.
  auto key_file = [](const fs::path& folder, std::map<std::string, std::string>& kv,
                     const char* key, size_t size) {
    if (!kv.count(key) || kv[key].empty()) return Bytes();
    fs::path p = folder / kv[key];
    std::error_code ec2;
    if (!fs::is_regular_file(p, ec2)) throw std::runtime_error(std::string(key) + "= file not found");
    Bytes b = ReadFile(p);
    if (b.size() != size)
      throw std::runtime_error(fmt::format("{}= file is {} bytes, must be {}", key, b.size(), size));
    return b;
  };
  auto field = [](const Bytes& dat, size_t at, size_t len) {
    const char* p = reinterpret_cast<const char*>(dat.data() + at);
    return std::string(p, strnlen(p, len));
  };

  // Pass 1: manifests.
  struct Pending {
    Extra e;
    uint32_t pinned = 0;  // id= (0 = none)
  };
  std::vector<Pending> pending;
  for (auto& folder : folders) {
    std::string key = folder.filename().string();
    try {
      auto kv = ReadManifest(folder / "manifest.txt");
      Pending p;
      Extra& e = p.e;
      e.folder = key;
      e.dir = folder;
      e.dat = key_file(folder, kv, "dat", kDatSize);
      e.pro = key_file(folder, kv, "pro", kProfileSize);
      e.gmd = key_file(folder, kv, "gmd", kGmSize);
      e.name = kv["name"];
      if (e.name.empty() && !e.dat.empty()) e.name = field(e.dat, kRecName, 46);
      e.short_name = kv.count("short") ? kv["short"]
                     : !e.dat.empty()   ? field(e.dat, kRecShort, 26)
                                        : e.name;
      e.nick = kv["nick"];
      e.base = uint32_t(std::stoul(kv.count("base") ? kv["base"] : "0"));
      if (kv.count("brand")) e.brand = std::stoi(kv["brand"]);
      if (kv.count("female")) e.female = std::stoi(kv["female"]);
      if (kv.count("id")) p.pinned = uint32_t(std::stoul(kv["id"]));
      e.group = kv["group"];
      if (e.group.empty() && g_folder_group.count(key)) e.group = g_folder_group[key];
      if (e.name.empty()) throw std::runtime_error("manifest.txt has no name= (and no dat=)");
      if (e.base >= kRosterCount) throw std::runtime_error("base= must be a roster id (0-92)");
      if (p.pinned && (p.pinned < kFirstExtra || p.pinned >= kFirstExtra + kMaxExtras ||
                       p.pinned == kNoId))
        throw std::runtime_error(fmt::format("id= must be {}-{}, not {}", kFirstExtra,
                                             kFirstExtra + kMaxExtras - 1, kNoId));
      for (const char* pic : {"body", "face", "bust"})
        for (const char* ext : {".png", ".dds"})
          if (fs::is_regular_file(folder / (std::string(pic) + ext), ec)) e.pictures = true;
      pending.push_back(std::move(p));
    } catch (const std::exception& ex) {
      REXLOG_WARN("Roster: skipping mods\\roster\\{}: {}", key, ex.what());
    }
  }

  // Pass 2: ids. id= first, then the folder's id from slots.txt (unless an id= took it),
  // then the first free one. slots.txt keeps them.
  std::map<uint32_t, std::string> owner;
  for (auto& p : pending) {
    if (!p.pinned) continue;
    if (owner.count(p.pinned)) {
      REXLOG_WARN("Roster: mods\\roster\\{}: id={} is also {}'s, left out", p.e.folder, p.pinned,
                  owner[p.pinned]);
      p.e.id = 0;
      p.pinned = uint32_t(-1);
      continue;
    }
    owner[p.pinned] = p.e.folder;
    p.e.id = p.pinned;
  }
  for (auto& p : pending) {
    if (p.pinned || !slots.count(p.e.folder)) continue;
    uint32_t id = slots[p.e.folder];
    if (id >= kFirstExtra && id < kFirstExtra + kMaxExtras && id != kNoId && !owner.count(id)) {
      owner[id] = p.e.folder;
      p.e.id = id;
    }
  }
  for (auto& p : pending) {
    if (p.e.id || p.pinned) continue;
    uint32_t id = kFirstExtra;
    // Keep ids slots.txt gives folders that aren't here right now.
    auto taken = [&](uint32_t i) {
      if (owner.count(i)) return true;
      for (auto& [f, sid] : slots)
        if (sid == i && f != p.e.folder) return true;
      return false;
    };
    while (id < kFirstExtra + kMaxExtras && (id == kNoId || taken(id))) ++id;
    if (id >= kFirstExtra + kMaxExtras) {
      REXLOG_WARN("Roster: skipping mods\\roster\\{}: no free id left (ids {}-{})", p.e.folder,
                  kFirstExtra, kFirstExtra + kMaxExtras - 1);
      continue;
    }
    owner[id] = p.e.folder;
    p.e.id = id;
  }

  // Pass 3: models. The merged ch92.pac is big (the WK pack alone is ~140 MB), so it is
  // only rebuilt when an input changed: a stamp lists the host pac and every extra's
  // id and ch.pac (size, time) as of the last build.
  const fs::path host = !mods.empty() && fs::exists(mods / host_rel, ec) ? mods / host_rel
                                                                         : game_root / host_rel;
  auto file_key = [&](const fs::path& f) {
    std::error_code ec2;
    return fmt::format("{}|{}|{}", f.string(), fs::file_size(f, ec2),
                       fs::last_write_time(f, ec2).time_since_epoch().count());
  };
  std::string stamp = "svr07-roster 1\n" + file_key(host) + "\n";
  for (auto& p : pending)
    if (p.e.id) stamp += fmt::format("{}|{}\n", p.e.id, file_key(p.e.dir / "ch.pac"));
  fs::path stamp_file = pac_out;
  stamp_file += ".stamp";
  bool cached = false;
  if (fs::exists(pac_out, ec) && fs::exists(stamp_file, ec)) {
    Bytes old = ReadFile(stamp_file);
    cached = std::string(old.begin(), old.end()) == stamp;
  }

  size_t extras_with_models = 0;
  for (auto& p : pending) {
    Extra& e = p.e;
    if (!e.id) continue;
    slots[e.folder] = e.id;
    try {
      size_t count = 0;
      if (!cached) {
        // Model: the folder's ch.pac (any superstar's), its entries renumbered to this id.
        auto entries = ReadModelPac(e.dir / "ch.pac", unused_header, unused_footer);
        count = entries.size();
        for (auto& m : entries) {
          uint32_t n = uint32_t(std::stoul(m.name));
          m.name = ExtraEntryName(e.id * 100 + n % 100);
          models.push_back(std::move(m));
        }
      }
      g_slot_of[e.id - kFirstExtra] = int(g_extras.size());
      g_extras.push_back(e);
      ++extras_with_models;
      REXLOG_INFO("Roster: {} = id {} (base {}, {}{}{}{}{})", e.name, e.id, e.base,
                  cached ? std::string("model cached") : fmt::format("{} model entries", count),
                  e.dat.empty() ? "" : ", dat", e.pro.empty() ? "" : ", pro",
                  e.gmd.empty() ? "" : ", gmd", e.pictures ? ", pictures" : "");
    } catch (const std::exception& ex) {
      REXLOG_WARN("Roster: skipping mods\\roster\\{}: {}", e.folder, ex.what());
    }
  }

  std::string text;
  for (auto& [f, id] : slots) text += f + "\t" + std::to_string(id) + "\r\n";
  WriteFileIfChanged(dir / "slots.txt", Bytes(text.begin(), text.end()));

  if (cached) {
    REXLOG_INFO("Roster: {} is up to date ({} extras)", host_rel.string(), extras_with_models);
    return;
  }
  fs::remove(stamp_file, ec);
  if (models.empty()) {
    fs::remove(pac_out, ec);
    return;
  }
  try {
    Bytes header, footer;
    auto merged = ReadModelPac(host, header, footer);
    merged.insert(merged.end(), std::make_move_iterator(models.begin()),
                  std::make_move_iterator(models.end()));
    WriteFileIfChanged(pac_out, WriteModelPac(merged, header, footer));
    // Only extras whose ch.pac made it in are in g_extras; a failed one changes the
    // stamp, so the next start tries again.
    std::string done = "svr07-roster 1\n" + file_key(host) + "\n";
    for (auto& e : g_extras) done += fmt::format("{}|{}\n", e.id, file_key(e.dir / "ch.pac"));
    WriteFileIfChanged(stamp_file, Bytes(done.begin(), done.end()));
  } catch (const std::exception& ex) {
    REXLOG_ERROR("Roster: couldn't write the extras' models into {}: {}", host_rel.string(),
                 ex.what());
    fs::remove(pac_out, ec);
  }
}

}  // namespace svr07::roster

// ---- hooks -------------------------------------------------------------------

using svr07::roster::IsExtra;
namespace R = svr07::roster;

// Generated: one wrapper per `cmp id,93` site.
#include "roster_sites.inc"

// ---- select list ----------------------------------------------------------------
//
// sub_822F8C08(block, ...) fills the select screen's list: 20-byte entries (id | flags << 16)
// from block+16, one loop per brand/mode, `for (id = 0; id < limit; ++id)` with limit 93
// or 123, and returns the count. Its callers clear a block of 2460 bytes in their object
// (123 entries) and hand it and the count to the select grid (sub_82301EF0 keeps them at
// +72 / +76). With the UE 2.5 roster plus 30 CAWs all 123 are used.
//
// So the list is built in a bigger block of ours (kSelectEntries), the first 123 entries
// are copied back for anything that reads the caller's block, and the grid gets ours.
// The loops (config/roster_manual.toml) carry on through the extra ids at their end;
// extras land after the CAWs, which the function's sort step leaves as they are.

namespace {
constexpr uint32_t kSelectEntries = 16 + R::kMaxExtras + 123;  // a little spare
constexpr uint32_t kSelectBlock = 16 + kSelectEntries * 20;
constexpr uint32_t kGameBlock = 2460;
std::mutex g_list_mutex;
std::unordered_map<uint32_t, uint32_t> g_lists;  // caller's block -> ours
}  // namespace

// Preview model size (config/roster_manual.toml): off = id*8 into a 93-entry table.
void RosterPreviewSize(PPCRegister& off, PPCRegister& id) {
  int slot = R::SlotOf(id.u32);
  if (slot < 0) return;
  off.u64 = R::g_extras[slot].base * 8;
}

void RosterSelectListLoop(PPCCRRegister& cr, PPCRegister& i, PPCRegister& limit,
                          PPCRegister& count) {
  if (i.s32 < limit.s32 || R::g_extras.empty()) return;
  uint32_t next = std::max<uint32_t>(i.u32, R::kFirstExtra);
  while (next < R::kFirstExtra + R::kMaxExtras && !IsExtra(next)) ++next;
  if (next >= R::kFirstExtra + R::kMaxExtras) return;
  if (count.u32 >= kSelectEntries) {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true))
      REXLOG_WARN("Roster: select list full ({} entries), extras from id {} left out", count.u32,
                  next);
    return;
  }
  i.u64 = next;
  cr.lt = 1;
  cr.gt = 0;
  cr.eq = 0;
}

// Select-screen sections (select_order.txt / group=): after the game's own sort (record
// +173, one byte, retail ids only), the list is sorted again by (section, name). Entries the
// sections don't cover (CAWs, templates) stay at the end in the game's order. Default section
// from the record: brand (+16) 2 = Legends, female (+146) = Divas, not normally playable
// (+160 != 1) = Managers / GMs, else WWE.
namespace {
std::string SectionOf(uint32_t id, uint8_t* base, uint32_t rec) {
  if (int slot = R::SlotOf(id); slot >= 0 && !R::g_extras[slot].group.empty())
    return R::g_extras[slot].group;
  if (auto it = R::g_id_group.find(id); it != R::g_id_group.end()) return it->second;
  if (base[rec + R::kRecBrand] == 2) return "Legends";
  if (base[rec + R::kRecFemale]) return "Divas";
  if (base[rec + R::kRecPlayable] != 1) return "Managers / GMs";
  return "WWE";
}

std::string Folded(const char* s, size_t n) {
  std::string out(s, strnlen(s, n));
  for (auto& c : out) c = char(std::tolower(uint8_t(c)));
  return out;
}

void SortSelectSections(PPCContext& ctx, uint8_t* base, uint32_t block, uint32_t count) {
  struct Item {
    uint32_t rank;
    std::string group, name;
    std::array<uint8_t, 20> raw;
  };
  std::vector<Item> items(count);
  const uint32_t unlisted = uint32_t(R::g_group_order.size());
  for (uint32_t i = 0; i < count; ++i) {
    Item& it = items[i];
    const uint32_t entry = block + 16 + i * 20;
    std::memcpy(it.raw.data(), base + entry, 20);
    const uint32_t id = REX_LOAD_U16(entry + 2);
    if (id >= R::kRosterCount && !IsExtra(id)) {
      it.rank = 0xFFFFFFFF;  // CAWs and the rest: last, game's order
      continue;
    }
    uint32_t r;
    if (const int slot = R::SlotOf(id); slot >= 0) {
      R::EnsureReady(ctx, base, slot);
      r = R::RecordAddr(slot);
    } else {
      r = R::Call(ctx, base, __imp__sub_821B9A70, id, uint32_t(-1));
    }
    it.group = SectionOf(id, base, r);
    it.name = Folded(reinterpret_cast<const char*>(base + r + R::kRecName), 46);
    auto pos = std::find(R::g_group_order.begin(), R::g_group_order.end(), it.group);
    it.rank = pos == R::g_group_order.end() ? unlisted : uint32_t(pos - R::g_group_order.begin());
  }
  std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
    if (a.rank != b.rank) return a.rank < b.rank;
    if (a.rank == 0xFFFFFFFF) return false;
    if (a.group != b.group) return a.group < b.group;  // sections not in "order", by name
    return a.name < b.name;
  });
  for (uint32_t i = 0; i < count; ++i) std::memcpy(base + block + 16 + i * 20, items[i].raw.data(), 20);
}
}  // namespace

REX_HOOK_RAW(sub_822F8C08) {
  const uint32_t game_block = ctx.r3.u32;
  if (R::g_extras.empty()) {
    __imp__sub_822F8C08(ctx, base);
    if (R::g_sections) {
      const uint32_t count = std::min<uint32_t>(ctx.r3.u32, 123);
      PPCContext keep = ctx;
      SortSelectSections(ctx, base, game_block, count);
      ctx = keep;
    }
    return;
  }
  uint32_t ours;
  {
    std::lock_guard lock(g_list_mutex);
    uint32_t& slot = g_lists[game_block];
    if (!slot) slot = REX_KERNEL_MEMORY()->SystemHeapAlloc(kSelectBlock);
    ours = slot;
  }
  std::memset(base + ours, 0, kSelectBlock);
  std::memcpy(base + ours, base + game_block, 16);
  ctx.r3.u64 = ours;
  __imp__sub_822F8C08(ctx, base);
  const uint32_t count = ctx.r3.u32;
  if (R::g_sections) {
    PPCContext keep = ctx;
    SortSelectSections(ctx, base, ours, count);
    ctx = keep;
  }
  std::memcpy(base + game_block, base + ours, std::min<uint32_t>(kGameBlock, 16 + count * 20));
  static uint32_t logged = 0;
  if (count > 123 && count != logged) {
    logged = count;
    REXLOG_INFO("Roster: select list has {} entries (game's block holds 123)", count);
  }
}

// The select grid: sub_82301EF0(grid, list block, count, ...) - give it our block.
REX_HOOK_RAW(sub_82301EF0) {
  {
    std::lock_guard lock(g_list_mutex);
    auto it = g_lists.find(ctx.r4.u32);
    if (it != g_lists.end()) ctx.r4.u64 = it->second;
  }
  __imp__sub_82301EF0(ctx, base);
}

// Record getter: sub_821B9A70(id, set) -> 178-byte record.
REX_HOOK_RAW(sub_821B9A70) {
  int slot = R::SlotOf(ctx.r3.u32);
  if (slot < 0) return __imp__sub_821B9A70(ctx, base);
  R::EnsureReady(ctx, base, slot);
  ctx.r3.u64 = R::RecordAddr(slot);
}

// Profile getter: sub_821B9AF0(id, ?, set) -> 412-byte moves profile.
REX_HOOK_RAW(sub_821B9AF0) {
  int slot = R::SlotOf(ctx.r3.u32);
  if (slot < 0) return __imp__sub_821B9AF0(ctx, base);
  R::EnsureReady(ctx, base, slot);
  ctx.r3.u64 = R::ProfileAddr(slot, R::ProfileSet(base, ctx.r4.s32));
}

// GM-mode record getter: sub_8234BDA8(game data, id) -> record + 58.
REX_HOOK_RAW(sub_8234BDA8) {
  int slot = R::SlotOf(ctx.r4.u32);
  if (slot < 0) return __imp__sub_8234BDA8(ctx, base);
  R::EnsureReady(ctx, base, slot);
  ctx.r3.u64 = R::GmAddr(slot) + 58;
}

// Select list: sub_82536E20(out, raw, smackdown, other, r7..r9 class filters, with_caws)
// returns how many ids it wrote. Extras go after the game's, by the same filters.
REX_HOOK_RAW(sub_82536E20) {
  const uint32_t out = ctx.r3.u32;
  const uint32_t want_brand[3] = {ctx.r4.u32, ctx.r5.u32, ctx.r6.u32};
  const uint32_t r7 = ctx.r7.u32, r8 = ctx.r8.u32, r9 = ctx.r9.u32;
  __imp__sub_82536E20(ctx, base);
  uint32_t count = ctx.r3.u32;
  static bool warned = false;
  for (auto& e : R::g_extras) {
    int slot = R::SlotOf(e.id);
    R::EnsureReady(ctx, base, slot);
    uint32_t rec = R::RecordAddr(slot);
    if (base[rec + R::kRecPlayable] != 1) continue;
    uint8_t brand = base[rec + R::kRecBrand];
    if (brand < 3 && !want_brand[brand]) continue;
    if (base[rec + R::kRecFemale]) {
      if (!r9) continue;
    } else if (R::Call(ctx, base, sub_82536190, e.id) == 1 ? !r8 : !r7) {
      continue;
    }
    if (count >= R::kSelectListMax) {
      if (!warned) REXLOG_WARN("Roster: select list full ({}), extras left out", count);
      warned = true;
      break;
    }
    REX_STORE_U32(out + 4 * count, e.id);
    ++count;
  }
  ctx.r3.u64 = count;
}

// Resource lookup by path: sub_8241C370("/GROUP/NAME", out) -> nonzero when found. With
// roster_trace_sites on, logs misses and every model lookup.
REX_HOOK_RAW(sub_8241C370) {
  const uint32_t path = ctx.r3.u32;
  __imp__sub_8241C370(ctx, base);
  if (!REXCVAR_GET(roster_trace_sites) || !path) return;
  const char* p = reinterpret_cast<const char*>(base + path);
  std::string s(p, strnlen(p, 64));
  if (ctx.r3.u32 == 0 || s.rfind("/EMD/", 0) == 0) {
    // Guest call chain: each frame's back chain at [sp], its caller's LR at [caller sp - 8].
    std::string chain = fmt::format("{:08X}", uint32_t(ctx.lr));
    uint32_t sp = ctx.r1.u32;
    for (int k = 0; k < 10 && sp; ++k) {
      uint32_t next = REX_LOAD_U32(sp);
      // The outermost frame chains to the stack's top (64 KB aligned), which is unmapped.
      if (next <= sp || next - sp > 0x10000 || (next & 0xFFFF) == 0) break;
      chain += fmt::format(" < {:08X}", REX_LOAD_U32(next - 8));
      sp = next;
    }
    REXLOG_INFO("Roster trace: lookup {} -> {} (from {})", s, ctx.r3.u32, chain);
  }
}

// Model entry names: sprintf("/EMD/%04d", n) for an extra's model gives "/EMD/Xnnn".
REX_HOOK_RAW(sub_8255F9F0) {
  if (ctx.r4.u32 == R::kEmdFormat && ctx.r5.u32 >= R::kFirstExtraModel &&
      ctx.r5.u32 < (R::kFirstExtra + R::kMaxExtras) * 100) {
    std::string s = "/EMD/" + R::ExtraEntryName(ctx.r5.u32);
    std::memcpy(base + ctx.r3.u32, s.c_str(), s.size() + 1);
    ctx.r3.u64 = s.size();
    return;
  }
  __imp__sub_8255F9F0(ctx, base);
}

// Model exists?: sub_820D4858(?, n, set) -> nonzero when "/EMD/%04d" n is there. Numbers
// of 10000 and up get a CAW-part test, then `n / 100 >= 100` returns 0 before the lookup,
// so an extra's (its own n - 10000 test is hooked) was always missing. Its callers then
// draw someone else: the match-intro / preview pair (sub_82538BB8) id 55, sub_82709A50
// model 3902 (Cena). For extras, look the entry up like the game does for roster numbers.
REX_HOOK_RAW(sub_820D4858) {
  const uint32_t n = ctx.r4.u32 & 0xFFFF;
  if (!R::IsExtraModel(n)) return __imp__sub_820D4858(ctx, base);
  PPCContext saved = ctx;
  ctx.r1.u64 = ctx.r1.u32 - 112;
  const uint32_t buf = ctx.r1.u32 + 80;
  const std::string s = "/EMD/" + R::ExtraEntryName(n);
  std::memcpy(base + buf, s.c_str(), s.size() + 1);
  ctx.r3.u64 = buf;
  ctx.r4.u64 = 0;
  sub_8241C660(ctx, base);
  const uint32_t found = ctx.r3.u32;
  ctx = saved;
  ctx.r3.u64 = found;
  if (REXCVAR_GET(roster_trace_sites))
    REXLOG_INFO("Roster trace: model {} exists? {} -> {}", n, s, found);
}

// Free physical memory (the 0-512 MB pool textures, movies and audio buffers come from),
// logged at the points that load big things, to see what runs it dry.
namespace {
void LogPhysicalFree(const std::string& what) {
  auto* heap = REX_KERNEL_MEMORY()->GetPhysicalHeap();
  if (!heap) return;
  const uint64_t page = heap->page_size();
  REXLOG_INFO("Roster mem: {}: physical free {} MB of {} MB", what,
              heap->GetUnreservedPageCount() * page >> 20, heap->GetTotalPageCount() * page >> 20);
}
}  // namespace

// ---- titantron movies -----------------------------------------------------------
//
// sub_821CB198(movie id) -> path in a static buffer: looks the id up in a 100-entry
// {id, "NN.bik"} table, formats "%s%s%s" (GAME:\, MOVIES\titantron\, name) and checks
// the file (sub_82421850), else formats "%s%sdummy.bik". An id the table doesn't list
// (the name pointer stays 0) or one whose file is missing gets <id as %02d>.bik when
// that file is there (mods overlay or game), so ported entrances can use new movie ids.

namespace {
constexpr uint32_t kTitantronTable = 0x82015768;  // 100 x {u32 id, u32 name}
constexpr uint32_t kTitantronCount = 100;
constexpr uint32_t kTitantronPathFmt = 0x82015A98;   // "%s%s%s"
constexpr uint32_t kTitantronDummyFmt = 0x82015A88;  // "%s%sdummy.bik"
constexpr uint32_t kTitantronRoot = 0x82015AA0;      // r5
constexpr uint32_t kTitantronDir = 0x82015AA8;       // r6
constexpr uint32_t kTitantronPath = 0x829414F0;      // the result buffer
}  // namespace

REX_HOOK_RAW(sub_821CB198) {
  const int32_t id = ctx.r3.s32;
  LogPhysicalFree(fmt::format("titantron {}", id));
  bool listed = false;
  for (uint32_t i = 0; i < kTitantronCount; ++i)
    if (int32_t(REX_LOAD_U32(kTitantronTable + 8 * i)) == id) listed = true;
  if (listed) {
    __imp__sub_821CB198(ctx, base);
    const char* got = reinterpret_cast<const char*>(base + ctx.r3.u32);
    if (!std::strstr(got, "dummy.bik")) return;
  }
  if (id < 0) {
    if (!listed) __imp__sub_821CB198(ctx, base);
    return;
  }
  // Same formatting and file test as the game, with the name "<id>.bik" in guest stack.
  PPCContext saved = ctx;
  ctx.r1.u64 = ctx.r1.u32 - 128;
  const uint32_t name = ctx.r1.u32 + 80;
  const std::string n = fmt::format("{:02d}.bik", id);
  std::memcpy(base + name, n.c_str(), n.size() + 1);
  ctx.r3.u64 = kTitantronPath;
  ctx.r4.u64 = kTitantronPathFmt;
  ctx.r5.u64 = kTitantronRoot;
  ctx.r6.u64 = kTitantronDir;
  ctx.r7.u64 = name;
  sub_8255F9F0(ctx, base);
  const uint32_t exists = R::Call(ctx, base, sub_82421850, kTitantronPath, 0, 0);
  ctx = saved;
  if (exists) {
    static std::mutex m;
    static std::set<int32_t> seen;
    std::lock_guard lock(m);
    if (seen.insert(id).second)
      REXLOG_INFO("Roster: titantron {} -> {}", id,
                  reinterpret_cast<const char*>(base + kTitantronPath));
    ctx.r3.u64 = kTitantronPath;
    return;
  }
  // Not there either: the game's own fallback, "%s%sdummy.bik".
  R::Call(ctx, base, sub_8255F9F0, kTitantronPath, kTitantronDummyFmt, kTitantronRoot,
          kTitantronDir);
  ctx.r3.u64 = kTitantronPath;
}

// ---- select pictures ----------------------------------------------------------
//
// A picture bank (menuHD/SD MENU/SF2A face, SF2B body, SF2D bust) holds a 108-entry
// table at bank+40: ids 0-92, then 200-214. sub_82466220 builds it, one 40-byte texture
// object per PACH child (sub_82708BF0); sub_82465DC8(out, bank, id, kind) copies the
// texture handle (entry+32, 8 bytes), or the silhouette (bank+412) when missing. Extras'
// pictures are PACH children with their id (mods\portraits\<id>, merged at startup);
// their texture objects are kept here per bank.
//
// The game frees the textures in a bank's own table with the bank, but not ours: making
// new ones at every build leaked ~10 MB of physical memory per select-screen visit (166
// extras) until a match load ran out (Bink/loader null allocs). So each picture's texture
// object is made once (own system-heap object, keyed by id + the DDS bytes + the bank's
// texture flags) and reused by every later build.

namespace {
std::mutex g_pic_mutex;
std::unordered_map<uint32_t, std::array<uint32_t, R::kMaxExtras>> g_pictures;  // bank -> objects
struct PictureKey {
  uint32_t id, size, flags;
  uint64_t hash;
  bool operator==(const PictureKey& o) const {
    return id == o.id && size == o.size && flags == o.flags && hash == o.hash;
  }
};
struct PictureKeyHash {
  size_t operator()(const PictureKey& k) const {
    return size_t(k.hash ^ (uint64_t(k.id) << 32) ^ k.size ^ (uint64_t(k.flags) << 16));
  }
};
std::unordered_map<PictureKey, uint32_t, PictureKeyHash> g_picture_objects;  // -> object

uint64_t Fnv1a(const uint8_t* p, uint32_t n) {
  uint64_t h = 1469598103934665603ull;
  for (uint32_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
  return h;
}
}  // namespace

// sub_82466220 ends with sub_82465CE8(bank) (the CAW picture cache), then frees the
// bank's PACH reader (+444). Only it calls sub_82465CE8, so the extras' pictures are made
// there, while the PACH is still open and the builder's heap is current.
REX_HOOK_RAW(sub_82465CE8) {
  const uint32_t bank = ctx.r3.u32;
  std::array<uint32_t, R::kMaxExtras> objs{};
  const uint32_t pach = REX_LOAD_U32(bank + 444);
  int made = 0, reused = 0;
  uint32_t bytes = 0;
  const uint32_t flags = REX_LOAD_U16(bank + 468);
  LogPhysicalFree(fmt::format("picture bank {:08X} before extras", bank));
  if (REXCVAR_GET(roster_trace_sites))
    REXLOG_INFO("Roster trace: picture bank {:08X} built, PACH {:08X}", bank, pach);
  for (auto& e : R::g_extras) {
    if (!pach) break;
    uint32_t idx = R::Call(ctx, base, sub_8241B428, pach, e.id);
    if (idx == 0xFFFFFFFF) {
      if (REXCVAR_GET(roster_trace_sites))
        REXLOG_INFO("Roster trace: bank {:08X} has no picture {}", bank, e.id);
      continue;
    }
    uint32_t data = R::Call(ctx, base, sub_8241B700, pach, idx);
    uint32_t size = R::Call(ctx, base, sub_8241B7C8, pach, idx);
    if (!data) continue;
    const PictureKey key{e.id, size, flags, Fnv1a(base + data, size)};
    uint32_t obj = 0;
    {
      std::lock_guard lock(g_pic_mutex);
      auto it = g_picture_objects.find(key);
      if (it != g_picture_objects.end()) obj = it->second;
    }
    if (obj) {
      objs[e.id - R::kFirstExtra] = obj;
      ++reused;
      continue;
    }
    obj = REX_KERNEL_MEMORY()->SystemHeapAlloc(40, 16);
    if (REXCVAR_GET(roster_trace_sites))
      REXLOG_INFO("Roster trace: bank {:08X} id {}: child {} data {:08X} size {} obj {:08X}", bank,
                  e.id, idx, data, size, obj);
    if (!obj) continue;
    std::memset(base + obj, 0, 40);
    PPCContext saved = ctx;
    ctx.r3.u64 = obj;
    ctx.r4.u64 = data;
    ctx.r5.u64 = size;
    ctx.r6.u64 = flags;
    sub_82708BF0(ctx, base);
    ctx = saved;
    {
      std::lock_guard lock(g_pic_mutex);
      g_picture_objects[key] = obj;
    }
    objs[e.id - R::kFirstExtra] = obj;
    ++made;
    bytes += size;
  }
  {
    std::lock_guard lock(g_pic_mutex);
    g_pictures[bank] = objs;
  }
  if (made || reused) {
    REXLOG_INFO("Roster: picture bank {:08X}: {} extra pictures made ({} KB of DDS), {} reused",
                bank, made, bytes >> 10, reused);
    LogPhysicalFree(fmt::format("picture bank {:08X} after extras", bank));
  }
  ctx.r3.u64 = bank;
  __imp__sub_82465CE8(ctx, base);
}

REX_HOOK_RAW(sub_82465DC8) {
  const uint32_t out = ctx.r3.u32, bank = ctx.r4.u32, id = ctx.r5.u32, kind = ctx.r6.u32;
  if (!IsExtra(id) || (kind >= 5 && kind <= 7)) return __imp__sub_82465DC8(ctx, base);
  uint32_t entry = 0;
  {
    std::lock_guard lock(g_pic_mutex);
    auto it = g_pictures.find(bank);
    if (it != g_pictures.end()) entry = it->second[id - R::kFirstExtra];
  }
  if (!entry) entry = REX_LOAD_U32(bank + 412);  // silhouette
  REX_STORE_U32(out, REX_LOAD_U32(entry + 32));
  REX_STORE_U32(out + 4, REX_LOAD_U32(entry + 36));
  ctx.r3.u64 = out;
}
