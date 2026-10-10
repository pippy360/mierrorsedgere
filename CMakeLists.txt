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
    src/assets/level_postprocess.cpp
    src/game/level_script.cpp
    src/physics/collision_world.cpp
    src/physics/parkour_controller.cpp
)

# me_replay: replays a recorded retail run through the controller, headless (tools/retail/README.md).
# Builds on any platform, so it runs on the Windows machine that records the retail game.
find_package(ZLIB REQUIRED)
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

# me_menu: renders front-end frames to PNG, headless (docs/MAIN_MENU_SYSTEM_RE.md).
find_package(Threads REQUIRED)
add_executable(me_menu src/tools/menu_main.cpp ${FRONTEND_SOURCES} ${CORE_SOURCES})
target_link_libraries(me_menu PRIVATE ZLIB::ZLIB Threads::Threads)
if(MINGW)
    target_link_options(me_menu PRIVATE -static)
endif()

# The game itself: everything but the renderer backend is shared between the platforms.
# main.cpp is compiled per target (renderer/renderer.hpp picks the backend from its defines).
set(GAME_SOURCES
    ${CORE_SOURCES}
    src/audio/audio_engine.cpp
    src/cutscene/cutscene_player.cpp
    src/platform/platform.cpp
    src/ui/main_menu.cpp
    ${FRONTEND_SOURCES}
)
add_library(me_game_objs OBJECT ${GAME_SOURCES})

# The OpenGL 4.1 backend (src/renderer/opengl_renderer.cpp): the Linux renderer. It can be built
# on macOS as well, as `mirrorsedge_opengl`, to check it against the Metal backend there.
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
