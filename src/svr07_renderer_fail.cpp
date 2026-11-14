// svr07 - native-only startup failures. See renderer_fail.h.

#include "svr07_renderer_fail.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#endif

#include <cstdlib>
#include <cstdio>
#include <cwchar>
#include <format>
#include <fstream>
#include <string>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

REXCVAR_DECLARE(std::string, svr07_native_adapter);

namespace svr07_renderer_fail {
namespace {

#if defined(_WIN32)
std::string Narrow(const wchar_t* w) {
  if (!w || !*w) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  std::string s(n > 0 ? n - 1 : 0, '\0');
  if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

std::wstring Widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(n > 0 ? n - 1 : 0, L'\0');
  if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
  return w;
}

// The card the native renderer would use: the one svr07_native_adapter names, else the
// high-performance one (what plume's automatic pick does), else the first hardware adapter. Its
// highest Direct3D 12 feature level is probed without keeping a device ("none" = no D3D12 at all).
void DescribeGpu(std::string& gpu, std::string& level) {
  gpu = "unknown";
  level = "unknown";
  IDXGIFactory1* f1 = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&f1))) || !f1)
    return;
  IDXGIAdapter1* pick = nullptr;
  const std::string wanted = REXCVAR_GET(svr07_native_adapter);
  IDXGIAdapter1* a = nullptr;
  for (UINT i = 0; !wanted.empty() && f1->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
    DXGI_ADAPTER_DESC1 d{};
    if (SUCCEEDED(a->GetDesc1(&d)) && Narrow(d.Description) == wanted) { pick = a; break; }
    a->Release();
  }
  if (!pick) {
    IDXGIFactory6* f6 = nullptr;
    if (SUCCEEDED(f1->QueryInterface(__uuidof(IDXGIFactory6), reinterpret_cast<void**>(&f6))) && f6) {
      f6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, __uuidof(IDXGIAdapter1),
                                     reinterpret_cast<void**>(&pick));
      f6->Release();
    }
  }
  for (UINT i = 0; !pick && f1->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
    DXGI_ADAPTER_DESC1 d{};
    if (SUCCEEDED(a->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) { pick = a; break; }
    a->Release();
  }
  f1->Release();
  if (!pick) {
    gpu = "no graphics card found";
    return;
  }
  DXGI_ADAPTER_DESC1 d{};
  if (SUCCEEDED(pick->GetDesc1(&d)))
    gpu = std::format("{} (vendor 0x{:04X}, {} MB)", Narrow(d.Description), d.VendorId,
                      static_cast<unsigned long long>(d.DedicatedVideoMemory) >> 20);
  static const struct { D3D_FEATURE_LEVEL fl; const char* name; } kLevels[] = {
      {D3D_FEATURE_LEVEL_12_2, "12_2"}, {D3D_FEATURE_LEVEL_12_1, "12_1"}, {D3D_FEATURE_LEVEL_12_0, "12_0"},
      {D3D_FEATURE_LEVEL_11_1, "11_1"}, {D3D_FEATURE_LEVEL_11_0, "11_0"}};
  level = "none (no Direct3D 12 support)";
  for (const auto& l : kLevels) {
    if (SUCCEEDED(D3D12CreateDevice(pick, l.fl, __uuidof(ID3D12Device), nullptr))) {
      level = l.name;
      break;
    }
  }
  pick->Release();
}
#endif

std::string LogPath() {
  std::string log = rex::cvar::GetFlagByName("log_file");
  return log.empty() ? std::string("the game's log file") : log;
}

}  // namespace

bool RendererOnCommandLine() {
#if defined(_WIN32)
  const wchar_t* cl = GetCommandLineW();
  if (!cl) return false;
  // --svr07_renderer=x, --svr07_renderer x, -svr07_renderer=x
  for (const wchar_t* p = wcsstr(cl, L"svr07_renderer"); p; p = wcsstr(p + 1, L"svr07_renderer")) {
    if (p > cl && p[-1] == L'-' && (p[13] == L'=' || p[13] == L' ' || p[13] == L'\0')) return true;
  }
#endif
  return false;
}

void Fail(int code, const std::string& what) {
  std::string gpu = "unknown", level = "unknown";
#if defined(_WIN32)
  DescribeGpu(gpu, level);
#endif
  const auto exe_dir = rex::filesystem::GetExecutableFolder();
  const std::string log = LogPath();
  REXLOG_ERROR("native: {} | GPU: {} | D3D12 feature level: {} | exit code {}", what, gpu, level, code);
  rex::FlushLogging();

  // For the launcher: one "key: value" per line (launcher/NativeRenderer.cs ReadError).
  {
    std::ofstream f(exe_dir / "native-error.txt", std::ios::binary | std::ios::trunc);
    if (f) {
      f << "code: " << code << "\n"
        << "what: " << what << "\n"
        << "gpu: " << gpu << "\n"
        << "feature_level: " << level << "\n"
        << "log: " << log << "\n";
    }
  }

#if defined(_WIN32)
  char env[8] = {};
  const bool from_launcher = GetEnvironmentVariableA("SVR07_LAUNCHER", env, sizeof(env)) > 0 && env[0] == '1';
  if (!from_launcher) {
    std::string text;
    if (code == kCacheMissing) {
      text = std::format(
          "The game can't start: {}\n\n"
          "What to do:\n"
          "- Start the game with \"SVR07 Launcher.exe\" (it is in the same folder). Its Play button builds the "
          "shader cache from your game files the first time (about a minute), then starts the game.\n"
          "- If that fails too, the launcher says why. Report it with the file native-error.txt from the game folder.\n",
          what);
    } else {
      text = std::format(
          "The game's renderer could not start on this graphics card.\n\n"
          "What failed: {}\n"
          "Graphics card: {}\n"
          "Direct3D 12 feature level: {} (12_0 or newer is needed)\n\n"
          "What to do:\n"
          "- Update your graphics driver (NVIDIA, AMD or Intel website), then start the game again.\n"
          "- On a laptop with two graphics cards: add svr07_native_adapter = \"part of the card's name\" to svr07.toml.\n"
          "- Still failing? Report it on the project's GitHub page (Issues) with native-error.txt and the log ({}).\n",
          what, gpu, level, log);
    }
    MessageBoxW(nullptr, Widen(text).c_str(), L"WWE SmackDown vs. Raw 2007 Recomp", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
  }
#endif
  std::_Exit(code);
}

}  // namespace svr07_renderer_fail
