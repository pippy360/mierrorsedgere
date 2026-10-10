#pragma once

// -----------------------------------------------------------------------------
// The level's particle systems: its Emitter actors and the ParticleSystem templates
// they name (docs/RENDERING_RE.md, "Particles"; renderer/particles.hpp runs and draws
// them).
//
// A template is a list of emitters; an emitter's first LOD level holds a required
// module (material, alignment, loops, the sub-image grid), a spawn module (rate and
// bursts) and a list of modules that act on a particle when it is spawned, every frame,
// or both. Every value is a raw distribution, baked into a lookup table as a lens
// flare's are, and saved as a difference from the module's class default object: a
// table, or just its operation or chunk size, that the level does not save is the
// class's (Engine.u).
//
// What is read is what renderer/particles.hpp runs: sprite emitters and sixteen module
// classes. An emitter that draws meshes or needs PhysX (a type-data module), or uses
// an enabled module outside that set, is left out, and so is a placement that only
// exists with hardware PhysX (bPhysXMutatable, Group "PhysXOnly").
// -----------------------------------------------------------------------------

#include "../math/types.hpp"

#include <memory>
#include <string>
#include <vector>

namespace me {

class UPKPackage;

// Fills scene.particle_templates and scene.particle_systems. The emitters' materials join
// `material_paths` (ParticleEmitterInfo::material is an index into it).
void extract_level_particles(const std::vector<std::shared_ptr<UPKPackage>>& packages, LevelScene& scene,
                             std::vector<std::string>& material_paths);

}  // namespace me
