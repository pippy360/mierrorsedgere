#include "soft_city.hpp"

namespace me::fe {

struct CityRenderer::Impl {
    explicit Impl(const City& c) : city(c) {}
    const City& city;
};

CityRenderer::CityRenderer(const City& city) : impl_(std::make_unique<Impl>(city)) {}
CityRenderer::~CityRenderer() = default;

void CityRenderer::render(const Frame&, int, int, std::vector<float>&) {}

}  // namespace me::fe
