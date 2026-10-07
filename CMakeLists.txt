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
    /opt/homebrew/include
    /opt/homebrew/include/SDL2
)

# Link directories
link_directories(
    /opt/homebrew/lib
)

# The asset loader and the parkour controller: plain C++, no window, GPU or audio.
set(CORE_SOURCES
    src/anim/anim_system.cpp
    src/assets/upk_loader.cpp
    src/assets/ue3_props.cpp
    src/assets/package_manager.cpp
    src/assets/texture_loader.cpp
    src/assets/material_system.cpp
    src/assets/ini_config.cpp
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

# The front end ("Press Any Key" and the main menu): its state machine and the CPU reference
# renderer. Plain C++, so it builds wherever the core does.
set(FRONTEND_SOURCES
    src/ui/frontend/frontend_assets.cpp
    src/ui/frontend/frontend_city.cpp
    src/ui/frontend/frontend.cpp
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

if(APPLE)

# Source files
set(SOURCES
    src/main.mm
    ${CORE_SOURCES}
    src/audio/audio_engine.cpp
    src/cutscene/cutscene_player.cpp
    src/ui/main_menu.cpp
    src/renderer/metal_renderer.mm
)

# Executable target
add_executable(mirrorsedge_macos ${SOURCES})

# Target libraries & frameworks
target_link_libraries(mirrorsedge_macos PRIVATE
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
    "-framework Metal"
    "-framework QuartzCore"
    "-framework Foundation"
    "-framework AppKit"
    "-framework CoreGraphics"
    "-framework ImageIO"
    "-framework OpenAL"
)

endif()
