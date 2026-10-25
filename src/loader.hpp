#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bb::loader {

struct PrepareResult {
    bool success{false};
    std::string error_message;
    uint64_t image_size{0};
    uint64_t entry_point{0};
    size_t segment_count{0};
    size_t relocation_count{0};
    size_t import_count{0};
    size_t binding_count{0};
    size_t fs_loads_patched{0};
};

class ModuleLinker {
public:
    static PrepareResult Link(
        const std::filesystem::path& game_dir,
        const std::filesystem::path& out_dir,
        const std::vector<std::string>& module_names = {"libc.prx", "libSceFios2.prx"}
    );
};

class ContentProfile {
public:
    static bool Generate(
        const std::filesystem::path& game_dir,
        const std::filesystem::path& out_file,
        std::string_view sku = "full"
    );
};

// Utilities exposed for verification and unit tests
std::string CalculateSonyNid(std::string_view name);
std::string EncodeId(uint64_t value);

} // namespace bb::loader
