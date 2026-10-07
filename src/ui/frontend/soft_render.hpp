#pragma once

// -----------------------------------------------------------------------------
// The reference renderer for a front-end Frame: a CPU rasteriser that draws the
// menu level's city behind the 2D draw list and returns RGBA8. It needs no GPU,
// so it produces the same picture on every platform; that is what lets the menu
// be compared with retail on the machine that has retail installed.
// -----------------------------------------------------------------------------

#include "frontend.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace me::fe {

class SoftRenderer {
public:
    explicit SoftRenderer(const Assets& assets);
    ~SoftRenderer();
    SoftRenderer(const SoftRenderer&) = delete;
    SoftRenderer& operator=(const SoftRenderer&) = delete;

    // Draws `frame` into `rgba` (frame.width x frame.height, top row first).
    void render(const Frame& frame, std::vector<uint8_t>& rgba);

    // Off: the 2D layer over a flat sky colour (for looking at the UI alone).
    void set_background(bool on) { background_ = on; }
    void set_ui(bool on) { ui_ = on; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    const Assets& assets_;
    bool background_ = true;
    bool ui_ = true;
};

// RGBA8, top row first.
bool write_png(const std::string& path, int width, int height, const uint8_t* rgba);

}  // namespace me::fe
