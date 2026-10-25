#include "runtime_content.hpp"
#include "runtime.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>

namespace fs = std::filesystem;

namespace bb::content {

constexpr int32_t INVALID_ID = static_cast<int32_t>(0x805a1000);
constexpr int32_t NOT_LOADED = static_cast<int32_t>(0x805a1001);
constexpr int32_t PARAMETER = static_cast<int32_t>(0x80d90002);
constexpr int32_t BUSY = static_cast<int32_t>(0x80d90003);
constexpr int32_t ADDON_NOT_FOUND = static_cast<int32_t>(0x80d90005);

ContentManager& ContentManager::Instance() {
    static ContentManager instance;
    return instance;
}

void ContentManager::Configure(const uint32_t values[5]) {
    std::scoped_lock lock(mutex_);
    if (refs_ || initialized_ || (values[0] != 1 && values[0] != 3)) {
        std::fputs("ERROR: invalid or late content profile\n", stderr);
        std::exit(1);
    }
    for (size_t i = 0; i < 5; ++i) {
        parameters_[i] = values[i];
    }
    configured_ = 1;
}

int32_t ContentManager::ModuleLoad(uint16_t id) {
    std::scoped_lock lock(mutex_);
    if (!id) return INVALID_ID;
    if (id == 0xb4 && !configured_) {
        std::fputs("STOP: AppContent needs --content-profile\n", stderr);
        std::exit(21);
    }
    if (module_refs_[id] == UINT32_MAX) {
        std::fputs("STOP: module reference overflow\n", stderr);
        std::exit(21);
    }
    ++module_refs_[id];
    if (id == 0xb4) {
        refs_ = module_refs_[id];
        std::printf("Runtime: AppContent host provider loaded; references=%u\n", refs_);
    } else {
        std::printf("Runtime: system module 0x%x loaded (host implementation)\n", id);
    }
    return 0;
}

int32_t ContentManager::ModuleLoaded(uint16_t id) const {
    std::scoped_lock lock(mutex_);
    if (!id) return INVALID_ID;
    return module_refs_[id] ? 0 : NOT_LOADED;
}

int32_t ContentManager::ModuleUnload(uint16_t id) {
    std::scoped_lock lock(mutex_);
    if (!id) return INVALID_ID;
    if (module_refs_[id] == 0) return NOT_LOADED;
    --module_refs_[id];
    if (id == 0xb4) {
        refs_ = module_refs_[id];
        if (!refs_) initialized_ = 0;
    }
    return 0;
}

int32_t ContentManager::Initialize(const unsigned char *init, unsigned char *boot) {
    std::scoped_lock lock(mutex_);
    if (!configured_) {
        std::fputs("STOP: AppContent needs --content-profile\n", stderr);
        std::exit(21);
    }
    if (!refs_) {
        std::fputs("STOP: AppContent called before module load\n", stderr);
        std::exit(21);
    }
    if (!init || !boot) return PARAMETER;
    if (initialized_) return BUSY;

    for (unsigned i = 0; i < 32; ++i) {
        if (init[i]) return PARAMETER;
    }
    std::memset(boot, 0, 40);
    initialized_ = 1;
    std::puts("Runtime: AppContent initialized; offline base-game profile, no mounted add-ons");
    return 0;
}

int32_t ContentManager::GetParam(uint32_t id, int32_t *out) {
    std::scoped_lock lock(mutex_);
    if (!configured_) {
        std::fputs("STOP: AppContent needs --content-profile\n", stderr);
        std::exit(21);
    }
    if (!refs_) {
        std::fputs("STOP: AppContent called before module load\n", stderr);
        std::exit(21);
    }
    if (!initialized_) {
        std::fputs("STOP: AppContent query before initialization\n", stderr);
        std::exit(21);
    }
    if (id > 4 || !out) return PARAMETER;
    std::memcpy(out, &parameters_[id], sizeof(int32_t));
    ++queries_;
    std::printf("Runtime: AppContent parameter %u = %d\n", id, *out);
    return 0;
}

bool ContentManager::FindAddonDir(std::string_view label, std::string &out_path) const {
    const char *user_dir = runtime_file_user_dir();
    if (!user_dir) return false;

    fs::path addcont_root = fs::path(user_dir) / "addcont";
    std::error_code ec;
    if (!fs::exists(addcont_root, ec) || !fs::is_directory(addcont_root, ec)) {
        return false;
    }

    for (const auto& title_entry : fs::directory_iterator(addcont_root, ec)) {
        if (ec) break;
        if (!title_entry.is_directory(ec)) continue;
        fs::path candidate = title_entry.path() / label;
        if (fs::exists(candidate, ec) && fs::is_directory(candidate, ec)) {
            out_path = candidate.string();
            return true;
        }
    }
    return false;
}

int32_t ContentManager::ListAddons(uint32_t service, void *list, uint32_t capacity, uint32_t *hits) {
    std::scoped_lock lock(mutex_);
    if (!configured_) {
        std::fputs("STOP: AppContent needs --content-profile\n", stderr);
        std::exit(21);
    }
    if (!refs_) {
        std::fputs("STOP: AppContent called before module load\n", stderr);
        std::exit(21);
    }
    if (!initialized_) {
        std::fputs("STOP: AppContent query before initialization\n", stderr);
        std::exit(21);
    }
    if (service != 0) {
        std::fputs("STOP: nonzero AppContent service label unsupported\n", stderr);
        std::exit(21);
    }
    if ((!capacity || !list) && !hits) {
        return PARAMETER;
    }

    const bool fill = (list != nullptr && capacity > 0);
    uint32_t count = 0;

    const char *user_dir = runtime_file_user_dir();
    if (user_dir) {
        fs::path addcont_root = fs::path(user_dir) / "addcont";
        std::error_code ec;
        if (fs::exists(addcont_root, ec) && fs::is_directory(addcont_root, ec)) {
            for (const auto& title_entry : fs::directory_iterator(addcont_root, ec)) {
                if (ec) break;
                if (!title_entry.is_directory(ec)) continue;
                std::string title_name = title_entry.path().filename().string();
                if (title_name.empty() || title_name[0] == '.') continue;

                for (const auto& label_entry : fs::directory_iterator(title_entry.path(), ec)) {
                    if (ec) break;
                    if (!label_entry.is_directory(ec)) continue;
                    std::string label_name = label_entry.path().filename().string();
                    if (label_name.empty() || label_name[0] == '.' || label_name.size() >= kLabelSize) {
                        continue;
                    }
                    if (fill && count < capacity) {
                        AddonInfo info{};
                        std::memcpy(info.label, label_name.data(), label_name.size());
                        info.status = kAddonInstalled;
                        std::memcpy(static_cast<char*>(list) + static_cast<size_t>(count) * sizeof(info),
                                    &info, sizeof(info));
                    }
                    ++count;
                }
            }
        }
        if (hits) {
            *hits = count;
        }
        if (count > 0) {
            std::printf("Runtime: AppContent add-ons reported installed (%s): %u\n",
                        addcont_root.string().c_str(), static_cast<unsigned>(count));
        }
    } else {
        if (hits) *hits = 0;
    }

    ++lists_;
    return 0;
}

int32_t ContentManager::GetAddonInfo(uint32_t service, const char *label, void *info) {
    std::scoped_lock lock(mutex_);
    (void)service;
    if (!configured_) {
        std::fputs("STOP: AppContent needs --content-profile\n", stderr);
        std::exit(21);
    }
    if (!refs_) {
        std::fputs("STOP: AppContent called before module load\n", stderr);
        std::exit(21);
    }
    if (!initialized_) {
        std::fputs("STOP: AppContent query before initialization\n", stderr);
        std::exit(21);
    }
    if (!label || !info) return PARAMETER;

    char name[kLabelSize + 1]{};
    std::memcpy(name, label, kLabelSize);
    std::string host_path;
    if (!FindAddonDir(name, host_path)) {
        return ADDON_NOT_FOUND;
    }

    AddonInfo out{};
    std::memcpy(out.label, name, std::strlen(name));
    out.status = kAddonInstalled;
    std::memcpy(info, &out, sizeof(out));
    return 0;
}

int32_t ContentManager::MountAddon(uint32_t service, const char *label, char *point) {
    std::scoped_lock lock(mutex_);
    (void)service;
    if (!configured_) {
        std::fputs("STOP: AppContent needs --content-profile\n", stderr);
        std::exit(21);
    }
    if (!refs_) {
        std::fputs("STOP: AppContent called before module load\n", stderr);
        std::exit(21);
    }
    if (!initialized_) {
        std::fputs("STOP: AppContent query before initialization\n", stderr);
        std::exit(21);
    }
    if (!label || !point) return PARAMETER;

    char name[kLabelSize + 1]{};
    std::memcpy(name, label, kLabelSize);
    std::string host_path;
    if (!FindAddonDir(name, host_path)) {
        std::printf("Runtime: add-on '%s' not installed\n", name);
        return ADDON_NOT_FOUND;
    }

    int slot = -1;
    for (int i = 0; i < 16; ++i) {
        if (mounted_[i] == name) {
            slot = i;
            break;
        }
        if (slot < 0 && mounted_[i].empty()) {
            slot = i;
        }
    }
    if (slot < 0) return BUSY;

    mounted_[slot] = name;
    char guest[32];
    std::snprintf(guest, sizeof(guest), "/addcont%d", slot & 15);
    std::memset(point, 0, 16);
    std::memcpy(point, guest, std::strlen(guest));

    if (runtime_file_mount(guest, host_path.c_str())) {
        std::fprintf(stderr, "Runtime: add-on '%s' could not be mounted: mount table full\n", name);
        return BUSY;
    }
    std::printf("Runtime: add-on '%s' mounted at %s (%s)\n", name, guest, host_path.c_str());
    return 0;
}

int32_t ContentManager::UnmountAddon(const char *point) {
    std::scoped_lock lock(mutex_);
    (void)point;
    return 0;
}

void ContentManager::Report() const {
    std::scoped_lock lock(mutex_);
    std::printf("Runtime: AppContent references=%u, initialized=%u, parameter queries=%u, list queries=%u\n",
                refs_, initialized_, queries_, lists_);
}

void ContentManager::Reset() {
    std::scoped_lock lock(mutex_);
    refs_ = 0;
    initialized_ = 0;
    configured_ = 0;
    queries_ = 0;
    lists_ = 0;
    parameters_.fill(0);
    module_refs_.fill(0);
    for (auto& m : mounted_) {
        m.clear();
    }
}

} // namespace bb::content

using namespace bb::content;

extern "C" {

static ABI int32_t guest_addon_mount(uint32_t service, const char *label, char *point) {
    return ContentManager::Instance().MountAddon(service, label, point);
}

static ABI int32_t guest_addon_unmount(const char *point) {
    return ContentManager::Instance().UnmountAddon(point);
}

static ABI int32_t guest_addon_info(uint32_t service, const char *label, void *info) {
    return ContentManager::Instance().GetAddonInfo(service, label, info);
}

static ABI int32_t guest_module_load(uint16_t id) {
    return ContentManager::Instance().ModuleLoad(id);
}

static ABI int32_t guest_module_loaded(uint16_t id) {
    return ContentManager::Instance().ModuleLoaded(id);
}

static ABI int32_t guest_module_unload(uint16_t id) {
    return ContentManager::Instance().ModuleUnload(id);
}

static ABI int32_t guest_content_init(const unsigned char *init, unsigned char *boot) {
    return ContentManager::Instance().Initialize(init, boot);
}

static ABI int32_t guest_param_int(uint32_t id, int32_t *out) {
    return ContentManager::Instance().GetParam(id, out);
}

static ABI int32_t guest_addon_list(uint32_t service, void *list, uint32_t capacity, uint32_t *hits) {
    return ContentManager::Instance().ListAddons(service, list, capacity, hits);
}

static const RuntimeExport content_exports[] = {
    {"VANhIWcqYak", reinterpret_cast<void*>(guest_addon_mount)},
    {"3rHWaV-1KC4", reinterpret_cast<void*>(guest_addon_unmount)},
    {"m47juOmH0VE", reinterpret_cast<void*>(guest_addon_info)},
    {"g8cM39EUZ6o", reinterpret_cast<void*>(guest_module_load)},
    {"fMP5NHUOaMk", reinterpret_cast<void*>(guest_module_loaded)},
    {"eR2bZFAAU0Q", reinterpret_cast<void*>(guest_module_unload)},
    {"R9lA82OraNs", reinterpret_cast<void*>(guest_content_init)},
    {"99b82IKXpH4", reinterpret_cast<void*>(guest_param_int)},
    {"xnd8BJzAxmk", reinterpret_cast<void*>(guest_addon_list)},
};

void runtime_content_register(void) {
    runtime_register_exports(content_exports, sizeof(content_exports) / sizeof(content_exports[0]));
}

uintptr_t runtime_content_resolve(const char *name) {
    if (runtime_nid_is(name, "VANhIWcqYak")) return reinterpret_cast<uintptr_t>(guest_addon_mount);
    if (runtime_nid_is(name, "3rHWaV-1KC4")) return reinterpret_cast<uintptr_t>(guest_addon_unmount);
    if (runtime_nid_is(name, "m47juOmH0VE")) return reinterpret_cast<uintptr_t>(guest_addon_info);
    if (runtime_nid_is(name, "g8cM39EUZ6o")) return reinterpret_cast<uintptr_t>(guest_module_load);
    if (runtime_nid_is(name, "fMP5NHUOaMk")) return reinterpret_cast<uintptr_t>(guest_module_loaded);
    if (runtime_nid_is(name, "eR2bZFAAU0Q")) return reinterpret_cast<uintptr_t>(guest_module_unload);
    if (runtime_nid_is(name, "R9lA82OraNs")) return reinterpret_cast<uintptr_t>(guest_content_init);
    if (runtime_nid_is(name, "99b82IKXpH4")) return reinterpret_cast<uintptr_t>(guest_param_int);
    if (runtime_nid_is(name, "xnd8BJzAxmk")) return reinterpret_cast<uintptr_t>(guest_addon_list);
    return 0;
}

void runtime_content_configure(const uint32_t values[5]) {
    ContentManager::Instance().Configure(values);
}

void runtime_content_report(void) {
    ContentManager::Instance().Report();
}

} // extern "C"
