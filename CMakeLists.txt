cmake_minimum_required(VERSION 3.20)
project(mirrorsedge_macos LANGUAGES CXX)
if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)

# Optimization & Objective-C ARC flags
if(NOT MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -Wall -Wextra -Wno-unused-parameter")
endif()
set(CMAKE_OBJCXX_FLAGS "${CMAKE_OBJCXX_FLAGS} -O2 -fobjc-arc -Wall -Wextra -Wno-unused-parameter")

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(APPLE)
    include_directories(
        /opt/homebrew/include
        /opt/homebrew/include/SDL2
    )

    # Link directories
    link_directories(
        /opt/homebrew/lib
    )
endif()

# The asset loader and the parkour controller: plain C++, no window, GPU or audio.
set(CORE_SOURCES
    src/anim/anim_system.cpp
    src/anim/fp_anim.cpp
    src/anim/fp_director.cpp
    src/anim/fp_pose.cpp
    src/anim/fp_controls.cpp
    src/assets/upk_loader.cpp
    src/assets/ue3_props.cpp
    src/assets/package_manager.cpp
    src/assets/texture_loader.cpp
    src/assets/material_system.cpp
    src/assets/ini_config.cpp
    src/assets/level_intro.cpp
    src/assets/level_lightmaps.cpp
    src/assets/level_decals.cpp
    src/assets/level_lensflares.cpp
    src/assets/level_lights.cpp
    src/assets/level_particles.cpp
    src/assets/level_impacts.cpp
    src/assets/level_postprocess.cpp
    src/game/level_script.cpp
    src/physics/collision_world.cpp
    src/physics/parkour_controller.cpp
)

# me_replay: replays a recorded retail run through the controller, headless (tools/retail/README.md).
# Builds on any platform, so it runs on the Windows machine that records the retail game.
find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)

# The front end ("Press Any Key" and the main menu): its state machine and the CPU reference
# renderer. Plain C++, so it builds wherever the core does.
set(FRONTEND_SOURCES
    src/ui/frontend/frontend_assets.cpp
    src/ui/frontend/frontend_city.cpp
    src/ui/frontend/frontend.cpp
    src/ui/frontend/kismet.cpp
    src/ui/frontend/ui_scene.cpp
    src/ui/frontend/frontend_menus.cpp
    src/ui/frontend/soft_city.cpp
    src/ui/frontend/soft_render.cpp
)

# The game itself: everything but the renderer backend is shared between the platforms.
# main.cpp is compiled per target (renderer/renderer.hpp picks the backend from its defines).
set(GAME_SOURCES
    ${CORE_SOURCES}
    src/audio/audio_engine.cpp
    src/cutscene/cutscene_player.cpp
    src/platform/platform.cpp
    src/platform/android_log.cpp
    src/platform/touch_controls.cpp
    src/ui/main_menu.cpp
    ${FRONTEND_SOURCES}
)
add_library(me_game_objs OBJECT ${GAME_SOURCES})

# The OpenGL 4.1 backend (src/renderer/opengl_renderer.cpp): the Linux renderer. It can be built
# on macOS as well, as `mirrorsedge_opengl`, to check it against the Metal backend there. On
# Android it is built with ME_GLES=1 and runs on OpenGL ES 3.0 (docs/ANDROID_PORT.md).
set(OPENGL_RENDERER_SOURCES
    src/renderer/opengl_renderer.cpp
    src/renderer/msl_to_glsl.cpp
    src/renderer/gl/gl_api.cpp
)
# glcorearb.h includes <KHR/khrplatform.h>; both are vendored under src/renderer/gl.
set(OPENGL_RENDERER_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/gl)
if(APPLE OR WIN32)
    option(ME_OPENGL "Also build mirrorsedge_opengl, the OpenGL (Linux) backend, on macOS and Windows" ON)
endif()

# The headless tools: desktop only (me_glsl needs the system's SDL2; the others are of no use on a
# phone).
if(NOT ANDROID)

add_executable(me_replay src/tools/replay_main.cpp ${CORE_SOURCES})
target_link_libraries(me_replay PRIVATE ZLIB::ZLIB)
if(MINGW)
    # no libstdc++ / libgcc DLLs to ship next to it
    target_link_options(me_replay PRIVATE -static)
endif()

# me_anim: runs the first-person animation tree over a recorded retail run and prints what it
# plays, to be compared with what retail played (docs/FIRST_PERSON_ANIMATION_RE.md).
add_executable(me_anim src/tools/anim_main.cpp ${CORE_SOURCES})
target_link_libraries(me_anim PRIVATE ZLIB::ZLIB)
if(MINGW)
    target_link_options(me_anim PRIVATE -static)
endif()

# me_menu: renders front-end frames to PNG, headless (docs/MAIN_MENU_SYSTEM_RE.md).
add_executable(me_menu src/tools/menu_main.cpp ${FRONTEND_SOURCES} ${CORE_SOURCES})
target_link_libraries(me_menu PRIVATE ZLIB::ZLIB Threads::Threads)
if(MINGW)
    target_link_options(me_menu PRIVATE -static)
endif()

# me_glsl: translates every built-in and material shader to GLSL and compiles it on the GPU
# (docs/LINUX_PORT.md). Builds wherever SDL2 is available.
add_executable(me_glsl src/tools/glsl_main.cpp src/renderer/msl_to_glsl.cpp src/platform/platform.cpp ${CORE_SOURCES})
target_link_libraries(me_glsl PRIVATE ZLIB::ZLIB Threads::Threads)
if(APPLE)
    target_link_libraries(me_glsl PRIVATE SDL2)
elseif(WIN32)
    find_package(SDL2 CONFIG REQUIRED)
    target_link_libraries(me_glsl PRIVATE SDL2::SDL2)
    target_compile_definitions(me_glsl PRIVATE SDL_MAIN_HANDLED)
else()
    find_package(SDL2 REQUIRED)
    target_link_libraries(me_glsl PRIVATE SDL2::SDL2 ${CMAKE_DL_LIBS})
endif()

endif()  # NOT ANDROID

if(APPLE)

# Executable target
add_executable(mirrorsedge_macos src/main.cpp $<TARGET_OBJECTS:me_game_objs> src/renderer/metal_renderer.mm)

# Target libraries & frameworks
set(ME_MACOS_LIBS
    SDL2
    avformat
    avcodec
    swscale
    swresample
    avutil
    vorbisfile
    vorbis
    ogg
    z
    "-framework Foundation"
    "-framework AppKit"
    "-framework CoreGraphics"
    "-framework ImageIO"
    "-framework OpenAL"
)
target_link_libraries(mirrorsedge_macos PRIVATE
    ${ME_MACOS_LIBS}
    "-framework Metal"
    "-framework QuartzCore"
)

if(ME_OPENGL)
    add_executable(mirrorsedge_opengl src/main.cpp $<TARGET_OBJECTS:me_game_objs> ${OPENGL_RENDERER_SOURCES})
    target_compile_definitions(mirrorsedge_opengl PRIVATE ME_RENDERER_OPENGL=1 GL_SILENCE_DEPRECATION=1)
    target_include_directories(mirrorsedge_opengl PRIVATE ${OPENGL_RENDERER_INCLUDE_DIR})
    # OpenGL itself is loaded through SDL_GL_GetProcAddress; no framework to link.
    target_link_libraries(mirrorsedge_opengl PRIVATE ${ME_MACOS_LIBS})
endif()

elseif(WIN32)

# mirrorsedge_windows: the same game on Direct3D 11 (src/renderer/d3d11_renderer.cpp).
# Built with MinGW-w64 against MSYS2's UCRT64 packages (docs/WINDOWS_PORT.md):
#   pacman -S mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,pkgconf,SDL2,openal,libvorbis,ffmpeg,zlib}
find_package(SDL2 CONFIG REQUIRED)
find_package(OpenAL CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(VORBISFILE REQUIRED IMPORTED_TARGET vorbisfile)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libswscale libswresample libavutil)

add_executable(mirrorsedge_windows
    src/main.cpp
    $<TARGET_OBJECTS:me_game_objs>
    src/renderer/d3d11_renderer.cpp
    src/renderer/msl_to_hlsl.cpp
)
target_link_libraries(mirrorsedge_windows PRIVATE
    SDL2::SDL2
    OpenAL::OpenAL
    PkgConfig::VORBISFILE
    PkgConfig::FFMPEG
    ZLIB::ZLIB
    Threads::Threads
    d3d11
    dxgi
    d3dcompiler
    dxguid
)

# mirrorsedge_opengl: the OpenGL (Linux) backend on Windows, to check it against the Direct3D one on
# the machine that has the retail assets (docs/LINUX_PORT.md). Same command line; it finds its DLLs
# next to mirrorsedge_windows.exe.
if(ME_OPENGL)
    add_executable(mirrorsedge_opengl src/main.cpp $<TARGET_OBJECTS:me_game_objs> ${OPENGL_RENDERER_SOURCES})
    target_compile_definitions(mirrorsedge_opengl PRIVATE ME_RENDERER_OPENGL=1)
    target_include_directories(mirrorsedge_opengl PRIVATE ${OPENGL_RENDERER_INCLUDE_DIR})
    target_link_libraries(mirrorsedge_opengl PRIVATE
        SDL2::SDL2
        OpenAL::OpenAL
        PkgConfig::VORBISFILE
        PkgConfig::FFMPEG
        ZLIB::ZLIB
        Threads::Threads
    )
endif()

# The DLLs the game loads (SDL2, OpenAL Soft, Vorbis, FFmpeg and what they need) are copied next to the
# executable, so it starts from Explorer or any shell without MSYS2 on PATH.
option(ME_BUNDLE_RUNTIME_DLLS "Copy the game's runtime DLLs next to mirrorsedge_windows.exe" ON)
if(ME_BUNDLE_RUNTIME_DLLS)
    get_filename_component(ME_COMPILER_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    add_custom_command(TARGET mirrorsedge_windows POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -DME_EXECUTABLE=$<TARGET_FILE:mirrorsedge_windows>
            -DME_SEARCH_DIRS=${ME_COMPILER_BIN_DIR}
            -DME_OBJDUMP=${CMAKE_OBJDUMP}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/bundle_runtime_dlls.cmake
        VERBATIM
    )
endif()

elseif(ANDROID)

# libmain.so: the game on OpenGL ES 3.0, loaded by SDL's Java activity (android/, docs/ANDROID_PORT.md).
# Built by Gradle's externalNativeBuild (android/app/build.gradle) with the NDK toolchain, or by
# hand with -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a.
# The third-party libraries are compiled from source, fetched by android/fetch_deps.sh into
# android/third_party (ME_ANDROID_THIRD_PARTY): SDL2 as a shared library next to libmain.so, the
# rest static inside it. Neither GLESv3 nor EGL is linked: the renderer resolves GL through
# SDL_GL_GetProcAddress, as on Linux.
enable_language(C)
set(ME_ANDROID_THIRD_PARTY "${CMAKE_CURRENT_SOURCE_DIR}/android/third_party" CACHE PATH
    "Where android/fetch_deps.sh unpacked SDL2, openal-soft, libogg and libvorbis")
foreach(dep SDL2 openal-soft libogg libvorbis)
    if(NOT EXISTS "${ME_ANDROID_THIRD_PARTY}/${dep}/CMakeLists.txt")
        message(FATAL_ERROR "${ME_ANDROID_THIRD_PARTY}/${dep} is missing: run android/fetch_deps.sh first")
    endif()
endforeach()
# CMake 4 refuses projects whose cmake_minimum_required is older than 3.5 (SDL2's is 3.0) unless told
# which policies to assume for them.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# --- SDL2 (shared: libSDL2.so ships in the APK and SDLActivity loads it before libmain.so) ---
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(SDL2_DISABLE_UNINSTALL ON CACHE BOOL "" FORCE)
add_subdirectory("${ME_ANDROID_THIRD_PARTY}/SDL2" "${CMAKE_BINARY_DIR}/third_party/SDL2" EXCLUDE_FROM_ALL)
# The game includes <SDL2/SDL.h> (the system packages' layout); the source tree has include/SDL.h,
# so an include directory with an SDL2 link to it is made.
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/sdl2_include")
file(CREATE_LINK "${ME_ANDROID_THIRD_PARTY}/SDL2/include" "${CMAKE_BINARY_DIR}/sdl2_include/SDL2" SYMBOLIC)
set(ME_SDL2_INCLUDE_DIR "${CMAKE_BINARY_DIR}/sdl2_include")
# Every game object (me_game_objs is declared above the branch) needs the SDL headers.
target_include_directories(me_game_objs PRIVATE ${ME_SDL2_INCLUDE_DIR})
target_compile_definitions(me_game_objs PRIVATE ME_NO_FFMPEG=1)

# --- OpenAL Soft (static, OpenSL ES backend) ---
set(LIBTYPE STATIC)
set(ALSOFT_UTILS OFF CACHE BOOL "" FORCE)
set(ALSOFT_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_CONFIG OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_HRTF_DATA OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_AMBDEC_PRESETS OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_UTILS OFF CACHE BOOL "" FORCE)
set(ALSOFT_UPDATE_BUILD_VERSION OFF CACHE BOOL "" FORCE)
set(ALSOFT_BACKEND_OPENSL ON CACHE BOOL "" FORCE)
set(ALSOFT_REQUIRE_OPENSL ON CACHE BOOL "" FORCE)
set(ALSOFT_BACKEND_OBOE OFF CACHE BOOL "" FORCE)
set(ALSOFT_BACKEND_WAVE OFF CACHE BOOL "" FORCE)
set(ALSOFT_RTKIT OFF CACHE BOOL "" FORCE)
add_subdirectory("${ME_ANDROID_THIRD_PARTY}/openal-soft" "${CMAKE_BINARY_DIR}/third_party/openal-soft" EXCLUDE_FROM_ALL)

# --- libogg / libvorbis (static; their own CMake wants a found Ogg, so plain rules over the sources) ---
set(ME_OGG_DIR "${ME_ANDROID_THIRD_PARTY}/libogg")
set(ME_VORBIS_DIR "${ME_ANDROID_THIRD_PARTY}/libvorbis")
set(INCLUDE_INTTYPES_H 1)
set(INCLUDE_STDINT_H 1)
set(INCLUDE_SYS_TYPES_H 1)
set(SIZE16 int16_t)
set(USIZE16 uint16_t)
set(SIZE32 int32_t)
set(USIZE32 uint32_t)
set(SIZE64 int64_t)
set(USIZE64 uint64_t)
configure_file("${ME_OGG_DIR}/include/ogg/config_types.h.in" "${CMAKE_BINARY_DIR}/ogg_include/ogg/config_types.h" @ONLY)
add_library(ogg STATIC "${ME_OGG_DIR}/src/bitwise.c" "${ME_OGG_DIR}/src/framing.c")
target_include_directories(ogg PUBLIC "${ME_OGG_DIR}/include" "${CMAKE_BINARY_DIR}/ogg_include")
set(ME_VORBIS_SOURCES
    mdct.c smallft.c block.c envelope.c window.c lsp.c lpc.c analysis.c synthesis.c psy.c info.c
    floor1.c floor0.c res0.c mapping0.c registry.c codebook.c sharedbook.c lookup.c bitrate.c
)
list(TRANSFORM ME_VORBIS_SOURCES PREPEND "${ME_VORBIS_DIR}/lib/")
add_library(vorbis STATIC ${ME_VORBIS_SOURCES})
target_include_directories(vorbis PUBLIC "${ME_VORBIS_DIR}/include" PRIVATE "${ME_VORBIS_DIR}/lib")
target_link_libraries(vorbis PUBLIC ogg)
add_library(vorbisfile STATIC "${ME_VORBIS_DIR}/lib/vorbisfile.c")
target_include_directories(vorbisfile PUBLIC "${ME_VORBIS_DIR}/include" PRIVATE "${ME_VORBIS_DIR}/lib")
target_link_libraries(vorbisfile PUBLIC vorbis)
foreach(t ogg vorbis vorbisfile)
    target_compile_options(${t} PRIVATE -w)   # their warnings are theirs
endforeach()
# The game objects include <AL/al.h> and <vorbis/vorbisfile.h>: the headers come with the targets.
target_link_libraries(me_game_objs PRIVATE OpenAL vorbisfile)

# --- the game ---
add_library(main SHARED src/main.cpp $<TARGET_OBJECTS:me_game_objs> ${OPENGL_RENDERER_SOURCES})
target_compile_definitions(main PRIVATE ME_RENDERER_OPENGL=1 ME_GLES=1 ME_NO_FFMPEG=1)
target_include_directories(main PRIVATE ${OPENGL_RENDERER_INCLUDE_DIR} ${ME_SDL2_INCLUDE_DIR})
set(ME_ANDROID_SDL_TARGETS SDL2::SDL2)
if(TARGET SDL2::SDL2main)
    list(APPEND ME_ANDROID_SDL_TARGETS SDL2::SDL2main)
endif()
target_link_libraries(main PRIVATE
    ${ME_ANDROID_SDL_TARGETS}
    OpenAL
    vorbisfile
    vorbis
    ogg
    ZLIB::ZLIB
    Threads::Threads
    log
    android
    ${CMAKE_DL_LIBS}
)
# SDLActivity dlsym()s SDL_main out of libmain.so: keep it exported whatever the visibility flags.
target_link_options(main PRIVATE "-Wl,--undefined=SDL_main")
# libSDL2.so is built with the app (it is not in `all` because of EXCLUDE_FROM_ALL above).
add_dependencies(main SDL2)
# 16 KB pages: Android 15+ warns at launch about a shared library whose LOAD segments are aligned
# to 4 KB, and a device booted with 16 KB pages will not load it at all. The NDK's libc++_shared.so
# is already aligned; libmain.so and SDL2's own library are linked so here (ANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES
# covers only targets the NDK toolchain file saw before SDL's CMake set its own flags).
foreach(t main SDL2)
    target_link_options(${t} PRIVATE "-Wl,-z,max-page-size=16384" "-Wl,-z,common-page-size=16384")
endforeach()

else()

# mirrorsedge_linux: the same game on OpenGL 4.1 core (docs/LINUX_PORT.md). Against the
# distribution's packages, e.g. on Debian / Ubuntu:
#   apt install g++ cmake ninja-build pkg-config libsdl2-dev libopenal-dev libvorbis-dev \
#               libavformat-dev libavcodec-dev libswscale-dev libswresample-dev libavutil-dev zlib1g-dev
find_package(SDL2 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(OPENAL REQUIRED IMPORTED_TARGET openal)
pkg_check_modules(VORBISFILE REQUIRED IMPORTED_TARGET vorbisfile)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libswscale libswresample libavutil)

add_executable(mirrorsedge_linux src/main.cpp $<TARGET_OBJECTS:me_game_objs> ${OPENGL_RENDERER_SOURCES})
target_compile_definitions(mirrorsedge_linux PRIVATE ME_RENDERER_OPENGL=1)
target_include_directories(mirrorsedge_linux PRIVATE ${OPENGL_RENDERER_INCLUDE_DIR})
# OpenGL itself is loaded through SDL_GL_GetProcAddress; libGL is not linked.
target_link_libraries(mirrorsedge_linux PRIVATE
    SDL2::SDL2
    PkgConfig::OPENAL
    PkgConfig::VORBISFILE
    PkgConfig::FFMPEG
    ZLIB::ZLIB
    Threads::Threads
    ${CMAKE_DL_LIBS}
)

endif()
