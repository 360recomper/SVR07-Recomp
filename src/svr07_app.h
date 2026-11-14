// svr07 - ReXGlue Recompiled Project
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
// Customize your app by overriding virtual hooks from rex::ReXApp.
//
// Native renderer (SvR07 native port, lane L4; template Rumble Roses XX src/rumble_roses_xx_app.h):
// everything native is under SVR07_NATIVE_RENDERER (scaffolding: svr07_renderer cvar, census build) or
// SVR07_NATIVE_HOOKS (plume D3D12 device, overlay, null guest GPU). Without either define (the default
// build, cmake option SVR07_NATIVE_RENDERER=OFF) this file compiles to exactly the emulated app.

#pragma once

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/rex_app.h>
#include <rex/system/gpu_plugin.h>

#include "svr07_attires.h"
#include "svr07_portraits.h"
#include "svr07_roster.h"
#if defined(SVR07_NATIVE_HOOKS)
#include "svr07_renderer_fail.h"
#include "svr07_settings_menu.h"
#endif

#if defined(SVR07_NATIVE_RENDERER)
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

#include <rex/logging.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/ui_event.h>

// Native renderer switch (src/svr07_native_flags.cpp).
REXCVAR_DECLARE(std::string, svr07_renderer);
REXCVAR_DECLARE(std::string, svr07_test_config);
REXCVAR_DECLARE(bool, svr07_native_test_device_fail);
REXCVAR_DECLARE(std::string, svr07_selftest_keys);

// svr07_renderer is read once in LatchRenderer and latched here; nothing switches renderers later.
// Every native hook starts with `if (!g_svr07_native) <game body>` (native/src/native/ufc_hook_gate.h),
// so the default (emulated) runs exactly as the emulated exe. Same guard as native/src/native/ufc_native.h.
#ifndef SVR07_NATIVE_LATCH_DEFINED
#define SVR07_NATIVE_LATCH_DEFINED
inline bool g_svr07_native = false;
#endif
#endif  // SVR07_NATIVE_RENDERER

#if defined(SVR07_NATIVE_HOOKS)
// Native renderer: plume D3D12 device, ImGui overlay drawer, crash traces (native/src).
#include "gpu/device.h"
#include "gpu/imgui_overlay_drawer.h"
#include "native/ufc_native.h"
void SvrInstallCrashTrace(const char* trace_dir);  // native/src/native/crash_trace.cpp
bool SvrCpuName(char* name, uint32_t size, uint32_t& logical);  // native/src/native/display_size.cpp
REXCVAR_DECLARE(std::string, svr07_shader_cache);  // native/src/reblue/gpu/shaders/guest_shaders.cpp
#endif

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

#if defined(SVR07_NATIVE_RENDERER)
  // Exit code when the native device can't be created: the launcher / self-test restarts the game
  // with --svr07_renderer=emulated (nothing is written to the toml). UFC/RR contract, but RR's bare 3
  // is replaced by a SvR07-specific value ('SW' 0003) so it can't be mistaken for another exit 3.
  static constexpr int kNativeDeviceFailedExitCode = 0x53570003;

  // svr07_renderer is read once, here (from RR's RumbleRosesXxApp::LatchRenderer).
  //
  // Native-only (UFC FINDINGS-native-only.md step 1, SvR07 FINDINGS-native-only.md): players always get the
  // native renderer. The emulated renderer is a developer / self-test switch: only --svr07_renderer=emulated
  // on the COMMAND LINE selects it; a svr07_renderer value in svr07.toml (older launchers wrote "emulated")
  // is ignored. No silent fallback: a missing shader cache is an error box + exit code 4, a failed native
  // device an error box + exit code 3 (src/svr07_renderer_fail.h); the launcher shows its own dialog.
  static void LatchRenderer() {
#if defined(SVR07_NATIVE_HOOKS)
    if (REXCVAR_GET(svr07_renderer) != "native" && !svr07_renderer_fail::RendererOnCommandLine()) {
      REXLOG_WARN("svr07_renderer = '{}' from svr07.toml ignored: players always run native "
                  "(developers: --svr07_renderer=emulated on the command line)",
                  REXCVAR_GET(svr07_renderer));
      rex::cvar::SetFlagByName("svr07_renderer", "native");
    }
#endif
    const std::string& renderer = REXCVAR_GET(svr07_renderer);
    g_svr07_native = false;
    if (renderer == "native") {
#if !defined(SVR07_NATIVE_HOOKS)
      if (REXCVAR_GET(svr07_native_test_device_fail)) {
        // Test of the launcher's restart on kNativeDeviceFailedExitCode; the hooks build takes the
        // same path when the plume device can't be created (OnPreLaunchModule).
        REXLOG_ERROR("svr07 native: host device creation failed; restart with --svr07_renderer=emulated");
        std::fprintf(stderr, "svr07 native: host device creation failed (exit 0x%08X)\n",
                     static_cast<unsigned>(kNativeDeviceFailedExitCode));
        rex::FlushLogging();
        std::_Exit(kNativeDeviceFailedExitCode);
      }
#endif
#if defined(SVR07_NATIVE_HOOKS)
      // A start without a built shader cache would come up black: tell the player (box + exit code 4).
      std::filesystem::path cache = rex::to_path(REXCVAR_GET(svr07_shader_cache));
      if (cache.is_relative())
        cache = rex::filesystem::GetExecutableFolder() / cache;
      std::error_code ec;
      if (!std::filesystem::is_regular_file(cache, ec)) {
        svr07_renderer_fail::Fail(svr07_renderer_fail::kCacheMissing,
                                  "the shader cache is missing (" + rex::path_to_utf8(cache) + ")");
      }
      g_svr07_native = true;
      // Keep "native: renderer on (": the launcher, package script and svr_selftest_native.py look for it.
      REXLOG_INFO("svr07 native: renderer on (plume D3D12)");
#else
      // Scaffolding / census build: no native hooks compiled in.
      REXLOG_WARN("svr07 native: not implemented, using the emulated renderer");
      std::fprintf(stderr, "svr07 native: not implemented, using the emulated renderer\n");
#endif
    } else if (renderer == "emulated") {
      REXLOG_WARN("svr07_renderer = emulated from the command line: DEVELOPER-ONLY emulated renderer (A/B, self-tests)");
    } else {
      REXLOG_WARN("svr07_renderer '{}' unknown, using the emulated renderer", renderer);
    }
  }

  // The native renderer's switches and debug cvars are not player settings: hidden from the F4
  // overlay (needs the SDK's overlay debug-only skip, RR my-fixes/v010-sdk-rrnr12-overlay-debug-only.patch,
  // lane L1). Only the native tree's categories: "Native", "SvR", "reblue", and "svr07" (the D3D census,
  // native/src/dev). SvR07's own settings use "Game" and "GPU" (src/*.cpp, checked 2026-11-10) and stay
  // visible; so do re:Blue's player graphics settings ("Blue Dragon": vsync, MSAA, ...), as in RR.
  static void HideNativeCvars() {
    for (auto& entry : rex::cvar::GetRegistry()) {
      if (entry.category == "Native" || entry.category == "SvR" || entry.category == "reblue" ||
          entry.category == "svr07")
        entry.is_debug_only = true;
    }
  }
#endif  // SVR07_NATIVE_RENDERER

  void OnPreSetup(rex::RuntimeConfig& config) override {
#if defined(SVR07_NATIVE_RENDERER)
    HideNativeCvars();
    const std::string& test_config = REXCVAR_GET(svr07_test_config);
    if (!test_config.empty()) {
      rex::cvar::LoadConfig(rex::to_path(test_config));
      REXLOG_INFO("test config {} loaded, svr07_renderer = {}", test_config,
                  REXCVAR_GET(svr07_renderer));
    }
    LatchRenderer();
#endif
#if defined(SVR07_NATIVE_HOOKS)
    if (g_svr07_native) {
      // Native renderer: the Xenos plugin's null backend (SDK null GPU) keeps the game's D3D command
      // stream moving (fences, interrupts, vblank) without drawing; the hooks draw. gpu_backend is
      // not used here (it would load the emulated Vulkan/D3D12 backend).
      if (!config.graphics) {
        config.graphics = rex::system::LoadGpuPlugin("xenos", "null");
        config.gpu_plugin = "xenos";
        REXLOG_INFO("svr07 native: guest GPU {}", config.graphics ? "null (sync only)" : "MISSING");
      }
      return;
    }
#endif
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

#if defined(SVR07_NATIVE_HOOKS)
  // Native (from RR, which took it from UFC-native ufc3_app.h / SvR 2010's svr2010_app.h): the plume
  // device draws the ImGui overlays at present time. Both return the SDK defaults on the emulated renderer.
  std::unique_ptr<rex::ui::ImmediateDrawer> OnCreateImmediateDrawer() override {
    if (!g_svr07_native)
      return nullptr;
    auto drawer = std::make_unique<bd::gpu::ImGuiOverlayDrawer>();
    overlay_drawer_ = drawer.get();
    return drawer;
  }

  void OnPreLaunchModule() override {
    if (!g_svr07_native)
      return;
    // Crash / hang tracer: traces go beside the game's log file (a self-test copies that folder back).
    {
      std::filesystem::path dir = rex::filesystem::GetExecutableFolder();
      const std::string log_file = rex::cvar::GetFlagByName("log_file");
      if (!log_file.empty()) {
        std::filesystem::path parent = std::filesystem::path(log_file).parent_path();
        if (!parent.empty()) dir = parent;
      }
      SvrInstallCrashTrace(dir.string().c_str());
      REXLOG_INFO("svr07 native: crash and hang traces go to {}", dir.string());
    }
    {
      char cpu[128] = {};
      uint32_t logical = 0;
      if (SvrCpuName(cpu, sizeof(cpu), logical))
        REXLOG_INFO("svr07 native: CPU {}, {} logical", cpu, logical);
    }
    // No fallback: the player is told what failed (GPU, feature level) and what to do; exit code 3 +
    // native-error.txt for the launcher (src/svr07_renderer_fail.h).
    if (REXCVAR_GET(svr07_native_test_device_fail)) {
      svr07_renderer_fail::Fail(svr07_renderer_fail::kDeviceFailed,
                                "test: svr07_native_test_device_fail is set");
    }
    if (!bd::gpu::Video::CreateHostDevice(window())) {
      const std::string why = bd::gpu::Video::LastError();
      svr07_renderer_fail::Fail(svr07_renderer_fail::kDeviceFailed,
                                "Direct3D 12 device creation failed" + (why.empty() ? std::string() : ": " + why));
    }
    // Present runs on the guest thread and ImGui on the UI thread: the UI thread records the
    // overlay in the background and Present replays the latest recording.
    bd::gpu::Video::SetOverlayDrawHook([this](plume::RenderCommandList* cmd,
                                              plume::RenderFramebuffer* fb, uint32_t w,
                                              uint32_t h) {
      auto* drawer = overlay_drawer_;
      if (!imgui_drawer() || !imgui_drawer()->HasDialogs()) {
        if (drawer)
          drawer->ClearCapture();
        return;
      }
      if (!drawer || drawer->NeedsDirectDraw() || !drawer->ReplayLatest(cmd, fb, w, h)) {
        app_context().CallInUIThreadSynchronous([this, cmd, fb, w, h] {
          bd::gpu::ReblueUIDrawContext ctx(w, h, cmd, fb);
          imgui_drawer()->Draw(ctx);
        });
      }
      if (drawer && !overlay_capture_pending_.exchange(true)) {
        app_context().CallInUIThread([this, w, h] {
          if (imgui_drawer() && overlay_drawer_)
            overlay_drawer_->Capture(
                w, h, [this](rex::ui::UIDrawContext& ctx) { imgui_drawer()->Draw(ctx); });
          overlay_capture_pending_.store(false);
        });
      }
    });
  }

  void OnWindowPixelSizeChanged(uint32_t w, uint32_t h) override {
    if (!g_svr07_native) {
      rex::ReXApp::OnWindowPixelSizeChanged(w, h);
      return;
    }
    bd::gpu::Video::RequestResize();
  }
#endif  // SVR07_NATIVE_HOOKS

  std::optional<rex::PathConfig> OnFinalizePaths(
      const rex::PathConfig& defaults, std::function<void(rex::PathConfig)> resume) override {
    (void)resume;
    // Before the game files are mounted: merge mods\portraits into the
    // select-screen banks and rebuild plist360.arc if it's stale (both are
    // laid over the game's files).
    // mods/roster first: it lists its model pac in plist360.h, which the index
    // rebuild below reads.
    // Attire picks: the roster merge applies those of ch92.pac's ids, the rest go into
    // copies of their chNN.pac (all before the index rebuild).
    const auto mods = rex::Runtime::ResolveModsRoot();
    const auto generated = defaults.cache_root / "mods_generated";
    svr07::attires::LoadPicks(mods);
    svr07::roster::BuildRosterFiles(defaults.game_data_root, mods, generated);
    svr07::attires::BuildAttirePacs(defaults.game_data_root, mods, generated);
    svr07::PreparePortraits(defaults.game_data_root, defaults.cache_root,
                            svr07::roster::ExtraPictureFolders());
    return defaults;
  }

#if defined(SVR07_NATIVE_RENDERER)
  // Self-tests: svr07_selftest_keys = "F4@75,F4@83" presses host keys (key binds such as the F4
  // settings overlay) at those seconds; scripted pads can't reach the overlay. Off when empty.
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    (void)drawer;
    const std::string spec = REXCVAR_GET(svr07_selftest_keys);
    if (spec.empty())
      return;
    auto alive = selftest_keys_alive_;
    std::thread([this, spec, alive] {
      const auto t0 = std::chrono::steady_clock::now();
      size_t pos = 0;
      while (pos < spec.size()) {
        size_t end = spec.find(',', pos);
        if (end == std::string::npos) end = spec.size();
        const std::string item = spec.substr(pos, end - pos);
        pos = end + 1;
        const size_t at = item.find('@');
        if (at == std::string::npos) continue;
        const std::string key = item.substr(0, at);
        const double secs = std::atof(item.c_str() + at + 1);
        std::this_thread::sleep_until(t0 + std::chrono::milliseconds(static_cast<int64_t>(secs * 1000)));
        if (!*alive) return;
        app_context().CallInUIThreadDeferred([this, key, alive] {
          if (!*alive || !window()) return;
          rex::ui::KeyEvent e(window(), rex::ui::ParseVirtualKey(key), 0, false, false, false, false, false);
          const bool hit = rex::ui::ProcessKeyEvent(e);
          REXLOG_INFO("svr07_selftest_keys: {} {}", key, hit ? "pressed" : "no bind");
        });
      }
    }).detach();
  }

#if defined(SVR07_NATIVE_HOOKS)
  // F4 / pad Back + Start: our settings menu instead of the SDK's (its SaveConfig rewrote svr07.toml and
  // showed settings that break the game): src/svr07_settings_menu.cpp. Runs after the SDK set its bind and
  // pad hook. The dev emulated path keeps the SDK menu.
  void OnPostSetup() override {
    if (g_svr07_native) svr07_settings_menu::Install(window());
  }
  // Runs before the renderer is latched (fonts are set up with the overlays), so both renderers bake them;
  // only the native menu uses them.
  void OnConfigureFonts(ImFontAtlas* atlas) override { svr07_settings_menu::ConfigureFonts(atlas); }
#endif

  void OnShutdown() override {
    *selftest_keys_alive_ = false;
#if defined(SVR07_NATIVE_HOOKS)
    overlay_drawer_ = nullptr;
#endif
  }
#endif  // SVR07_NATIVE_RENDERER

  // Override virtual hooks for customization:
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostSetup() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}  (native build: self-test keys above)
  // void OnShutdown() override {}                                    (native build: above)
  // void OnConfigurePaths(rex::PathConfig& paths) override {}

#if defined(SVR07_NATIVE_RENDERER)
 private:
  std::shared_ptr<std::atomic<bool>> selftest_keys_alive_ = std::make_shared<std::atomic<bool>>(true);
#if defined(SVR07_NATIVE_HOOKS)
  bd::gpu::ImGuiOverlayDrawer* overlay_drawer_ = nullptr;  // owned by ReXApp's immediate drawer
  std::atomic<bool> overlay_capture_pending_{false};
#endif
#endif
};
