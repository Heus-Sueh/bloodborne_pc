#include "runtime_registry.hpp"

#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>

static const struct {
    const char *nid;
    const char *symbol;
} import_names[] = {
#include "import_names.inc"
#include "import_names_extra.inc"
};

extern "C" {
void runtime_libc_register(void);
void runtime_mutex_register(void);
void runtime_rwlock_register(void);
void runtime_sema_register(void);
void runtime_thread_register(void);
void runtime_content_register(void);
void runtime_kernel_register(void);
void runtime_file_register(void);
void runtime_memory_register(void);
void runtime_services_register(void);
void runtime_ajm_register(void);
void runtime_audio_register(void);
void runtime_pad_register(void);
void runtime_rtc_register(void);
void runtime_savedata_register(void);
void runtime_toypad_register(void);
void runtime_ngs2_register(void);
void runtime_compat_register(void);
}

namespace bb::registry {

Registry& Registry::Instance() {
    static Registry instance;
    return instance;
}

Registry::Registry() {
    BuildNidIndex();
}

void Registry::BuildNidIndex() {
    if (nid_index_ready_) return;
    const size_t count = sizeof(import_names) / sizeof(import_names[0]);
    nid_to_symbol_.reserve(count * 2);

    for (size_t i = 0; i < count; ++i) {
        const char *nid = import_names[i].nid;
        const char *sym = import_names[i].symbol;
        if (!nid || !sym) continue;

        // Register exact NID string
        nid_to_symbol_[nid] = sym;

        // Register prefix before '#' if present
        const char *hash_pos = std::strchr(nid, '#');
        if (hash_pos) {
            std::string prefix(nid, static_cast<size_t>(hash_pos - nid));
            nid_to_symbol_[prefix] = sym;
        }
    }
    nid_index_ready_ = true;
}

void Registry::RegisterExports(const RuntimeExport *table, size_t count) {
    if (!table || count == 0) return;
    std::unique_lock<std::shared_mutex> lock(mutex_);

    for (size_t i = 0; i < count; ++i) {
        if (!table[i].name) continue;
        function_exports_[table[i].name] = table[i].function;

        const char *hash_pos = std::strchr(table[i].name, '#');
        if (hash_pos) {
            std::string prefix(table[i].name, static_cast<size_t>(hash_pos - table[i].name));
            function_exports_[prefix] = table[i].function;
        }
    }
}

void Registry::RegisterDataExports(const RuntimeExport *table, size_t count) {
    if (!table || count == 0) return;
    std::unique_lock<std::shared_mutex> lock(mutex_);

    for (size_t i = 0; i < count; ++i) {
        if (!table[i].name) continue;
        data_exports_[table[i].name] = table[i].function;

        const char *hash_pos = std::strchr(table[i].name, '#');
        if (hash_pos) {
            std::string prefix(table[i].name, static_cast<size_t>(hash_pos - table[i].name));
            data_exports_[prefix] = table[i].function;
        }
    }
}

const char* Registry::ResolveSymbol(const char *nid) const {
    if (!nid) return nullptr;
    std::shared_lock<std::shared_mutex> lock(mutex_);

    auto it = nid_to_symbol_.find(nid);
    if (it != nid_to_symbol_.end()) {
        return it->second;
    }

    const char *hash_pos = std::strchr(nid, '#');
    if (hash_pos) {
        std::string prefix(nid, static_cast<size_t>(hash_pos - nid));
        auto it_prefix = nid_to_symbol_.find(prefix);
        if (it_prefix != nid_to_symbol_.end()) {
            return it_prefix->second;
        }
    }

    return nullptr;
}

uintptr_t Registry::Resolve(const char *name, int is_data) const {
    if (!name) return 0;
    std::shared_lock<std::shared_mutex> lock(mutex_);

    const auto& table = is_data ? data_exports_ : function_exports_;

    // 1. Try prefix before '#'
    const char *hash_pos = std::strchr(name, '#');
    if (hash_pos) {
        std::string prefix(name, static_cast<size_t>(hash_pos - name));
        auto it = table.find(prefix);
        if (it != table.end()) {
            return reinterpret_cast<uintptr_t>(it->second);
        }
    }

    // 2. Try full name
    auto it = table.find(name);
    if (it != table.end()) {
        return reinterpret_cast<uintptr_t>(it->second);
    }

    // 3. Try looking up symbol via NID index
    const char *sym = ResolveSymbol(name);
    if (sym) {
        auto it_sym = table.find(sym);
        if (it_sym != table.end()) {
            return reinterpret_cast<uintptr_t>(it_sym->second);
        }
    }

    return 0;
}

uintptr_t Registry::Lookup(const RuntimeExport *table, size_t count, const char *nid) const {
    const char *symbol = ResolveSymbol(nid);
    if (!symbol) return 0;

    for (size_t i = 0; i < count; ++i) {
        if (!table[i].name) continue;
        if (std::strcmp(symbol, table[i].name) == 0) {
            return reinterpret_cast<uintptr_t>(table[i].function);
        }
    }
    return 0;
}

void Registry::Init() {
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        if (initialized_) return;
        BuildNidIndex();
        initialized_ = true;
    }

    runtime_libc_register();
    runtime_mutex_register();
    runtime_rwlock_register();
    runtime_sema_register();
    runtime_thread_register();
    runtime_content_register();
    runtime_kernel_register();
    runtime_file_register();
    runtime_memory_register();
    runtime_services_register();
    runtime_ajm_register();
    runtime_audio_register();
    runtime_pad_register();
    runtime_rtc_register();
    runtime_savedata_register();
    runtime_toypad_register();
    runtime_ngs2_register();
    runtime_compat_register();
}

} // namespace bb::registry

using namespace bb::registry;

extern "C" {

void runtime_registry_init(void) {
    Registry::Instance().Init();
}

void runtime_register_exports(const RuntimeExport *table, size_t count) {
    Registry::Instance().RegisterExports(table, count);
}

void runtime_register_data_exports(const RuntimeExport *table, size_t count) {
    Registry::Instance().RegisterDataExports(table, count);
}

uintptr_t runtime_registry_resolve(const char *name, int is_data) {
    return Registry::Instance().Resolve(name, is_data);
}

const char *runtime_symbol(const char *nid) {
    return Registry::Instance().ResolveSymbol(nid);
}

uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid) {
    return Registry::Instance().Lookup(table, count, nid);
}

} // extern "C"
