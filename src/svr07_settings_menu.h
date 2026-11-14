// svr07 - the in-game settings menu for the native renderer (F4, or Back + Start on the pad).
//
// Replaces the SDK's F4 menu under native (owner 2026-11-12: "change it to only update the same
// settings as the launcher, or make a new one for our native version"). It shows only settings that
// work under native, the same svr07.toml keys the launcher's Settings tab writes, each marked "now"
// (applied live) or "next start". Saving edits only the changed key's line in svr07.toml (as the
// launcher's Toml.cs does) and never calls the SDK's cvar::SaveConfig, which rewrote the whole file
// with unescaped Windows paths (FINDINGS-settings-ignored.md). The emulated renderer keeps the SDK menu.
#pragma once

struct ImFontAtlas;
namespace rex::ui {
class Window;
}

namespace svr07_settings_menu {

// Native only. Call from Svr07App::OnPostSetup (after the SDK set its F4 bind and pad hook): takes
// over the F4 key, the Back + Start pad combo and the input gate while the menu is open.
void Install(rex::ui::Window* window);

// Bakes Segoe UI (regular + semibold, a ladder of sizes) into the ImGui atlas for the menu, which
// uses the launcher's look. Call from Svr07App::OnConfigureFonts.
void ConfigureFonts(ImFontAtlas* atlas);

}  // namespace svr07_settings_menu
