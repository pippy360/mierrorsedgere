#include "package_manager.hpp"
#include "ue3_props.hpp"
#include "upk_loader.hpp"

#include <filesystem>
#include <fstream>

namespace me {

namespace fs = std::filesystem;

PackageManager::PackageManager(const std::string& cooked_root) : root_(cooked_root) {
    std::error_code ec;
    if (!fs::exists(root_, ec)) return;
    for (auto it = fs::recursive_directory_iterator(root_, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        std::string ext = to_lower(it->path().extension().string());
        if (ext != ".upk" && ext != ".u" && ext != ".me1") continue;
        std::string stem = to_lower(it->path().stem().string());
        // Prefer the first hit; content packages have unique names in CookedPC.
        if (index_.find(stem) == index_.end()) index_[stem] = it->path().string();
    }
}

std::string PackageManager::find_package_path(const std::string& pkg_name) const {
    auto it = index_.find(to_lower(pkg_name));
    return it == index_.end() ? std::string() : it->second;
}

std::shared_ptr<UPKPackage> PackageManager::load(const std::string& pkg_name) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::string key = to_lower(pkg_name);
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second;
    for (const auto& m : missing_) {
        if (m == key) return nullptr;
    }
    std::string path = find_package_path(key);
    if (path.empty()) {
        missing_.push_back(key);
        return nullptr;
    }
    auto pkg = std::make_shared<UPKPackage>(path);
    if (!pkg->is_valid()) {
        missing_.push_back(key);
        return nullptr;
    }
    cache_[key] = pkg;
    return pkg;
}

void PackageManager::add_loaded(const std::string& pkg_name, std::shared_ptr<UPKPackage> pkg) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (pkg) cache_[to_lower(pkg_name)] = std::move(pkg);
}

std::vector<std::shared_ptr<UPKPackage>> PackageManager::loaded_packages() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<std::shared_ptr<UPKPackage>> out;
    out.reserve(cache_.size());
    for (auto& [k, v] : cache_) out.push_back(v);
    return out;
}

bool PackageManager::read_raw(const std::string& pkg_name, uint64_t offset, size_t size, std::vector<uint8_t>& out) {
    std::string path = find_package_path(pkg_name);
    if (path.empty()) return false;
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    f.seekg(0, std::ios::end);
    uint64_t len = static_cast<uint64_t>(f.tellg());
    if (offset + size > len) return false;
    out.resize(size);
    f.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size));
    return static_cast<size_t>(f.gcount()) == size;
}

int32_t PackageManager::find_export(const UPKPackage& pkg, const std::string& full_path_lower) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = export_index_.find(&pkg);
    if (it == export_index_.end()) {
        std::unordered_map<std::string, int32_t> idx;
        const auto& exports = pkg.get_exports();
        idx.reserve(exports.size());
        for (size_t i = 0; i < exports.size(); ++i) {
            const int32_t idx_1based = static_cast<int32_t>(i + 1);
            // Canonical (package-rooted) path first; the file-relative path stays as an alias.
            idx.emplace(to_lower(object_canonical_path(pkg, idx_1based)), idx_1based);
            idx.emplace(to_lower(object_full_path(pkg, idx_1based)), idx_1based);
        }
        it = export_index_.emplace(&pkg, std::move(idx)).first;
    }
    auto f = it->second.find(full_path_lower);
    return f == it->second.end() ? 0 : f->second;
}

}  // namespace me
