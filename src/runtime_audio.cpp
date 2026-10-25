#include "runtime_audio.hpp"
#include "runtime.h"

#include <SDL3/SDL.h>
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

constexpr int PORTS = 25;
constexpr int32_t ERR_NOT_OPENED       = static_cast<int32_t>(0x80260001);
constexpr int32_t ERR_INVALID_PORT     = static_cast<int32_t>(0x80260003);
constexpr int32_t ERR_INVALID_POINTER  = static_cast<int32_t>(0x80260004);
constexpr int32_t ERR_PORT_FULL        = static_cast<int32_t>(0x80260005);
constexpr int32_t ERR_INVALID_SIZE     = static_cast<int32_t>(0x80260006);
constexpr int32_t ERR_INVALID_FORMAT   = static_cast<int32_t>(0x80260007);
constexpr int32_t ERR_INVALID_FREQ     = static_cast<int32_t>(0x80260008);
constexpr int32_t ERR_INVALID_VOLUME   = static_cast<int32_t>(0x80260009);
constexpr int32_t ERR_INVALID_TYPE     = static_cast<int32_t>(0x8026000A);
constexpr int32_t ERR_ALREADY_INIT     = static_cast<int32_t>(0x8026000E);
constexpr int32_t ERR_NOT_INIT         = static_cast<int32_t>(0x8026000F);
constexpr int32_t VOLUME_0DB           = 32768;

struct Port {
    bool used{false};
    int type{0};
    int channels{0};
    bool is_float{false};
    int frames{0};
    int sample_bytes{0};
    bool std_layout{false};
    std::array<int32_t, 8> volume{};
    SDL_AudioStream* stream{nullptr};
    uint64_t next_deadline_ns{0};
    int64_t adjust_ns{0};
    int window_min{0};
    int window_count{0};
    uint64_t last_output_us{0};
    std::mutex lock;
    FILE* dump{nullptr};
    bool stats{false};
    uint64_t stat_start_ns{0};
    uint64_t stat_last_ns{0};
    uint64_t stat_max_gap_ns{0};
    unsigned stat_starved{0};
    unsigned stat_buffers{0};
    int stat_min_queued{0};

    void Reset() {
        if (stream) {
            SDL_DestroyAudioStream(stream);
            stream = nullptr;
        }
        if (dump) {
            std::fclose(dump);
            dump = nullptr;
        }
        used = false;
        type = 0;
        channels = 0;
        is_float = false;
        frames = 0;
        sample_bytes = 0;
        std_layout = false;
        volume.fill(VOLUME_0DB);
        next_deadline_ns = 0;
        adjust_ns = 0;
        window_min = 0;
        window_count = 0;
        last_output_us = 0;
        stats = false;
        stat_start_ns = 0;
        stat_last_ns = 0;
        stat_max_gap_ns = 0;
        stat_starved = 0;
        stat_buffers = 0;
        stat_min_queued = 0;
    }
};

struct PortState {
    uint16_t output;
    uint8_t channel;
    uint8_t reserved;
    int16_t volume;
    uint16_t reroute;
    uint64_t flag;
    uint64_t reserved64[2];
};
static_assert(sizeof(PortState) == 32, "AudioOut port state layout");

struct OutputParam {
    int32_t handle;
    const void* data;
};

class AudioManager {
public:
    static AudioManager& Instance() {
        static AudioManager instance;
        return instance;
    }

    int32_t Init() {
        std::lock_guard<std::mutex> lock(table_lock_);
        if (initialized_) {
            return ERR_ALREADY_INIT;
        }
        initialized_ = true;
        return 0;
    }

    int32_t Open(int32_t /*user*/, int32_t type, int32_t /*index*/,
                 uint32_t length, uint32_t freq, uint32_t param) {
        if (!initialized_) return ERR_NOT_INIT;
        if (!length || length > 2048 || (length & 0xff)) return ERR_INVALID_SIZE;
        if (freq != 48000) return ERR_INVALID_FREQ;

        const uint32_t format = param & 0xff;
        if (format > 7) return ERR_INVALID_FORMAT;

        int first = 0;
        int last = 0;
        if (!GetPortRange(type, first, last)) return ERR_INVALID_TYPE;

        static constexpr std::array<int, 8> channels_table = {1, 2, 8, 1, 2, 8, 8, 8};

        std::lock_guard<std::mutex> lock(table_lock_);
        int id = -1;
        for (int i = first; i <= last; ++i) {
            if (!ports_[i].used) {
                id = i;
                break;
            }
        }
        if (id < 0) return ERR_PORT_FULL;

        Port& p = ports_[id];
        {
            std::lock_guard<std::mutex> port_lock(p.lock);
            p.Reset();
            p.used = true;
            p.type = type;
            p.channels = channels_table[format];
            p.is_float = (format >= 3 && format != 6);
            p.sample_bytes = p.is_float ? 4 : 2;
            p.frames = static_cast<int>(length);
            p.std_layout = (format >= 6);
            p.volume.fill(VOLUME_0DB);

            if (IsSdlAudioReady()) {
                SDL_AudioSpec spec{
                    .format = p.is_float ? SDL_AUDIO_F32 : SDL_AUDIO_S16,
                    .channels = p.channels,
                    .freq = 48000
                };
                p.stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
                if (p.stream) {
                    SDL_ResumeAudioStreamDevice(p.stream);
                } else {
                    std::fprintf(stderr, "Runtime: SDL audio stream failed (%s); port %d uses the timer sink\n",
                                 SDL_GetError(), id);
                }
            }

            p.stats = (std::getenv("BB_AUDIO_STATS") != nullptr);
            if (const char* dump = std::getenv("BB_AUDIO_DUMP"); dump && dump[0]) {
                char path[4096];
                std::snprintf(path, sizeof(path), "%s.port%d.%dch.%s",
                              dump, id, p.channels, p.is_float ? "f32" : "s16");
                p.dump = std::fopen(path, "wb");
            }
        }

        ports_opened_.fetch_add(1, std::memory_order_relaxed);
        std::printf("Runtime: audio port %d opened (type %d, %d ch, %s, %u frames)\n",
                    id, type, p.channels, p.is_float ? "float" : "s16", length);

        return (type << 16) | id | 0x20000000;
    }

    int32_t Close(int32_t handle) {
        std::lock_guard<std::mutex> lock(table_lock_);
        int32_t error = 0;
        Port* p = FindPort(handle, error);
        if (!p) return error;

        std::lock_guard<std::mutex> port_lock(p->lock);
        p->Reset();
        return 0;
    }

    int32_t Output(int32_t handle, const void* data) {
        return OutputPort(handle, data, true);
    }

    int32_t Outputs(const OutputParam* params, uint32_t count) {
        if (!params) return ERR_INVALID_POINTER;
        if (!count || count > PORTS) return ERR_PORT_FULL;

        int32_t result = 0;
        for (uint32_t i = 0; i < count; ++i) {
            int32_t r = OutputPort(params[i].handle, params[i].data, (i == 0));
            if (r < 0) return r;
            result = r;
        }
        return result;
    }

    int32_t SetVolume(int32_t handle, int32_t flags, const int32_t* volume) {
        if (!volume) return ERR_INVALID_POINTER;

        int32_t error = 0;
        Port* p = nullptr;
        {
            std::lock_guard<std::mutex> lock(table_lock_);
            p = FindPort(handle, error);
        }
        if (!p) return error;

        std::lock_guard<std::mutex> port_lock(p->lock);
        for (int c = 0; c < 8; ++c) {
            if (flags & (1 << c)) {
                if (volume[c] < 0 || volume[c] > VOLUME_0DB) {
                    return ERR_INVALID_VOLUME;
                }
                p->volume[c] = volume[c];
            }
        }
        return 0;
    }

    int32_t GetPortState(int32_t handle, PortState* state) {
        if (!state) return ERR_INVALID_POINTER;

        int32_t error = 0;
        Port* p = nullptr;
        {
            std::lock_guard<std::mutex> lock(table_lock_);
            p = FindPort(handle, error);
        }
        if (!p) return error;

        std::lock_guard<std::mutex> port_lock(p->lock);
        std::memset(state, 0, sizeof(*state));
        switch (p->type) {
            case 2:
            case 3:
                state->output = 0x40;
                state->channel = 1;
                break;
            case 4:
                state->output = 0x04;
                state->channel = 1;
                state->volume = 127;
                break;
            default:
                state->output = 0x01;
                state->channel = static_cast<uint8_t>(p->channels > 2 ? 2 : p->channels);
                break;
        }
        return 0;
    }

    int32_t GetLastOutputTime(int32_t handle, uint64_t* time) {
        if (!time) return ERR_INVALID_POINTER;

        int32_t error = 0;
        Port* p = nullptr;
        {
            std::lock_guard<std::mutex> lock(table_lock_);
            p = FindPort(handle, error);
        }
        if (!p) return error;

        std::lock_guard<std::mutex> port_lock(p->lock);
        *time = p->last_output_us;
        return 0;
    }

    void Report() const {
        std::printf("Runtime: audio ports opened=%zu, buffers output=%zu\n",
                    ports_opened_.load(std::memory_order_relaxed),
                    buffers_out_.load(std::memory_order_relaxed));
    }

private:
    static uint64_t NowNs() {
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        return static_cast<uint64_t>(t.tv_sec) * 1000000000ULL + static_cast<uint64_t>(t.tv_nsec);
    }

    static void SleepUntil(uint64_t deadline) {
        struct timespec t = {
            .tv_sec = static_cast<time_t>(deadline / 1000000000ULL),
            .tv_nsec = static_cast<long>(deadline % 1000000000ULL)
        };
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, nullptr)) {}
    }

    bool IsSdlAudioReady() {
        if (sdl_ready_ < 0) {
            const char* mode = std::getenv("BB_AUDIO");
            sdl_ready_ = (!mode || std::strcmp(mode, "none") != 0) && SDL_InitSubSystem(SDL_INIT_AUDIO);
            std::printf("Runtime: audio backend %s\n", sdl_ready_ ? SDL_GetCurrentAudioDriver() : "timer (silent)");
        }
        return sdl_ready_ > 0;
    }

    static bool GetPortRange(int type, int& first, int& last) {
        switch (type) {
            case 0: first = 0; last = 7; return true;     // main
            case 1: first = 8; last = 8; return true;     // bgm
            case 2: first = 9; last = 12; return true;    // voice
            case 3: first = 13; last = 16; return true;   // personal
            case 4: first = 17; last = 20; return true;   // pad speaker
            case 126: first = 21; last = 22; return true; // audio3d
            case 127: first = 23; last = 24; return true; // aux
            default: return false;
        }
    }

    Port* FindPort(int32_t handle, int32_t& error) {
        int id = handle & 0xff;
        if ((handle & 0x3f000000) != 0x20000000 || id >= PORTS) {
            error = ERR_INVALID_PORT;
            return nullptr;
        }
        if (!ports_[id].used) {
            error = ERR_NOT_OPENED;
            return nullptr;
        }
        return &ports_[id];
    }

    int32_t OutputPort(int32_t handle, const void* data, bool pace) {
        int32_t error = 0;
        Port* p = nullptr;
        {
            std::lock_guard<std::mutex> lock(table_lock_);
            p = FindPort(handle, error);
        }
        if (!p) return error;

        std::lock_guard<std::mutex> port_lock(p->lock);
        const size_t samples = static_cast<size_t>(p->frames) * static_cast<size_t>(p->channels);
        const size_t bytes = samples * static_cast<size_t>(p->sample_bytes);
        const uint64_t period = static_cast<uint64_t>(p->frames) * 1000000000ULL / 48000ULL;

        if (data) {
            alignas(16) unsigned char converted[2048 * 8 * 4];
            static constexpr std::array<int, 8> remap = {0, 1, 2, 3, 6, 7, 4, 5};

            for (size_t f = 0; f < static_cast<size_t>(p->frames); ++f) {
                for (int c = 0; c < p->channels; ++c) {
                    int target = (p->channels == 8 && !p->std_layout) ? remap[c] : c;
                    size_t from = f * static_cast<size_t>(p->channels) + static_cast<size_t>(c);
                    size_t to = f * static_cast<size_t>(p->channels) + static_cast<size_t>(target);
                    float gain = static_cast<float>(p->volume[c]) / static_cast<float>(VOLUME_0DB);

                    if (p->is_float) {
                        float v = 0.0f;
                        std::memcpy(&v, static_cast<const char*>(data) + from * 4, 4);
                        v *= gain;
                        std::memcpy(converted + to * 4, &v, 4);
                    } else {
                        int16_t v = 0;
                        std::memcpy(&v, static_cast<const char*>(data) + from * 2, 2);
                        v = static_cast<int16_t>(static_cast<float>(v) * gain);
                        std::memcpy(converted + to * 2, &v, 2);
                    }
                }
            }

            if (p->dump) {
                std::fwrite(converted, 1, bytes, p->dump);
            }

            if (p->stats) {
                uint64_t now = NowNs();
                if (!p->stat_start_ns) {
                    p->stat_start_ns = now;
                    p->stat_min_queued = 1 << 30;
                } else if (now - p->stat_last_ns > p->stat_max_gap_ns) {
                    p->stat_max_gap_ns = now - p->stat_last_ns;
                }
                p->stat_last_ns = now;
                ++p->stat_buffers;

                if (p->stream) {
                    int queued = SDL_GetAudioStreamQueued(p->stream);
                    if (queued < p->stat_min_queued) p->stat_min_queued = queued;
                    if (queued == 0) ++p->stat_starved;
                }

                if (now - p->stat_start_ns >= 5000000000ULL) {
                    std::printf("Audio stats port %d: %u buffers/5s (expected %u), max gap %.2f ms, min queued %d bytes, starved %u\n",
                                static_cast<int>(p - ports_.data()),
                                p->stat_buffers,
                                static_cast<unsigned>(5ULL * 48000ULL / static_cast<unsigned>(p->frames)),
                                static_cast<double>(p->stat_max_gap_ns) / 1e6,
                                p->stat_min_queued,
                                p->stat_starved);
                    p->stat_start_ns = now;
                    p->stat_max_gap_ns = 0;
                    p->stat_starved = 0;
                    p->stat_buffers = 0;
                    p->stat_min_queued = 1 << 30;
                }
            }

            if (pace) {
                uint64_t now = NowNs();
                if (!p->next_deadline_ns || now > p->next_deadline_ns + 8 * period) {
                    p->next_deadline_ns = now;
                } else {
                    SleepUntil(p->next_deadline_ns);
                }
                p->next_deadline_ns += period;
            }

            if (p->stream) {
                int low = 2 * static_cast<int>(bytes);
                int queued = SDL_GetAudioStreamQueued(p->stream);
                if (queued < static_cast<int>(bytes)) {
                    alignas(16) static const unsigned char silence[2048 * 8 * 4]{};
                    int to_put = (low - queued < static_cast<int>(bytes)) ? (low - queued) : static_cast<int>(bytes);
                    SDL_PutAudioStreamData(p->stream, silence, to_put);
                    queued = low;
                }
                if (!p->window_count || queued < p->window_min) {
                    p->window_min = queued;
                }
                if (++p->window_count == 32) {
                    p->adjust_ns = (p->window_min > low + 2 * static_cast<int>(bytes))
                                       ? static_cast<int64_t>(period / 32)
                                       : (p->window_min < low ? -static_cast<int64_t>(period / 32) : 0);
                    p->window_count = 0;
                }
                p->next_deadline_ns += static_cast<uint64_t>(p->adjust_ns);
                SDL_PutAudioStreamData(p->stream, converted, static_cast<int>(bytes));
            }

            buffers_out_.fetch_add(1, std::memory_order_relaxed);
        }

        p->last_output_us = NowNs() / 1000;
        return data ? static_cast<int32_t>(samples) : 0;
    }

    std::mutex table_lock_;
    std::array<Port, PORTS> ports_{};
    bool initialized_{false};
    int sdl_ready_{-1};

    std::atomic<std::size_t> buffers_out_{0};
    std::atomic<std::size_t> ports_opened_{0};
};

} // namespace bb::runtime

// Guest ABI Trampolines
extern "C" {

using namespace bb::runtime;

static ABI int32_t audio_init(void) {
    return AudioManager::Instance().Init();
}

static ABI int32_t audio_open(int32_t user, int32_t type, int32_t index,
                              uint32_t length, uint32_t freq, uint32_t param) {
    return AudioManager::Instance().Open(user, type, index, length, freq, param);
}

static ABI int32_t audio_close(int32_t handle) {
    return AudioManager::Instance().Close(handle);
}

static ABI int32_t audio_output(int32_t handle, const void *data) {
    return AudioManager::Instance().Output(handle, data);
}

static ABI int32_t audio_outputs(const OutputParam *params, uint32_t count) {
    return AudioManager::Instance().Outputs(params, count);
}

static ABI int32_t audio_volume(int32_t handle, int32_t flags, const int32_t *volume) {
    return AudioManager::Instance().SetVolume(handle, flags, volume);
}

static ABI int32_t audio_state(int32_t handle, PortState *state) {
    return AudioManager::Instance().GetPortState(handle, state);
}

static ABI int32_t audio_last_time(int32_t handle, uint64_t *time) {
    return AudioManager::Instance().GetLastOutputTime(handle, time);
}

static const RuntimeExport exports[] = {
    {"sceAudioOutInit", reinterpret_cast<void*>(audio_init)},
    {"sceAudioOutOpen", reinterpret_cast<void*>(audio_open)},
    {"sceAudioOutClose", reinterpret_cast<void*>(audio_close)},
    {"sceAudioOutOutput", reinterpret_cast<void*>(audio_output)},
    {"sceAudioOutOutputs", reinterpret_cast<void*>(audio_outputs)},
    {"sceAudioOutSetVolume", reinterpret_cast<void*>(audio_volume)},
    {"sceAudioOutGetPortState", reinterpret_cast<void*>(audio_state)},
    {"sceAudioOutGetLastOutputTime", reinterpret_cast<void*>(audio_last_time)},
};

void runtime_audio_register(void) {
    runtime_register_exports(exports, sizeof(exports) / sizeof(exports[0]));
}

uintptr_t runtime_audio_resolve(const char *name) {
    return runtime_registry_resolve(name, EXPORT_FUNCTION);
}

void runtime_audio_report(void) {
    AudioManager::Instance().Report();
}

} // extern "C"
