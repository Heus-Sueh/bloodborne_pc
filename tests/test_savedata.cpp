#include "runtime.h"
#include "runtime_savedata.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace fs = std::filesystem;

// Minimal stubs for runtime functions needed by savedata
static std::string g_test_user_dir_str;
extern "C" {
const char* runtime_file_user_dir(void) {
    return g_test_user_dir_str.c_str();
}
int runtime_file_mount(const char* guest, const char* host) {
    (void)guest; (void)host;
    return 0;
}
void runtime_file_unmount(const char* guest) {
    (void)guest;
}
void runtime_register_exports(const RuntimeExport* table, size_t count) {
    (void)table; (void)count;
}
uintptr_t runtime_registry_resolve(const char* name, int is_data) {
    (void)name; (void)is_data;
    return 0;
}
}

int main() {
    std::cout << "[SAVEDATA C++23 TEST] Initializing...\n";

    fs::path test_user_dir = fs::temp_directory_path() / "bb_savedata_test";
    g_test_user_dir_str = test_user_dir.string();
    fs::remove_all(test_user_dir);
    fs::create_directories(test_user_dir);

    runtime_savedata_configure("CUSA03173");

    // Test SoundHack functionality
    auto sound_hack_path = test_user_dir / "savedata" / "0" / "CUSA03173" / "SPRJ0005" / "userdata0010";
    fs::create_directories(sound_hack_path.parent_path());
    {
        std::vector<uint8_t> sound_data(0x2100, 0);
        sound_data[0x204E] = 0; // Cleared sound flag
        std::ofstream sf(sound_hack_path, std::ios::binary);
        sf.write(reinterpret_cast<const char*>(sound_data.data()), sound_data.size());
    }

    bb::savedata::SaveDataManager::Instance().SoundHack();

    {
        std::ifstream sf(sound_hack_path, std::ios::binary);
        sf.seekg(0x204E);
        int flag = sf.get();
        assert(flag == 1); // Verify SoundHack fixed the flag
    }
    std::cout << "PASS: Bloodborne SoundHack audio flag verified!\n";

    runtime_savedata_report();

    fs::remove_all(test_user_dir);
    std::cout << "PASS: SaveData modern C++23 subsystem verified successfully!\n";
    return 0;
}
