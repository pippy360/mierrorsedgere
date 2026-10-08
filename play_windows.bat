@echo off
rem Mirror's Edge native Windows engine: configure, build and launch (docs/WINDOWS_PORT.md).
rem Needs MSYS2 with the UCRT64 packages:
rem   pacman -S mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,pkgconf,SDL2,openal,libvorbis,ffmpeg,zlib}
rem Arguments are passed to the game, e.g.  play_windows.bat --chapter 1
setlocal

if not defined MSYS2_ROOT set "MSYS2_ROOT=C:\msys64"
set "UCRT64_BIN=%MSYS2_ROOT%\ucrt64\bin"
if not exist "%UCRT64_BIN%\g++.exe" (
    echo [Build] No MinGW-w64 toolchain at %UCRT64_BIN%.
    echo         Install MSYS2 from https://www.msys2.org and the packages listed in docs\WINDOWS_PORT.md,
    echo         or set MSYS2_ROOT to where MSYS2 is installed.
    exit /b 1
)
set "PATH=%UCRT64_BIN%;%PATH%"

cd /d "%~dp0"
echo === Mirror's Edge Native Windows Engine ===

if not exist "build-win\build.ninja" (
    echo [CMake] Configuring build...
    cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
)

echo [CMake] Building mirrorsedge_windows...
cmake --build build-win --target mirrorsedge_windows || exit /b 1

echo [Game] Launching...
"build-win\mirrorsedge_windows.exe" %*
