#include "runtime_net.hpp"
#include "runtime.h"
#include "gpu/bbnet/bbnet.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <string_view>

namespace fs = std::filesystem;

namespace bb::net {

NetworkBridge& NetworkBridge::Instance() {
    static NetworkBridge instance;
    return instance;
}

void NetworkBridge::Configure(std::string_view app0, std::string_view serial, std::string_view title, std::string_view version) {
    app0_ = std::string(app0);
    serial_ = std::string(serial);
    title_ = std::string(title);
    version_ = std::string(version);
}

bool NetworkBridge::IsEnabled() const {
    const char *v = std::getenv("BB_ONLINE");
    return (v != nullptr && v[0] == '1');
}

bool NetworkBridge::IsOnlineSymbol(std::string_view symbol) const {
    if (symbol.empty()) return false;

    static constexpr std::string_view online[] = {"sceNet", "sceNp", "sceHttp", "sceSsl"};
    static constexpr std::string_view local[] = {"sceNpTrophy", "sceNpCommerce", "sceNpProfileDialog"};

    bool match = false;
    for (const auto& prefix : online) {
        if (symbol.starts_with(prefix)) {
            match = true;
            break;
        }
    }
    if (!match) return false;

    for (const auto& prefix : local) {
        if (symbol.starts_with(prefix)) {
            return false;
        }
    }
    return true;
}

void NetworkBridge::OnImageReady(uintptr_t base, uint64_t size) {
    if (!module_handle_) return;
    auto ready = reinterpret_cast<BbNetImageReadyFn>(dlsym(module_handle_, "bbnet_image_ready"));
    if (ready) ready(base, size);
}

void NetworkBridge::OnImageMapped(uint8_t *image, uint64_t size) {
    if (!module_handle_) return;
    auto mapped = reinterpret_cast<BbNetImageMappedFn>(dlsym(module_handle_, "bbnet_image_mapped"));
    if (mapped) mapped(image, size);
}

void NetworkBridge::Start() {
    if (!module_handle_) return;
    auto start = reinterpret_cast<BbNetStartFn>(dlsym(module_handle_, "bbnet_start"));
    if (start) start();
}

uintptr_t NetworkBridge::Resolve(const char *name) {
    if (!IsEnabled()) return 0;
    const char *symbol = runtime_symbol(name);
    if (!symbol || !IsOnlineSymbol(symbol)) return 0;

    if (!tried_load_) {
        tried_load_ = true;
        std::string path;
        const char *forced = std::getenv("BB_NET_MODULE");
        if (forced && forced[0]) {
            path = forced;
        } else {
            std::error_code ec;
            fs::path exe_path = fs::read_symlink("/proc/self/exe", ec);
            if (!ec) {
                fs::path dir = exe_path.parent_path();
                fs::path p1 = dir / "gpu" / "libbbnet.so";
                fs::path p2 = dir / "gpu" / "libbbnet20.so";
                if (fs::exists(p1, ec)) {
                    path = p1.string();
                } else if (fs::exists(p2, ec)) {
                    path = p2.string();
                } else {
                    path = p1.string();
                }
            }
        }

        void *module = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!module) {
            std::printf("Online: the online module is not available (%s): offline play\n", dlerror());
            return 0;
        }
        module_handle_ = module;

        auto init = reinterpret_cast<BbNetInitFn>(dlsym(module, "bbnet_init"));
        auto found = reinterpret_cast<BbNetResolveFn>(dlsym(module, "bbnet_resolve"));

        BbNetHost host{};
        host.abi = BBNET_ABI_VERSION;
        host.app0 = app0_.c_str();
        host.serial = serial_.c_str();
        host.title = title_.c_str();
        host.app_ver = version_.c_str();
        host.thread_spawn = runtime_thread_spawn;
        host.thread_join = runtime_thread_join_spawned;
        host.thread_exit = runtime_thread_exit;
        host.memory_is_mapped = runtime_memory_is_mapped;

        if (!init || !found || init(&host) != 0) {
            std::printf("Online: %s did not start: offline play\n", path.c_str());
            return 0;
        }

        std::printf("Online: module %s\n", path.c_str());
        resolve_fn_ = reinterpret_cast<void*>(found);
    }

    if (!resolve_fn_) return 0;

    auto resolve = reinterpret_cast<BbNetResolveFn>(resolve_fn_);
    const uintptr_t address = resolve(name);
    if (!address) {
        std::printf("Online: %s has no online version; the offline one is used\n", symbol);
    }
    return address;
}

} // namespace bb::net

using namespace bb::net;

extern "C" {

void runtime_net_configure(const char *app0, const char *serial, const char *title, const char *version) {
    NetworkBridge::Instance().Configure(app0 ? app0 : "",
                                        serial ? serial : "",
                                        title ? title : "",
                                        version ? version : "");
}

int runtime_net_enabled(void) {
    return NetworkBridge::Instance().IsEnabled() ? 1 : 0;
}

int runtime_net_symbol(const char *symbol) {
    return (symbol && NetworkBridge::Instance().IsOnlineSymbol(symbol)) ? 1 : 0;
}

void runtime_net_image_ready(uintptr_t base, uint64_t size) {
    NetworkBridge::Instance().OnImageReady(base, size);
}

void runtime_net_image_mapped(uint8_t *image, uint64_t size) {
    NetworkBridge::Instance().OnImageMapped(image, size);
}

void runtime_net_start(void) {
    NetworkBridge::Instance().Start();
}

uintptr_t runtime_net_resolve(const char *name) {
    return NetworkBridge::Instance().Resolve(name);
}

} // extern "C"
