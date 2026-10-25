#include "runtime_savedata.hpp"
#include "runtime.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace bb::savedata {

static constexpr int32_t ERR_PARAMETER = static_cast<int32_t>(0x809F0000);
static constexpr int32_t ERR_NOT_INITIALIZED = static_cast<int32_t>(0x809F0001);
static constexpr int32_t ERR_NOT_MOUNTED = static_cast<int32_t>(0x809F0004);
static constexpr int32_t ERR_EXISTS = static_cast<int32_t>(0x809F0007);
static constexpr int32_t ERR_NOT_FOUND = static_cast<int32_t>(0x809F0008);
static constexpr int32_t ERR_INTERNAL = static_cast<int32_t>(0x809F000B);
static constexpr int32_t ERR_MOUNT_FULL = static_cast<int32_t>(0x809F000C);
static constexpr int32_t ERR_BAD_MOUNTED = static_cast<int32_t>(0x809F000D);
static constexpr int32_t ERR_INVALID_USER = static_cast<int32_t>(0x809F0011);
static constexpr int32_t ERR_MEMORY_NOT_READY = static_cast<int32_t>(0x809F0012);

static constexpr uint32_t MODE_RDONLY = 1;
static constexpr uint32_t MODE_CREATE = 4;
static constexpr uint32_t MODE_COPY_ICON = 16;
static constexpr uint32_t MODE_CREATE2 = 32;
static constexpr size_t SLOTS = 16;

struct TitleId { char data[10]; char pad[6]; };
struct DirName { char data[32]; };
struct MountPoint { char data[16]; };
struct Param {
    char title[128], subtitle[128], detail[1024];
    uint32_t user_param; int32_t pad;
    int64_t mtime;
    uint8_t reserved[32];
};
struct Mount1 { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; const void *fingerprint;
                uint64_t blocks; uint32_t mode; uint8_t reserved[32]; };
struct Mount2 { int32_t user; int32_t pad; const DirName *dir; uint64_t blocks; uint32_t mode;
                uint8_t reserved[32]; int32_t pad2; };
struct MountResult { MountPoint point; uint64_t required_blocks; uint32_t unused, status; uint8_t reserved[28]; int32_t pad; };
struct Delete { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; uint32_t unused;
                uint8_t reserved[32]; int32_t pad2; };
struct SearchCond { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; uint32_t key, order;
                    uint8_t reserved[32]; };
struct SearchInfo { uint64_t blocks, free_blocks; uint8_t reserved[32]; };
struct SearchResult { uint32_t hits; int32_t pad; DirName *names; uint32_t names_capacity, set_count;
                      Param *params; SearchInfo *infos; uint8_t reserved[12]; int32_t pad2; };
struct Icon { const void *buffer; uint64_t buffer_size, data_size; uint8_t reserved[32]; };

static_assert(sizeof(Param) == 1328, "OrbisSaveDataParam layout");
static_assert(sizeof(Mount1) == 80 && sizeof(Mount2) == 64, "OrbisSaveDataMount layouts");
static_assert(sizeof(MountResult) == 64, "OrbisSaveDataMountResult layout");
static_assert(sizeof(SearchCond) == 64 && sizeof(SearchResult) == 56, "dir name search layouts");

struct SaveSlot {
    bool used{false};
    fs::path host;
    fs::path meta;
};

class SaveDataState {
public:
    static SaveDataState& Instance() {
        static SaveDataState state;
        return state;
    }

    std::mutex mutex;
    bool initialized{false};
    std::string title_id{"UNKNOWN"};
    std::array<SaveSlot, SLOTS> slots;
    fs::path memory_path;
    uint64_t memory_size{0};
    size_t mounts_done{0};
    size_t memory_writes{0};

    void Reset() {
        std::scoped_lock lock(mutex);
        initialized = false;
        title_id = "UNKNOWN";
        for (auto& s : slots) {
            s.used = false;
            s.host.clear();
            s.meta.clear();
        }
        memory_path.clear();
        memory_size = 0;
        mounts_done = 0;
        memory_writes = 0;
    }
};

SaveDataManager& SaveDataManager::Instance() {
    static SaveDataManager manager;
    return manager;
}

void SaveDataManager::Reset() {
    SaveDataState::Instance().Reset();
}

void SaveDataManager::Configure(std::string_view title) {
    if (!title.empty()) {
        std::scoped_lock lock(SaveDataState::Instance().mutex);
        SaveDataState::Instance().title_id = title;
    }
    SoundHack();
}

void SaveDataManager::Report() const {
    auto& state = SaveDataState::Instance();
    std::scoped_lock lock(state.mutex);
    std::printf("Runtime: save data mounts=%zu, memory writes=%zu\n", state.mounts_done, state.memory_writes);
}

void SaveDataManager::SoundHack() {
    static const std::string_view ids[] = {
        "CUSA00207", "CUSA00208", "CUSA00299", "CUSA00900", "CUSA01363",
        "CUSA03014", "CUSA03023", "CUSA03173", "CUSA03179"
    };

    const char* env = std::getenv("BB_SOUND_HACK");
    if (env && env[0] == '0') return;

    std::string cur_title;
    {
        std::scoped_lock lock(SaveDataState::Instance().mutex);
        cur_title = SaveDataState::Instance().title_id;
    }

    bool is_bloodborne = false;
    for (const auto& id : ids) {
        if (cur_title == id) {
            is_bloodborne = true;
            break;
        }
    }
    if (!is_bloodborne) return;

    fs::path users_dir = fs::path(runtime_file_user_dir()) / "savedata";
    std::error_code ec;
    if (!fs::is_directory(users_dir, ec)) return;

    for (const auto& entry : fs::directory_iterator(users_dir, ec)) {
        if (!entry.is_directory(ec)) continue;
        fs::path save_file = entry.path() / cur_title / "SPRJ0005" / "userdata0010";
        if (!fs::exists(save_file, ec)) continue;

        FILE* f = std::fopen(save_file.string().c_str(), "r+b");
        if (!f) continue;

        if (std::fseek(f, 0x204E, SEEK_SET) == 0) {
            int old = std::fgetc(f);
            if (old != EOF && old != 1) {
                if (std::fseek(f, 0x204E, SEEK_SET) == 0) {
                    std::fputc(1, f);
                    std::printf("Runtime: Bloodborne sound flag set in %s (was %d)\n", save_file.string().c_str(), old);
                }
            }
        }
        std::fclose(f);
    }
}

namespace {

static fs::path UserRoot(int32_t user, const char* title) {
    auto& state = SaveDataState::Instance();
    std::string tid = (title && *title) ? title : state.title_id;
    return fs::path(runtime_file_user_dir()) / "savedata" / std::to_string(user) / tid;
}

static bool IsValidName(std::string_view name) {
    if (name.empty() || name.size() >= 32) return false;
    for (size_t i = 0; i < name.size(); ++i) {
        if (name[i] == '/' || name[i] == '\\') return false;
        if (name[i] == '.' && (i == 0 || name[i - 1] == '.')) return false;
    }
    return true;
}

static bool WriteAtomic(const fs::path& path, const void* data, size_t size) {
    fs::path temp = path;
    temp += ".bbtmp";

    FILE* f = std::fopen(temp.string().c_str(), "wb");
    if (!f) return false;

    bool ok = (std::fwrite(data, 1, size, f) == size) && (std::fflush(f) == 0) && (fsync(fileno(f)) == 0);
    ok = (std::fclose(f) == 0) && ok;

    if (!ok) {
        std::error_code ec;
        fs::remove(temp, ec);
        return false;
    }

    std::error_code ec;
    fs::rename(temp, path, ec);
    if (ec) {
        fs::remove(temp, ec);
        return false;
    }
    return true;
}

static void WriteParam(const fs::path& meta, const Param* p) {
    fs::path path = meta / "param.bin";
    Param copy = *p;
    copy.mtime = std::time(nullptr);
    WriteAtomic(path, &copy, sizeof(copy));
}

static bool ReadParam(const fs::path& meta, Param* p) {
    fs::path path = meta / "param.bin";
    std::memset(p, 0, sizeof(*p));

    FILE* f = std::fopen(path.string().c_str(), "rb");
    if (!f) return false;

    size_t n = std::fread(p, sizeof(*p), 1, f);
    std::fclose(f);

    std::error_code ec;
    auto s = fs::last_write_time(path, ec);
    if (!ec) {
        p->mtime = std::chrono::duration_cast<std::chrono::seconds>(s.time_since_epoch()).count();
    }
    return (n == 1);
}

static int32_t DoMount(int32_t user, const char* title, const DirName* dir, uint32_t mode, MountResult* result) {
    auto& state = SaveDataState::Instance();
    if (!state.initialized) return ERR_NOT_INITIALIZED;
    if (user < 0) return ERR_INVALID_USER;
    if (!dir || !result || !IsValidName(dir->data)) return ERR_PARAMETER;

    fs::path base = UserRoot(user, title);
    fs::path host = base / dir->data;
    fs::path meta = base / (std::string(dir->data) + ".sce_sys");

    std::error_code ec;
    bool exists = fs::is_directory(host, ec);

    if ((mode & MODE_CREATE) && exists) return ERR_EXISTS;
    if (!(mode & (MODE_CREATE | MODE_CREATE2)) && !exists) return ERR_NOT_FOUND;

    std::scoped_lock lock(state.mutex);

    int slot = -1;
    for (size_t i = 0; i < SLOTS; ++i) {
        if (state.slots[i].used && state.slots[i].host == host) {
            return ERR_BAD_MOUNTED;
        }
        if (!state.slots[i].used && slot < 0) {
            slot = static_cast<int>(i);
        }
    }
    if (slot < 0) return ERR_MOUNT_FULL;

    if (!exists) {
        if (!fs::create_directories(host, ec) || !fs::create_directories(meta, ec)) {
            return ERR_INTERNAL;
        }
        Param empty;
        WriteParam(meta, &empty);
    }

    state.slots[slot].used = true;
    state.slots[slot].host = host;
    state.slots[slot].meta = meta;

    std::memset(result, 0, sizeof(*result));
    std::string point = "/savedata" + std::to_string(slot & 15);
    std::memcpy(result->point.data, point.c_str(), point.size() + 1);
    result->status = exists ? 0 : 1; // 1 = CREATED

    if (runtime_file_mount(result->point.data, host.string().c_str()) != 0) {
        std::fprintf(stderr, "Runtime: save data '%s' could not be mounted: mount table full\n", dir->data);
    }
    state.mounts_done++;

    std::printf("Runtime: save data '%s' mounted at %s (%s%s)\n",
                dir->data, result->point.data,
                exists ? "existing" : "created",
                (mode & MODE_RDONLY) ? ", read-only" : "");
    return 0;
}

static int SlotOf(const MountPoint* point) {
    if (!point || std::strncmp(point->data, "/savedata", 9) != 0) return -1;
    int slot = std::atoi(point->data + 9);
    auto& state = SaveDataState::Instance();
    return (slot >= 0 && slot < static_cast<int>(SLOTS) && state.slots[slot].used) ? slot : -1;
}

static bool Like(std::string_view s, std::string_view p) {
    if (p.empty()) return s.empty();
    if (p[0] == '%') {
        for (size_t i = 0; i <= s.size(); ++i) {
            if (Like(s.substr(i), p.substr(1))) return true;
        }
        return false;
    }
    return !s.empty() && (p[0] == '_' || p[0] == s[0]) && Like(s.substr(1), p.substr(1));
}

} // namespace

} // namespace bb::savedata

/* --------------------------------------------------------------------------
 * Guest ABI Callbacks (sysv_abi)
 * -------------------------------------------------------------------------- */

using namespace bb::savedata;

extern "C" {

static ABI int32_t save_initialize(const void* param) {
    (void)param;
    SaveDataState::Instance().initialized = true;
    return 0;
}

static ABI int32_t save_terminate(void) {
    auto& state = SaveDataState::Instance();
    if (!state.initialized) return ERR_NOT_INITIALIZED;
    state.initialized = false;
    return 0;
}

static ABI int32_t save_mount(const Mount1* m, MountResult* result) {
    if (!m) return ERR_PARAMETER;
    return DoMount(m->user, m->title ? m->title->data : nullptr, m->dir, m->mode, result);
}

static ABI int32_t save_mount2(const Mount2* m, MountResult* result) {
    if (!m) return ERR_PARAMETER;
    return DoMount(m->user, nullptr, m->dir, m->mode, result);
}

static ABI int32_t save_umount(const MountPoint* point) {
    auto& state = SaveDataState::Instance();
    if (!state.initialized) return ERR_NOT_INITIALIZED;

    std::scoped_lock lock(state.mutex);
    int slot = SlotOf(point);
    if (slot >= 0) {
        runtime_file_unmount(point->data);
        state.slots[slot].used = false;
        state.slots[slot].host.clear();
        state.slots[slot].meta.clear();
        return 0;
    }
    return ERR_NOT_FOUND;
}

static ABI int32_t save_set_param(const MountPoint* point, uint32_t type, const void* buffer, uint64_t size) {
    auto& state = SaveDataState::Instance();
    if (!state.initialized) return ERR_NOT_INITIALIZED;
    if (!buffer) return ERR_PARAMETER;

    std::scoped_lock lock(state.mutex);
    int slot = SlotOf(point);
    if (slot < 0) return ERR_NOT_MOUNTED;

    Param p;
    ReadParam(state.slots[slot].meta, &p);

    switch (type) {
    case 0: // ALL
        if (size < sizeof(Param)) return ERR_PARAMETER;
        std::memcpy(&p, buffer, sizeof(p));
        break;
    case 1:
        std::snprintf(p.title, sizeof(p.title), "%.*s", static_cast<int>(size), static_cast<const char*>(buffer));
        break;
    case 2:
        std::snprintf(p.subtitle, sizeof(p.subtitle), "%.*s", static_cast<int>(size), static_cast<const char*>(buffer));
        break;
    case 3:
        std::snprintf(p.detail, sizeof(p.detail), "%.*s", static_cast<int>(size), static_cast<const char*>(buffer));
        break;
    case 4:
        if (size < 4) return ERR_PARAMETER;
        std::memcpy(&p.user_param, buffer, 4);
        break;
    default:
        return ERR_PARAMETER;
    }

    WriteParam(state.slots[slot].meta, &p);
    return 0;
}

static ABI int32_t save_icon(const MountPoint* point, const Icon* icon) {
    auto& state = SaveDataState::Instance();
    if (!state.initialized) return ERR_NOT_INITIALIZED;
    if (!icon || !icon->buffer) return ERR_PARAMETER;

    std::scoped_lock lock(state.mutex);
    int slot = SlotOf(point);
    if (slot < 0) return ERR_NOT_MOUNTED;

    fs::path icon_path = state.slots[slot].meta / "icon0.png";
    return WriteAtomic(icon_path, icon->buffer, icon->data_size) ? 0 : ERR_INTERNAL;
}

static ABI int32_t save_delete(const Delete* d) {
    auto& state = SaveDataState::Instance();
    if (!state.initialized) return ERR_NOT_INITIALIZED;
    if (!d || !d->dir || !IsValidName(d->dir->data)) return ERR_PARAMETER;

    fs::path base = UserRoot(d->user, d->title ? d->title->data : nullptr);
    fs::path host = base / d->dir->data;
    fs::path meta = base / (std::string(d->dir->data) + ".sce_sys");

    std::error_code ec;
    if (!fs::exists(host, ec)) return ERR_NOT_FOUND;

    fs::remove_all(host, ec);
    fs::remove_all(meta, ec);

    std::printf("Runtime: save data '%s' deleted\n", d->dir->data);
    return 0;
}

struct SearchEntry {
    std::string name;
    Param param;
};

static ABI int32_t save_search(const SearchCond* cond, SearchResult* result) {
    auto& state = SaveDataState::Instance();
    if (!state.initialized) return ERR_NOT_INITIALIZED;
    if (!cond || !result) return ERR_PARAMETER;

    fs::path base = UserRoot(cond->user, cond->title ? cond->title->data : nullptr);
    std::vector<SearchEntry> entries;

    std::error_code ec;
    if (fs::is_directory(base, ec)) {
        for (const auto& entry : fs::directory_iterator(base, ec)) {
            std::string name = entry.path().filename().string();
            if (name.starts_with(".") || name.size() >= 32 || name.ends_with(".sce_sys")) continue;
            if (cond->dir && cond->dir->data[0] && !Like(name, cond->dir->data)) continue;

            Param p;
            fs::path meta = base / (name + ".sce_sys");
            ReadParam(meta, &p);
            entries.push_back({std::move(name), p});
        }
    }

    // Thread-safe sort with local lambda (no static global sort keys!)
    std::sort(entries.begin(), entries.end(), [key = cond->key, order = cond->order](const SearchEntry& a, const SearchEntry& b) {
        int c = 0;
        if (key == 1) {
            c = (a.param.user_param > b.param.user_param) - (a.param.user_param < b.param.user_param);
        } else if (key == 3) {
            c = (a.param.mtime > b.param.mtime) - (a.param.mtime < b.param.mtime);
        } else {
            c = a.name.compare(b.name);
        }
        return order ? (c > 0) : (c < 0);
    });

    result->hits = static_cast<uint32_t>(entries.size());
    uint32_t set = std::min(static_cast<uint32_t>(entries.size()), result->names_capacity);

    for (uint32_t i = 0; i < set; ++i) {
        if (result->names) {
            std::memset(result->names[i].data, 0, sizeof(result->names[i].data));
            std::memcpy(result->names[i].data, entries[i].name.c_str(), entries[i].name.size());
        }
        if (result->params) result->params[i] = entries[i].param;
        if (result->infos) {
            std::memset(&result->infos[i], 0, sizeof(SearchInfo));
            result->infos[i].blocks = 32768;
            result->infos[i].free_blocks = 16384;
        }
    }
    result->set_count = set;
    return 0;
}

static ABI int32_t memory_setup(int32_t user, uint64_t size, const Param* param) {
    auto& state = SaveDataState::Instance();
    if (!state.initialized) return ERR_NOT_INITIALIZED;
    if (!size) return ERR_PARAMETER;

    fs::path base = UserRoot(user, nullptr);
    fs::path dir = base.string() + ".memory";

    std::error_code ec;
    if (!fs::create_directories(dir, ec) && ec) return ERR_INTERNAL;

    std::scoped_lock lock(state.mutex);
    state.memory_path = dir / "memory.dat";

    FILE* f = std::fopen(state.memory_path.string().c_str(), "r+b");
    if (!f) f = std::fopen(state.memory_path.string().c_str(), "w+b");
    if (!f) return ERR_INTERNAL;

    std::fseek(f, 0, SEEK_END);
    if (static_cast<uint64_t>(std::ftell(f)) < size && ftruncate(fileno(f), static_cast<off_t>(size)) != 0) {
        std::fclose(f);
        return ERR_INTERNAL;
    }
    state.memory_size = size;
    std::fclose(f);

    if (param) WriteParam(dir, param);
    std::printf("Runtime: save data memory ready (%llu bytes)\n", static_cast<unsigned long long>(size));
    return 0;
}

static int32_t MemoryIo(void* buffer, uint64_t size, int64_t offset, bool is_write) {
    auto& state = SaveDataState::Instance();
    if (!state.initialized) return ERR_NOT_INITIALIZED;
    if (!buffer || offset < 0) return ERR_PARAMETER;

    std::scoped_lock lock(state.mutex);
    if (!state.memory_size) return ERR_MEMORY_NOT_READY;
    if (static_cast<uint64_t>(offset) + size > state.memory_size) return ERR_PARAMETER;

    FILE* f = std::fopen(state.memory_path.string().c_str(), "r+b");
    if (!f) return ERR_INTERNAL;

    if (std::fseek(f, offset, SEEK_SET) != 0) {
        std::fclose(f);
        return ERR_INTERNAL;
    }

    size_t rw = is_write ? std::fwrite(buffer, 1, size, f) : std::fread(buffer, 1, size, f);
    std::fclose(f);

    if (rw != size) return ERR_INTERNAL;
    if (is_write) state.memory_writes++;
    return 0;
}

static ABI int32_t memory_get(int32_t user, void* buffer, uint64_t size, int64_t offset) {
    (void)user;
    return MemoryIo(buffer, size, offset, false);
}

static ABI int32_t memory_set(int32_t user, void* buffer, uint64_t size, int64_t offset) {
    (void)user;
    return MemoryIo(buffer, size, offset, true);
}

static const RuntimeExport exports[] = {
    {"sceSaveDataInitialize", reinterpret_cast<void*>(save_initialize)},
    {"sceSaveDataInitialize2", reinterpret_cast<void*>(save_initialize)},
    {"sceSaveDataInitialize3", reinterpret_cast<void*>(save_initialize)},
    {"sceSaveDataTerminate", reinterpret_cast<void*>(save_terminate)},
    {"sceSaveDataMount", reinterpret_cast<void*>(save_mount)},
    {"sceSaveDataMount2", reinterpret_cast<void*>(save_mount2)},
    {"sceSaveDataUmount", reinterpret_cast<void*>(save_umount)},
    {"sceSaveDataSetParam", reinterpret_cast<void*>(save_set_param)},
    {"sceSaveDataSaveIcon", reinterpret_cast<void*>(save_icon)},
    {"sceSaveDataDelete", reinterpret_cast<void*>(save_delete)},
    {"sceSaveDataDirNameSearch", reinterpret_cast<void*>(save_search)},
    {"sceSaveDataSetupSaveDataMemory", reinterpret_cast<void*>(memory_setup)},
    {"sceSaveDataGetSaveDataMemory", reinterpret_cast<void*>(memory_get)},
    {"sceSaveDataSetSaveDataMemory", reinterpret_cast<void*>(memory_set)},
};

void runtime_savedata_configure(const char* title) {
    SaveDataManager::Instance().Configure(title ? title : "");
}

void runtime_savedata_register(void) {
    runtime_register_exports(exports, sizeof(exports) / sizeof(exports[0]));
}

uintptr_t runtime_savedata_resolve(const char* name) {
    return runtime_registry_resolve(name, EXPORT_FUNCTION);
}

void runtime_savedata_report(void) {
    SaveDataManager::Instance().Report();
}

} // extern "C"
