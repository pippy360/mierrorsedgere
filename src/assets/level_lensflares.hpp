#pragma once

// -----------------------------------------------------------------------------
// The level's lens flares: its LensFlareSource actors and the LensFlare templates
// they name (docs/RENDERING_RE.md, "Lens flares"; renderer/lens_flare.hpp draws them).
//
// A template is a list of camera-facing quads ("reflections") strung along the line
// from the source's place on the screen through the screen's centre. Everything about
// a quad that varies is a raw distribution, which the cooked data carries as a lookup
// table: [min, max, entries...] with a start and a scale for the input
// (FRawDistribution::GetEntry, 0x00d72100 in the game's executable). The templates are
// cooked into the level packages that place them.
//
// A source whose component has bAutoActivate off waits for Kismet to switch it on; it
// is kept, inactive. A hidden source is kept hidden.
// -----------------------------------------------------------------------------

#include "../math/types.hpp"

#include <memory>
#include <string>
#include <vector>

namespace me {

class UPKPackage;

// Fills scene.lens_flare_templates and scene.lens_flares. The elements' materials join
// `material_paths` (an element's `materials` are indices into it).
void extract_level_lens_flares(const std::vector<std::shared_ptr<UPKPackage>>& packages, LevelScene& scene,
                               std::vector<std::string>& material_paths);

}  // namespace me
