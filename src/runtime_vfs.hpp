#pragma once

#include "platform/platform.h"
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

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

class VirtualFileSystem {
public:
    static VirtualFileSystem& Instance();

    void Initialize();
    bool Mount(VfsLayerType layer, const std::filesystem::path& host_dir);
    void Unmount(VfsLayerType layer);
    [[nodiscard]] bool IsActive(VfsLayerType layer) const;
    [[nodiscard]] std::optional<std::filesystem::path> GetMount(VfsLayerType layer) const;
    [[nodiscard]] std::optional<std::filesystem::path> ResolvePath(std::string_view guest_path) const;

private:
    VirtualFileSystem();
    std::array<Layer, static_cast<size_t>(VfsLayerType::Count)> layers_;
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
bool bb_vfs_resolve_path(const char *guest_path, char *host_path_out, size_t max_len);

#ifdef __cplusplus
}
#endif
