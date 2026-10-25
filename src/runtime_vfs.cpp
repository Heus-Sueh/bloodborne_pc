#include "runtime_vfs.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <mutex>
#include <ranges>
#include <string>
#include <unordered_set>

namespace fs = std::filesystem;

namespace bb::vfs {

// Top-level folders of Bloodborne's dvdroot_ps4
static const std::unordered_set<std::string> kGameFolders = {
    "action", "adhoc", "chr", "event", "facegen", "font", "map", "menu", "movie", "msg", "mtd",
    "obj", "other", "param", "paramdef", "parts", "remo", "script", "sfx", "shader", "sound"
};

// Retail release title serials
static const std::vector<std::string> kSerials = {
    "CUSA03173", "CUSA00900", "CUSA00207", "CUSA00208", "CUSA00299", "CUSA01363",
    "CUSA03179", "CUSA03014", "CUSA03023"
};

static std::optional<fs::path> FindChildCaseInsensitive(const fs::path& folder, std::string_view name) {
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) return std::nullopt;
    for (const auto& entry : fs::directory_iterator(folder, ec)) {
        if (strcasecmp(entry.path().filename().string().c_str(), std::string(name).c_str()) == 0) {
            return entry.path();
        }
    }
    return std::nullopt;
}

struct ModLayout {
    fs::path root;
    std::string prefix; // e.g. "dvdroot_ps4" or ""
};

static std::optional<ModLayout> DetectModLayout(const fs::path& folder, int depth = 0) {
    if (depth > 2) return std::nullopt;
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) return std::nullopt;

    // Check wrappers: "", "app0", and serials
    std::vector<std::string> wrappers = {"", "app0"};
    wrappers.insert(wrappers.end(), kSerials.begin(), kSerials.end());

    for (const auto& wrapper : wrappers) {
        fs::path base = folder;
        if (!wrapper.empty()) {
            auto wrapped_base = FindChildCaseInsensitive(folder, wrapper);
            if (!wrapped_base) continue;
            base = *wrapped_base;
        }
        auto dvdroot = FindChildCaseInsensitive(base, "dvdroot_ps4");
        if (dvdroot && fs::is_directory(*dvdroot, ec)) {
            return ModLayout{base, ""};
        }
    }

    // Check loose folders (folder contains chr, parts, map, etc.)
    std::vector<fs::path> subdirs;
    bool only_game_folders = true;
    size_t game_folder_count = 0;
    for (const auto& entry : fs::directory_iterator(folder, ec)) {
        std::string filename = entry.path().filename().string();
        if (filename.starts_with(".")) continue;
        if (entry.is_directory(ec)) {
            subdirs.push_back(entry.path());
            std::string lower_name = filename;
            std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
            if (kGameFolders.contains(lower_name)) {
                game_folder_count++;
            } else {
                only_game_folders = false;
            }
        }
    }

    if (!subdirs.empty() && only_game_folders && game_folder_count > 0) {
        return ModLayout{folder, "dvdroot_ps4"};
    }

    // Check single wrapper folder (e.g. archive extracted with its name)
    if (subdirs.size() == 1) {
        bool has_other_files = false;
        for (const auto& entry : fs::directory_iterator(folder, ec)) {
            if (entry.is_regular_file(ec)) {
                std::string ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext != ".txt" && ext != ".md" && ext != ".jpg" && ext != ".png" && ext != ".ini") {
                    has_other_files = true;
                    break;
                }
            }
        }
        if (!has_other_files) {
            return DetectModLayout(subdirs[0], depth + 1);
        }
    }

    return std::nullopt;
}

static std::vector<std::string> ParseJsonStringArray(std::string_view content, std::string_view key) {
    std::vector<std::string> results;
    std::string search = "\"" + std::string(key) + "\"";
    size_t pos = content.find(search);
    if (pos == std::string_view::npos) return results;

    size_t colon = content.find(':', pos + search.size());
    if (colon == std::string_view::npos) return results;

    size_t bracket_open = content.find('[', colon);
    if (bracket_open == std::string_view::npos) return results;

    size_t bracket_close = content.find(']', bracket_open);
    if (bracket_close == std::string_view::npos) return results;

    std::string_view inner = content.substr(bracket_open + 1, bracket_close - bracket_open - 1);
    size_t idx = 0;
    while (idx < inner.size()) {
        size_t quote_start = inner.find('"', idx);
        if (quote_start == std::string_view::npos) break;
        size_t quote_end = inner.find('"', quote_start + 1);
        if (quote_end == std::string_view::npos) break;

        results.emplace_back(inner.substr(quote_start + 1, quote_end - quote_start - 1));
        idx = quote_end + 1;
    }

    return results;
}

static std::optional<fs::path> ResolveCaseInsensitive(const fs::path& base, const fs::path& rel) {
    std::error_code ec;
    auto direct = base / rel;
    if (fs::exists(direct, ec)) {
        return direct;
    }

    // Component by component case-insensitive lookup
    fs::path current = base;
    for (const auto& part : rel) {
        std::string target_name = part.string();
        if (target_name.empty() || target_name == ".") continue;

        auto next = current / target_name;
        if (fs::exists(next, ec)) {
            current = next;
            continue;
        }

        DIR* d = opendir(current.string().c_str());
        if (!d) return std::nullopt;

        bool found = false;
        struct dirent* entry;
        while ((entry = readdir(d)) != nullptr) {
            if (strcasecmp(entry->d_name, target_name.c_str()) == 0) {
                current /= entry->d_name;
                found = true;
                break;
            }
        }
        closedir(d);
        if (!found) return std::nullopt;
    }
    return current;
}

VirtualFileSystem& VirtualFileSystem::Instance() {
    static VirtualFileSystem instance;
    return instance;
}

VirtualFileSystem::VirtualFileSystem() {
    Initialize();
}

void VirtualFileSystem::Initialize() {
    std::unique_lock lock(mutex_);
    layers_[static_cast<size_t>(VfsLayerType::Base)] =
        Layer{.type = VfsLayerType::Base, .name = "Base Game", .mount_path = {}, .active = false};
    layers_[static_cast<size_t>(VfsLayerType::Patch)] =
        Layer{.type = VfsLayerType::Patch, .name = "Update Patch", .mount_path = {}, .active = false};
    layers_[static_cast<size_t>(VfsLayerType::DLC)] =
        Layer{.type = VfsLayerType::DLC, .name = "DLC Add-ons", .mount_path = {}, .active = false};
    layers_[static_cast<size_t>(VfsLayerType::Mod)] =
        Layer{.type = VfsLayerType::Mod, .name = "Mod Overlay", .mount_path = {}, .active = false};
    layers_[static_cast<size_t>(VfsLayerType::Save)] =
        Layer{.type = VfsLayerType::Save, .name = "User SaveData", .mount_path = {}, .active = false};
    mod_layers_.clear();
}

bool VirtualFileSystem::Mount(VfsLayerType layer, const fs::path& host_dir) {
    if (layer == VfsLayerType::Mod) {
        return MountMod(host_dir, host_dir.filename().string());
    }

    std::unique_lock lock(mutex_);
    const auto idx = static_cast<size_t>(layer);
    if (idx >= layers_.size()) {
        return false;
    }
    std::error_code ec;
    if (!fs::is_directory(host_dir, ec)) {
        return false;
    }
    layers_[idx].mount_path = fs::canonical(host_dir, ec);
    if (ec) {
        layers_[idx].mount_path = host_dir;
    }
    layers_[idx].active = true;
    return true;
}

void VirtualFileSystem::Unmount(VfsLayerType layer) {
    if (layer == VfsLayerType::Mod) {
        ClearMods();
        return;
    }

    std::unique_lock lock(mutex_);
    const auto idx = static_cast<size_t>(layer);
    if (idx < layers_.size()) {
        layers_[idx].active = false;
        layers_[idx].mount_path.clear();
    }
}

bool VirtualFileSystem::IsActive(VfsLayerType layer) const {
    std::shared_lock lock(mutex_);
    if (layer == VfsLayerType::Mod) {
        return !mod_layers_.empty() || layers_[static_cast<size_t>(VfsLayerType::Mod)].active;
    }
    const auto idx = static_cast<size_t>(layer);
    return idx < layers_.size() && layers_[idx].active;
}

std::optional<fs::path> VirtualFileSystem::GetMount(VfsLayerType layer) const {
    std::shared_lock lock(mutex_);
    const auto idx = static_cast<size_t>(layer);
    if (idx < layers_.size() && layers_[idx].active) {
        return layers_[idx].mount_path;
    }
    if (layer == VfsLayerType::Mod && !mod_layers_.empty()) {
        return mod_layers_.back().root_path;
    }
    return std::nullopt;
}

bool VirtualFileSystem::MountMod(const fs::path& host_dir, std::string_view name) {
    std::error_code ec;
    if (!fs::is_directory(host_dir, ec)) {
        return false;
    }

    auto layout = DetectModLayout(host_dir);
    fs::path root = layout ? layout->root : host_dir;
    std::string prefix = layout ? layout->prefix : "";

    std::unique_lock lock(mutex_);
    mod_layers_.push_back(ModLayer{
        .name = name.empty() ? host_dir.filename().string() : std::string(name),
        .root_path = root,
        .prefix = prefix,
        .active = true
    });
    layers_[static_cast<size_t>(VfsLayerType::Mod)].active = true;
    return true;
}

void VirtualFileSystem::ClearMods() {
    std::unique_lock lock(mutex_);
    mod_layers_.clear();
    layers_[static_cast<size_t>(VfsLayerType::Mod)].active = false;
    layers_[static_cast<size_t>(VfsLayerType::Mod)].mount_path.clear();
}

int VirtualFileSystem::LoadMods(const fs::path& mods_dir, const fs::path& config_path) {
    std::error_code ec;
    if (!fs::is_directory(mods_dir, ec)) {
        return 0;
    }

    // Check if mods_dir itself is a direct mod (e.g. dvdroot_ps4 inside it)
    auto single_layout = DetectModLayout(mods_dir);
    if (single_layout) {
        MountMod(mods_dir, mods_dir.filename().string());
        return 1;
    }

    struct DiscoveredMod {
        std::string name;
        fs::path path;
        ModLayout layout;
    };
    std::vector<DiscoveredMod> discovered;

    for (const auto& entry : fs::directory_iterator(mods_dir, ec)) {
        if (!entry.is_directory(ec)) continue;
        std::string name = entry.path().filename().string();
        if (name.starts_with(".")) continue;

        auto layout = DetectModLayout(entry.path());
        if (layout) {
            discovered.push_back({name, entry.path(), *layout});
        }
    }

    std::sort(discovered.begin(), discovered.end(), [](const auto& a, const auto& b) {
        return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });

    std::vector<std::string> disabled_mods;
    std::vector<std::string> ordered_mods;
    if (!config_path.empty() && fs::is_regular_file(config_path, ec)) {
        std::ifstream f(config_path);
        if (f.is_open()) {
            std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            disabled_mods = ParseJsonStringArray(content, "disabled");
            ordered_mods = ParseJsonStringArray(content, "order");
        }
    }

    std::unordered_set<std::string> disabled_set(disabled_mods.begin(), disabled_mods.end());
    std::unordered_set<std::string> added_set;

    ClearMods();

    // 1. Add ordered mods
    for (const auto& mod_name : ordered_mods) {
        if (disabled_set.contains(mod_name)) continue;
        auto it = std::find_if(discovered.begin(), discovered.end(),
                               [&](const auto& d) { return strcasecmp(d.name.c_str(), mod_name.c_str()) == 0; });
        if (it != discovered.end() && !added_set.contains(it->name)) {
            MountMod(it->path, it->name);
            added_set.insert(it->name);
        }
    }

    // 2. Append remaining discovered mods
    for (const auto& d : discovered) {
        if (disabled_set.contains(d.name) || added_set.contains(d.name)) continue;
        MountMod(d.path, d.name);
        added_set.insert(d.name);
    }

    return static_cast<int>(GetModCount());
}

size_t VirtualFileSystem::GetModCount() const {
    std::shared_lock lock(mutex_);
    return mod_layers_.size();
}

static std::string_view NormalizeGuestPath(std::string_view path) {
    while (!path.empty() && path.front() == '/') {
        path.remove_prefix(1);
    }
    if (path.starts_with("app0/")) {
        path.remove_prefix(5);
    } else if (path.starts_with("hostapp/")) {
        path.remove_prefix(8);
    }
    return path;
}

std::optional<fs::path> VirtualFileSystem::ResolvePath(std::string_view guest_path) const {
    std::shared_lock lock(mutex_);
    const auto rel_path = NormalizeGuestPath(guest_path);

    // 1. Search active mod layers in reverse order (later mounted mods have higher priority)
    for (auto it = mod_layers_.rbegin(); it != mod_layers_.rend(); ++it) {
        if (!it->active) continue;

        fs::path search_subpath;
        if (it->prefix == "dvdroot_ps4") {
            if (rel_path.starts_with("dvdroot_ps4/")) {
                search_subpath = rel_path.substr(12);
            } else if (rel_path == "dvdroot_ps4") {
                search_subpath = "";
            } else {
                continue;
            }
        } else {
            search_subpath = rel_path;
        }

        auto candidate = ResolveCaseInsensitive(it->root_path, search_subpath);
        if (candidate) {
            return candidate;
        }
    }

    // Legacy single mod layer check if mounted directly via Mount(VfsLayerType::Mod, ...)
    const auto mod_idx = static_cast<size_t>(VfsLayerType::Mod);
    if (layers_[mod_idx].active && !layers_[mod_idx].mount_path.empty()) {
        auto candidate = ResolveCaseInsensitive(layers_[mod_idx].mount_path, rel_path);
        if (candidate) {
            return candidate;
        }
    }

    // 2. Search DLC, Patch, Base
    for (auto type : {VfsLayerType::DLC, VfsLayerType::Patch, VfsLayerType::Base}) {
        const auto idx = static_cast<size_t>(type);
        if (idx < layers_.size() && layers_[idx].active) {
            auto candidate = ResolveCaseInsensitive(layers_[idx].mount_path, rel_path);
            if (candidate) {
                return candidate;
            }
        }
    }

    return std::nullopt;
}

} // namespace bb::vfs

/* --------------------------------------------------------------------------
 * C API Bridges
 * -------------------------------------------------------------------------- */

extern "C" {

void bb_vfs_init(void) {
    bb::vfs::VirtualFileSystem::Instance().Initialize();
}

int bb_vfs_mount(BbVfsLayerType layer, const char *host_dir) {
    if (!host_dir) return -1;
    bool ok = bb::vfs::VirtualFileSystem::Instance().Mount(
        static_cast<bb::vfs::VfsLayerType>(layer), host_dir);
    return ok ? 0 : -1;
}

void bb_vfs_unmount(BbVfsLayerType layer) {
    bb::vfs::VirtualFileSystem::Instance().Unmount(
        static_cast<bb::vfs::VfsLayerType>(layer));
}

bool bb_vfs_is_layer_active(BbVfsLayerType layer) {
    return bb::vfs::VirtualFileSystem::Instance().IsActive(
        static_cast<bb::vfs::VfsLayerType>(layer));
}

static thread_local char g_mount_cache[1024];

const char *bb_vfs_get_mount(BbVfsLayerType layer) {
    auto path = bb::vfs::VirtualFileSystem::Instance().GetMount(
        static_cast<bb::vfs::VfsLayerType>(layer));
    if (!path) return nullptr;
    std::strncpy(g_mount_cache, path->c_str(), sizeof(g_mount_cache) - 1);
    g_mount_cache[sizeof(g_mount_cache) - 1] = '\0';
    return g_mount_cache;
}

int bb_vfs_mount_mod(const char *host_dir, const char *name) {
    if (!host_dir) return -1;
    bool ok = bb::vfs::VirtualFileSystem::Instance().MountMod(
        host_dir, name ? name : "");
    return ok ? 0 : -1;
}

void bb_vfs_clear_mods(void) {
    bb::vfs::VirtualFileSystem::Instance().ClearMods();
}

int bb_vfs_load_mods(const char *mods_dir, const char *config_json) {
    if (!mods_dir) return 0;
    return bb::vfs::VirtualFileSystem::Instance().LoadMods(
        mods_dir, config_json ? config_json : "");
}

size_t bb_vfs_get_mod_count(void) {
    return bb::vfs::VirtualFileSystem::Instance().GetModCount();
}

bool bb_vfs_resolve_path(const char *guest_path, char *host_path_out, size_t max_len) {
    if (!guest_path || !host_path_out || max_len == 0) return false;

    auto resolved = bb::vfs::VirtualFileSystem::Instance().ResolvePath(guest_path);
    if (resolved) {
        std::strncpy(host_path_out, resolved->c_str(), max_len - 1);
        host_path_out[max_len - 1] = '\0';
        return true;
    }

    // Fallback path to base mount if available
    auto base_path = bb::vfs::VirtualFileSystem::Instance().GetMount(bb::vfs::VfsLayerType::Base);
    if (base_path) {
        std::string_view rel = guest_path;
        while (!rel.empty() && rel.front() == '/') rel.remove_prefix(1);
        if (rel.starts_with("app0/")) rel.remove_prefix(5);
        else if (rel.starts_with("hostapp/")) rel.remove_prefix(8);

        auto fallback = *base_path / rel;
        std::strncpy(host_path_out, fallback.c_str(), max_len - 1);
        host_path_out[max_len - 1] = '\0';
    } else {
        host_path_out[0] = '\0';
    }
    return false;
}

}
