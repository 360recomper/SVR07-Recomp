@echo off
rem L2 shaders (SvR07 native renderer): build the shader tools from native\third_party:
rem   XenosRecomp.exe (UFC patch stack + xenosrecomp-0e.patch) and svr07_shader_cache.exe (native\tools\native\shader_extract),
rem then gather them with dxcompiler.dll + dxil.dll (XenosRecomp's dxc-bin = the DXC the game build links) in
rem   SVR07-Recomp-native\out\native-tools\
rem XenosRecomp is built -O2 -g WITHOUT NDEBUG (see the cmake line): the RR release lesson, Defender flagged the NDEBUG build.
rem Also builds svr07_prelink.exe standalone (no game CMake needed), so make-shader-cache.bat has all tools in one folder.
rem native\third_party\XenosRecomp is a plain copy (no .git), so instead of svr07_fetch_third_party.py --check this only checks
rem that the 0E patch is in (marker kShaderFlagUpgraded0E). Do NOT run the fetch script without --from-dir: it re-clones.
setlocal
set "NATIVE=%~dp0.."
set "ROOT=%~dp0..\.."
set "TP=%NATIVE%\third_party"
set "B=%ROOT%\out\native-tools\build"
set "OUT=%ROOT%\out\native-tools"
rem Run from an x64 Native Tools / Developer prompt (clang, cmake, ninja on PATH); an optional env.bat next to the repo is used if present.
if exist "%ROOT%\..\env.bat" call "%ROOT%\..\env.bat"
findstr /c:"kShaderFlagUpgraded0E" "%TP%\XenosRecomp\XenosRecomp\main.cpp" >nul || ( echo xenosrecomp-0e.patch is not applied in %TP%\XenosRecomp & exit /b 1 )
rem An empty CMAKE_BUILD_TYPE does NOT work: thirdparty/xxHash/cmake_unofficial forces it to Release (cache, FORCE), which
rem appends -O3 -DNDEBUG (RR's own native-tools-build has exactly that). So: Release, with the Release flags replaced.
rem BUILD_SHARED_LIBS=OFF: xxHash's option() puts ON in the cache, and on the 2nd configure fmt becomes fmt.dll (XenosRecomp
rem then dies with 0xC0000135 beside the cache tool). Static fmt/xxHash = one exe + the two DXC DLLs.
cmake -S "%TP%\XenosRecomp" -B "%B%\xenosrecomp" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_RC_COMPILER=llvm-rc "-DCMAKE_C_FLAGS=" "-DCMAKE_CXX_FLAGS=-DREBLUE_RECOMP" "-DCMAKE_C_FLAGS_RELEASE=-O2" "-DCMAKE_CXX_FLAGS_RELEASE=-O2 -g" || exit /b 1
findstr /c:"-DREBLUE_RECOMP -O2 -g" "%B%\xenosrecomp\build.ninja" >nul || ( echo build.ninja lacks -O2 -g & exit /b 1 )
findstr /c:"NDEBUG" "%B%\xenosrecomp\build.ninja" >nul && ( echo build.ninja has NDEBUG & exit /b 1 )
cmake --build "%B%\xenosrecomp" --target XenosRecomp || exit /b 2
cmake -S "%NATIVE%\tools\native\shader_extract" -B "%B%\shader_extract" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_COMPILER=clang++ "-DSVR07_SC_THIRD_PARTY=%TP%" || exit /b 3
cmake --build "%B%\shader_extract" || exit /b 4
rem svr07_prelink.exe standalone (same 3 sources + libs as cmake\native_renderer.cmake's svr07_prelink target, which only
rem exists in the game build with SVR07_NATIVE_HOOKS=ON). Plain clang calls, no CMake project needed.
set "PB=%B%\prelink"
set "RT=%TP%\reblue_thirdparty"
set "XT=%TP%\XenosRecomp\thirdparty"
if not exist "%PB%" mkdir "%PB%"
set "PINC=-I"%NATIVE%\src\reblue" -I"%RT%\zstd" -I"%RT%\miniz" -I"%XT%\xxHash" -I"%XT%\dxc-bin\inc""
set "PDEF=-DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_CRT_SECURE_NO_WARNINGS"
clang -O2 -g %PDEF% %PINC% -c "%RT%\zstd\zstddeclib.c" -o "%PB%\zstddeclib.obj" || exit /b 6
clang -O2 -g %PDEF% %PINC% -c "%XT%\xxHash\xxhash.c" -o "%PB%\xxhash.obj" || exit /b 6
for %%s in ("%RT%\miniz\miniz.cpp" "%NATIVE%\tools\native\prelink_shader_cache.cpp" "%NATIVE%\src\reblue\gpu\shaders\dxc_link.cpp" "%NATIVE%\src\reblue\gpu\shaders\shader_cache_file.cpp") do (
  clang++ -std=c++20 -O2 -g %PDEF% %PINC% -c "%%~s" -o "%PB%\%%~ns.obj" || exit /b 6
)
clang++ -fuse-ld=lld -g "%PB%\zstddeclib.obj" "%PB%\xxhash.obj" "%PB%\miniz.obj" "%PB%\prelink_shader_cache.obj" "%PB%\dxc_link.obj" "%PB%\shader_cache_file.obj" "%XT%\dxc-bin\lib\x64\dxcompiler.lib" -o "%PB%\svr07_prelink.exe" || exit /b 6
if not exist "%OUT%" mkdir "%OUT%"
copy /y "%PB%\svr07_prelink.exe" "%OUT%\" >nul || exit /b 5
copy /y "%B%\xenosrecomp\XenosRecomp\XenosRecomp.exe" "%OUT%\" >nul || exit /b 5
copy /y "%B%\shader_extract\svr07_shader_cache.exe" "%OUT%\" >nul || exit /b 5
copy /y "%TP%\XenosRecomp\thirdparty\dxc-bin\bin\x64\dxcompiler.dll" "%OUT%\" >nul || exit /b 5
copy /y "%TP%\XenosRecomp\thirdparty\dxc-bin\bin\x64\dxil.dll" "%OUT%\" >nul || exit /b 5
echo Shader tools: %OUT%
