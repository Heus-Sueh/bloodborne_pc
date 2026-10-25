#pragma once

#include "runtime_registry.h"

#ifdef __cplusplus
#include <string>
#include <string_view>
#include <unordered_map>
#include <shared_mutex>

namespace bb::registry {

class Registry {
public:
    static Registry& Instance();

    void Init();
    void RegisterExports(const RuntimeExport *table, size_t count);
    void RegisterDataExports(const RuntimeExport *table, size_t count);
    uintptr_t Resolve(const char *name, int is_data) const;
    const char* ResolveSymbol(const char *nid) const;
    uintptr_t Lookup(const RuntimeExport *table, size_t count, const char *nid) const;

private:
    Registry();
    ~Registry() = default;
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    void BuildNidIndex();

    mutable std::shared_mutex mutex_;
    bool initialized_{false};
    bool nid_index_ready_{false};

    std::unordered_map<std::string, const char*> nid_to_symbol_;
    std::unordered_map<std::string, void*> function_exports_;
    std::unordered_map<std::string, void*> data_exports_;
};

} // namespace bb::registry
#endif
