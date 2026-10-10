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
// What is read is what renderer/particles.hpp runs: sprite and mesh emitters, every LOD
// level of each, and the module classes level_particles.cpp lists. An emitter that needs
// PhysX (its type-data module), or uses an enabled module outside that list, is left out,
// and so is a placement that only exists with hardware PhysX (bPhysXMutatable, Group
// "PhysXOnly").
// -----------------------------------------------------------------------------

#include "../math/types.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace me {

class UPKPackage;

// Fills scene.particle_templates and scene.particle_systems. The emitters' materials join
// `material_paths` (ParticleEmitterInfo::material is an index into it). `mesh_for` is asked for a
// mesh emitter's static mesh, by its path and its name, and answers with its place in
// scene.particle_meshes (-1: not loaded).
void extract_level_particles(const std::vector<std::shared_ptr<UPKPackage>>& packages, LevelScene& scene,
                             std::vector<std::string>& material_paths,
                             const std::function<int32_t(const std::string& path, const std::string& name)>& mesh_for);

// The place in scene.particle_templates of a ParticleSystem export of `pkg`, read when it is first
// asked for (what the game makes while it runs: a bullet's impact, an actor factory's emitter).
// -1 when none of its emitters can be run.
int32_t particle_template_for(const UPKPackage& pkg, int32_t template_export, LevelScene& scene,
                              std::vector<std::string>& material_paths,
                              const std::function<int32_t(const std::string& path, const std::string& name)>& mesh_for);

}  // namespace me
