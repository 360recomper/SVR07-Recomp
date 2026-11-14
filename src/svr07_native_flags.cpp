// Native renderer switch cvars for the app side (SvR07 native port, lane L4).
// Compiled only with SVR07_NATIVE_RENDERER (cmake/native_renderer.cmake); read once in
// Svr07App::LatchRenderer (src/svr07_app.h). Template: Rumble Roses XX src/rumble_roses_xx_flags.cpp.
//
// Category "Native": hidden from the F4 overlay with the other native categories (Svr07App::HideNativeCvars)
// because switching the renderer there would bypass the shader cache build. The game's own settings
// (categories "Game" and "GPU") stay visible.
// Prefix svr07_ (not RR's rrxx_), so F4/toml keys and the launcher marker logic never collide with RRXX
// on the same machine (RR lessons 7.12).

#include <rex/cvar.h>

// Native-only for players: the default is native and a value from svr07.toml is ignored (Svr07App::LatchRenderer).
// Only --svr07_renderer=emulated on the command line selects the emulated renderer (developer / self-test).
REXCVAR_DEFINE_STRING(svr07_renderer, "native", "Native",
                      "DEVELOPER ONLY: --svr07_renderer=emulated on the command line runs the old Xenos GPU "
                      "emulation (A/B, self-tests). Ignored in svr07.toml: players always run native "
                      "(needs shader_cache.bin beside the exe)")
    .allowed({"emulated", "native"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(svr07_test_config, "", "Native",
                      "Test only: extra .toml loaded before the renderer is chosen");
REXCVAR_DEFINE_BOOL(svr07_native_test_device_fail, false, "Native",
                    "Test only: svr07_renderer=native acts as if the host device failed "
                    "(error box, exit code 3)");
REXCVAR_DEFINE_STRING(svr07_selftest_keys, "", "Native",
                      "Test only: host key presses for self-tests, KEY@seconds after the overlay drawer "
                      "comes up, comma separated (e.g. F4@75,F4@83 opens and closes the settings overlay)");
