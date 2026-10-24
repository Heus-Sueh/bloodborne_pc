#include "bbport_write_log.h"
#include "bbport_frame_rate.h"
#include "bbport_game_menu.h"
#include "bbport_gnm_hooks.h"
// bbport: glue between the C loader and the vendored shadPS4 video core.
#include "bbport_overlay.h"
#include "bbport_guest_hooks.h"
#include "bbport_settings.h"
#include "game_profile.h"
#include "bbport_copy.h"
#include <sys/resource.h>
#include "bbport_free_check.h"
#include "bbport_toggles.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include "common/polyfill_thread.h"
#include <cstring>
#include <mutex>
#include <string>
#include <chrono>
#include <thread>
#include <vector>
#include <SDL3/SDL.h>
#include "../bbgpu.h"
#include "platform/kernel/event_queue.h"
#include "platform/videoout/videoout.h"
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/rdtsc.h"
#include "platform/errors.h"
#include "core/libraries/libs.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "core/signals.h"
#include "sdl_window.h"
#include "render/rasterizer.h"

extern "C" {
// runtime_memory.c
int runtime_memory_is_mapped(uintptr_t address, uint64_t size);
int runtime_memory_check_mapped(uintptr_t address, uint64_t size);
int runtime_memory_write_backing(uintptr_t address, const void* data, uint64_t size);
void runtime_memory_read_backing(uintptr_t address, void* data, uint64_t size);
uint64_t runtime_memory_clamp(uintptr_t address, uint64_t size);
int runtime_memory_region(uintptr_t address, uintptr_t* start, uintptr_t* end, int* mapped);
const uint64_t* runtime_memory_generation(void);
void runtime_memory_gpu_protect(uintptr_t address, uint64_t size, int read, int write);
typedef void (*RuntimeGpuRange)(uintptr_t address, uint64_t size);
struct RuntimeMemoryCoherenceHooks {
    RuntimeGpuRange map;
    RuntimeGpuRange unmap;
    RuntimeGpuRange invalidate;
    RuntimeGpuRange note_write;
    RuntimeGpuRange cpu_write;
};
void runtime_memory_set_coherence_hooks(const RuntimeMemoryCoherenceHooks* hooks);
// runtime_kernel.c: one clock for guest and GPU timestamps
uint64_t runtime_process_time_us(void);
uint64_t runtime_process_time_counter(void);
uint64_t runtime_tsc_frequency(void);
int32_t* runtime_errno(void);
void runtime_memory_set_gpu_hooks(RuntimeGpuRange map, RuntimeGpuRange unmap, RuntimeGpuRange invalidate);
void runtime_memory_set_note_write_hook(RuntimeGpuRange note);
void runtime_memory_set_cpu_write_hook(RuntimeGpuRange hook);
}

Frontend::WindowSDL* g_window = nullptr;

namespace BbGnm { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace BbAvPlayer { void RegisterLib(Core::Loader::SymbolsResolver* sym); }

namespace {
struct Symbol {
    std::string nid, library, module;
    u64 address;
};
std::vector<Symbol> g_symbols;
u32 g_sdk_version;
} // namespace

namespace Core::Loader {
void SymbolsResolver::AddSymbol(const char* nid, const char* library, const char* module, SymbolType,
                                u64 address) {
    g_symbols.push_back({nid, library, module, address});
}
} // namespace Core::Loader

namespace Core {
void MemoryManager::SetRasterizer(Vulkan::Rasterizer* rasterizer_) {
    rasterizer = rasterizer_;
    // Existing guest mappings are replayed by the runtime when hooks are installed.
    const RuntimeMemoryCoherenceHooks hooks{
        .map = [](uintptr_t address, uint64_t size) {
            auto* r = Memory::Instance()->GetRasterizer();
            r->RegisterMemory(address, size);
            r->MapMemory(address, size);
        },
        .unmap = [](uintptr_t address, uint64_t size) { Memory::Instance()->GetRasterizer()->UnmapMemory(address, size); },
        .invalidate = [](uintptr_t address, uint64_t size) {
            Memory::Instance()->GetRasterizer()->InvalidateMemory(address, size);
        },
        .note_write = [](uintptr_t address, uint64_t size) {
            Memory::Instance()->GetRasterizer()->NoteAssetWrite(address, size);
        },
        .cpu_write = [](uintptr_t address, uint64_t size) {
            Memory::Instance()->GetRasterizer()->OnCpuWrite(address, size);
        },
    };
    runtime_memory_set_coherence_hooks(&hooks);
}
void MemoryManager::InvalidateMemory(VAddr address, u64 size) {
    if (rasterizer) rasterizer->InvalidateMemory(address, size);
}
namespace {
// bbport: fast cached region check encapsulated inside the runtime's memory module.
bool CachedMapped(VAddr address, u64 size) {
    return runtime_memory_check_mapped(address, size) != 0;
}
} // namespace

u64 MemoryManager::ClampRangeSize(VAddr virtual_addr, u64 size) {
    if (size && CachedMapped(virtual_addr, size)) {
        return size;
    }
    return runtime_memory_clamp(virtual_addr, size);
}
static void CopySparseSerial(VAddr source, u8* dest, u64 size) {
    // BB_READBACKS=2 protects GPU-written pages against reads: the copy threads and recorders
    // read through the backing view, as a read fault on them would wait for the GPU thread,
    // which waits for them.
    static const bool precise =
        EmulatorSettings.GetReadbacksMode() == GpuReadbacksMode::Precise;
    if (precise) {
        runtime_memory_read_backing(source, dest, size);
        return;
    }
    if (size && CachedMapped(source, size)) {
        std::memcpy(dest, reinterpret_cast<const void*>(source), size);
        return;
    }
    while (size) {
        uintptr_t start = 0, end = 0;
        int mapped = 0;
        if (!runtime_memory_region(source, &start, &end, &mapped)) {
            end = source + size;
        }
        const u64 n = std::min<u64>(size, end - source);
        if (mapped) std::memcpy(dest, reinterpret_cast<const void*>(source), n);
        else std::memset(dest, 0, n);
        source += n; dest += n; size -= n;
    }
}
void MemoryManager::CopySparseMemory(VAddr source, u8* dest, u64 size) {
    // bbport: large uploads (streaming) are split across the copy threads.
    constexpr u64 Chunk = 512 * 1024;
    if (size < 4 * Chunk || !BbCopy::Enabled()) {
        return CopySparseSerial(source, dest, size);
    }
    BbCopy::ParallelFor((size + Chunk - 1) / Chunk, [&](std::size_t i) {
        const u64 offset = i * Chunk;
        CopySparseSerial(source + offset, dest + offset, std::min(Chunk, size - offset));
    });
}
void MemoryManager::ReadBacking(VAddr address, void* data, u64 size) {
    runtime_memory_read_backing(address, data, size);
}
bool MemoryManager::TryWriteBacking(void* address, const void* data, u64 size) {
    BbWriteLog::Note(reinterpret_cast<uintptr_t>(address), data, size, BbWriteLog::Backing);
    return runtime_memory_write_backing(reinterpret_cast<uintptr_t>(address), data, size) != 0;
}
void AddressSpace::Protect(VAddr virtual_addr, u64 size, MemoryPermission perms) {
    BbStats::Timer timer{BbStats::t_protect};
    const u64 pages = (size + 4095) / 4096;
    BbStats::protect_calls.fetch_add(1, std::memory_order_relaxed);
    BbStats::protect_pages.fetch_add(pages, std::memory_order_relaxed);
    if (!True(perms & MemoryPermission::Write)) {
        BbStats::protect_revoke_calls.fetch_add(1, std::memory_order_relaxed);
        BbStats::protect_revoke_pages.fetch_add(pages, std::memory_order_relaxed);
    }
    runtime_memory_gpu_protect(virtual_addr, size, True(perms & MemoryPermission::Read),
                               True(perms & MemoryPermission::Write));
}
boost::icl::interval_set<VAddr> AddressSpace::GetUsableRegions() {
    boost::icl::interval_set<VAddr> set;
    set += boost::icl::interval<VAddr>::right_open(0x10'0000'0000ULL, 0xfc'0000'0000ULL);
    return set;
}
} // namespace Core

// Kernel services the vendored libraries call directly.
namespace Libraries::Kernel {
u64 PS4_SYSV_ABI sceKernelGetTscFrequency() { return runtime_tsc_frequency(); }
u64 PS4_SYSV_ABI sceKernelReadTsc() { return Common::FencedRDTSC(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTime() { return runtime_process_time_us(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounter() { return runtime_process_time_counter(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounterFrequency() { return 1'000'000'000; }
s32 PS4_SYSV_ABI sceKernelIsNeoMode() { return 0; } // base PS4
s32 PS4_SYSV_ABI sceKernelGetCompiledSdkVersion(s32* ver) {
    if (!ver) return BbErr::KernelInvalid;
    *ver = s32(g_sdk_version);
    return BbErr::Ok;
}
s32 PS4_SYSV_ABI sceKernelUsleep(u32 microseconds) {
    std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
    return BbErr::Ok;
}
} // namespace Libraries::Kernel

namespace {
// SDL video must be driven from one thread: the window lives on its own host thread.
std::thread g_window_thread;
std::mutex g_window_mutex;
std::condition_variable g_window_cv;
bool g_window_ready;
} // namespace

#ifdef BB_PGO_GENERATE
extern "C" void __gcov_dump(void);
extern "C" void __gcov_reset(void);
// Instrumented build (BB_PGO=generate): the game often ends through _exit (watchdog, guest
// exit), which skips the profile write at exit. Write every 30 s and reset: the files sum the
// intervals.
static void StartProfileWriter() {
    std::thread([] {
        Common::SetCurrentThreadName("bb:pgo");
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            __gcov_dump();
            __gcov_reset();
            std::printf("PGO: profile written\n");
        }
    }).detach();
}
#endif

extern "C" int bbgpu_init(const BbGpuConfig* config) {
    BbSettings::Load();
    // What the translator knows about this game (games/), before anything asks for it.
    Game::Select(config->serial);
#ifdef BB_PGO_GENERATE
    StartProfileWriter();
#endif
    g_sdk_version = config->sdk_version;
    if (config->user_dir) setenv("BB_GPU_USER_DIR", config->user_dir, 0);
    Common::ElfInfo::Instance().Set(config->serial ? config->serial : "UNKNOWN",
                                    config->title ? config->title : "", config->sdk_version,
                                    config->psf_attributes);
    const std::string title = config->title ? config->title : "Bloodborne";
    const s32 width = config->width, height = config->height;
    g_window_thread = std::thread([title, width, height] {
        Common::SetCurrentThreadName("bb:window");
        auto* window = new Frontend::WindowSDL(width, height, title.c_str());
        {
            std::scoped_lock lock{g_window_mutex};
            g_window = window;
            g_window_ready = true;
        }
        g_window_cv.notify_all();
        while (window->PollEvents()) {
            SDL_Delay(2);
        }
        LOG_INFO(Frontend, "Window closed by user");
        std::fflush(stdout);
        std::_Exit(0);
    });
    g_window_thread.detach();
    {
        std::unique_lock lock{g_window_mutex};
        g_window_cv.wait(lock, [] { return g_window_ready; });
    }
    Core::Loader::SymbolsResolver resolver;
    // GnmDriver creates the presenter that the VideoOut present thread uses.
    BbGnm::RegisterLib(&resolver);
    BbVideoOut::RegisterLib(&resolver);
    return 0;
}

extern "C" void bbgpu_register_kernel(void) {
    Core::Loader::SymbolsResolver resolver;
    BbKernel::RegisterEventQueue(&resolver);
    BbAvPlayer::RegisterLib(&resolver);
}

extern "C" uintptr_t bbgpu_resolve(const char* scoped_nid) {
    const char* hash = std::strchr(scoped_nid, '#');
    const size_t length = hash ? size_t(hash - scoped_nid) : std::strlen(scoped_nid);
    for (const auto& symbol : g_symbols) {
        if (symbol.nid.size() == length && !std::memcmp(symbol.nid.data(), scoped_nid, length)) {
            return uintptr_t(symbol.address);
        }
    }
    return 0;
}

extern "C" int bbgpu_handle_fault(void* ucontext, void* address) {
    // BB_LABEL_TRAP (diagnostic): its read-only label pages first. Not passed on to GPU page
    // tracking (a write fault drains the draw pipe: thousands a second would change the timing
    // under test); label pages are not expected to be GPU-tracked.
    if (BbFreeCheck::OnTrapFault(ucontext, reinterpret_cast<std::uint64_t>(address))) {
        return 1;
    }
    if (Core::Signals::Instance()->DispatchAccessViolation(ucontext, address)) {
        return 1;
    }
    return BbFreeCheck::OnStaleTrapFault(reinterpret_cast<std::uint64_t>(address)) ? 1 : 0;
}

extern "C" void bbhook_patch_image(unsigned char* image, uint64_t size) {
    BbGnmHooks::PatchImage(image, size);
    BbGameMenu::PatchImage(image, size);
    BbFrameRate::PatchImage(image, size);
}
extern "C" void bbgpu_patch_image(unsigned char* image, uint64_t size) {
    bbhook_patch_image(image, size);
}

extern "C" unsigned bbgpu_symbol_count(void) {
    return unsigned(g_symbols.size());
}

namespace Libraries::Kernel {
int* PS4_SYSV_ABI __Error() {
    return runtime_errno();
}
} // namespace Libraries::Kernel

extern "C" int bbhost_overlay_captures_input(void) {
    return BbOverlay::CapturesInput() ? 1 : 0;
}
extern "C" int bbgpu_overlay_captures_input(void) {
    return bbhost_overlay_captures_input();
}

extern "C" void bbhost_mouse_take(double* dx, double* dy, int* wheel_up, int* wheel_down) {
    double x = 0, y = 0;
    int up = 0, down = 0;
    if (g_window) {
        g_window->TakeMouse(x, y, up, down);
    }
    *dx = x;
    *dy = y;
    *wheel_up = up;
    *wheel_down = down;
}
extern "C" void bbgpu_mouse_take(double* dx, double* dy, int* wheel_up, int* wheel_down) {
    bbhost_mouse_take(dx, dy, wheel_up, wheel_down);
}

extern "C" int bbhost_mouse_captured(void) {
    return g_window && g_window->MouseCaptured() ? 1 : 0;
}
extern "C" int bbgpu_mouse_captured(void) {
    return bbhost_mouse_captured();
}

extern "C" void bbhost_mouse_capture(void) {
    if (g_window) {
        g_window->RequestMouseCapture();
    }
}
extern "C" void bbgpu_mouse_capture(void) {
    bbhost_mouse_capture();
}

extern "C" uint64_t bbhook_camera_state(void) {
    return BbGuestHooks::CameraState();
}
extern "C" uint64_t bbgpu_camera_state(void) {
    return bbhook_camera_state();
}

extern "C" int bbhook_free_camera(void) {
    return BbGuestHooks::FreeCameraActive() ? 1 : 0;
}
extern "C" int bbgpu_free_camera(void) {
    return bbhook_free_camera();
}

extern "C" int bbhook_camera_look(float yaw, float pitch) {
    if (!BbGuestHooks::CameraLookAvailable()) {
        return 0;
    }
    return BbGuestHooks::CameraLookAdd(pitch, yaw) ? 1 : 0;
}
extern "C" int bbgpu_camera_look(float yaw, float pitch) {
    return bbhook_camera_look(yaw, pitch);
}

extern "C" void bbhost_mouse_look_enable(int enabled) {
    Frontend::WindowSDL::EnableMouseLook(enabled != 0);
}
extern "C" void bbgpu_mouse_look_enable(int enabled) {
    bbhost_mouse_look_enable(enabled);
}

extern "C" int bbhost_text_input_begin(const char* initial, const char* prompt) {
    if (!g_window) return 0;
    g_window->BeginTextInput(initial ? initial : "", prompt ? prompt : "Text");
    return 1;
}
extern "C" int bbgpu_text_input_begin(const char* initial, const char* prompt) {
    return bbhost_text_input_begin(initial, prompt);
}

extern "C" int bbhost_text_input_poll(char* out, uint64_t size) {
    if (!g_window) return 2;
    std::string text;
    const int state = g_window->PollTextInput(text);
    if (size) {
        const size_t n = std::min<size_t>(text.size(), size - 1);
        std::memcpy(out, text.data(), n);
        out[n] = 0;
    }
    return state;
}
extern "C" int bbgpu_text_input_poll(char* out, uint64_t size) {
    return bbhost_text_input_poll(out, size);
}
