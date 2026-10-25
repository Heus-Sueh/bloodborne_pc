#include "runtime_vfs.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

int main() {
    std::cout << "[VFS C++23 TEST] Initializing...\n";

    auto& vfs = bb::vfs::VirtualFileSystem::Instance();
    vfs.Initialize();

    // 1. Initial layer queries
    for (size_t i = 0; i < static_cast<size_t>(bb::vfs::VfsLayerType::Count); ++i) {
        assert(!vfs.IsActive(static_cast<bb::vfs::VfsLayerType>(i)));
        assert(!vfs.GetMount(static_cast<bb::vfs::VfsLayerType>(i)).has_value());
    }

    // 2. Create temp dirs
    auto base_dir = fs::temp_directory_path() / "bb_vfs_test_base";
    auto mod_dir = fs::temp_directory_path() / "bb_vfs_test_mod";
    fs::create_directories(base_dir);
    fs::create_directories(mod_dir);

    // 3. Mount layers
    assert(vfs.Mount(bb::vfs::VfsLayerType::Base, base_dir));
    assert(vfs.Mount(bb::vfs::VfsLayerType::Mod, mod_dir));
    assert(vfs.IsActive(bb::vfs::VfsLayerType::Base));
    assert(vfs.IsActive(bb::vfs::VfsLayerType::Mod));
    assert(!vfs.IsActive(bb::vfs::VfsLayerType::Patch));

    // 4. Test resolution with base file
    auto base_file = base_dir / "param.sfo";
    {
        std::ofstream ofs(base_file);
        ofs << "base_sfo";
    }

    auto resolved = vfs.ResolvePath("/app0/param.sfo");
    assert(resolved.has_value());
    assert(fs::equivalent(*resolved, base_file));

    // 5. Test mod overlay overriding base file
    auto mod_file = mod_dir / "param.sfo";
    {
        std::ofstream ofs(mod_file);
        ofs << "mod_sfo";
    }

    resolved = vfs.ResolvePath("app0/param.sfo");
    assert(resolved.has_value());
    assert(fs::equivalent(*resolved, mod_file)); // Mod overrides base!

    // 6. Test C API bridge
    char c_resolved[1024] = {0};
    assert(bb_vfs_resolve_path("/app0/param.sfo", c_resolved, sizeof(c_resolved)));
    assert(fs::equivalent(fs::path(c_resolved), mod_file));

    // 7. Unmount Mod
    vfs.Unmount(bb::vfs::VfsLayerType::Mod);
    assert(!vfs.IsActive(bb::vfs::VfsLayerType::Mod));

    resolved = vfs.ResolvePath("param.sfo");
    assert(resolved.has_value());
    assert(fs::equivalent(*resolved, base_file));

    // Cleanup
    fs::remove_all(base_dir);
    fs::remove_all(mod_dir);

    std::cout << "PASS: Virtual File System C++23 layered resolution verified!\n";
    return 0;
}
