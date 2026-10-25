#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <array>
#include <mutex>

#ifdef __cplusplus
namespace bb::content {

constexpr size_t kLabelSize = 17;
constexpr uint32_t kAddonInstalled = 4u;

struct AddonInfo {
    char label[kLabelSize];
    char padding[3];
    uint32_t status;
};
static_assert(sizeof(AddonInfo) == 24, "AddonInfo size must be 24 bytes");

class ContentManager {
public:
    static ContentManager& Instance();

    void Configure(const uint32_t values[5]);
    int32_t ModuleLoad(uint16_t id);
    int32_t ModuleLoaded(uint16_t id) const;
    int32_t ModuleUnload(uint16_t id);

    int32_t Initialize(const unsigned char *init, unsigned char *boot);
    int32_t GetParam(uint32_t id, int32_t *out);

    int32_t ListAddons(uint32_t service, void *list, uint32_t capacity, uint32_t *hits);
    int32_t GetAddonInfo(uint32_t service, const char *label, void *info);
    int32_t MountAddon(uint32_t service, const char *label, char *point);
    int32_t UnmountAddon(const char *point);

    void Report() const;
    void Reset();

private:
    ContentManager() = default;
    ~ContentManager() = default;
    ContentManager(const ContentManager&) = delete;
    ContentManager& operator=(const ContentManager&) = delete;

    bool FindAddonDir(std::string_view label, std::string &out_path) const;

    mutable std::mutex mutex_;
    unsigned refs_{0};
    unsigned initialized_{0};
    unsigned configured_{0};
    unsigned queries_{0};
    unsigned lists_{0};
    std::array<uint32_t, 5> parameters_{};
    std::array<unsigned, 0x10000> module_refs_{};
    std::array<std::string, 16> mounted_{};
};

} // namespace bb::content
#endif

#ifdef __cplusplus
extern "C" {
#endif

void runtime_content_configure(const uint32_t values[5]);
void runtime_content_register(void);
uintptr_t runtime_content_resolve(const char *name);
void runtime_content_report(void);

#ifdef __cplusplus
}
#endif
