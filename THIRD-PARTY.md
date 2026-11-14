# Third-party components

This build as a whole is licensed under the GNU GPL v3.0 (`LICENSE`), because its native renderer is
adapted from GPL-3.0 code (nefariousjosiah's SmackDown vs. Raw recomp renderer). Code from other projects
keeps its own licence.

No game files and no game shaders are in this download or in this repository. The shader cache (`shader_cache.bin`,
`shader_cache_linked.bin`) is built on your own PC from your own game files.

| Component | Use | Licence | Text |
|---|---|---|---|
| SVR07-Recomp (HollywoodAkeem) | base recomp | BSD 3-Clause | `licenses/SVR07-Recomp-BSD-3.txt`, `DISCLAIMER.md` |
| SmackDown vs. Raw recomp native renderer (nefariousjosiah) | renderer, adapted | GPL-3.0 | `LICENSE` |
| [re:Blue](https://github.com/zolaware/reblue) renderer core (zolaware) | renderer, adapted | BSD 3-Clause | `LICENSE.reblue` |
| [plume](https://github.com/zolaware/plume) (renderbag and contributors) | rendering hardware interface | MIT | `licenses/plume-MIT.txt` |
| [reblue-XenosRecomp](https://github.com/zolaware/reblue-XenosRecomp) (hedge-dev and contributors) | `XenosRecomp.exe` | MIT | `licenses/XenosRecomp-MIT.md` |
| [DirectX Shader Compiler](https://github.com/microsoft/DirectXShaderCompiler) | `dxcompiler.dll`, `dxil.dll` | NCSA / Microsoft terms | `licenses/DirectXShaderCompiler-LICENSE.txt` |
| zstd (decoder) | shader cache | BSD | `licenses/zstd-BSD.txt` |
| miniz | shader cache | Unlicense | `licenses/miniz.txt` |
| stb, disruptorplus, renderdoc_app header | renderer third party | see texts | `licenses/` |
| [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) (portions from Xenia) | recompiler and runtime | BSD 3-Clause | `licenses/ReXGlue-SDK-LICENSE.txt` |
| AMD FidelityFX SDK | CAS sharpening | MIT | `licenses/AMD-FidelityFX-SDK-MIT.txt` |
| FFmpeg | video playback | LGPL v2.1+ | `licenses/FFmpeg-*` |
| SDL 3, Dear ImGui, fmt, spdlog, toml++, CLI11, inja, utfcpp, xxHash, simde, o1heap, aes_128, glslang, SPIRV-Headers/Tools, Vulkan-Headers/Loader, VulkanMemoryAllocator | runtime libraries | see texts | `licenses/` |

This repository is the source code of that build (GPL-3.0). The native renderer is in `native/`, the ReXGlue SDK changes in `sdk-patch/`.
