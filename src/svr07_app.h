// svr07 - ReXGlue Recompiled Project
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/rex_app.h>
#include <rex/system/gpu_plugin.h>

#include "svr07_portraits.h"
#include "svr07_roster.h"

REXCVAR_DEFINE_STRING(gpu_backend, "vulkan", "GPU",
                      "Graphics backend: vulkan (recommended) or d3d12 (skinned-model glitches)");

class Svr07App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Svr07App>(
        new Svr07App(ctx, "svr07", PPCImageConfig));
  }

   void OnConfigurePaths(rex::PathConfig& paths) override {
    // SDK v0.10 no longer defaults the game mount; use the assets folder next
    // to the exe (the release layout) unless --game_data_root was given.
    if (paths.game_data_root.empty()) {
      auto assets_dir = rex::filesystem::GetExecutableFolder() / "assets";
      if (std::filesystem::is_directory(assets_dir)) {
        paths.game_data_root = assets_dir;
      }
    }

    // Point UPDATE: to a separate folder containing the title update files.
    // Relative to the game data root; a leading backslash would resolve to
    // the root of the current drive instead.
    if (paths.update_data_root.empty()) {
      auto update_dir = paths.game_data_root / "1_update";
      if (std::filesystem::is_directory(update_dir)) {
        paths.update_data_root = update_dir;
      }
    }

    // Optional: point cache to a custom directory.
    // paths.cache_data_root =
    //   std::filesystem::path(R"(path\to\your\cache\folder)");

    // Optional: keep user data wherever you already want it.
    // paths.user_data_root = std::filesystem::path(R"(path\to\your\user\data)");
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    // SDK v0.10 moved Xenos GPU emulation into the runtime-loaded rexgpu-xenos
    // plugin, and an empty gpu_plugin cvar means no GPU at all.
    if (!config.graphics) {
      config.graphics = rex::system::LoadGpuPlugin("xenos", REXCVAR_GET(gpu_backend));
      if (!config.graphics) {
        // Let ReXApp retry with any backend and show its fatal dialog on failure.
        config.gpu_plugin = "xenos";
      }
    }
  }

  std::optional<rex::PathConfig> OnFinalizePaths(
      const rex::PathConfig& defaults, std::function<void(rex::PathConfig)> resume) override {
    (void)resume;
    // Before the game files are mounted: merge mods\portraits into the
    // select-screen banks and rebuild plist360.arc if it's stale (both are
    // laid over the game's files).
    // mods/roster first: it lists its model pac in plist360.h, which the index
    // rebuild below reads.
    svr07::roster::BuildRosterFiles(defaults.game_data_root, rex::Runtime::ResolveModsRoot(),
                                    defaults.cache_root / "mods_generated");
    svr07::PreparePortraits(defaults.game_data_root, defaults.cache_root,
                            svr07::roster::ExtraPictureFolders());
    return defaults;
  }

  // Override virtual hooks for customization:
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostSetup() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // void OnShutdown() override {}
  // void OnConfigurePaths(rex::PathConfig& paths) override {}
};