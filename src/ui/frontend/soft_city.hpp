#pragma once

// The 3D half of the reference renderer: the menu level's city, sky and water.

#include "frontend.hpp"

#include <memory>
#include <vector>

namespace me::fe {

class CityRenderer {
public:
    explicit CityRenderer(const City& city);
    ~CityRenderer();
    CityRenderer(const CityRenderer&) = delete;
    CityRenderer& operator=(const CityRenderer&) = delete;

    // Fills `rgb` (w x h, three floats per pixel, display space) with the view from the frame's camera.
    void render(const Frame& frame, int w, int h, std::vector<float>& rgb);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace me::fe
