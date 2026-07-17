@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
set "PATH=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
cd /d C:\Repos\Palworld-Mods\PalCrosschat
if not exist build (
  cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Game__Shipping__Win64 -DUE4SS_ROOT=C:/Repos/RE-UE4SS . || exit /b 1
)
cmake --build build --target PalCrosschat || exit /b 1
echo BUILD_OK: dist\PalCrosschat\dlls\main.dll
