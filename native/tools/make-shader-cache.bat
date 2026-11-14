@echo off
rem L2 shaders (SvR07 native renderer): build shader_cache.bin + shader_cache_linked.bin from the game's own files,
rem the way a launcher would on a player's machine:
rem   svr07_shader_cache.exe <game data> <cache dir>   (XenosRecomp.exe, svr07_prelink.exe, dxcompiler.dll, dxil.dll beside it)
rem   make-shader-cache.bat [cache dir] [game data dir]
rem Defaults: SVR07-Recomp-native\out\shader-cache, SVR07-Recomp-native\assets (junction to the extracted game).
rem Needs build-shader-tools.bat first (all four tools + DLLs in out\native-tools). CPU only, no game run.
rem The cache is derived from the game's files: never commit or ship it. Copy it beside the game exe (L4/L5).
setlocal
set "ROOT=%~dp0..\.."
set "CACHE=%~1"
if "%CACHE%"=="" set "CACHE=%ROOT%\out\shader-cache"
set "DATA=%~2"
if "%DATA%"=="" set "DATA=%ROOT%\assets"
set "TOOLS=%ROOT%\out\native-tools"
for %%f in (svr07_shader_cache.exe XenosRecomp.exe svr07_prelink.exe dxcompiler.dll dxil.dll) do (
  if not exist "%TOOLS%\%%f" ( echo %TOOLS%\%%f missing: native\tools\build-shader-tools.bat first & exit /b 1 )
)
"%TOOLS%\svr07_shader_cache.exe" "%DATA%" "%CACHE%" %3 %4 %5 || exit /b 2
echo Shader cache: %CACHE%\shader_cache.bin
