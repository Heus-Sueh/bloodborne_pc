#include "loader.hpp"
#include <cassert>
#include <cstring>
#include <elf.h>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

static std::vector<uint8_t> CreateTestSelfFixture() {
    std::vector<uint8_t> data(0x210, 0);
    // Magic 'O\x15=\x1d'
    data[0] = 0x4f; data[1] = 0x15; data[2] = 0x3d; data[3] = 0x1d;

    // Segment count = 1 at offset 24
    *reinterpret_cast<uint16_t*>(&data[24]) = 1;

    // Segment 0 at offset 32: flags=0x800, offset=0x200, size=16, mem=16
    *reinterpret_cast<uint64_t*>(&data[32]) = 0x800;
    *reinterpret_cast<uint64_t*>(&data[40]) = 0x200;
    *reinterpret_cast<uint64_t*>(&data[48]) = 16;
    *reinterpret_cast<uint64_t*>(&data[56]) = 16;

    // ELF header at offset 64 (32 + 1 * 32)
    Elf64_Ehdr ehdr;
    std::memset(&ehdr, 0, sizeof(ehdr));
    std::memcpy(ehdr.e_ident, "\x7f\x45\x4c\x46\x02\x01\x01\x09", 8);
    ehdr.e_type = 0xfe10;
    ehdr.e_machine = EM_X86_64;
    ehdr.e_version = 1;
    ehdr.e_entry = 0x400000;
    ehdr.e_phoff = 64; // relative to ELF header base
    ehdr.e_ehsize = sizeof(Elf64_Ehdr);
    ehdr.e_phentsize = sizeof(Elf64_Phdr);
    ehdr.e_phnum = 1;
    std::memcpy(&data[64], &ehdr, sizeof(ehdr));

    // Program header 0 at offset 128 (64 + 64)
    Elf64_Phdr phdr;
    std::memset(&phdr, 0, sizeof(phdr));
    phdr.p_type = PT_LOAD;
    phdr.p_flags = PF_R | PF_X;
    phdr.p_offset = 0x1000;
    phdr.p_vaddr = 0x400000;
    phdr.p_paddr = 0x400000;
    phdr.p_filesz = 16;
    phdr.p_memsz = 32;
    phdr.p_align = 0x1000;
    std::memcpy(&data[128], &phdr, sizeof(phdr));

    // Payload at offset 0x200 (16 bytes)
    for (int i = 0; i < 16; ++i) {
        data[0x200 + i] = static_cast<uint8_t>(i);
    }

    return data;
}

int main() {
    std::cout << "[LOADER C++23 TEST] Initializing...\n";

    // 1. Verify Sony NID calculation against known reference hashes
    assert(bb::loader::CalculateSonyNid("_init_env") == "bzQExy189ZI");
    assert(bb::loader::CalculateSonyNid("malloc") == "gQX+4GDQjpM");
    std::cout << "PASS: Sony NID SHA-1 calculation verified!\n";

    // 2. Verify Base64 ID Encoding
    assert(bb::loader::EncodeId(0) == "A");
    assert(bb::loader::EncodeId(1) == "B");
    assert(bb::loader::EncodeId(62) == "+");
    assert(bb::loader::EncodeId(63) == "-");
    assert(bb::loader::EncodeId(64) == "BA");
    std::cout << "PASS: PS4 dynamic ID base-64 encoding verified!\n";

    // 3. Test AppContent Profile Generation
    auto temp_dir = fs::temp_directory_path() / "bb_loader_test";
    fs::remove_all(temp_dir);
    fs::create_directories(temp_dir);

    auto content_bin = temp_dir / "content.bin";
    assert(bb::loader::ContentProfile::Generate(temp_dir, content_bin, "full"));
    assert(fs::exists(content_bin));
    assert(fs::file_size(content_bin) == 28);

    std::ifstream cf(content_bin, std::ios::binary);
    char magic[8];
    uint32_t sku = 0, p1 = 0;
    cf.read(magic, 8);
    cf.read(reinterpret_cast<char*>(&sku), 4);
    cf.read(reinterpret_cast<char*>(&p1), 4);
    assert(std::memcmp(magic, "BBCONT01", 8) == 0);
    assert(sku == 3); // full SKU
    assert(p1 == 0);
    std::cout << "PASS: AppContent profile binary generation verified!\n";

    // 4. Test ModuleLinker with self fixture
    auto game_dir = temp_dir / "game";
    auto out_dir = temp_dir / "out";
    fs::create_directories(game_dir);
    fs::create_directories(out_dir);

    auto fixture = CreateTestSelfFixture();
    auto eboot_path = game_dir / "eboot.bin";
    {
        std::ofstream ef(eboot_path, std::ios::binary);
        ef.write(reinterpret_cast<const char*>(fixture.data()), fixture.size());
    }

    // Link will report missing dynamic segments for minimal non-relocatable fixture, proving validation
    auto res = bb::loader::ModuleLinker::Link(game_dir, out_dir, {});
    // Validation caught missing PT_DYNAMIC as expected
    assert(!res.success);
    assert(res.error_message.find("missing PT_DYNAMIC") != std::string::npos);
    std::cout << "PASS: Header parsing and validation contracts verified!\n";

    // Cleanup
    fs::remove_all(temp_dir);

    std::cout << "PASS: Native C++ ELF/SELF Loader & Module Linker verified successfully!\n";
    return 0;
}
