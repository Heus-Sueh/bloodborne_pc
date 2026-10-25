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
    auto temp_root = fs::temp_directory_path() / "bb_vfs_full_test";
    fs::remove_all(temp_root);
    fs::create_directories(temp_root);

    auto base_dir = temp_root / "base";
    auto base_dvdroot = base_dir / "dvdroot_ps4" / "chr";
    fs::create_directories(base_dvdroot);

    // Create base files
    auto base_param = base_dir / "param.sfo";
    {
        std::ofstream ofs(base_param);
        ofs << "base_sfo";
    }
    auto base_chr_a = base_dvdroot / "a.dcx";
    {
        std::ofstream ofs(base_chr_a);
        ofs << "base_a";
    }
    auto base_chr_b = base_dvdroot / "b.dcx";
    {
        std::ofstream ofs(base_chr_b);
        ofs << "base_b";
    }

    // 3. Mount base
    assert(vfs.Mount(bb::vfs::VfsLayerType::Base, base_dir));
    assert(vfs.IsActive(bb::vfs::VfsLayerType::Base));

    auto res = vfs.ResolvePath("/app0/param.sfo");
    assert(res.has_value() && fs::equivalent(*res, base_param));
    res = vfs.ResolvePath("/app0/dvdroot_ps4/chr/a.dcx");
    assert(res.has_value() && fs::equivalent(*res, base_chr_a));

    // 4. Test Single Mod Mount & Override
    auto mod_dir = temp_root / "test_mod";
    auto mod_dvdroot = mod_dir / "dvdroot_ps4" / "chr";
    fs::create_directories(mod_dvdroot);
    auto mod_chr_a = mod_dvdroot / "a.dcx";
    {
        std::ofstream ofs(mod_chr_a);
        ofs << "mod_a";
    }
    assert(vfs.MountMod(mod_dir, "test_mod"));
    assert(vfs.IsActive(bb::vfs::VfsLayerType::Mod));

    // a.dcx should now resolve to mod_chr_a
    res = vfs.ResolvePath("/app0/dvdroot_ps4/chr/a.dcx");
    assert(res.has_value() && fs::equivalent(*res, mod_chr_a));
    // b.dcx should still fall through to base_chr_b
    res = vfs.ResolvePath("/app0/dvdroot_ps4/chr/b.dcx");
    assert(res.has_value() && fs::equivalent(*res, base_chr_b));

    // 5. Test Loose Folder Mod Layout (<mod>/chr/a.dcx mapping to dvdroot_ps4/chr/a.dcx)
    auto loose_mod_dir = temp_root / "loose_mod";
    auto loose_chr = loose_mod_dir / "chr";
    fs::create_directories(loose_chr);
    auto loose_chr_a = loose_chr / "a.dcx";
    {
        std::ofstream ofs(loose_chr_a);
        ofs << "loose_a";
    }
    assert(vfs.MountMod(loose_mod_dir, "loose_mod"));
    // Since loose_mod was mounted after test_mod, loose_mod should win!
    res = vfs.ResolvePath("/app0/dvdroot_ps4/chr/a.dcx");
    assert(res.has_value() && fs::equivalent(*res, loose_chr_a));

    // 6. Test Case-Insensitive Matching
    auto case_mod_dir = temp_root / "case_mod";
    auto case_chr = case_mod_dir / "Chr";
    fs::create_directories(case_chr);
    auto case_file = case_chr / "UPPER.DCX";
    {
        std::ofstream ofs(case_file);
        ofs << "upper_content";
    }
    assert(vfs.MountMod(case_mod_dir, "case_mod"));
    // Query lowercase: /app0/dvdroot_ps4/chr/upper.dcx
    res = vfs.ResolvePath("/app0/dvdroot_ps4/chr/upper.dcx");
    assert(res.has_value() && fs::equivalent(*res, case_file));

    // 7. Test LoadMods with mods.json (ordering and disabled)
    auto mods_container = temp_root / "mods";
    fs::create_directories(mods_container);

    auto mod_x = mods_container / "ModX" / "chr";
    auto mod_y = mods_container / "ModY" / "chr";
    auto mod_z = mods_container / "ModZ" / "chr";
    fs::create_directories(mod_x);
    fs::create_directories(mod_y);
    fs::create_directories(mod_z);

    {
        std::ofstream ofs(mod_x / "c.dcx");
        ofs << "X";
    }
    {
        std::ofstream ofs(mod_y / "c.dcx");
        ofs << "Y";
    }
    {
        std::ofstream ofs(mod_z / "c.dcx");
        ofs << "Z";
    }

    auto mods_json = temp_root / "mods.json";
    {
        std::ofstream ofs(mods_json);
        ofs << "{\n"
            << "  \"order\": [\"ModY\", \"ModX\"],\n"
            << "  \"disabled\": [\"ModZ\"]\n"
            << "}\n";
    }

    int loaded = vfs.LoadMods(mods_container, mods_json);
    assert(loaded == 2); // ModX and ModY loaded; ModZ disabled
    assert(vfs.GetModCount() == 2);

    // In order ["ModY", "ModX"]: ModX is later, so ModX wins for c.dcx!
    res = vfs.ResolvePath("/app0/dvdroot_ps4/chr/c.dcx");
    assert(res.has_value() && fs::equivalent(*res, mod_x / "c.dcx"));

    // 8. Test C API bridges
    char host_out[1024] = {0};
    assert(bb_vfs_resolve_path("/app0/dvdroot_ps4/chr/c.dcx", host_out, sizeof(host_out)));
    assert(fs::equivalent(fs::path(host_out), mod_x / "c.dcx"));

    // Non-existent file should return false, but provide fallback in host_out
    char missing_out[1024] = {0};
    bool found = bb_vfs_resolve_path("/app0/dvdroot_ps4/chr/missing.dcx", missing_out, sizeof(missing_out));
    assert(!found);
    assert(std::string(missing_out).find("missing.dcx") != std::string::npos);

    // Cleanup
    fs::remove_all(temp_root);

    std::cout << "PASS: Virtual File System C++23 native mod layering & case-insensitive resolution verified!\n";
    return 0;
}
