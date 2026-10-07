#pragma once

// Shared between the front end's asset loaders (frontend_assets.cpp, frontend_city.cpp).

#include "frontend_assets.hpp"

#include "../../assets/scene_materials.hpp"

#include <memory>
#include <string>
#include <vector>

namespace me {
class PackageManager;
class UPKPackage;
}  // namespace me

namespace me::fe {

// Decodes one mip (DXT1/3/5, BGRA8 or G8) to RGBA8.
bool decode_texture_mip(const TextureMip& mip, TexFormat fmt, Image& out);

// The StaticMeshActors, light maps and textures of Maps/Menu/TdMainMenu.me1.
bool load_city(PackageManager& pm, const std::shared_ptr<UPKPackage>& menu_map, City& out, std::vector<std::string>& warnings);

}  // namespace me::fe
