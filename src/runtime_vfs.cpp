#include "runtime_vfs.hpp"
#include <cstring>
#include <ranges>

namespace fs = std::filesystem;

namespace bb::vfs {

VirtualFileSystem& VirtualFileSystem::Instance() {
    static VirtualFileSystem instance;
    return instance;
}

VirtualFileSystem::VirtualFileSystem() {
    Initialize();
}

void VirtualFileSystem::Initialize() {
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
}

bool VirtualFileSystem::Mount(VfsLayerType layer, const fs::path& host_dir) {
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
    const auto idx = static_cast<size_t>(layer);
    if (idx < layers_.size()) {
        layers_[idx].active = false;
        layers_[idx].mount_path.clear();
    }
}

bool VirtualFileSystem::IsActive(VfsLayerType layer) const {
    const auto idx = static_cast<size_t>(layer);
    return idx < layers_.size() && layers_[idx].active;
}

std::optional<fs::path> VirtualFileSystem::GetMount(VfsLayerType layer) const {
    const auto idx = static_cast<size_t>(layer);
    if (idx < layers_.size() && layers_[idx].active) {
        return layers_[idx].mount_path;
    }
    return std::nullopt;
}

static std::string_view NormalizeGuestPath(std::string_view path) {
    while (!path.empty() && path.front() == '/') {
        path.remove_prefix(1);
    }
    if (path.starts_with("app0/")) {
        path.remove_prefix(5);
    }
    return path;
}

std::optional<fs::path> VirtualFileSystem::ResolvePath(std::string_view guest_path) const {
    const auto rel_path = NormalizeGuestPath(guest_path);

    // Search active layers in descending priority order (Save -> Mod -> DLC -> Patch -> Base)
    for (const auto& layer : std::ranges::reverse_view(layers_)) {
        if (!layer.active) continue;

        auto candidate = layer.mount_path / rel_path;
        std::error_code ec;
        if (fs::exists(candidate, ec)) {
            return candidate;
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
        auto fallback = *base_path / guest_path;
        std::strncpy(host_path_out, fallback.c_str(), max_len - 1);
        host_path_out[max_len - 1] = '\0';
    } else {
        host_path_out[0] = '\0';
    }
    return false;
}

}
