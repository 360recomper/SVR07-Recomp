<p align="center">
  <img src="docs/images/logo.png" alt="WWE SmackDown vs. Raw 2007 — reCompiled" width="640">
</p>

# SVR07-Recomp (native renderer)

A static recompilation of **WWE SmackDown vs. Raw 2007** (Xbox 360) that runs natively on Windows.
This fork draws the game with a **native Direct3D 12 renderer** (no emulated Xbox 360 GPU).
Download the game build from the [Releases page](../../releases).

**No game files are in this repository or in any release.** You need your own legally obtained copy of the game.
The shader cache the renderer needs is built on your PC from your own game files.

## v1.3
- **Native D3D12 renderer** (native only). Steady 60 fps; entrances also at 60 (`entrance_60fps`).
- **New F4 settings menu** in game.
- **Launcher** (`launcher/`): game setup from your own disc image, settings, saves, shader cache build,
  picks the AVX2 exe on CPUs that support it.
- Clear error messages when the graphics card or driver can't run the renderer.

## Layout
| Folder | What |
|---|---|
| `src/`, `config/`, `svr07_manifest.toml` | game hooks, app, codegen config |
| `cmake/native_renderer.cmake` | compiles the native renderer into the game exe |
| `native/` | the native renderer (GPL-3; see `native/README.md`, `native/UPSTREAM.txt`) |
| `sdk-patch/` | all changes to ReXGlue SDK `v0.10.0` this build uses |
| `launcher/` | the launcher (C#, .NET) |
| `tools/` | roster site generator, PSO seed tool, hook audit |

## Building (short)
Windows, Visual Studio 2022 Build Tools (C++), LLVM clang, CMake 3.25+, Ninja, Python 3, Git; run from an
x64 Developer prompt with clang/cmake/ninja on PATH.

1. **SDK:** clone [rexglue-sdk](https://github.com/rexglue/rexglue-sdk) at tag `v0.10.0`,
   `git apply <this repo>/sdk-patch/rexglue-sdk-v0.10-svr07-native.patch`, then
   ```bat
   cmake --preset win-amd64 -DREXGLUE_USE_VULKAN=ON
   cmake --build --preset win-amd64-relwithdebinfo --target install
   cmake --build --preset win-amd64-release --target install
   ```
2. **Third party:** in this repo, `python -I native/tools/svr07_fetch_third_party.py`
   (clones plume and XenosRecomp at their pins and applies `native/patches/`).
3. **Game files:** put your extracted game in `assets/` (`assets/default.xex`, `assets/pac/`, ...).
4. **Codegen + build** (`SDK` = the SDK's `out/install/win-amd64`):
   ```bat
   "%SDK%\bin\rexglue.exe" codegen svr07_manifest.toml
   cmake --preset win-amd64-relwithdebinfo -DCMAKE_PREFIX_PATH="%SDK%" -Drexglue_DIR="%SDK%\lib\cmake\rexglue" ^
     -DSVR07_NATIVE_RENDERER=ON -DSVR07_NATIVE_HOOKS=ON
   cmake --build --preset win-amd64-relwithdebinfo
   ```
   Output: `out/build/win-amd64-relwithdebinfo/svr07.exe` (plus `svr07_prelink.exe`). The AVX2 exe is the same
   build with `-march=x86-64-v3` added to the compiler flags.
5. **Shader tools:** `native\tools\build-shader-tools.bat` builds `XenosRecomp.exe`, `svr07_shader_cache.exe`
   and `svr07_prelink.exe` into `out\native-tools`; the launcher runs them to build the shader cache.
6. **Launcher:** `dotnet publish launcher/SVR07Launcher.csproj -c Release`.

The recompiled code in `generated/default/` is made from your own game by codegen and is never committed.

## Credits
- **HollywoodAkeem**: the original [SVR07-Recomp](https://github.com/HollywoodAkeem/SVR07-Recomp) (BSD 3-Clause,
  `licenses/SVR07-Recomp-BSD-3.txt`). All credit for the recompilation goes to them.
- **nefariousjosiah**: the native renderer of the WWE SmackDown vs. Raw 2008 / 2010 recomps, which this renderer
  is adapted from (GPL-3.0).
- **re:Blue** ([zolaware/reblue](https://github.com/zolaware/reblue)): the renderer core (BSD 3-Clause, `LICENSE.reblue`).
- **[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk)** (and Xenia, which its runtime derives from).
- **plume**, **XenosRecomp**, **DirectX Shader Compiler**, **AMD FidelityFX**: see `THIRD-PARTY.md`.
- **YucaGuapo**: this port (native renderer for SvR 2007, launcher, SDK v0.10 port, frame pacing, F4 menu).

Full list: `CREDITS.md`, `THIRD-PARTY.md`, `licenses/`.

## License
GPL-3.0 (`LICENSE`), because the native renderer is adapted from GPL-3.0 code. Code from other projects keeps its
own licence: the original recomp is BSD 3-Clause (`licenses/SVR07-Recomp-BSD-3.txt`), re:Blue parts BSD 3-Clause
(`LICENSE.reblue`); see `THIRD-PARTY.md`.

## Legal
Unofficial fan project, not affiliated with, endorsed by, or sponsored by Take-Two Interactive, 2K, THQ, Yuke's,
or WWE. All trademarks remain the property of their respective owners. See [DISCLAIMER.md](DISCLAIMER.md).
