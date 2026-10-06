#pragma once

// -----------------------------------------------------------------------------
// PackageManager: indexes every package under CookedPC by name and provides
//  - lazily loaded, cached UPKPackage instances (for resolving imports)
//  - raw file-range reads (for "StoreInSeparateFile" bulk data: texture mips
//    that the cooker left in the texture's original content package)
//  - export lookup by full object path
// -----------------------------------------------------------------------------

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace me {

class UPKPackage;

class PackageManager {
public:
    explicit PackageManager(const std::string& cooked_root);

    [[nodiscard]] const std::string& cooked_root() const { return root_; }

    // Returns the on-disk path for a package name (case-insensitive), or "".
    [[nodiscard]] std::string find_package_path(const std::string& pkg_name) const;

    // Loads (or returns cached) package by name. Returns nullptr if missing/invalid.
    std::shared_ptr<UPKPackage> load(const std::string& pkg_name);

    // Registers an already-loaded package (e.g. the level packages) under its name.
    void add_loaded(const std::string& pkg_name, std::shared_ptr<UPKPackage> pkg);

    // Returns already-loaded packages (level + resolved imports).
    std::vector<std::shared_ptr<UPKPackage>> loaded_packages();

    // Reads `size` raw bytes at absolute file `offset` of package `pkg_name`.
    bool read_raw(const std::string& pkg_name, uint64_t offset, size_t size, std::vector<uint8_t>& out);

    // Finds an export (1-based index) by full lower-case object path. Returns 0 if not found.
    int32_t find_export(const UPKPackage& pkg, const std::string& full_path_lower);

private:
    std::string root_;
    std::unordered_map<std::string, std::string> index_;  // lower-case stem -> path
    std::unordered_map<std::string, std::shared_ptr<UPKPackage>> cache_;
    std::unordered_map<const UPKPackage*, std::unordered_map<std::string, int32_t>> export_index_;
    std::vector<std::string> missing_;
    std::recursive_mutex mutex_;
};

}  // namespace me
