cmake_minimum_required(VERSION 3.20)
project(mirrorsedge_macos LANGUAGES CXX OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)

# Optimization & Objective-C ARC flags
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -Wall -Wextra -Wno-unused-parameter")
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

# Source files
set(SOURCES
    src/main.mm
    src/anim/anim_system.cpp
    src/assets/upk_loader.cpp
    src/assets/ue3_props.cpp
    src/assets/package_manager.cpp
    src/assets/texture_loader.cpp
    src/assets/material_system.cpp
    src/assets/ini_config.cpp
    src/audio/audio_engine.cpp
    src/cutscene/cutscene_player.cpp
    src/physics/parkour_controller.cpp
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
