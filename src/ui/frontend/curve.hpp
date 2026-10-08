#pragma once

// FInterpCurve, as the Matinee tracks of the menu level store it.

#include "../../math/types.hpp"

#include <cstdint>
#include <vector>

namespace me::fe {

// UE3 EInterpCurveMode.
enum class CurveMode : uint8_t { Linear, CurveAuto, Constant, CurveUser, CurveBreak };

struct CurveKey {
    float t = 0.0f;
    Vec3 v{0.0f, 0.0f, 0.0f};
    Vec3 arrive{0.0f, 0.0f, 0.0f};
    Vec3 leave{0.0f, 0.0f, 0.0f};
    CurveMode mode = CurveMode::Linear;
};

// FInterpCurve<FVector>. A float curve keeps its value in x.
struct Curve {
    std::vector<CurveKey> keys;
    [[nodiscard]] Vec3 eval(float t, const Vec3& fallback = Vec3{0.0f, 0.0f, 0.0f}) const;
};

}  // namespace me::fe
