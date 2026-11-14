// svr07 - attire picks: which look fills a superstar's attire 1-4.
//
// A superstar offers attire N when its model entry (id*10 + N-1)*10 + 2 exists (ring
// model; + 4 = entrance model, optional): the count is stored nowhere and the record
// always lists 4 attires (FINDINGS-attires.md). So a pick is a model entry copied into the
// pac that holds that superstar's models, in a copy laid over the game's.
//
//   attire_picks = "183:2=lesnar_hctp, 6:3=jericho68"
//
// <id>:<attire>=<look>, look = mods\attire\<look>\ch.pac, any superstar's model pac: its
// lowest attire that has a ring model is used (with that attire's entrance model, if any).
// A pick replaces the attire if the superstar has it, or adds it. The launcher's Attire tab
// writes the cvar; front.jpg / back.jpg next to ch.pac are its previews.

#include "svr07_attires.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <sstream>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "svr07_roster.h"

REXCVAR_DEFINE_STRING(attire_picks, "", "Game",
                      "Attire picks: \"<id>:<attire 1-4>=<look>, ...\", look = mods\\attire\\<look>\\ch.pac");

namespace svr07::attires {
namespace {

namespace fs = std::filesystem;
using modelpac::Bytes;
using modelpac::PacEntry;

struct Pick {
  uint32_t id = 0;
  int attire = 0;  // 1-4
  std::string look;
  fs::path pac;
  Bytes ring, entrance;  // entrance empty = the look has none
};

bool g_loaded = false;
std::vector<Pick> g_picks;

std::string Trim(std::string s) {
  while (!s.empty() && std::isspace(uint8_t(s.back()))) s.pop_back();
  size_t i = 0;
  while (i < s.size() && std::isspace(uint8_t(s[i]))) ++i;
  return s.substr(i);
}

std::string FileKey(const fs::path& f) {
  std::error_code ec;
  return fmt::format("{}|{}|{}", f.string(), fs::file_size(f, ec),
                     fs::last_write_time(f, ec).time_since_epoch().count());
}

bool ValidId(uint32_t id) {
  return id < roster::kRosterCount ||
         (id >= roster::kFirstExtra && id < roster::kFirstExtra + roster::kMaxExtras &&
          id != roster::kNoId);
}

// The look's ring/entrance model: its lowest attire with a kind-2 entry.
void LoadLook(Pick& p) {
  Bytes header, footer;
  auto entries = modelpac::ReadModelPac(p.pac, header, footer);
  std::map<int, std::map<int, const PacEntry*>> by_attire;  // attire digit -> kind -> entry
  for (auto& e : entries) {
    if (e.name.size() != 4 || !std::all_of(e.name.begin(), e.name.end(), ::isdigit)) continue;
    uint32_t n = uint32_t(std::stoul(e.name));
    by_attire[(n / 10) % 10][n % 10] = &e;
  }
  for (auto& [attire, kinds] : by_attire) {
    if (!kinds.count(2)) continue;
    p.ring = kinds[2]->data;
    if (kinds.count(4)) p.entrance = kinds[4]->data;
    return;
  }
  throw std::runtime_error("ch.pac has no ring model (an entry ending in 2)");
}

}  // namespace

void LoadPicks(const fs::path& mods) {
  if (g_loaded) return;
  g_loaded = true;
  std::string text = REXCVAR_GET(attire_picks);
  std::replace(text.begin(), text.end(), ';', ',');  // ';' separates too
  std::map<std::pair<uint32_t, int>, size_t> at;  // (id, attire) -> index in g_picks
  std::istringstream in(text);
  for (std::string one; std::getline(in, one, ',');) {
    one = Trim(one);
    if (one.empty()) continue;
    {
      try {
        size_t colon = one.find(':'), eq = one.find('=');
        if (colon == std::string::npos || eq == std::string::npos || eq < colon)
          throw std::runtime_error("not <id>:<attire>=<look>");
        Pick p;
        p.id = uint32_t(std::stoul(Trim(one.substr(0, colon))));
        p.attire = std::stoi(Trim(one.substr(colon + 1, eq - colon - 1)));
        p.look = Trim(one.substr(eq + 1));
        if (!ValidId(p.id))
          throw std::runtime_error(fmt::format("id {} is not a roster id (0-92) or an extra's ({}-{})",
                                               p.id, roster::kFirstExtra,
                                               roster::kFirstExtra + roster::kMaxExtras - 1));
        if (p.attire < 1 || p.attire > 4) throw std::runtime_error("attire must be 1-4");
        if (p.look.empty() || !std::all_of(p.look.begin(), p.look.end(), [](unsigned char c) {
              return std::isalnum(c) || c == '_' || c == '-';
            }))
          throw std::runtime_error("look must be a folder name (letters, digits, _ -)");
        if (mods.empty()) throw std::runtime_error("no mods folder");
        p.pac = mods / "attire" / p.look / "ch.pac";
        std::error_code ec;
        if (!fs::is_regular_file(p.pac, ec))
          throw std::runtime_error("mods\\attire\\" + p.look + "\\ch.pac not found");
        LoadLook(p);
        REXLOG_INFO("Attires: {} attire {} = {} (ring model{})", p.id, p.attire, p.look,
                    p.entrance.empty() ? "" : " + entrance model");
        auto key = std::make_pair(p.id, p.attire);
        if (at.count(key)) {
          REXLOG_WARN("Attires: {} attire {} picked twice, {} wins", p.id, p.attire, p.look);
          g_picks[at[key]] = std::move(p);
        } else {
          at[key] = g_picks.size();
          g_picks.push_back(std::move(p));
        }
      } catch (const std::exception& e) {
        REXLOG_WARN("Attires: skipping pick \"{}\": {}", one, e.what());
      }
    }
  }
}

std::string Stamp(const std::function<bool(uint32_t)>& want) {
  std::string s;
  for (auto& p : g_picks)
    if (want(p.id)) s += fmt::format("{}:{}={}|{}\n", p.id, p.attire, p.look, FileKey(p.pac));
  return s;
}

int Apply(std::vector<PacEntry>& entries, uint32_t id,
          const std::function<std::string(uint32_t)>& name) {
  auto put = [&](const std::string& n, const Bytes* data) {
    auto it = std::find_if(entries.begin(), entries.end(),
                           [&](const PacEntry& e) { return e.name == n; });
    if (!data) {
      if (it != entries.end()) entries.erase(it);
    } else if (it != entries.end()) {
      it->data = *data;
    } else {
      entries.push_back({n, *data});
    }
  };
  int applied = 0;
  for (auto& p : g_picks) {
    if (p.id != id) continue;
    const uint32_t n = (id * 10 + uint32_t(p.attire - 1)) * 10;
    put(name(n + 2), &p.ring);
    put(name(n + 4), p.entrance.empty() ? nullptr : &p.entrance);
    ++applied;
  }
  return applied;
}

void BuildAttirePacs(const fs::path& game_root, const fs::path& mods, const fs::path& out_root) {
  const fs::path ch_dir = out_root / "pac" / "ch";
  std::error_code ec;
  // ch92.pac is the roster merge's when it wrote it (its stamp is next to it).
  auto roster_owns = [&](const fs::path& pac) {
    fs::path stamp = pac;
    stamp += ".stamp";
    return pac.filename() == "ch92.pac" && fs::exists(pac, ec) && fs::exists(stamp, ec);
  };

  std::set<uint32_t> ids;
  for (auto& p : g_picks) {
    if (p.id < roster::kRosterCount) {
      ids.insert(p.id);
    } else if (!roster_owns(ch_dir / "ch92.pac")) {
      REXLOG_WARN("Attires: {} attire {} not applied: no extras' model pac (mods\\roster)", p.id,
                  p.attire);
    }
  }

  std::set<fs::path> made;
  for (uint32_t id : ids) {
    const fs::path rel = fs::path("pac") / "ch" / fmt::format("ch{:02d}.pac", id);
    const fs::path out = out_root / rel;
    if (roster_owns(out)) continue;  // BuildRosterFiles applied them
    fs::path src = mods.empty() ? fs::path() : mods / rel;
    if (src.empty() || !fs::is_regular_file(src, ec)) src = game_root / rel;
    if (!fs::is_regular_file(src, ec)) {
      REXLOG_WARN("Attires: picks for {} not applied: no {} (blank slot)", id, rel.string());
      continue;
    }
    fs::path stamp_file = out;
    stamp_file += ".attires";
    const std::string stamp = "svr07-attires 1\n" + FileKey(src) + "\n" +
                              Stamp([&](uint32_t i) { return i == id; });
    made.insert(out);
    if (fs::exists(out, ec) && fs::exists(stamp_file, ec)) {
      std::ifstream f(stamp_file, std::ios::binary);
      std::string old(std::istreambuf_iterator<char>(f), {});
      if (old == stamp) {
        REXLOG_INFO("Attires: {} is up to date", rel.string());
        continue;
      }
    }
    try {
      Bytes header, footer;
      uint32_t group_id = modelpac::kDefaultGroupId;
      auto entries = modelpac::ReadModelPac(src, header, footer, &group_id);
      int n = Apply(entries, id, [](uint32_t m) { return fmt::format("{:04d}", m); });
      Bytes pac = modelpac::WriteModelPac(entries, header, footer, group_id);
      fs::create_directories(out.parent_path(), ec);
      fs::path tmp = out;
      tmp += ".tmp";
      {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(pac.data()), std::streamsize(pac.size()));
        if (!f.good()) throw std::runtime_error("can't write " + tmp.string());
      }
      fs::rename(tmp, out);
      std::ofstream(stamp_file, std::ios::binary | std::ios::trunc) << stamp;
      REXLOG_INFO("Attires: wrote {} ({} picks)", rel.string(), n);
    } catch (const std::exception& e) {
      REXLOG_WARN("Attires: couldn't write {}: {} (using the game's)", rel.string(), e.what());
      fs::remove(out, ec);
      fs::remove(stamp_file, ec);
      made.erase(out);
    }
  }

  // Ones written for picks that are gone now.
  for (auto& it : fs::directory_iterator(ch_dir, ec)) {
    const fs::path stamp_file = it.path();
    if (stamp_file.extension() != ".attires") continue;
    fs::path pac = stamp_file;
    pac.replace_extension();
    if (made.count(pac)) continue;
    std::error_code ec2;
    fs::remove(stamp_file, ec2);
    if (!roster_owns(pac)) fs::remove(pac, ec2);
    REXLOG_INFO("Attires: {} no longer picked, removed", pac.filename().string());
  }
}

}  // namespace svr07::attires
