#pragma once

#include "platform/platform.h"
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

namespace bb::vfs {

enum class VfsLayerType : uint8_t {
    Base = 0,    // Base game assets (/app0/dvdroot/ from PKG/ISO)
    Patch,       // Title update 1.09 patch overlay
    DLC,         // Downloadable content add-ons (/addcont/...)
    Mod,         // User modifications overlay
    Save,        // Isolated user save data directory
    Count
};

struct Layer {
    VfsLayerType type;
    std::string_view name;
    std::filesystem::path mount_path;
    bool active{false};
};

struct ModLayer {
    std::string name;
    std::filesystem::path root_path;
    std::string prefix; // e.g. "dvdroot_ps4" if loose folders, or ""
    bool active{true};
};

class VirtualFileSystem {
public:
    static VirtualFileSystem& Instance();

    void Initialize();
    bool Mount(VfsLayerType layer, const std::filesystem::path& host_dir);
    void Unmount(VfsLayerType layer);
    [[nodiscard]] bool IsActive(VfsLayerType layer) const;
    [[nodiscard]] std::optional<std::filesystem::path> GetMount(VfsLayerType layer) const;

    // Mod-specific layering
    bool MountMod(const std::filesystem::path& host_dir, std::string_view name = "");
    void ClearMods();
    int LoadMods(const std::filesystem::path& mods_dir, const std::filesystem::path& config_path = "");
    [[nodiscard]] size_t GetModCount() const;

    [[nodiscard]] std::optional<std::filesystem::path> ResolvePath(std::string_view guest_path) const;

private:
    VirtualFileSystem();
    mutable std::shared_mutex mutex_;
    std::array<Layer, static_cast<size_t>(VfsLayerType::Count)> layers_;
    std::vector<ModLayer> mod_layers_;
};

} // namespace bb::vfs

/* C API Bridges */
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VFS_LAYER_BASE = 0,
    VFS_LAYER_PATCH,
    VFS_LAYER_DLC,
    VFS_LAYER_MOD,
    VFS_LAYER_SAVE,
    VFS_LAYER_COUNT
} BbVfsLayerType;

typedef struct {
    BbVfsLayerType type;
    const char *name;
    char mount_path[1024];
    bool active;
} BbVfsLayer;

void bb_vfs_init(void);
int bb_vfs_mount(BbVfsLayerType layer, const char *host_dir);
void bb_vfs_unmount(BbVfsLayerType layer);
bool bb_vfs_is_layer_active(BbVfsLayerType layer);
const char *bb_vfs_get_mount(BbVfsLayerType layer);
int bb_vfs_mount_mod(const char *host_dir, const char *name);
void bb_vfs_clear_mods(void);
int bb_vfs_load_mods(const char *mods_dir, const char *config_json);
size_t bb_vfs_get_mod_count(void);
bool bb_vfs_resolve_path(const char *guest_path, char *host_path_out, size_t max_len);

#ifdef __cplusplus
}
#endif
