#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#ifdef __cplusplus
namespace bb::net {

class NetworkBridge {
public:
    static NetworkBridge& Instance();

    void Configure(std::string_view app0, std::string_view serial, std::string_view title, std::string_view version);
    bool IsEnabled() const;
    bool IsOnlineSymbol(std::string_view symbol) const;
    void OnImageReady(uintptr_t base, uint64_t size);
    void OnImageMapped(uint8_t *image, uint64_t size);
    void Start();
    uintptr_t Resolve(const char *name);

private:
    NetworkBridge() = default;
    ~NetworkBridge() = default;
    NetworkBridge(const NetworkBridge&) = delete;
    NetworkBridge& operator=(const NetworkBridge&) = delete;

    std::string app0_;
    std::string serial_;
    std::string title_;
    std::string version_;
    void *module_handle_{nullptr};
    bool tried_load_{false};
    void *resolve_fn_{nullptr};
};

} // namespace bb::net
#endif

#ifdef __cplusplus
extern "C" {
#endif

void runtime_net_configure(const char *app0, const char *serial, const char *title, const char *version);
int runtime_net_enabled(void);
int runtime_net_symbol(const char *symbol);
void runtime_net_image_ready(uintptr_t base, uint64_t size);
void runtime_net_image_mapped(uint8_t *image, uint64_t size);
void runtime_net_start(void);
uintptr_t runtime_net_resolve(const char *name);

#ifdef __cplusplus
}
#endif
