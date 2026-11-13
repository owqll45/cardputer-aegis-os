/*
 * App Registry - Lazy loading support
 *
 * Stores app metadata (name) + factory function WITHOUT instantiating apps.
 * Apps are instantiated (installed into Mooncake) only when opened,
 * and uninstalled when closed, keeping RAM usage minimal.
 */
#pragma once
#include <mooncake.h>
#include <string>
#include <vector>
#include <functional>
#include <memory>

struct AppEntry {
    std::string name;
    std::function<std::unique_ptr<mooncake::AppAbility>()> factory;
    int cached_id = -1;  // Mooncake app id when installed, -1 = not installed
};

inline std::vector<AppEntry>& GetAppRegistry()
{
    static std::vector<AppEntry> registry;
    return registry;
}

// Register an app (metadata + factory, NO instantiation)
template <typename T>
inline void RegisterApp(const char* name)
{
    AppEntry entry;
    entry.name = name;
    entry.factory = []() -> std::unique_ptr<mooncake::AppAbility> {
        return std::make_unique<T>();
    };
    GetAppRegistry().push_back(std::move(entry));
}

// Install a registered app into Mooncake on demand
inline int InstallFromRegistry(size_t registry_index)
{
    if (registry_index >= GetAppRegistry().size()) return -1;
    auto& entry = GetAppRegistry()[registry_index];
    if (entry.cached_id >= 0) return entry.cached_id;  // Already installed
    int id = mooncake::GetMooncake().installApp(entry.factory());
    entry.cached_id = id;
    return id;
}

// Uninstall (free memory) when app closes
inline void UninstallFromRegistry(size_t registry_index)
{
    if (registry_index >= GetAppRegistry().size()) return;
    auto& entry = GetAppRegistry()[registry_index];
    if (entry.cached_id >= 0) {
        mooncake::GetMooncake().closeApp(entry.cached_id);
        mooncake::GetMooncake().uninstallApp(entry.cached_id);
        entry.cached_id = -1;
    }
}
