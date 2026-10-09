#pragma once

// -----------------------------------------------------------------------------
// What the level says about the picture after the scene is drawn: its
// post-process settings and its height fog.
//
// The game's post-process chain (FX_PostProcess.FX_PostProcess) is, in order:
//   TdDirectionalHazePostProcess   glow towards the sun, by depth         (world settings)
//   DOFAndBloomEffect              bloom of what is brighter than 1       (its own: scale 0.15, kernel 16, no blur)
//   TdToneMappingPostProcess       exposure, shadows / midtones / highlights, desaturation,
//                                  gamma, and three 16-segment colour curves   (world settings)
//   material effects               health, melee, reaction time...        (off until triggered)
//   TdMotionBlurPostProcess        radial blur when she runs              (its own: 0.6 from 400 uu/s)
// "World settings" are WorldInfo.DefaultPostProcessSettings of the persistent level, a
// PostProcessSettings struct DICE extended with the haze, the exposure and the curves. Only
// what differs from the struct's defaults is serialized.
//
// Height fog is the HeightFog actors of every loaded level, at most four layers.
// -----------------------------------------------------------------------------

#include "../math/types.hpp"

#include <memory>
#include <vector>

namespace me {

class UPKPackage;

void extract_level_postprocess(const UPKPackage& persistent_level, const std::vector<std::shared_ptr<UPKPackage>>& packages,
                               LevelScene& out);

}  // namespace me
