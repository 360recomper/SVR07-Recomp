// svr07 - native-only startup (FINDINGS-native-only.md, step 1).
//
// Players always run the native renderer. When it can't start, the game says why and exits; it never
// falls back to the emulated GPU on its own. The emulated renderer stays reachable for developers only
// through --svr07_renderer=emulated on the command line (A/B of bugs); svr07.toml can't select it.

#pragma once

#include <string>

namespace svr07_renderer_fail {

// Exit codes the launcher reads (launcher/NativeRenderer.cs). The details are in native-error.txt
// beside the exe.
inline constexpr int kDeviceFailed = 3;
inline constexpr int kCacheMissing = 4;

// True when --svr07_renderer (or -svr07_renderer) is on this process's command line. Only then may the
// cvar pick the emulated renderer; a value from svr07.toml (older launchers wrote "emulated") is ignored.
bool RendererOnCommandLine();

// Logs the failure, writes native-error.txt beside the exe (what failed, the GPU and its highest
// Direct3D 12 feature level), shows a message box unless the launcher started the game (it shows its
// own dialog from that file; env SVR07_LAUNCHER=1), then exits with `code`.
[[noreturn]] void Fail(int code, const std::string& what);

}  // namespace svr07_renderer_fail
