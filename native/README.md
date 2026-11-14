# native/ - SvR07 native renderer

The native Direct3D 12 renderer of the SvR07 recomp (v1.3 and later). Licensed GPL-3 (`LICENSE`); re:Blue-derived
parts: `LICENSE.reblue`; third-party notices: `THIRD-PARTY.md`; where the code came from: `UPSTREAM.txt`.

- `src/` - the renderer (D3D12 through plume, shader translation through XenosRecomp, game hooks).
- `patches/` - our patches to the two fetched third-party libraries (plume, XenosRecomp).
- `third_party/` - small vendored pieces and licence texts. plume and XenosRecomp are not committed:
  `python tools/svr07_fetch_third_party.py` clones them at their pinned commits and applies `patches/`.
- `tools/` - the fetch script and the shader-cache tools (`svr07_shader_cache`, `svr07_prelink`,
  `build-shader-tools.bat`, `make-shader-cache.bat`).

No game data and no game shaders are in here. The shader cache is built on the player's PC from their own game files.
