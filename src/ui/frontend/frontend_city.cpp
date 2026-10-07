#include "frontend_assets.hpp"

#include "../../assets/package_manager.hpp"
#include "../../assets/upk_loader.hpp"

#include <memory>

namespace me::fe {

bool load_city(PackageManager&, const std::shared_ptr<UPKPackage>&, City&, std::vector<std::string>&) { return false; }

}  // namespace me::fe
