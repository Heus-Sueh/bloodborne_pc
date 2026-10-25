#pragma once

#include "platform/platform.h"
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace bb::savedata {

class SaveDataManager {
public:
    static SaveDataManager& Instance();

    void Configure(std::string_view title);
    void Report() const;
    void SoundHack();

    // Reset/clear for testing or shutdown
    void Reset();

private:
    SaveDataManager() = default;
};

} // namespace bb::savedata

#ifdef __cplusplus
extern "C" {
#endif

void runtime_savedata_configure(const char *title);
void runtime_savedata_register(void);
uintptr_t runtime_savedata_resolve(const char *name);
void runtime_savedata_report(void);

#ifdef __cplusplus
}
#endif
