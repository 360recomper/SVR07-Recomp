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

struct Extra {
  uint32_t id = 0;
  std::string folder, name, nick, short_name;
  uint32_t base = 0;  // roster superstar it starts from (stats, moves, entrance)
  int brand = -1, female = -1;
};

std::vector<Extra> g_extras;  // filled at startup, read-only afterwards
std::array<int, kMaxExtras> g_slot_of{};  // id - kFirstExtra -> index in g_extras, or -1

std::mutex g_mutex;  // guards the storage setup below
uint32_t g_records = 0, g_profiles = 0, g_gm = 0;  // guest addresses
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
  return g_profiles + (set * kMaxExtras + slot) * kProfileSize;
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
  if (!g_records) {
    auto* mem = REX_KERNEL_MEMORY();
    g_records = mem->SystemHeapAlloc(kMaxExtras * kRecordSize);
    g_profiles = mem->SystemHeapAlloc(kProfileSets * kMaxExtras * kProfileSize);
    g_gm = mem->SystemHeapAlloc(kMaxExtras * kGmSize);
    std::memset(base + g_records, 0, kMaxExtras * kRecordSize);
    std::memset(base + g_profiles, 0, kProfileSets * kMaxExtras * kProfileSize);
    std::memset(base + g_gm, 0, kMaxExtras * kGmSize);
    REXLOG_INFO("Roster: extra storage at {:08X} (records) {:08X} (profiles) {:08X} (GM)",
                g_records, g_profiles, g_gm);
  }
  const Extra& e = g_extras[slot];
  // Record: the base's, renamed, with its own id and model number.
  uint32_t rec = RecordAddr(slot);
  Copy(base, rec, Call(ctx, base, __imp__sub_821B9A70, e.base, uint32_t(-1)), kRecordSize);
  PutString(base, rec + kRecName, e.name, 46);
  PutString(base, rec + kRecNick, e.nick.empty() ? e.short_name : e.nick, 46);
  PutString(base, rec + kRecShort, e.short_name, 26);
  REX_STORE_U16(rec + kRecModel, uint16_t(e.id));
  base[rec + kRecId] = uint8_t(e.id);
  for (uint32_t a = 0; a < 4; ++a) REX_STORE_U16(rec + kRecAttires + 2 * a, uint16_t(e.id * 10 + 1 + a));
  base[rec + kRecPlayable] = 1;
  if (e.brand >= 0) base[rec + kRecBrand] = uint8_t(e.brand);
  if (e.female >= 0) base[rec + kRecFemale] = uint8_t(e.female);
  // Profiles: the base's in every set.
  for (uint32_t s = 0; s < kProfileSets; ++s) {
    uint32_t src = Call(ctx, base, __imp__sub_821B9AF0, e.base, uint32_t(-1), s);
    Copy(base, ProfileAddr(slot, s), src, kProfileSize);
  }
  // GM-mode record: the base's, own id, not in the draft pool.
  uint32_t gm = GmAddr(slot);
  Copy(base, gm, Call(ctx, base, sub_821B8E30) + 29648 + e.base * kGmSize, kGmSize);
  base[gm + 0] = uint8_t(e.id);
  base[gm + 1] = 0;
  g_ready[slot] = true;
  REXLOG_INFO("Roster: {} ready as id {} (base {})", e.name, e.id, e.base);
}

}  // namespace

bool IsExtra(uint32_t id) { return SlotOf(id) >= 0; }

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
  std::set<uint32_t> used;
  for (auto& [f, id] : slots) used.insert(id);
  std::vector<PacEntry> models;
  Bytes unused_header, unused_footer;
  std::vector<fs::path> folders;
  for (auto& it : fs::directory_iterator(dir, ec))
    if (it.is_directory() && !fs::exists(it.path() / "disabled", ec)) folders.push_back(it.path());
  std::sort(folders.begin(), folders.end());

  for (auto& folder : folders) {
    std::string key = folder.filename().string();
    try {
      auto kv = ReadManifest(folder / "manifest.txt");
      Extra e;
      e.folder = key;
      e.name = kv["name"];
      e.short_name = kv.count("short") ? kv["short"] : e.name;
      e.nick = kv["nick"];
      e.base = uint32_t(std::stoul(kv.count("base") ? kv["base"] : "0"));
      if (kv.count("brand")) e.brand = std::stoi(kv["brand"]);
      if (kv.count("female")) e.female = std::stoi(kv["female"]);
      if (e.name.empty()) throw std::runtime_error("manifest.txt has no name=");
      if (e.base >= kRosterCount) throw std::runtime_error("base= must be a roster id (0-92)");
      if (!slots.count(key)) {
        uint32_t id = kFirstExtra;
        while (used.count(id)) ++id;
        slots[key] = id;
        used.insert(id);
      }
      e.id = slots[key];
      if (e.id < kFirstExtra || e.id >= kFirstExtra + kMaxExtras)
        throw std::runtime_error("no free id left (ids 167-198)");
      // Model: the folder's ch.pac (any superstar's), its entries renumbered to this id.
      auto entries = ReadModelPac(folder / "ch.pac", unused_header, unused_footer);
      for (auto& m : entries) {
        uint32_t n = uint32_t(std::stoul(m.name));
        m.name = ExtraEntryName(e.id * 100 + n % 100);
        models.push_back(std::move(m));
      }
      g_slot_of[e.id - kFirstExtra] = int(g_extras.size());
      g_extras.push_back(e);
      REXLOG_INFO("Roster: {} = id {} (base {}, {} model entries)", e.name, e.id, e.base,
                  entries.size());
    } catch (const std::exception& ex) {
      REXLOG_WARN("Roster: skipping mods\\roster\\{}: {}", key, ex.what());
    }
  }

  std::string text;
  for (auto& [f, id] : slots) text += f + "\t" + std::to_string(id) + "\r\n";
  WriteFileIfChanged(dir / "slots.txt", Bytes(text.begin(), text.end()));

  if (models.empty()) {
    fs::remove(pac_out, ec);
    return;
  }
  try {
    fs::path host = !mods.empty() && fs::exists(mods / host_rel, ec) ? mods / host_rel
                                                                     : game_root / host_rel;
    Bytes header, footer;
    auto merged = ReadModelPac(host, header, footer);
    merged.insert(merged.end(), std::make_move_iterator(models.begin()),
                  std::make_move_iterator(models.end()));
    WriteFileIfChanged(pac_out, WriteModelPac(merged, header, footer));
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

REX_HOOK_RAW(sub_822F8C08) {
  const uint32_t game_block = ctx.r3.u32;
  if (R::g_extras.empty()) return __imp__sub_822F8C08(ctx, base);
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

// ---- select pictures ----------------------------------------------------------
//
// A picture bank (menuHD/SD MENU/SF2A face, SF2B body, SF2D bust) holds a 108-entry
// table at bank+40: ids 0-92, then 200-214. sub_82466220 builds it, one 40-byte texture
// object per PACH child (sub_82708BF0); sub_82465DC8(out, bank, id, kind) copies the
// texture handle (entry+32, 8 bytes), or the silhouette (bank+412) when missing. Extras'
// pictures are PACH children with their id (mods\portraits\<id>, merged at startup);
// their texture objects are kept here per bank.

namespace {
std::mutex g_pic_mutex;
std::unordered_map<uint32_t, std::array<uint32_t, R::kMaxExtras>> g_pictures;  // bank -> objects
}  // namespace

// sub_82466220 ends with sub_82465CE8(bank) (the CAW picture cache), then frees the
// bank's PACH reader (+444). Only it calls sub_82465CE8, so the extras' pictures are made
// there, while the PACH is still open and the builder's heap is current.
REX_HOOK_RAW(sub_82465CE8) {
  const uint32_t bank = ctx.r3.u32;
  std::array<uint32_t, R::kMaxExtras> objs{};
  const uint32_t pach = REX_LOAD_U32(bank + 444);
  int made = 0;
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
    uint32_t obj = R::Call(ctx, base, sub_82414CE0)
                       ? R::Call(ctx, base, sub_82414850, 40, 1)
                       : R::Call(ctx, base, sub_82415658, 3, 1);
    if (REXCVAR_GET(roster_trace_sites))
      REXLOG_INFO("Roster trace: bank {:08X} id {}: child {} data {:08X} size {} obj {:08X}", bank,
                  e.id, idx, data, size, obj);
    if (!obj) continue;
    PPCContext saved = ctx;
    ctx.r3.u64 = obj;
    ctx.r4.u64 = data;
    ctx.r5.u64 = size;
    ctx.r6.u64 = REX_LOAD_U16(bank + 468);
    sub_82708BF0(ctx, base);
    ctx = saved;
    objs[e.id - R::kFirstExtra] = obj;
    ++made;
  }
  {
    std::lock_guard lock(g_pic_mutex);
    g_pictures[bank] = objs;
  }
  if (made) REXLOG_INFO("Roster: picture bank {:08X}: {} extra pictures", bank, made);
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
