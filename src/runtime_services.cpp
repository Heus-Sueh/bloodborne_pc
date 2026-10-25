#include "runtime_services.hpp"
#include "runtime.h"
#include "core/bbhost.h"

#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <time.h>

namespace bb::runtime {

constexpr int32_t USER_ID                = 1;
constexpr int32_t USER_INVALID_ARGUMENT  = static_cast<int32_t>(0x80960005);
constexpr int32_t USER_NO_EVENT          = static_cast<int32_t>(0x80960007);
constexpr int32_t SYSTEM_NO_EVENT        = static_cast<int32_t>(0x80A10004);
constexpr int32_t SYSTEM_PARAMETER       = static_cast<int32_t>(0x80A10003);
constexpr int32_t NET_CTL_NOT_CONNECTED  = static_cast<int32_t>(0x80412108);
constexpr int32_t NET_CTL_INVALID_ADDR   = static_cast<int32_t>(0x80412107);
constexpr int32_t NP_SIGNED_OUT          = static_cast<int32_t>(0x80550006);
constexpr int32_t NP_INVALID_ARGUMENT    = static_cast<int32_t>(0x80550003);
constexpr int32_t NET_ENETUNREACH        = static_cast<int32_t>(0x80410133);
constexpr int32_t NET_EINVAL             = static_cast<int32_t>(0x80410116);
constexpr int32_t HTTP_NETWORK           = static_cast<int32_t>(0x80431063);
constexpr int32_t DIALOG_NOT_INITIALIZED = static_cast<int32_t>(0x80B80003);
constexpr int32_t PLAYGO_BAD_HANDLE      = static_cast<int32_t>(0x80B20009);
constexpr int32_t PLAYGO_BAD_POINTER     = static_cast<int32_t>(0x80B2000A);
constexpr int32_t PLAYGO_BAD_SIZE        = static_cast<int32_t>(0x80B2000B);
constexpr int32_t PLAYGO_BAD_CHUNK       = static_cast<int32_t>(0x80B2000C);
constexpr int32_t AUDIO_IN_NOT_OPENED    = static_cast<int32_t>(0x80260109);
constexpr int32_t TROPHY_INVALID         = static_cast<int32_t>(0x80551604);
constexpr int32_t DISC_MAP_NO_BITMAP     = static_cast<int32_t>(0x81100004);

static std::atomic<int> g_next_id{1};
inline int NewId() {
    return g_next_id.fetch_add(1, std::memory_order_relaxed);
}

inline void Note(const char* what) {
    std::printf("Runtime: %s\n", what);
}

// User Services
class UserService {
public:
    static UserService& Instance() {
        static UserService instance;
        return instance;
    }

    int32_t Initialize(const void* /*params*/) {
        Note("UserService initialized (user 1 logged in)");
        return 0;
    }

    int32_t Terminate() {
        return 0;
    }

    int32_t GetInitial(int32_t* id) {
        if (!id) return USER_INVALID_ARGUMENT;
        *id = USER_ID;
        return 0;
    }

    int32_t GetList(int32_t* ids) {
        if (!ids) return USER_INVALID_ARGUMENT;
        ids[0] = USER_ID;
        ids[1] = ids[2] = ids[3] = -1;
        return 0;
    }

    int32_t GetName(int32_t id, char* name, uint64_t size) {
        if (id != USER_ID || !name) return USER_INVALID_ARGUMENT;
        const char* value = std::getenv("BB_USER_NAME");
        if (!value) value = "Hunter";
        if (std::strlen(value) + 1 > size) return static_cast<int32_t>(0x8096000a); // BUFFER_TOO_SHORT
        std::strcpy(name, value);
        return 0;
    }

    int32_t GetEvent(int32_t* event) {
        if (!event) return USER_INVALID_ARGUMENT;
        bool expected = true;
        if (!login_event_pending_.compare_exchange_strong(expected, false)) {
            return USER_NO_EVENT;
        }
        event[0] = 0;       // LOGIN
        event[1] = USER_ID;
        return 0;
    }

private:
    std::atomic<bool> login_event_pending_{true};
};

// System Services
class SystemService {
public:
    static SystemService& Instance() {
        static SystemService instance;
        return instance;
    }

    static int GetLanguage() {
        const char* v = std::getenv("BB_LANGUAGE");
        return v ? std::atoi(v) : 1;
    }

    int32_t GetParam(int32_t id, int32_t* value) {
        if (!value) return SYSTEM_PARAMETER;
        switch (id) {
            case 1: *value = GetLanguage(); break; // language (1 = English US, 8 = Russian)
            case 2: *value = 1; break;             // date format DD/MM/YYYY
            case 3: *value = 1; break;             // 24-hour clock
            case 4: {
                time_t now = time(nullptr);
                struct tm t;
                localtime_r(&now, &t);
                *value = static_cast<int32_t>(RUNTIME_TM_GMTOFF(t) / 60);
                break;
            }
            case 5: *value = 0; break;             // summer time
            case 7: *value = 0; break;             // parental level off
            case 1000: *value = 1; break;          // enter button = cross
            default:
                std::fprintf(stderr, "STOP: unsupported system parameter %d\n", id);
                std::exit(21);
        }
        return 0;
    }

    int32_t GetStatus(unsigned char* status) {
        if (!status) return SYSTEM_PARAMETER;
        std::memset(status, 0, 12); // event_num=0, no overlay, foreground, normal CPU mode
        return 0;
    }

    int32_t GetEvent(void* /*event*/) {
        return SYSTEM_NO_EVENT;
    }

    int32_t HideSplash() {
        Note("SystemService: splash screen hidden");
        return 0;
    }

    int32_t LaunchBrowser() {
        Note("SystemService: web browser request ignored (offline)");
        return 0;
    }
};

// Common Dialogs
struct DialogInfo {
    const char* name;
    int initialized{0};
    int status{0};
};

class DialogManager {
public:
    static DialogManager& Instance() {
        static DialogManager instance;
        return instance;
    }

    int32_t CommonInit() {
        std::lock_guard<std::mutex> lock(lock_);
        common_initialized_ = true;
        return 0;
    }

    int32_t Init(size_t index) {
        std::lock_guard<std::mutex> lock(lock_);
        if (index >= dialogs_.size()) return DIALOG_NOT_INITIALIZED;
        if (dialogs_[index].initialized) return static_cast<int32_t>(0x80B80004);
        dialogs_[index].initialized = 1;
        dialogs_[index].status = 1;
        return 0;
    }

    int32_t Open(size_t index) {
        std::lock_guard<std::mutex> lock(lock_);
        if (index >= dialogs_.size() || !dialogs_[index].initialized) return DIALOG_NOT_INITIALIZED;
        std::printf("Runtime: %s opened; completed immediately (no dialog UI yet)\n", dialogs_[index].name);
        dialogs_[index].status = 3;
        return 0;
    }

    int32_t Status(size_t index) {
        std::lock_guard<std::mutex> lock(lock_);
        if (index >= dialogs_.size()) return 0;
        return dialogs_[index].status;
    }

    int32_t Terminate(size_t index) {
        std::lock_guard<std::mutex> lock(lock_);
        if (index >= dialogs_.size() || !dialogs_[index].initialized) return DIALOG_NOT_INITIALIZED;
        dialogs_[index].initialized = 0;
        dialogs_[index].status = 0;
        return 0;
    }

private:
    std::mutex lock_;
    bool common_initialized_{false};
    std::array<DialogInfo, 6> dialogs_{{
        {"CommonDialog", 0, 0},
        {"MsgDialog", 0, 0},
        {"SaveDataDialog", 0, 0},
        {"NpProfileDialog", 0, 0},
        {"NpCommerceDialog", 0, 0},
        {"ImeDialog", 0, 0}
    }};
};

// Ime Dialog
struct ImeParam {
    int32_t user;
    uint32_t type;
    uint64_t languages;
    uint32_t enter_label;
    uint32_t input_method;
    void* filter;
    uint32_t option;
    uint32_t max_length;
    uint16_t* buffer;
    float x;
    float y;
    uint32_t halign;
    uint32_t valign;
    const uint16_t* placeholder;
    const uint16_t* title;
    int8_t reserved[16];
};

class ImeDialogManager {
public:
    static ImeDialogManager& Instance() {
        static ImeDialogManager instance;
        return instance;
    }

    int32_t Init(const ImeParam* param, const void* /*extended*/) {
        if (!param || !param->buffer || !param->max_length) {
            return static_cast<int32_t>(0x80BC0004); // IME INVALID_ADDRESS
        }

        std::lock_guard<std::mutex> lock(lock_);
        if (running_) {
            return static_cast<int32_t>(0x80BC0003); // BUSY
        }

        buffer_ = param->buffer;
        max_length_ = param->max_length;
        running_ = true;
        finished_ = false;
        end_status_ = 0;

        char initial[512]{};
        char prompt[256]{};
        Utf16ToUtf8(param->buffer, param->max_length, initial, sizeof(initial));
        Utf16ToUtf8(param->title, 128, prompt, sizeof(prompt));

        if (const char* preset = std::getenv("BB_IME_TEXT"); preset) {
            CompleteLocked(0, preset);
            return 0;
        }

        if (!bbhost_text_input_begin(initial, prompt[0] ? prompt : "Text")) {
            const char* name = std::getenv("BB_USER_NAME");
            CompleteLocked(0, name ? name : (initial[0] ? initial : "Hunter"));
        } else {
            std::printf("Runtime: ImeDialog opened: type in the game window, Enter to confirm, Esc to cancel\n");
        }
        return 0;
    }

    int32_t PollStatus() {
        std::lock_guard<std::mutex> lock(lock_);
        if (running_) {
            char text[512]{};
            int state = bbhost_text_input_poll(text, sizeof(text));
            if (state) {
                CompleteLocked(state == 1 ? 0 : 1, text);
            }
        }
        return running_ ? 1 : finished_ ? 2 : 0;
    }

    int32_t GetResult(uint32_t* result) {
        std::lock_guard<std::mutex> lock(lock_);
        if (!finished_) return static_cast<int32_t>(0x80BC0101); // DIALOG NOT_FINISHED
        if (result) *result = static_cast<uint32_t>(end_status_);
        return 0;
    }

    int32_t Terminate() {
        std::lock_guard<std::mutex> lock(lock_);
        running_ = false;
        finished_ = false;
        end_status_ = 0;
        buffer_ = nullptr;
        max_length_ = 0;
        return 0;
    }

private:
    void CompleteLocked(int end_status, const char* text) {
        if (!end_status && buffer_ && max_length_) {
            Utf8ToUtf16(text, buffer_, max_length_);
        }
        end_status_ = end_status;
        finished_ = true;
        running_ = false;
        std::printf("Runtime: ImeDialog %s%s\n", end_status ? "cancelled" : "text: ", end_status ? "" : text);
    }

    static size_t Utf16ToUtf8(const uint16_t* in, size_t limit, char* out, size_t size) {
        size_t n = 0;
        for (size_t i = 0; in && i < limit && in[i] && n + 4 < size; ++i) {
            uint32_t c = in[i];
            if (c >= 0xD800 && c < 0xDC00 && i + 1 < limit && in[i + 1] >= 0xDC00 && in[i + 1] < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (in[i + 1] - 0xDC00);
                ++i;
            }
            if (c < 0x80) out[n++] = static_cast<char>(c);
            else if (c < 0x800) {
                out[n++] = static_cast<char>(0xC0 | (c >> 6));
                out[n++] = static_cast<char>(0x80 | (c & 63));
            } else if (c < 0x10000) {
                out[n++] = static_cast<char>(0xE0 | (c >> 12));
                out[n++] = static_cast<char>(0x80 | ((c >> 6) & 63));
                out[n++] = static_cast<char>(0x80 | (c & 63));
            } else {
                out[n++] = static_cast<char>(0xF0 | (c >> 18));
                out[n++] = static_cast<char>(0x80 | ((c >> 12) & 63));
                out[n++] = static_cast<char>(0x80 | ((c >> 6) & 63));
                out[n++] = static_cast<char>(0x80 | (c & 63));
            }
        }
        out[n] = 0;
        return n;
    }

    static void Utf8ToUtf16(const char* in, uint16_t* out, uint32_t max) {
        uint32_t n = 0;
        for (const auto* p = reinterpret_cast<const unsigned char*>(in); *p && n < max;) {
            uint32_t c = 0;
            int extra = 0;
            if (*p < 0x80) { c = *p; extra = 0; }
            else if ((*p & 0xE0) == 0xC0) { c = *p & 31; extra = 1; }
            else if ((*p & 0xF0) == 0xE0) { c = *p & 15; extra = 2; }
            else { c = *p & 7; extra = 3; }
            ++p;
            for (int k = 0; k < extra && (*p & 0xC0) == 0x80; ++k) {
                c = (c << 6) | (*p++ & 63);
            }
            if (c >= 0x10000) {
                if (n + 2 > max) break;
                c -= 0x10000;
                out[n++] = static_cast<uint16_t>(0xD800 + (c >> 10));
                out[n++] = static_cast<uint16_t>(0xDC00 + (c & 1023));
            } else {
                out[n++] = static_cast<uint16_t>(c);
            }
        }
        out[n < max ? n : max] = 0;
    }

    std::mutex lock_;
    bool running_{false};
    bool finished_{false};
    int end_status_{0};
    uint16_t* buffer_{nullptr};
    uint32_t max_length_{0};
};

// PlayGo
class PlayGoManager {
public:
    static PlayGoManager& Instance() {
        static PlayGoManager instance;
        return instance;
    }

    int32_t Initialize(const void* /*params*/) {
        int fd = static_cast<int>(runtime_file_open("/app0/sce_sys/playgo-chunk.dat", 0, 0));
        unsigned char header[12]{};
        chunks_ = 1;
        if (fd >= 0) {
            if (runtime_file_read(fd, header, sizeof(header)) == sizeof(header)) {
                chunks_ = header[10] | (header[11] << 8);
            }
            runtime_file_close(fd);
        }
        std::printf("Runtime: PlayGo initialized; %d chunks, all installed locally\n", chunks_);
        return 0;
    }

    int32_t Open(int32_t* handle, const void* /*param*/) {
        if (!handle) return PLAYGO_BAD_POINTER;
        handle_ = 1;
        *handle = 1;
        return 0;
    }

    int32_t GetChunkIds(int32_t handle, uint16_t* ids, uint32_t count, uint32_t* out) {
        if (handle != handle_ || !handle) return PLAYGO_BAD_HANDLE;
        if (!out) return PLAYGO_BAD_POINTER;
        if (ids && !count) return PLAYGO_BAD_SIZE;
        if (!ids) {
            *out = static_cast<uint32_t>(chunks_);
            return 0;
        }
        uint32_t n = count < static_cast<uint32_t>(chunks_) ? count : static_cast<uint32_t>(chunks_);
        for (uint32_t i = 0; i < n; ++i) {
            ids[i] = static_cast<uint16_t>(i);
        }
        *out = n;
        return 0;
    }

    int32_t GetLocus(int32_t handle, const uint16_t* ids, uint32_t count, int8_t* loci) {
        if (handle != handle_ || !handle) return PLAYGO_BAD_HANDLE;
        if (!ids || !loci) return PLAYGO_BAD_POINTER;
        if (!count) return PLAYGO_BAD_SIZE;
        for (uint32_t i = 0; i < count; ++i) {
            if (ids[i] >= chunks_) return PLAYGO_BAD_CHUNK;
            loci[i] = 3; // LocalFast
        }
        return 0;
    }

    int32_t SetInstallSpeed(int32_t handle, int32_t /*speed*/) {
        return (handle == handle_ && handle) ? 0 : PLAYGO_BAD_HANDLE;
    }

private:
    int handle_{0};
    int chunks_{-1};
};

} // namespace bb::runtime

// Guest ABI Trampolines
extern "C" {

using namespace bb::runtime;

static thread_local int32_t net_errno = 0;

static ABI int32_t user_initialize(const void *params) { return UserService::Instance().Initialize(params); }
static ABI int32_t user_terminate(void) { return UserService::Instance().Terminate(); }
static ABI int32_t user_initial(int32_t *id) { return UserService::Instance().GetInitial(id); }
static ABI int32_t user_list(int32_t *ids) { return UserService::Instance().GetList(ids); }
static ABI int32_t user_name(int32_t id, char *name, uint64_t size) { return UserService::Instance().GetName(id, name, size); }
static ABI int32_t user_event(int32_t *event) { return UserService::Instance().GetEvent(event); }

static ABI int32_t system_param(int32_t id, int32_t *value) { return SystemService::Instance().GetParam(id, value); }
static ABI int32_t system_status(unsigned char *status) { return SystemService::Instance().GetStatus(status); }
static ABI int32_t system_event(void *event) { return SystemService::Instance().GetEvent(event); }
static ABI int32_t hide_splash(void) { return SystemService::Instance().HideSplash(); }
static ABI int32_t launch_browser(void) { return SystemService::Instance().LaunchBrowser(); }

static ABI int32_t net_init(void) { return 0; }
static ABI int32_t net_term(void) { return 0; }
static ABI int32_t *net_errno_loc(void) { return &net_errno; }
static ABI int32_t net_pool_create(const char */*name*/, int /*size*/, int /*flags*/) { return NewId(); }
static ABI int32_t net_pool_destroy(int /*id*/) { return 0; }
static ABI int32_t net_unreachable(void) { net_errno = 51; return NET_ENETUNREACH; }
static ABI int32_t net_epoll_create(const char */*name*/, int /*flags*/) { return NewId(); }
static ABI int32_t net_epoll_destroy(int /*id*/) { return 0; }
static ABI int32_t net_resolver_create(const char */*name*/, int /*pool*/, int /*flags*/) { return NewId(); }
static ABI int32_t net_resolver_destroy(int /*id*/) { return 0; }
static ABI uint16_t net_htons(uint16_t v) { return htons(v); }
static ABI uint16_t net_ntohs(uint16_t v) { return ntohs(v); }
static ABI uint32_t net_htonl(uint32_t v) { return htonl(v); }
static ABI uint32_t net_ntohl(uint32_t v) { return ntohl(v); }
static ABI int32_t net_pton(int af, const char *src, void *dst) {
    if (af != 2) { net_errno = 47; return NET_EINVAL; }
    return inet_pton(AF_INET, src, dst);
}
static ABI const char *net_ntop(int af, const void *src, char *dst, uint32_t size) {
    if (af != 2) { net_errno = 47; return nullptr; }
    return inet_ntop(AF_INET, src, dst, size);
}

static ABI int32_t netctl_state(int32_t *state) { if (!state) return NET_CTL_INVALID_ADDR; *state = 0; return 0; }
static ABI int32_t netctl_info(int /*code*/, void */*info*/) { return NET_CTL_NOT_CONNECTED; }
static ABI int32_t netctl_register(void */*cb*/, void */*arg*/, int32_t *cid) {
    if (!cid) return NET_CTL_INVALID_ADDR;
    *cid = NewId();
    return 0;
}
static ABI int32_t netctl_check(void) { return 0; }
static ABI int32_t netctl_unregister(int /*cid*/) { return 0; }
static ABI int32_t netctl_nat(uint32_t *info) {
    if (!info) return NET_CTL_INVALID_ADDR;
    info[1] = 0; info[2] = 3; info[3] = 0;
    return 0;
}

static ABI int32_t lib_init_id(void) { return NewId(); }
static ABI int32_t ok_void(void) { return 0; }
static ABI int32_t http_fail(void) { return HTTP_NETWORK; }
static ABI int32_t http_object(void) { return NewId(); }
static ABI int32_t http_epoll(int32_t /*ctx*/, void **handle) {
    if (!handle) return static_cast<int32_t>(0x80431077); // HTTP INVALID_VALUE
    *handle = reinterpret_cast<void*>(static_cast<uintptr_t>(0x100 + NewId()));
    return 0;
}
static ABI int32_t http_wait(void */*handle*/, void */*events*/, int32_t /*max*/, int64_t timeout) {
    if (timeout > 0) {
        struct timespec t = {static_cast<time_t>(timeout / 1000000), static_cast<long>((timeout % 1000000) * 1000)};
        nanosleep(&t, nullptr);
    }
    return 0;
}

static ABI int32_t np_state(int32_t /*user*/, int32_t *state) {
    if (!state) return NP_INVALID_ARGUMENT;
    *state = 1; // SIGNED_OUT
    return 0;
}
static ABI int32_t np_signed_out(void) { return NP_SIGNED_OUT; }
static ABI int32_t np_register(void */*cb*/, void */*arg*/) { return NewId(); }
static ABI void np_register_void(void */*cb*/, void */*arg*/) {}
static ABI int32_t np_request(const void */*param*/) { return NewId(); }
static ABI int32_t np_request_ctx(int32_t /*ctx*/, const void */*param*/) { return NewId(); }
static ABI int32_t np_poll(int32_t /*request*/, int32_t *result) { if (result) *result = NP_SIGNED_OUT; return 0; }
static ABI int32_t np_compare(const void *a, const void *b) {
    if (!a || !b) return NP_INVALID_ARGUMENT;
    return std::memcmp(a, b, 16) ? static_cast<int32_t>(0x80550609) : 0; // NP_UTIL NOT_MATCH
}

static ABI int32_t voice_port(void */*param*/, uint32_t *port) {
    if (!port) return static_cast<int32_t>(0x8029000b);
    *port = static_cast<uint32_t>(NewId());
    return 0;
}
static ABI int32_t voice_read(uint32_t /*port*/, void */*data*/, uint32_t *size) { if (size) *size = 0; return 0; }
static ABI int32_t voice_write(uint32_t /*port*/, const void */*data*/, uint32_t */*size*/) { return 0; }
static ABI int32_t voice_info(uint32_t /*port*/, uint32_t *info) {
    if (!info) return static_cast<int32_t>(0x8029000b);
    std::memset(info, 0, 40);
    return 0;
}

static ABI int32_t common_init(void) { return DialogManager::Instance().CommonInit(); }
static ABI int32_t msg_init(void) { return DialogManager::Instance().Init(1); }
static ABI int32_t msg_open(const void */*p*/) { return DialogManager::Instance().Open(1); }
static ABI int32_t msg_status(void) { return DialogManager::Instance().Status(1); }
static ABI int32_t msg_term(void) { return DialogManager::Instance().Terminate(1); }

static ABI int32_t save_init(void) { return DialogManager::Instance().Init(2); }
static ABI int32_t save_open(const void */*p*/) { return DialogManager::Instance().Open(2); }
static ABI int32_t save_status(void) { return DialogManager::Instance().Status(2); }
static ABI int32_t save_term(void) { return DialogManager::Instance().Terminate(2); }

static ABI int32_t profile_init(void) { return DialogManager::Instance().Init(3); }
static ABI int32_t profile_open(const void */*p*/) { return DialogManager::Instance().Open(3); }
static ABI int32_t profile_status(void) { return DialogManager::Instance().Status(3); }
static ABI int32_t profile_term(void) { return DialogManager::Instance().Terminate(3); }
static ABI int32_t profile_result(void *result) { if (result) std::memset(result, 0, 4); return 0; }

static ABI int32_t commerce_init(void) { return DialogManager::Instance().Init(4); }
static ABI int32_t commerce_open(const void */*p*/) { return DialogManager::Instance().Open(4); }
static ABI int32_t commerce_status(void) { return DialogManager::Instance().Status(4); }
static ABI int32_t commerce_term(void) { return DialogManager::Instance().Terminate(4); }

static ABI int32_t ime_init(const ImeParam *param, const void *extended) {
    return ImeDialogManager::Instance().Init(param, extended);
}
static ABI int32_t ime_status(void) { return ImeDialogManager::Instance().PollStatus(); }
static ABI int32_t ime_result(uint32_t *result) { return ImeDialogManager::Instance().GetResult(result); }
static ABI int32_t ime_term(void) { return ImeDialogManager::Instance().Terminate(); }

static ABI int32_t trophy_context(int32_t *ctx, int32_t /*user*/, uint32_t /*label*/, uint64_t /*options*/) {
    if (!ctx) return TROPHY_INVALID;
    *ctx = NewId();
    return 0;
}
static ABI int32_t trophy_handle(int32_t *handle) {
    if (!handle) return TROPHY_INVALID;
    *handle = NewId();
    return 0;
}
static ABI int32_t trophy_register(int32_t /*ctx*/, int32_t /*handle*/, uint64_t /*options*/) { return 0; }
static ABI int32_t trophy_unlock(int32_t /*ctx*/, int32_t /*handle*/, int32_t id, int32_t *platinum) {
    std::printf("Runtime: trophy %d unlocked\n", id);
    if (platinum) *platinum = -1;
    return 0;
}
static ABI int32_t trophy_game_info(int32_t /*ctx*/, int32_t /*handle*/, void *details, void *data) {
    if (details) {
        uint64_t size = 0;
        std::memcpy(&size, details, 8);
        std::memset(static_cast<char*>(details) + 8, 0, (size > 8 && size < 4096) ? (size - 8) : 0);
    }
    if (data) {
        uint64_t size = 0;
        std::memcpy(&size, data, 8);
        std::memset(static_cast<char*>(data) + 8, 0, (size > 8 && size < 4096) ? (size - 8) : 0);
    }
    return 0;
}
static ABI int32_t trophy_info(int32_t ctx, int32_t handle, int32_t /*id*/, void *details, void *data) {
    return trophy_game_info(ctx, handle, details, data);
}

static ABI int32_t playgo_init(const void *params) { return PlayGoManager::Instance().Initialize(params); }
static ABI int32_t playgo_open(int32_t *handle, const void *param) { return PlayGoManager::Instance().Open(handle, param); }
static ABI int32_t playgo_chunk_ids(int32_t handle, uint16_t *ids, uint32_t count, uint32_t *out) {
    return PlayGoManager::Instance().GetChunkIds(handle, ids, count, out);
}
static ABI int32_t playgo_locus(int32_t handle, const uint16_t *ids, uint32_t count, int8_t *loci) {
    return PlayGoManager::Instance().GetLocus(handle, ids, count, loci);
}
static ABI int32_t playgo_speed(int32_t handle, int32_t speed) {
    return PlayGoManager::Instance().SetInstallSpeed(handle, speed);
}

static ABI int32_t discmap_on_hdd(const char */*path*/, int64_t /*offset*/, int64_t /*size*/, int32_t */*result*/) {
    return DISC_MAP_NO_BITMAP;
}
static ABI int32_t discmap_8a82(const char */*path*/, int64_t /*offset*/, int64_t /*size*/, int32_t */*flags*/, int32_t */*r1*/, int32_t */*r2*/) {
    return DISC_MAP_NO_BITMAP;
}

static ABI int32_t mouse_open(int32_t /*user*/, int32_t /*type*/, int32_t /*index*/, const void */*param*/) { return NewId(); }
static ABI int32_t mouse_read(int32_t /*handle*/, unsigned char *data, int32_t count) {
    if (!data || count < 1) return static_cast<int32_t>(0x80DF0001);
    std::memset(data, 0, 40);
    return 1; // timestamp 0, connected=false
}
static ABI int32_t mouse_close(int32_t /*handle*/) { return 0; }
static ABI int32_t audio_in_open(void) { return AUDIO_IN_NOT_OPENED; }

static const RuntimeExport services_exports[] = {
    {"sceUserServiceInitialize", reinterpret_cast<void*>(user_initialize)},
    {"sceUserServiceTerminate", reinterpret_cast<void*>(user_terminate)},
    {"sceUserServiceGetInitialUser", reinterpret_cast<void*>(user_initial)},
    {"sceUserServiceGetLoginUserIdList", reinterpret_cast<void*>(user_list)},
    {"sceUserServiceGetUserName", reinterpret_cast<void*>(user_name)},
    {"sceUserServiceGetEvent", reinterpret_cast<void*>(user_event)},
    {"sceSystemServiceGetParam", reinterpret_cast<void*>(system_param)},
    {"sceSystemServiceGetStatus", reinterpret_cast<void*>(system_status)},
    {"sceSystemServiceReceiveEvent", reinterpret_cast<void*>(system_event)},
    {"sceSystemServiceHideSplashScreen", reinterpret_cast<void*>(hide_splash)},
    {"sceSystemServiceLaunchWebBrowser", reinterpret_cast<void*>(launch_browser)},
    {"sceNetInit", reinterpret_cast<void*>(net_init)},
    {"sceNetTerm", reinterpret_cast<void*>(net_term)},
    {"sceNetErrnoLoc", reinterpret_cast<void*>(net_errno_loc)},
    {"sceNetPoolCreate", reinterpret_cast<void*>(net_pool_create)},
    {"sceNetPoolDestroy", reinterpret_cast<void*>(net_pool_destroy)},
    {"sceNetSocket", reinterpret_cast<void*>(net_unreachable)},
    {"sceNetSocketClose", reinterpret_cast<void*>(ok_void)},
    {"sceNetConnect", reinterpret_cast<void*>(net_unreachable)},
    {"sceNetSend", reinterpret_cast<void*>(net_unreachable)},
    {"sceNetRecv", reinterpret_cast<void*>(net_unreachable)},
    {"sceNetEpollCreate", reinterpret_cast<void*>(net_epoll_create)},
    {"sceNetEpollDestroy", reinterpret_cast<void*>(net_epoll_destroy)},
    {"sceNetEpollControl", reinterpret_cast<void*>(ok_void)},
    {"sceNetEpollWait", reinterpret_cast<void*>(ok_void)},
    {"sceNetResolverCreate", reinterpret_cast<void*>(net_resolver_create)},
    {"sceNetResolverDestroy", reinterpret_cast<void*>(net_resolver_destroy)},
    {"sceNetResolverStartNtoa", reinterpret_cast<void*>(net_unreachable)},
    {"sceNetHtons", reinterpret_cast<void*>(net_htons)},
    {"sceNetNtohs", reinterpret_cast<void*>(net_ntohs)},
    {"sceNetHtonl", reinterpret_cast<void*>(net_htonl)},
    {"sceNetNtohl", reinterpret_cast<void*>(net_ntohl)},
    {"sceNetInetPton", reinterpret_cast<void*>(net_pton)},
    {"sceNetInetNtop", reinterpret_cast<void*>(net_ntop)},
    {"sceNetCtlInit", reinterpret_cast<void*>(net_init)},
    {"sceNetCtlTerm", reinterpret_cast<void*>(net_term)},
    {"sceNetCtlGetState", reinterpret_cast<void*>(netctl_state)},
    {"sceNetCtlGetInfo", reinterpret_cast<void*>(netctl_info)},
    {"sceNetCtlRegisterCallback", reinterpret_cast<void*>(netctl_register)},
    {"sceNetCtlCheckCallback", reinterpret_cast<void*>(netctl_check)},
    {"sceNetCtlUnregisterCallback", reinterpret_cast<void*>(netctl_unregister)},
    {"sceNetCtlGetNatInfo", reinterpret_cast<void*>(netctl_nat)},
    {"sceSslInit", reinterpret_cast<void*>(lib_init_id)},
    {"sceSslTerm", reinterpret_cast<void*>(ok_void)},
    {"sceHttpInit", reinterpret_cast<void*>(lib_init_id)},
    {"sceHttpTerm", reinterpret_cast<void*>(ok_void)},
    {"sceHttpCreateConnectionPool", reinterpret_cast<void*>(http_object)},
    {"sceHttpCreateTemplate", reinterpret_cast<void*>(http_object)},
    {"sceHttpCreateRequestWithURL", reinterpret_cast<void*>(http_object)},
    {"sceHttpDeleteConnectionPool", reinterpret_cast<void*>(ok_void)},
    {"sceHttpDeleteTemplate", reinterpret_cast<void*>(ok_void)},
    {"sceHttpDeleteRequest", reinterpret_cast<void*>(ok_void)},
    {"sceHttpSendRequest", reinterpret_cast<void*>(http_fail)},
    {"sceHttpAbortRequest", reinterpret_cast<void*>(ok_void)},
    {"sceHttpReadData", reinterpret_cast<void*>(http_fail)},
    {"sceHttpGetResponseContentLength", reinterpret_cast<void*>(http_fail)},
    {"sceHttpGetStatusCode", reinterpret_cast<void*>(http_fail)},
    {"sceHttpSetEpoll", reinterpret_cast<void*>(http_epoll)},
    {"sceHttpUnsetEpoll", reinterpret_cast<void*>(ok_void)},
    {"sceHttpWaitRequest", reinterpret_cast<void*>(http_wait)},
    {"sceNpCheckNpAvailability", reinterpret_cast<void*>(np_signed_out)},
    {"sceNpGetState", reinterpret_cast<void*>(np_state)},
    {"sceNpRegisterStateCallback", reinterpret_cast<void*>(np_register)},
    {"sceNpUnregisterStateCallback", reinterpret_cast<void*>(ok_void)},
    {"sceNpRegisterPlusEventCallback", reinterpret_cast<void*>(np_register_void)},
    {"sceNpUnregisterPlusEventCallback", reinterpret_cast<void*>(ok_void)},
    {"sceNpCheckPlus", reinterpret_cast<void*>(np_signed_out)},
    {"sceNpCmpNpId", reinterpret_cast<void*>(np_compare)},
    {"sceNpCmpOnlineId", reinterpret_cast<void*>(np_compare)},
    {"sceNpAuthCreateAsyncRequest", reinterpret_cast<void*>(np_request)},
    {"sceNpAuthDeleteRequest", reinterpret_cast<void*>(ok_void)},
    {"sceNpAuthPollAsync", reinterpret_cast<void*>(np_poll)},
    {"sceNpAuthGetAuthorizationCode", reinterpret_cast<void*>(np_signed_out)},
    {"sceNpLookupCreateTitleCtx", reinterpret_cast<void*>(np_request)},
    {"sceNpLookupDeleteTitleCtx", reinterpret_cast<void*>(ok_void)},
    {"sceNpLookupCreateAsyncRequest", reinterpret_cast<void*>(np_request_ctx)},
    {"sceNpLookupDeleteRequest", reinterpret_cast<void*>(ok_void)},
    {"sceNpLookupAbortRequest", reinterpret_cast<void*>(ok_void)},
    {"sceNpLookupPollAsync", reinterpret_cast<void*>(np_poll)},
    {"sceNpLookupNpId", reinterpret_cast<void*>(np_signed_out)},
    {"sceNpScoreCreateNpTitleCtx", reinterpret_cast<void*>(np_request_ctx)},
    {"sceNpScoreDeleteNpTitleCtx", reinterpret_cast<void*>(ok_void)},
    {"sceNpScoreCreateRequest", reinterpret_cast<void*>(np_request)},
    {"sceNpScoreDeleteRequest", reinterpret_cast<void*>(ok_void)},
    {"sceNpScoreAbortRequest", reinterpret_cast<void*>(ok_void)},
    {"sceNpWebApiInitialize", reinterpret_cast<void*>(lib_init_id)},
    {"sceNpWebApiTerminate", reinterpret_cast<void*>(ok_void)},
    {"sceNpWebApiCreateContext", reinterpret_cast<void*>(np_signed_out)},
    {"sceNpWebApiCreateRequest", reinterpret_cast<void*>(np_signed_out)},
    {"sceNpWebApiSendRequest", reinterpret_cast<void*>(np_signed_out)},
    {"sceNpWebApiDeleteRequest", reinterpret_cast<void*>(ok_void)},
    {"sceNpWebApiAbortRequest", reinterpret_cast<void*>(ok_void)},
    {"sceNpWebApiDeleteContext", reinterpret_cast<void*>(ok_void)},
    {"sceNpWebApiReadData", reinterpret_cast<void*>(np_signed_out)},
    {"sceNpWebApiGetHttpStatusCode", reinterpret_cast<void*>(np_signed_out)},
    {"sceNpWebApiGetHttpResponseHeaderValue", reinterpret_cast<void*>(np_signed_out)},
    {"sceNpWebApiGetHttpResponseHeaderValueLength", reinterpret_cast<void*>(np_signed_out)},
    {"sceNpWebApiCreatePushEventFilter", reinterpret_cast<void*>(np_signed_out)},
    {"sceNpWebApiDeletePushEventFilter", reinterpret_cast<void*>(ok_void)},
    {"sceNpWebApiRegisterPushEventCallback", reinterpret_cast<void*>(np_signed_out)},
    {"sceNpWebApiUnregisterPushEventCallback", reinterpret_cast<void*>(ok_void)},
    {"sceNpWebApiUtilityParseNpId", reinterpret_cast<void*>(np_signed_out)},
    {"sceNpMatching2ContextStart", reinterpret_cast<void*>(np_signed_out)},
    {"sceNpMatching2ContextStop", reinterpret_cast<void*>(ok_void)},
    {"sceNpMatching2DestroyContext", reinterpret_cast<void*>(ok_void)},
    {"sceNpMatching2RegisterContextCallback", reinterpret_cast<void*>(ok_void)},
    {"sceNpMatching2RegisterLobbyEventCallback", reinterpret_cast<void*>(ok_void)},
    {"sceNpMatching2RegisterRoomEventCallback", reinterpret_cast<void*>(ok_void)},
    {"sceNpMatching2RegisterSignalingCallback", reinterpret_cast<void*>(ok_void)},
    {"sceNpMatching2SetDefaultRequestOptParam", reinterpret_cast<void*>(ok_void)},
    {"sceNpMatching2CreateJoinRoom", reinterpret_cast<void*>(np_signed_out)},
    {"sceNpMatching2JoinRoom", reinterpret_cast<void*>(np_signed_out)},
    {"sceNpMatching2LeaveRoom", reinterpret_cast<void*>(np_signed_out)},
    {"sceNpMatching2SearchRoom", reinterpret_cast<void*>(np_signed_out)},
    {"sceNpMatching2GetServerId", reinterpret_cast<void*>(np_signed_out)},
    {"sceNpMatching2GetWorldInfoList", reinterpret_cast<void*>(np_signed_out)},
    {"sceNpMatching2GetLobbyInfoList", reinterpret_cast<void*>(np_signed_out)},
    {"sceNpMatching2JoinLobby", reinterpret_cast<void*>(np_signed_out)},
    {"sceNpMatching2LeaveLobby", reinterpret_cast<void*>(np_signed_out)},
    {"sceNpMatching2GrantRoomOwner", reinterpret_cast<void*>(np_signed_out)},
    {"sceNpMatching2KickoutRoomMember", reinterpret_cast<void*>(np_signed_out)},
    {"sceNpMatching2SetRoomDataExternal", reinterpret_cast<void*>(np_signed_out)},
    {"sceNpMatching2SetRoomDataInternal", reinterpret_cast<void*>(np_signed_out)},
    {"sceNpMatching2SetRoomMemberDataInternal", reinterpret_cast<void*>(np_signed_out)},
    {"sceNpMatching2SignalingGetConnectionStatus", reinterpret_cast<void*>(np_signed_out)},
    {"sceNpMatching2SignalingGetPingInfo", reinterpret_cast<void*>(np_signed_out)},
    {"sceNpSignalingDeleteContext", reinterpret_cast<void*>(ok_void)},
    {"sceNpSignalingActivateConnection", reinterpret_cast<void*>(np_signed_out)},
    {"sceNpSignalingDeactivateConnection", reinterpret_cast<void*>(ok_void)},
    {"sceNpSignalingGetConnectionStatus", reinterpret_cast<void*>(np_signed_out)},
    {"sceNpScoreCensorComment", reinterpret_cast<void*>(np_signed_out)},
    {"sceNpScoreSanitizeComment", reinterpret_cast<void*>(np_signed_out)},
    {"sceNpScoreGetBoardInfo", reinterpret_cast<void*>(np_signed_out)},
    {"sceNpScoreGetGameData", reinterpret_cast<void*>(np_signed_out)},
    {"sceNpScoreGetRankingByNpIdPcId", reinterpret_cast<void*>(np_signed_out)},
    {"sceNpScoreGetRankingByRange", reinterpret_cast<void*>(np_signed_out)},
    {"sceNpScoreRecordGameData", reinterpret_cast<void*>(np_signed_out)},
    {"sceNpScoreRecordScore", reinterpret_cast<void*>(np_signed_out)},
    {"sceNpScoreSetPlayerCharacterId", reinterpret_cast<void*>(ok_void)},
    {"sceVoiceCreatePort", reinterpret_cast<void*>(voice_port)},
    {"sceVoiceDeletePort", reinterpret_cast<void*>(ok_void)},
    {"sceVoiceConnectIPortToOPort", reinterpret_cast<void*>(ok_void)},
    {"sceVoiceDisconnectIPortFromOPort", reinterpret_cast<void*>(ok_void)},
    {"sceVoiceStart", reinterpret_cast<void*>(ok_void)},
    {"sceVoiceStop", reinterpret_cast<void*>(ok_void)},
    {"sceVoiceGetPortInfo", reinterpret_cast<void*>(voice_info)},
    {"sceVoiceReadFromOPort", reinterpret_cast<void*>(voice_read)},
    {"sceVoiceWriteToIPort", reinterpret_cast<void*>(voice_write)},
    {"sceAudioInInput", reinterpret_cast<void*>(audio_in_open)},
    {"sceAudioInClose", reinterpret_cast<void*>(audio_in_open)},
    {"sceNpMatching2Initialize", reinterpret_cast<void*>(ok_void)},
    {"sceNpMatching2Terminate", reinterpret_cast<void*>(ok_void)},
    {"sceNpMatching2CreateContext", reinterpret_cast<void*>(np_signed_out)},
    {"sceNpSignalingInitialize", reinterpret_cast<void*>(ok_void)},
    {"sceNpSignalingTerminate", reinterpret_cast<void*>(ok_void)},
    {"sceNpSignalingCreateContext", reinterpret_cast<void*>(np_signed_out)},
    {"sceCommonDialogInitialize", reinterpret_cast<void*>(common_init)},
    {"sceMsgDialogInitialize", reinterpret_cast<void*>(msg_init)},
    {"sceMsgDialogOpen", reinterpret_cast<void*>(msg_open)},
    {"sceMsgDialogUpdateStatus", reinterpret_cast<void*>(msg_status)},
    {"sceMsgDialogTerminate", reinterpret_cast<void*>(msg_term)},
    {"sceSaveDataDialogInitialize", reinterpret_cast<void*>(save_init)},
    {"sceSaveDataDialogOpen", reinterpret_cast<void*>(save_open)},
    {"sceSaveDataDialogUpdateStatus", reinterpret_cast<void*>(save_status)},
    {"sceSaveDataDialogTerminate", reinterpret_cast<void*>(save_term)},
    {"sceNpProfileDialogInitialize", reinterpret_cast<void*>(profile_init)},
    {"sceNpProfileDialogOpen", reinterpret_cast<void*>(profile_open)},
    {"sceNpProfileDialogUpdateStatus", reinterpret_cast<void*>(profile_status)},
    {"sceNpProfileDialogTerminate", reinterpret_cast<void*>(profile_term)},
    {"sceNpProfileDialogGetResult", reinterpret_cast<void*>(profile_result)},
    {"sceNpCommerceDialogInitialize", reinterpret_cast<void*>(commerce_init)},
    {"sceNpCommerceDialogOpen", reinterpret_cast<void*>(commerce_open)},
    {"sceNpCommerceDialogUpdateStatus", reinterpret_cast<void*>(commerce_status)},
    {"sceNpCommerceDialogTerminate", reinterpret_cast<void*>(commerce_term)},
    {"sceImeDialogInit", reinterpret_cast<void*>(ime_init)},
    {"sceImeDialogGetStatus", reinterpret_cast<void*>(ime_status)},
    {"sceImeDialogGetResult", reinterpret_cast<void*>(ime_result)},
    {"sceImeDialogTerm", reinterpret_cast<void*>(ime_term)},
    {"sceImeDialogAbort", reinterpret_cast<void*>(ime_term)},
    {"sceNpTrophyCreateContext", reinterpret_cast<void*>(trophy_context)},
    {"sceNpTrophyCreateHandle", reinterpret_cast<void*>(trophy_handle)},
    {"sceNpTrophyRegisterContext", reinterpret_cast<void*>(trophy_register)},
    {"sceNpTrophyUnlockTrophy", reinterpret_cast<void*>(trophy_unlock)},
    {"sceNpTrophyGetGameInfo", reinterpret_cast<void*>(trophy_game_info)},
    {"sceNpTrophyGetTrophyInfo", reinterpret_cast<void*>(trophy_info)},
    {"scePlayGoInitialize", reinterpret_cast<void*>(playgo_init)},
    {"scePlayGoOpen", reinterpret_cast<void*>(playgo_open)},
    {"scePlayGoGetChunkId", reinterpret_cast<void*>(playgo_chunk_ids)},
    {"scePlayGoGetLocus", reinterpret_cast<void*>(playgo_locus)},
    {"scePlayGoSetInstallSpeed", reinterpret_cast<void*>(playgo_speed)},
    {"sceMouseInit", reinterpret_cast<void*>(ok_void)},
    {"sceMouseOpen", reinterpret_cast<void*>(mouse_open)},
    {"sceMouseRead", reinterpret_cast<void*>(mouse_read)},
    {"sceMouseClose", reinterpret_cast<void*>(mouse_close)},
    {"sceAudioInOpen", reinterpret_cast<void*>(audio_in_open)},
    {"sceDiscMapIsRequestOnHDD", reinterpret_cast<void*>(discmap_on_hdd)},
    {"sceDiscMap_8A828CAEE7EDD5E9", reinterpret_cast<void*>(discmap_8a82)},
    {"sceVoiceInit", reinterpret_cast<void*>(ok_void)},
    {"sceVoiceEnd", reinterpret_cast<void*>(ok_void)},
};

void runtime_services_register(void) {
    runtime_register_exports(services_exports, sizeof(services_exports) / sizeof(services_exports[0]));
}

uintptr_t runtime_services_resolve(const char *name) {
    const uintptr_t online = runtime_net_resolve(name); // BB_ONLINE=1: the online module first
    return online ? online : runtime_registry_resolve(name, EXPORT_FUNCTION);
}

} // extern "C"
