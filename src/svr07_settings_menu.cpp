// svr07 - in-game settings menu for the native renderer. See svr07_settings_menu.h. Ported from UFC 3
// (src/native/ufc_settings_menu.cpp): same look and behaviour, SvR07 rows.
//
// What is in it (2026-11-12) and why:
// - Only svr07.toml keys the launcher's Settings tab writes (launcher/Tabs/SettingsTab.cs), plus the
//   game's own toggles the old F4 menu had (bloom, mods folder); every row works under native.
// - Dropped: Graphics backend, CAS sharpening, VSync, texture sharpness (SDK emulated-GPU cvars that
//   native ignores), the "All engine settings" cvar browser and the saves buttons. Graphics card and
//   Renderer stay in the launcher (they pick the device the running game already uses).
// - "now" rows set the cvar live (each is read when it is used: per frame, per fight event, or by the
//   shader-patch thread) and write svr07.toml; "next start" rows only write svr07.toml (resolution_scale
//   and the roster table are latched at boot, the mods folder is mounted at boot).
// - Saving: re-read svr07.toml, change only the changed key's line (or add it above the first [table]),
//   keep comments, other keys and the line ends, check that the result still parses with toml++, then
//   replace the file. Never cvar::SaveConfig (it rewrote the file with every non-default cvar and wrote
//   Windows paths unescaped, which made the game drop the whole file: FINDINGS-settings-ignored.md).

#include "svr07_settings_menu.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <imgui.h>
#include <toml++/toml.hpp>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/string.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>


REXCVAR_DEFINE_STRING(svr07_settings_menu_test, "", "Native",
                      "Test only: 'open' opens the in-game settings menu at boot; 'open,key=value,wait,close' "
                      "then changes rows through the menu's own save path, one step every 2 s");

namespace svr07_settings_menu {

// ------------------------------------------------------------------------------------ svr07.toml
namespace toml_lines {

std::filesystem::path File() { return rex::filesystem::GetExecutableFolder() / "svr07.toml"; }

// "" = parses; else "line N: description".
std::string ParseError(const std::string& text) {
#if TOML_EXCEPTIONS
  try {
    (void)toml::parse(text);
    return {};
  } catch (const toml::parse_error& e) {
    return "line " + std::to_string(e.source().begin.line) + ": " + std::string(e.description());
  }
#else
  auto r = toml::parse(text);
  if (r) return {};
  return "line " + std::to_string(r.error().source().begin.line) + ": " +
         std::string(r.error().description());
#endif
}

// A TOML basic string: "..." with \ and " escaped (a raw backslash is an invalid escape).
std::string Quote(const std::string& v) {
  std::string out = "\"";
  for (char c : v) {
    if (c == '\\' || c == '"') out += '\\';
    out += c;
  }
  return out + "\"";
}

struct Lines {
  std::vector<std::string> lines;
  std::string eol = "\r\n";  // the launcher writes CRLF
  bool had_file = false;

  void Load() {
    lines.clear();
    std::ifstream in(File(), std::ios::binary);
    if (!in) return;
    had_file = true;
    std::stringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();
    if (text.rfind("\xEF\xBB\xBF", 0) == 0) text.erase(0, 3);
    if (text.find('\n') != std::string::npos && text.find("\r\n") == std::string::npos) eol = "\n";
    size_t pos = 0;
    while (pos < text.size()) {
      size_t nl = text.find('\n', pos);
      std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      lines.push_back(std::move(line));
      if (nl == std::string::npos) break;
      pos = nl + 1;
    }
  }

  // Top-level keys only: the line index of "key = ...", stopping at the first [table] header.
  int Find(const std::string& key, int* first_table = nullptr) const {
    if (first_table) *first_table = -1;
    for (int i = 0; i < int(lines.size()); ++i) {
      const std::string& l = lines[i];
      size_t s = l.find_first_not_of(" \t");
      if (s == std::string::npos || l[s] == '#') continue;
      if (l[s] == '[') {
        if (first_table) *first_table = i;
        return -1;
      }
      if (l.compare(s, key.size(), key) != 0) continue;
      size_t e = l.find_first_not_of(" \t", s + key.size());
      if (e != std::string::npos && l[e] == '=') return i;
    }
    return -1;
  }

  // The value as text: a "..." string unescaped, anything else with a trailing # comment cut.
  std::optional<std::string> Get(const std::string& key) const {
    int i = Find(key);
    if (i < 0) return std::nullopt;
    std::string v = lines[i].substr(lines[i].find('=') + 1);
    v.erase(0, v.find_first_not_of(" \t"));
    if (!v.empty() && v[0] == '"') {
      std::string out;
      for (size_t k = 1; k < v.size(); ++k) {
        if (v[k] == '\\' && k + 1 < v.size()) {
          out += v[++k];
        } else if (v[k] == '"') {
          break;
        } else {
          out += v[k];
        }
      }
      return out;
    }
    if (size_t h = v.find('#'); h != std::string::npos) v.erase(h);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
    return v;
  }

  void Set(const std::string& key, const std::string& raw) {
    int table = -1;
    int i = Find(key, &table);
    std::string line = key + " = " + raw;
    if (i >= 0)
      lines[i] = line;
    else if (table >= 0)
      lines.insert(lines.begin() + table, line);
    else
      lines.push_back(line);
  }

  // Writes svr07.toml.tmp, then replaces svr07.toml with it. Refuses a result that doesn't parse.
  bool Save(std::string* error) const {
    std::string text;
    if (!had_file) text = "# WWE SmackDown vs. Raw 2007 Recomp settings (the launcher's Settings tab and F4 in game)" + eol;
    for (const auto& l : lines) text += l + eol;
    if (std::string e = ParseError(text); !e.empty()) {
      *error = "not saved, the result would not parse (" + e + ")";
      return false;
    }
    std::filesystem::path tmp = File();
    tmp += ".tmp";
    {
      std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
      if (!out) {
        *error = "could not write " + rex::path_to_utf8(tmp);
        return false;
      }
      out << text;
      if (!out.flush()) {
        *error = "could not write " + rex::path_to_utf8(tmp);
        return false;
      }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, File(), ec);  // replaces (MoveFileEx REPLACE_EXISTING)
    if (ec) {
      std::filesystem::remove(tmp, ec);
      *error = "could not replace svr07.toml (open in another program?)";
      return false;
    }
    return true;
  }
};

}  // namespace toml_lines

// ----------------------------------------------------------------------------------------- rows
enum class When { kNow, kNextStart };

struct Choice {
  const char* label;
  const char* value;  // TOML text for numbers and bools, the plain string for strings
};

struct Row {
  const char* section;  // starts a new heading when it differs from the row before
  const char* key;
  const char* label;
  When when;
  const char* hint;
  std::vector<Choice> choices;  // empty = on / off
  bool string_value = false;     // written as a quoted TOML string
};

const std::vector<Row>& Rows() {
  static const std::vector<Row> rows = {
      {"Graphics", "resolution_scale", "Render scale", When::kNextStart,
       "Internal resolution. Read when the game starts.",
       {{"1x (720p)", "1"}, {"2x (1440p)", "2"}, {"3x (4K)", "3"}, {"4x (5K+)", "4"}}},
      {"Graphics", "svr07_native_upscale", "Upscaling", When::kNow,
       "AMD FSR 1 sharpens a picture smaller than the window; same look otherwise.",
       {{"Bilinear", "bilinear"}, {"AMD FSR 1", "auto"}}, true},
      {"Graphics", "fullscreen", "Fullscreen (F11)", When::kNow, "Borderless full screen."},
      {"Graphics", "svr07_no_blur", "Depth-of-field blur off", When::kNow,
       "Removes the blur on the crowd, near the ropes and around wrestlers."},
      {"Graphics", "entrance_60fps", "Entrances and cutscenes at 60 fps", When::kNextStart, nullptr},
      {"Sound", "pause_on_focus_loss", "Mute while in the background", When::kNow,
       "Sound stops while another window has focus."},
      {"Mods", "mods_enabled", "Load mods folder", When::kNextStart, "Off = the stock game."},
      {"Mods", "extra_arenas", "Extra arenas on Arena Select", When::kNextStart, nullptr},
      {"Mods", "roster_extras", "Extra superstars from the mod pack", When::kNextStart, nullptr},
  };
  return rows;
}

bool IsBoolText(const std::string& v, bool& out) {
  if (v == "true" || v == "1") {
    out = true;
    return true;
  }
  if (v == "false" || v == "0") {
    out = false;
    return true;
  }
  return false;
}

// Choice index of a value ("1.500000" matches "1.5"), or -1.
int ChoiceIndex(const Row& r, const std::string& v) {
  for (int i = 0; i < int(r.choices.size()); ++i) {
    if (v == r.choices[i].value) return i;
    if (!r.string_value) {
      char* end = nullptr;
      double a = std::strtod(v.c_str(), &end);
      if (end != v.c_str() && std::abs(a - std::atof(r.choices[i].value)) < 1e-6) return i;
    }
  }
  return -1;
}

// ------------------------------------------------------------------------------------ the look
// The launcher's palette (launcher/Tabs/Theme.cs) and Segoe UI, so the menu looks like the launcher.
namespace theme {

constexpr uint32_t kBg = 0x0B0B0D, kSide = 0x050506, kSurface = 0x17171B, kSurface2 = 0x24242A, kLine = 0x3A3A42,
                   kInk = 0xF4F4F6, kMuted = 0xB4B4BE, kFaint = 0x6E6E78, kAccent = 0xFF2A2A,
                   kAccentText = 0xFF5C5C, kPrimary = 0xD01212, kPrimaryHot = 0xE62222, kPrimaryDown = 0xA30E0E,
                   kGold = 0xC9A24B,
                   kBlue = 0x2E7BFF, kBlueText = 0x6FA8FF;  // SmackDown blue beside Raw red

ImVec4 V(uint32_t rgb, float a = 1.0f) {
  return ImVec4(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
}
ImU32 U(uint32_t rgb, float a = 1.0f) { return ImGui::GetColorU32(V(rgb, a)); }

// The SDK's ImGui backend uploads one baked atlas (no dynamic glyphs), so each size is baked up front;
// the menu picks the size nearest to what the window height asks for.
constexpr float kSizes[] = {13, 15, 17, 19, 22, 25, 29, 33, 38, 44, 50};
constexpr int kNumSizes = int(std::size(kSizes));
ImFont* g_text[kNumSizes] = {};
ImFont* g_semi[kNumSizes] = {};

struct Fonts {
  ImFont* text = nullptr;   // body
  ImFont* semi = nullptr;   // section headers
  ImFont* title = nullptr;  // "Settings"
};

Fonts Pick(float px, int shrink) {
  int best = 0;
  for (int i = 1; i < kNumSizes; ++i)
    if (std::abs(kSizes[i] - px) < std::abs(kSizes[best] - px)) best = i;
  best = std::max(0, best - shrink);
  Fonts f;
  f.text = g_text[best];
  f.semi = g_semi[best];
  f.title = g_semi[std::min(best + 3, kNumSizes - 1)];
  if (!f.semi) f.semi = f.text;
  if (!f.title) f.title = f.semi;
  return f;
}

// Pushes the launcher look for one Begin/End; pops it when it goes out of scope.
struct Scope {
  int colors = 0, vars = 0;
  void C(ImGuiCol c, const ImVec4& v) {
    ImGui::PushStyleColor(c, v);
    ++colors;
  }
  void F(ImGuiStyleVar v, float x) {
    ImGui::PushStyleVar(v, x);
    ++vars;
  }
  void F(ImGuiStyleVar v, ImVec2 x) {
    ImGui::PushStyleVar(v, x);
    ++vars;
  }
  explicit Scope(float s) {
    C(ImGuiCol_WindowBg, V(kBg, 0.92f));
    C(ImGuiCol_PopupBg, V(kSurface, 0.98f));
    C(ImGuiCol_Border, V(kLine));
    C(ImGuiCol_Text, V(kInk));
    C(ImGuiCol_TextDisabled, V(kFaint));
    C(ImGuiCol_FrameBg, V(kSurface));
    C(ImGuiCol_FrameBgHovered, V(kSurface2));
    C(ImGuiCol_FrameBgActive, V(kSurface2));
    C(ImGuiCol_CheckMark, V(kAccent));
    C(ImGuiCol_SliderGrab, V(kBlue));
    C(ImGuiCol_SliderGrabActive, V(kBlueText));
    C(ImGuiCol_Button, V(kSurface2));
    C(ImGuiCol_ButtonHovered, V(kLine));
    C(ImGuiCol_ButtonActive, V(kSurface));
    C(ImGuiCol_Header, V(kSurface2));
    C(ImGuiCol_HeaderHovered, V(kSurface2));
    C(ImGuiCol_HeaderActive, V(kBlue, 0.40f));
    C(ImGuiCol_Separator, V(kLine));
    C(ImGuiCol_NavCursor, V(kAccent));
    C(ImGuiCol_ScrollbarBg, V(kBg, 0.0f));
    C(ImGuiCol_ScrollbarGrab, V(kLine));
    C(ImGuiCol_TextSelectedBg, V(kAccent, 0.35f));
    F(ImGuiStyleVar_WindowRounding, 8.0f * s);
    F(ImGuiStyleVar_FrameRounding, 4.0f * s);
    F(ImGuiStyleVar_PopupRounding, 4.0f * s);
    F(ImGuiStyleVar_GrabRounding, 4.0f * s);
    F(ImGuiStyleVar_WindowBorderSize, 1.0f);
    F(ImGuiStyleVar_FrameBorderSize, 1.0f);
    F(ImGuiStyleVar_PopupBorderSize, 1.0f);
    F(ImGuiStyleVar_WindowPadding, ImVec2(22.0f * s, 14.0f * s));
    F(ImGuiStyleVar_FramePadding, ImVec2(10.0f * s, 2.0f * s));
    F(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f * s, 4.0f * s));
    F(ImGuiStyleVar_SeparatorTextBorderSize, std::max(1.0f, 1.0f * s));
    F(ImGuiStyleVar_SeparatorTextPadding, ImVec2(0.0f, 2.0f * s));
    F(ImGuiStyleVar_DisabledAlpha, 0.45f);
  }
  ~Scope() {
    ImGui::PopStyleVar(vars);
    ImGui::PopStyleColor(colors);
  }
};

}  // namespace theme

void ConfigureFonts(ImFontAtlas* atlas) {
  static const ImWchar kRanges[] = {0x0020, 0x00FF, 0x2013, 0x2026, 0};  // Latin-1, dashes, quotes, ellipsis
  const char* dir = "C:\\Windows\\Fonts\\";
  auto add = [&](const char* file, float px) -> ImFont* {
    std::error_code ec;
    const std::string path = std::string(dir) + file;
    if (!std::filesystem::is_regular_file(path, ec)) return nullptr;
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 1;
    return atlas->AddFontFromFileTTF(path.c_str(), px, &cfg, kRanges);
  };
  int loaded = 0;
  for (int i = 0; i < theme::kNumSizes; ++i) {
    theme::g_text[i] = add("segoeui.ttf", theme::kSizes[i]);
    theme::g_semi[i] = add("seguisb.ttf", theme::kSizes[i]);
    if (!theme::g_semi[i]) theme::g_semi[i] = add("segoeuib.ttf", theme::kSizes[i]);
    loaded += theme::g_text[i] != nullptr;
  }
  if (!loaded) REXLOG_WARN("[settings] Segoe UI not found in C:\\Windows\\Fonts: the menu uses the default font");
}

// ------------------------------------------------------------------------------------- the menu
class Menu;
Menu* g_menu = nullptr;           // UI thread only
std::atomic<bool> g_open{false};  // read by the pad hook (guest thread)
std::atomic<bool> g_combo_down{false};
rex::ui::Window* g_window = nullptr;

class Menu final : public rex::ui::ImGuiDialog {
 public:
  explicit Menu(rex::ui::ImGuiDrawer* d) : ImGuiDialog(d) {
    // Next-start rows show what the file holds (the launcher may have changed it since boot).
    file_.Load();
    g_open.store(true);
    REXLOG_INFO("[settings] menu opened");
  }
  void RequestClose() { Close(); }

 protected:
  void OnClose() override {
    g_menu = nullptr;
    g_open.store(false);
    if (auto* d = imgui_drawer()) d->SetGamepadInput(false, 0, 0, 0);
    REXLOG_INFO("[settings] menu closed");
  }

  void OnDraw(ImGuiIO& io) override {
    RunTestSteps();
    if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f || io.MouseClicked[0])
      pad_mode_ = false;
    else if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadUp) ||
             ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown))
      pad_mode_ = true;
    const bool popup_open =
        ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    // B / Esc closes, unless it just closed a dropdown.
    if ((ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) &&
        !popup_open_prev_) {
      Close();
    }
    popup_open_prev_ = popup_open;

    // Sizes follow the window height (1080p = 1): text ~17 px at 1080p, ~33 px at 4K. If the menu was
    // taller than the window last frame (small window), the next smaller size is used.
    const float s = std::max(0.75f, io.DisplaySize.y / 1080.0f) * (1.0f - 0.1f * shrink_);
    const theme::Fonts f = theme::Pick(17.0f * std::max(0.75f, io.DisplaySize.y / 1080.0f), shrink_);
    theme::Scope style(s);
    if (f.text) ImGui::PushFont(f.text, f.text->LegacySize);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    const bool shown = ImGui::Begin("Settings##svr07", nullptr,
                                    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
    if (shown) DrawBody(s, f);
    if (shown && ImGui::GetWindowHeight() > io.DisplaySize.y - 8.0f && shrink_ < 4) ++shrink_;
    ImGui::End();
    if (f.text) ImGui::PopFont();
  }

 private:
  void DrawBody(float s, const theme::Fonts& f) {
    using namespace theme;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->ChannelsSplit(2);  // 0 = title / footer bands, 1 = content
    dl->ChannelsSetCurrent(1);
    const ImVec2 wpos = ImGui::GetWindowPos();
    const float rounding = ImGui::GetStyle().WindowRounding;

    // Title band (Side): "Settings" and a muted "WWE SmackDown vs. Raw 2007 Recomp" on the same baseline.
    const float title_px = f.title ? f.title->LegacySize : ImGui::GetFontSize();
    const float text_px = f.text ? f.text->LegacySize : ImGui::GetFontSize();
    const float y0 = ImGui::GetCursorPosY();
    if (f.title) ImGui::PushFont(f.title, f.title->LegacySize);
    ImGui::TextUnformatted("Settings");
    if (f.title) ImGui::PopFont();
    ImGui::SameLine(0, 14.0f * s);
    ImGui::SetCursorPosY(y0 + (title_px - text_px) * 0.8f);
    ImGui::TextColored(V(kMuted), "WWE SmackDown vs. Raw 2007 Recomp");
    const float title_bottom = wpos.y + y0 + title_px + ImGui::GetStyle().WindowPadding.y * 0.8f;
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, title_bottom + 12.0f * s));

    const float value_x = 300.0f * s;
    const float value_w = 220.0f * s;
    const float when_x = value_x + value_w + 18.0f * s;
    ImGui::TextColored(V(kMuted), "Saved to svr07.toml, the same settings as the launcher's Settings tab.");
    ImGui::TextColored(V(kMuted), "now");
    ImGui::SameLine();
    ImGui::TextColored(V(kMuted), "= applies right away;");
    ImGui::SameLine();
    ImGui::TextColored(V(kBlueText), "next start");
    ImGui::SameLine();
    ImGui::TextColored(V(kMuted), "= when you start the game again.");

    const char* section = "";
    for (const Row& r : Rows()) {
      if (!rex::cvar::GetFlagInfo(r.key)) continue;
      if (std::string(section) != r.section) {
        section = r.section;
        ImGui::Dummy(ImVec2(0, 1.0f * s));
        if (f.semi) ImGui::PushFont(f.semi, f.semi->LegacySize);
        ImGui::PushStyleColor(ImGuiCol_Separator, V(kBlue, 0.9f));
        ImGui::PushStyleColor(ImGuiCol_Text, V(kBlueText));
        ImGui::SeparatorText(section);
        ImGui::PopStyleColor();
        ImGui::PopStyleColor();
        if (f.semi) ImGui::PopFont();
      }
      DrawRow(r, s, value_x, value_w, when_x, true);
    }

    // Footer band (Side).
    ImGui::Dummy(ImVec2(0, 4.0f * s));
    const float footer_top = ImGui::GetCursorScreenPos().y;
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, footer_top + 12.0f * s));
    if (!status_.empty()) ImGui::TextColored(V(kMuted), "%s", status_.c_str());
    ImGui::PushStyleColor(ImGuiCol_Button, V(kPrimary));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, V(kPrimaryHot));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, V(kPrimaryDown));
    ImGui::PushStyleColor(ImGuiCol_Border, V(kPrimary));
    if (ImGui::Button("Close", ImVec2(130.0f * s, 0))) Close();
    ImGui::PopStyleColor(4);
    ImGui::SameLine();
    if (ImGui::Button("Quit game", ImVec2(130.0f * s, 0))) {
      if (auto* rt = rex::Runtime::instance(); rt && rt->app_context())
        rt->app_context()->CallInUIThreadDeferred([] {
          if (g_window) g_window->RequestClose();
        });
    }
    ImGui::SameLine(0, 16.0f * s);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(V(kMuted), "F4 / Esc / B closes this menu. ");

    // Bands behind the content, full window width (last frame's size while auto-resizing).
    dl->ChannelsSetCurrent(0);
    const float w = ImGui::GetWindowWidth(), h = ImGui::GetWindowHeight();
    dl->AddRectFilled(wpos, ImVec2(wpos.x + w, title_bottom), U(kSide), rounding, ImDrawFlags_RoundCornersTop);
    dl->AddLine(ImVec2(wpos.x, title_bottom), ImVec2(wpos.x + w, title_bottom), U(kBlue), 2.0f);
    dl->AddRectFilled(ImVec2(wpos.x, footer_top), ImVec2(wpos.x + w, wpos.y + h), U(kSide), rounding,
                      ImDrawFlags_RoundCornersBottom);
    dl->AddLine(ImVec2(wpos.x, footer_top), ImVec2(wpos.x + w, footer_top), U(kAccent), 2.0f);
    dl->ChannelsMerge();
  }

  // The value the row shows: the running cvar for "now" rows, the file (else the running value)
  // for "next start" rows.
  std::string Current(const Row& r) const {
    if (r.when == When::kNextStart) {
      if (auto v = file_.Get(r.key)) return *v;
    }
    return rex::cvar::GetFlagByName(r.key);
  }

  void Apply(const Row& r, const std::string& value) {
    if (r.when == When::kNow) rex::cvar::SetFlagByName(r.key, value);
    toml_lines::Lines t;
    t.Load();  // re-read: the launcher may have written it while the game ran
    t.Set(r.key, r.string_value ? toml_lines::Quote(value) : value);
    std::string err;
    if (t.Save(&err)) {
      file_ = t;
      status_ = std::string(r.label) + ": saved" + (r.when == When::kNow ? "." : ", applies when you start the game again.");
      REXLOG_INFO("[settings] {} = {} ({}), svr07.toml updated", r.key, value,
                  r.when == When::kNow ? "now" : "next start");
    } else {
      status_ = std::string(r.label) + ": " + err;
      REXLOG_ERROR("[settings] {} = {}: {}", r.key, value, err);
    }
  }

  void DrawRow(const Row& r, float s, float value_x, float value_w, float when_x, bool enabled) {
    using namespace theme;
    ImGui::PushID(r.key);
    if (!enabled) ImGui::BeginDisabled();
    const ImVec2 row_min = ImGui::GetCursorScreenPos();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(r.label);
    if (r.hint && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", r.hint);
    ImGui::SameLine(value_x);
    if (focus_key_ == r.key) {  // test step "focus=<key>"
      ImGui::SetKeyboardFocusHere();
      ImGui::SetNavCursorVisible(true);
      focus_key_.clear();
    }
    const std::string cur = Current(r);
    bool focused = false;
    if (r.choices.empty()) {
      bool v = false;
      IsBoolText(cur, v);
      if (ImGui::Checkbox("##v", &v)) Apply(r, v ? "true" : "false");
      focused = ImGui::IsItemFocused();
      if (r.hint && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", r.hint);
    } else {
      const int idx = ChoiceIndex(r, cur);
      const std::string preview = idx >= 0 ? r.choices[idx].label : "custom (" + cur + ")";
      const int n = int(r.choices.size());
      ImGui::SetNextItemWidth(value_w);
      if (pad_mode_) {
        // Pad: dropdowns swallow the pad, so A / D-pad right step to the next value, D-pad left back.
        ImGui::PushStyleColor(ImGuiCol_Button, V(kSurface));
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
        int step = ImGui::Button((preview + "##cyc").c_str(), ImVec2(value_w, 0)) ? 1 : 0;
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        const ImVec2 bmin = ImGui::GetItemRectMin(), bmax = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddText(ImVec2(bmax.x - 20.0f * s, bmin.y + ImGui::GetStyle().FramePadding.y),
                                            U(kMuted), ">");
        focused = ImGui::IsItemFocused();
        if (focused) {
          if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight)) step = 1;
          if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft)) step = -1;
        }
        if (step != 0) Apply(r, r.choices[((idx < 0 ? 0 : idx) + step + n) % n].value);
      } else {
        const bool open = ImGui::BeginCombo("##v", preview.c_str());
        focused = ImGui::IsItemFocused();
        if (open) {
          for (int i = 0; i < n; ++i) {
            if (ImGui::Selectable(r.choices[i].label, i == idx)) Apply(r, r.choices[i].value);
          }
          ImGui::EndCombo();
        }
      }
      if (r.hint && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", r.hint);
    }
    ImGui::SameLine(when_x);
    if (r.when == When::kNow) {
      ImGui::TextColored(V(kMuted), "now");
    } else {
      ImGui::TextColored(V(kBlueText), "next start");
      // Say so when the file differs from what this run uses.
      const std::string running = rex::cvar::GetFlagByName(r.key);
      bool a = false, b = false;
      const bool same = running == cur || (IsBoolText(running, a) && IsBoolText(cur, b) && a == b) ||
                        (!r.choices.empty() && ChoiceIndex(r, running) >= 0 &&
                         ChoiceIndex(r, running) == ChoiceIndex(r, cur));
      if (!same) {
        ImGui::SameLine();
        const int ri = r.choices.empty() ? -1 : ChoiceIndex(r, running);
        ImGui::TextColored(V(kMuted), "(now: %s)", ri >= 0 ? r.choices[ri].label : running.c_str());
      }
    }
    // Pad / keyboard focus: a red bar at the row's left edge, inside the window padding.
    if (focused && ImGui::GetIO().NavVisible) {
      const float x = ImGui::GetWindowPos().x + 7.0f * s;
      ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(x, row_min.y), ImVec2(x + 4.0f * s, ImGui::GetItemRectMax().y),
                                                U(kAccent), 2.0f * s);
    }
    if (!enabled) ImGui::EndDisabled();
    ImGui::PopID();
  }

  // svr07_settings_menu_test "open,key=value,...": the self-test can't click (SDL ignores input posted
  // to an unfocused window, and scripted pads skip the SDK's pad hook), so it changes rows here,
  // through Apply like a click does.
  void RunTestSteps() {
    if (test_steps_.empty()) return;
    const auto now = std::chrono::steady_clock::now();
    if (now < test_next_) return;
    test_next_ = now + std::chrono::seconds(2);
    std::string step = test_steps_.front();
    test_steps_.erase(test_steps_.begin());
    if (step == "close") {
      REXLOG_INFO("[settings] test: close");
      Close();
      return;
    }
    if (step.rfind("wait", 0) == 0) return;  // one more 2 s pause (for a screenshot)
    if (step.rfind("focus=", 0) == 0) {        // pad/keyboard focus on that row (shows the focus bar)
      focus_key_ = step.substr(6);
      pad_mode_ = true;
      return;
    }
    const size_t eq = step.find('=');
    if (eq == std::string::npos) return;
    for (const Row& r : Rows()) {
      if (step.compare(0, eq, r.key) == 0) {
        REXLOG_INFO("[settings] test: {}", step);
        Apply(r, step.substr(eq + 1));
      }
    }
  }

 public:
  void SetTestSteps(std::vector<std::string> steps) {
    test_steps_ = std::move(steps);
    test_next_ = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  }

 private:
  std::vector<std::string> test_steps_;
  std::string focus_key_;
  std::chrono::steady_clock::time_point test_next_{};
  int shrink_ = 0;  // font steps below the size the window height asks for
  toml_lines::Lines file_;
  bool pad_mode_ = false;
  bool popup_open_prev_ = false;
  std::string status_;
};

// UI thread.
void Toggle() {
  if (g_menu) {
    g_menu->RequestClose();  // deleted after its next draw (OnClose clears g_menu)
    return;
  }
  auto* rt = rex::Runtime::instance();
  if (!rt || !rt->imgui_drawer()) return;
  g_menu = new Menu(rt->imgui_drawer());  // owned by the drawer's dialog list; deletes itself on Close()
}

void Install(rex::ui::Window* window) {
  auto* rt = rex::Runtime::instance();
  if (!rt || !rt->imgui_drawer()) return;
  g_window = window;
  static bool bound = false;
  if (!bound) {
    bound = true;
    // The SDK's F4 menu calls SaveConfig: take its key (a rebound bind_settings keeps its key).
    std::string key = rex::cvar::GetFlagByName("bind_settings");
    if (key.empty()) key = "F4";
    rex::ui::UnregisterBind("bind_settings");
    rex::ui::RegisterBind("bind_svr07_settings", key, "Toggle the settings menu", [] { Toggle(); });
  }
  auto* input = static_cast<rex::input::InputSystem*>(rt->input_system());
  if (!input) return;
  // Replaces the SDK's hooks (same behaviour, our menu instead of its one): Back + Start toggles; while
  // the menu is open the pad drives it (ImGui nav) and the game sees neither pad nor keys and mouse.
  input->SetPadHook([](const rex::input::X_INPUT_GAMEPAD& pad) {
    const uint16_t b = pad.buttons;
    constexpr uint16_t kCombo = rex::input::X_INPUT_GAMEPAD_BACK | rex::input::X_INPUT_GAMEPAD_START;
    const bool combo = (b & kCombo) == kCombo;
    const bool was_combo = g_combo_down.exchange(combo);
    if (combo && !was_combo) {
      if (auto* r = rex::Runtime::instance(); r && r->app_context()) {
        REXLOG_INFO("[settings] pad Back + Start");
        r->app_context()->CallInUIThreadDeferred([] { Toggle(); });
      }
    }
    const bool open = g_open.load();
    if (open) {
      if (auto* r = rex::Runtime::instance(); r && r->imgui_drawer())
        r->imgui_drawer()->SetGamepadInput(true, combo ? 0 : b, pad.thumb_lx, pad.thumb_ly);
    }
    return open || combo;
  });
  input->SetActiveCallback([] {
    if (g_window && !g_window->HasFocus()) return false;
    if (g_open.load()) return false;
    auto* r = rex::Runtime::instance();
    auto* d = r ? r->imgui_drawer() : nullptr;
    // Other overlays (F3 stats, console): the game keeps the input unless the mouse is over them.
    return !d || !d->HasDialogs() || !d->GetIO().WantCaptureMouse;
  });
  const std::string test = REXCVAR_GET(svr07_settings_menu_test);
  if (test.rfind("open", 0) == 0 && rt->app_context())
    rt->app_context()->CallInUIThreadDeferred([test] {
      if (!g_menu) Toggle();
      std::vector<std::string> steps;
      std::stringstream ss(test);
      for (std::string item; std::getline(ss, item, ',');)
        if (item != "open") steps.push_back(item);
      if (g_menu) g_menu->SetTestSteps(std::move(steps));
    });
  REXLOG_INFO("[settings] native settings menu on {} and pad Back + Start",
              rex::cvar::GetFlagByName("bind_svr07_settings"));
}

}  // namespace svr07_settings_menu
