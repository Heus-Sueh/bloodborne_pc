#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "runtime_ngs2.hpp"
#include "runtime.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

extern "C" {
#include "third_party/LibAtrac9/C/src/libatrac9.h"
}

#define NGS2_INVALID_HANDLE ((int32_t)0x804A0230)
#define NGS2_INVALID_ARG ((int32_t)0x804A0001)
#define NGS2_NO_MEMORY ((int32_t)0x804A0050)
#define SYSTEM_RATE 48000u
#define GRAIN 256u
#define MAX_CH 8u
#define MAX_PORTS 4u
#define MAX_MATRICES 4u
#define MAX_BLOCKS 32u
#define CACHE_FRAMES 2048u

namespace {

enum { MAGIC_SYSTEM = 0x4e475331, MAGIC_RACK = 0x4e475332, MAGIC_VOICE = 0x4e475333 };
enum { KIND_SAMPLER = 1, KIND_SUBMIXER = 2, KIND_MASTERING = 3, KIND_REVERB = 4 };
enum {
    STATE_INUSE = 0x1,
    STATE_PLAYING = 0x2,
    STATE_PAUSED = 0x4,
    STATE_STOPPED = 0x8,
    STATE_ERROR = 0x10,
    STATE_EMPTY = 0x20
};
enum { EVENT_PLAY, EVENT_STOP, EVENT_STOP_IMM, EVENT_KILL, EVENT_PAUSE, EVENT_RESUME };
enum {
    WAVE_I8 = 0x10,
    WAVE_U8,
    WAVE_I16L,
    WAVE_I16B,
    WAVE_I24L,
    WAVE_I24B,
    WAVE_I32L,
    WAVE_I32B,
    WAVE_F32L,
    WAVE_F32B,
    WAVE_F64L,
    WAVE_F64B,
    WAVE_VAG,
    WAVE_ATRAC9 = 0x40
};
enum {
    PARAM_MATRIX_LEVELS = 1,
    PARAM_PORT_VOLUME,
    PARAM_PORT_MATRIX,
    PARAM_PORT_DELAY,
    PARAM_PATCH,
    PARAM_EVENT,
    PARAM_CALLBACK
};
enum {
    SAMPLER_SETUP = 0x10000000,
    SAMPLER_BLOCKS,
    SAMPLER_ADDRESS,
    SAMPLER_FRAME_OFFSET,
    SAMPLER_EXIT_LOOP,
    SAMPLER_PITCH
};

struct ParamHeader {
    uint16_t size;
    int16_t next;
    uint32_t id;
};
struct WaveBlock {
    uint32_t data_offset, data_size, repeats, skip_samples, samples, reserved;
    uint64_t user_data;
};
struct RenderBuffer {
    void *buffer;
    uint64_t size;
    uint32_t waveform, channels;
};
struct BufferInfo {
    void *buffer;
    uint64_t size;
    uint64_t reserved[5];
    uint64_t user_data;
};

struct Block {
    const uint8_t *data;
    uint32_t size, repeats, skip, samples;
    uint64_t user_data;
};

struct Rack;
struct Voice {
    uint32_t magic, index;
    Rack *rack;
    uint32_t state;
    uint32_t channels; ///< sampler: the waveform's; others: input and output channels
    struct {
        int32_t matrix;
        float volume;
        Voice *dest;
    } port[MAX_PORTS];
    float matrix[MAX_MATRICES][MAX_CH * MAX_CH];
    uint32_t matrix_levels[MAX_MATRICES];
    float *input;   ///< GRAIN x MAX_CH, for voices others send to
    int input_used; ///< something was sent this grain
    /* Sampler. */
    uint32_t waveform, rate, config;
    Block blocks[MAX_BLOCKS];
    uint32_t head, count;
    uint32_t offset;  ///< bytes of the current block consumed
    uint32_t emitted; ///< frames of the current block played
    uint32_t skip_left, repeats_left;
    float pitch;
    uint64_t decoded_samples, decoded_bytes;
    float cache[CACHE_FRAMES * MAX_CH];
    uint32_t cache_frames, cache_pos;
    float prev[MAX_CH], next[MAX_CH];
    double phase; ///< position between prev (0) and next (1)
    int primed;
    int32_t vag_hist[MAX_CH][2];
    void *atrac9;
    Atrac9CodecInfo atrac9_info;
};

struct System;
struct Rack {
    uint32_t magic, id, kind, num_voices;
    System *system;
    Voice *voices;
    Rack *next;
};

struct System {
    uint32_t magic, rate;
    Rack *racks;
};

std::recursive_mutex s_lock;
std::atomic<size_t> s_renders{0};
std::atomic<size_t> s_blocks_added{0};
std::atomic<size_t> s_blocks_played{0};
std::atomic<size_t> s_starved{0};

/* BB_NGS2_TRACE: every 10 s */
size_t s_window_restarts = 0;
size_t s_window_dropped = 0;
size_t s_window_starved = 0;
size_t s_window_limited = 0;
size_t s_window_renders = 0;
float s_window_peak = 0.0f;

bool Trace() {
    static int on = -1;
    if (on < 0) {
        const char *env = std::getenv("BB_NGS2_TRACE");
        on = (env && env[0] == '1') ? 1 : 0;
    }
    return on != 0;
}

uint32_t RackKind(uint32_t id) {
    switch (id >> 12) {
    case 1:
        return KIND_SAMPLER;
    case 3:
        return KIND_MASTERING;
    case 4:
        return KIND_REVERB;
    default:
        return KIND_SUBMIXER; /* 0x2000 submixer, custom racks */
    }
}

Voice *VoiceOf(void *handle) {
    auto *v = static_cast<Voice *>(handle);
    return (v && v->magic == MAGIC_VOICE) ? v : nullptr;
}

/* ---- Waveform decoding ----------------------------------------------------------------------- */

uint32_t SampleBytes(uint32_t waveform) {
    switch (waveform) {
    case WAVE_I8:
    case WAVE_U8:
        return 1;
    case WAVE_I16L:
    case WAVE_I16B:
        return 2;
    case WAVE_I24L:
    case WAVE_I24B:
        return 3;
    case WAVE_I32L:
    case WAVE_I32B:
    case WAVE_F32L:
    case WAVE_F32B:
        return 4;
    case WAVE_F64L:
    case WAVE_F64B:
        return 8;
    default:
        return 0;
    }
}

float PcmSample(const uint8_t *p, uint32_t waveform) {
    switch (waveform) {
    case WAVE_I8:
        return static_cast<int8_t>(p[0]) / 128.0f;
    case WAVE_U8:
        return (p[0] - 128) / 128.0f;
    case WAVE_I16L:
        return static_cast<int16_t>(p[0] | (p[1] << 8)) / 32768.0f;
    case WAVE_I16B:
        return static_cast<int16_t>(p[1] | (p[0] << 8)) / 32768.0f;
    case WAVE_I24L:
        return static_cast<int32_t>((static_cast<uint32_t>(p[0]) << 8) | (static_cast<uint32_t>(p[1]) << 16) |
                                    (static_cast<uint32_t>(p[2]) << 24)) /
               2147483648.0f;
    case WAVE_I24B:
        return static_cast<int32_t>((static_cast<uint32_t>(p[2]) << 8) | (static_cast<uint32_t>(p[1]) << 16) |
                                    (static_cast<uint32_t>(p[0]) << 24)) /
               2147483648.0f;
    case WAVE_I32L: {
        int32_t s = 0;
        std::memcpy(&s, p, 4);
        return s / 2147483648.0f;
    }
    case WAVE_I32B:
        return static_cast<int32_t>(static_cast<uint32_t>(p[3]) | (static_cast<uint32_t>(p[2]) << 8) |
                                    (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[0]) << 24)) /
               2147483648.0f;
    case WAVE_F32L: {
        float f = 0.0f;
        std::memcpy(&f, p, 4);
        return f;
    }
    case WAVE_F32B: {
        const uint32_t u = static_cast<uint32_t>(p[3]) | (static_cast<uint32_t>(p[2]) << 8) |
                           (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[0]) << 24);
        float f = 0.0f;
        std::memcpy(&f, &u, 4);
        return f;
    }
    case WAVE_F64L: {
        double d = 0.0;
        std::memcpy(&d, p, 8);
        return static_cast<float>(d);
    }
    case WAVE_F64B: {
        uint8_t b[8];
        for (int i = 0; i < 8; ++i) {
            b[i] = p[7 - i];
        }
        double d = 0.0;
        std::memcpy(&d, b, 8);
        return static_cast<float>(d);
    }
    default:
        return 0.0f;
    }
}

/* PlayStation ADPCM: 16-byte frames of 28 samples per channel */
uint32_t DecodeVag(Voice *v, const uint8_t *src, uint32_t avail) {
    const uint32_t ch = v->channels;
    const uint32_t unit = 16 * ch;
    if (avail < unit) {
        return 0;
    }
    static const int f0[5] = {0, 60, 115, 98, 122};
    static const int f1[5] = {0, 0, -52, -55, -60};
    for (uint32_t c = 0; c < ch; ++c) {
        const uint8_t *frame = src + 16 * c;
        int shift = frame[0] & 15;
        int filter = frame[0] >> 4;
        if (shift > 12) shift = 9;
        if (filter > 4) filter = 0;
        int32_t *h = v->vag_hist[c];
        for (int i = 0; i < 28; ++i) {
            const int nibble = (frame[2 + i / 2] >> ((i & 1) * 4)) & 15;
            int32_t s = static_cast<int32_t>((static_cast<uint32_t>(nibble << 12) << 16)) >> 16;
            s = (s >> shift) + ((h[0] * f0[filter] + h[1] * f1[filter] + 32) >> 6);
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            h[1] = h[0];
            h[0] = s;
            v->cache[i * ch + c] = s / 32768.0f;
        }
    }
    v->cache_frames = 28;
    return unit;
}

uint32_t DecodeAtrac9(Voice *v, const uint8_t *src, uint32_t avail) {
    if (!v->atrac9) {
        uint8_t config[4];
        std::memcpy(config, &v->config, 4);
        v->atrac9 = Atrac9GetHandle();
        if (!v->atrac9 || Atrac9InitDecoder(v->atrac9, config) ||
            Atrac9GetCodecInfo(v->atrac9, &v->atrac9_info)) {
            if (v->atrac9) {
                Atrac9ReleaseHandle(v->atrac9);
            }
            v->atrac9 = nullptr;
            return 0;
        }
    }
    const Atrac9CodecInfo *info = &v->atrac9_info;
    const uint32_t ch = static_cast<uint32_t>(info->channels);
    const uint32_t frames = static_cast<uint32_t>(info->frameSamples);
    if (!avail || ch > MAX_CH || frames * ch > CACHE_FRAMES * MAX_CH) {
        return 0;
    }
    short pcm[CACHE_FRAMES * MAX_CH];
    int used = 0;
    if (Atrac9Decode(v->atrac9, src, pcm, &used) || used <= 0 || static_cast<uint32_t>(used) > avail) {
        return 0;
    }
    for (uint32_t i = 0; i < frames; ++i) {
        for (uint32_t c = 0; c < v->channels; ++c) {
            v->cache[i * v->channels + c] = (c < ch) ? pcm[i * ch + c] / 32768.0f : 0.0f;
        }
    }
    v->cache_frames = frames;
    return static_cast<uint32_t>(used);
}

void ResetDecoder(Voice *v) {
    std::memset(v->vag_hist, 0, sizeof(v->vag_hist));
    if (v->atrac9) {
        Atrac9ReleaseHandle(v->atrac9);
        v->atrac9 = nullptr;
    }
}

/* Ends the block in play: repeats it while it has repeats left, else moves to the next one. */
void FinishBlock(Voice *v) {
    Block *b = &v->blocks[v->head];
    if (v->repeats_left) {
        --v->repeats_left;
        v->offset = 0;
        v->emitted = 0;
        v->skip_left = b->skip;
        return;
    }
    s_blocks_played.fetch_add(1, std::memory_order_relaxed);
    v->head = (v->head + 1) % MAX_BLOCKS;
    --v->count;
    v->offset = 0;
    v->emitted = 0;
    if (v->count) {
        v->skip_left = v->blocks[v->head].skip;
        v->repeats_left = v->blocks[v->head].repeats;
    }
}

/* Decodes the next piece of the queued blocks into the cache; 0 when nothing is left. */
int Refill(Voice *v) {
    v->cache_frames = v->cache_pos = 0;
    while (v->count) {
        Block *b = &v->blocks[v->head];
        const uint32_t limit = (b->samples && b->samples != UINT32_MAX) ? b->samples : UINT32_MAX;
        const uint32_t avail = (b->size > v->offset) ? b->size - v->offset : 0;
        const uint8_t *src = b->data + v->offset;
        uint32_t used = 0;
        if (v->emitted < limit && avail) {
            if (v->waveform == WAVE_VAG) {
                used = DecodeVag(v, src, avail);
            } else if (v->waveform == WAVE_ATRAC9) {
                used = DecodeAtrac9(v, src, avail);
            } else {
                const uint32_t frame = SampleBytes(v->waveform) * v->channels;
                uint32_t n = frame ? avail / frame : 0;
                if (n > CACHE_FRAMES) n = CACHE_FRAMES;
                for (uint32_t i = 0; i < n * v->channels; ++i) {
                    v->cache[i] = PcmSample(src + i * SampleBytes(v->waveform), v->waveform);
                }
                v->cache_frames = n;
                used = n * frame;
            }
        }
        if (!used) {
            FinishBlock(v);
            continue;
        }
        v->offset += used;
        v->decoded_bytes += used;
        if (v->skip_left) {
            const uint32_t s = (v->skip_left < v->cache_frames) ? v->skip_left : v->cache_frames;
            v->skip_left -= s;
            v->cache_pos = s;
        }
        if (v->emitted + (v->cache_frames - v->cache_pos) > limit) {
            v->cache_frames = v->cache_pos + (limit - v->emitted);
        }
        v->emitted += v->cache_frames - v->cache_pos;
        if (v->cache_pos < v->cache_frames) {
            return 1;
        }
        v->cache_frames = v->cache_pos = 0;
    }
    return 0;
}

int NextFrame(Voice *v, float *out) {
    if (v->cache_pos >= v->cache_frames && !Refill(v)) {
        return 0;
    }
    std::memcpy(out, &v->cache[v->cache_pos * v->channels], v->channels * sizeof(float));
    ++v->cache_pos;
    ++v->decoded_samples;
    return 1;
}

/* ---- Mixing ---------------------------------------------------------------------------------- */

float *InputOf(Voice *v) {
    if (!v->input) {
        v->input = static_cast<float *>(std::calloc(GRAIN * MAX_CH, sizeof(float)));
    }
    return v->input;
}

void Send(Voice *v, const float *src, uint32_t frames, uint32_t channels) {
    for (uint32_t p = 0; p < MAX_PORTS; ++p) {
        Voice *d = v->port[p].dest;
        const float volume = v->port[p].volume;
        if (!d || volume == 0.0f) {
            continue;
        }
        float *dst = InputOf(d);
        if (!dst) {
            continue;
        }
        const uint32_t out = d->channels ? d->channels : 2;
        const int32_t m = v->port[p].matrix;
        const float *levels =
            (m >= 0 && m < static_cast<int32_t>(MAX_MATRICES) && v->matrix_levels[m] >= channels * out)
                ? v->matrix[m]
                : nullptr;
        for (uint32_t f = 0; f < frames; ++f) {
            const float *s = src + f * channels;
            float *o = dst + f * MAX_CH;
            if (levels) {
                for (uint32_t i = 0; i < channels; ++i) {
                    for (uint32_t c = 0; c < out; ++c) {
                        o[c] += s[i] * levels[c * channels + i] * volume;
                    }
                }
            } else if (channels == 1) {
                o[0] += s[0] * volume;
                if (out > 1) {
                    o[1] += s[0] * volume;
                }
            } else {
                for (uint32_t c = 0; c < channels && c < out; ++c) {
                    o[c] += s[c] * volume;
                }
            }
        }
        d->input_used = 1;
    }
}

void RenderSampler(Voice *v, uint32_t frames, uint32_t system_rate) {
    if (!(v->state & STATE_PLAYING) || (v->state & STATE_PAUSED) || !v->channels) {
        return;
    }
    float out[GRAIN * MAX_CH];
    const double step =
        static_cast<double>(v->rate) / system_rate * (v->pitch > 0.0f ? v->pitch : 1.0f);
    const uint32_t ch = v->channels;
    uint32_t f = 0;
    int ended = 0;
    if (!v->primed) {
        if (!NextFrame(v, v->prev) || !NextFrame(v, v->next)) {
            ended = 1;
        }
        v->phase = 0.0;
        v->primed = 1;
    }
    for (; f < frames && !ended; ++f) {
        for (uint32_t c = 0; c < ch; ++c) {
            out[f * ch + c] = v->prev[c] + static_cast<float>(v->phase) * (v->next[c] - v->prev[c]);
        }
        v->phase += step;
        while (v->phase >= 1.0) {
            v->phase -= 1.0;
            std::memcpy(v->prev, v->next, ch * sizeof(float));
            if (!NextFrame(v, v->next)) {
                ended = 1;
                ++f;
                break;
            }
        }
    }
    if (f) {
        Send(v, out, f, ch);
    }
    if (ended) {
        if (Trace()) {
            std::printf("NGS2 trace: sampler %u ran out of blocks (%llu samples played)\n", v->index,
                        static_cast<unsigned long long>(v->decoded_samples));
        }
        s_starved.fetch_add(1, std::memory_order_relaxed);
        ++s_window_starved;
        v->state = 0;
        v->primed = 0;
    }
}

void RenderPassthrough(Voice *v, uint32_t frames) {
    if (!v->input_used) {
        return;
    }
    if (v->state & STATE_PLAYING) {
        float out[GRAIN * MAX_CH];
        const uint32_t ch = v->channels ? v->channels : MAX_CH;
        for (uint32_t f = 0; f < frames; ++f) {
            for (uint32_t c = 0; c < ch; ++c) {
                out[f * ch + c] = v->input[f * MAX_CH + c];
            }
        }
        Send(v, out, frames, ch);
    }
}

float s_limiter_gain = 1.0f;
void WriteOutput(const RenderBuffer *b, uint64_t first, const float *mix, uint32_t frames) {
    const uint32_t ch = b->channels ? b->channels : 2;
    const uint32_t bytes = SampleBytes(b->waveform);
    auto *dst = static_cast<uint8_t *>(b->buffer) + first * ch * bytes;
    constexpr float threshold = 0.95f;
    constexpr float recover = 1.0f / (0.05f * SYSTEM_RATE);
    for (uint32_t f = 0; f < frames; ++f) {
        float peak = 0.0f;
        for (uint32_t c = 0; c < ch && c < MAX_CH; ++c) {
            peak = std::fmax(peak, std::fabs(mix[f * MAX_CH + c]));
        }
        if (peak > s_window_peak) {
            s_window_peak = peak;
        }
        if (peak * s_limiter_gain > threshold) {
            s_limiter_gain = threshold / peak;
            ++s_window_limited;
        } else if (s_limiter_gain < 1.0f) {
            s_limiter_gain = std::fmin(1.0f, s_limiter_gain + recover);
        }
        for (uint32_t c = 0; c < ch; ++c) {
            float s = (c < MAX_CH) ? mix[f * MAX_CH + c] * s_limiter_gain : 0.0f;
            if (s > 1.0f) s = 1.0f;
            if (s < -1.0f) s = -1.0f;
            if (b->waveform == WAVE_F32L) {
                std::memcpy(dst, &s, 4);
                dst += 4;
                continue;
            }
            if (b->waveform == WAVE_I16L) {
                const auto i = static_cast<int16_t>(std::lrintf(s * 32767.0f));
                std::memcpy(dst, &i, 2);
                dst += 2;
            } else {
                std::memset(dst, 0, bytes);
                dst += bytes;
            }
        }
    }
}

void RenderGrain(System *s, const RenderBuffer *buffers, uint32_t count, uint64_t first, uint32_t frames) {
    for (Rack *r = s->racks; r; r = r->next) {
        for (uint32_t i = 0; i < r->num_voices; ++i) {
            Voice *v = &r->voices[i];
            if (v->input && v->input_used) {
                std::memset(v->input, 0, GRAIN * MAX_CH * sizeof(float));
            }
            v->input_used = 0;
        }
    }
    static const uint32_t order[] = {KIND_SAMPLER, KIND_REVERB, KIND_SUBMIXER};
    for (uint32_t k = 0; k < sizeof(order) / sizeof(*order); ++k) {
        for (Rack *r = s->racks; r; r = r->next) {
            if (r->kind != order[k]) {
                continue;
            }
            for (uint32_t i = 0; i < r->num_voices; ++i) {
                Voice *v = &r->voices[i];
                if (r->kind == KIND_SAMPLER) {
                    RenderSampler(v, frames, s->rate);
                } else if (r->kind == KIND_SUBMIXER) {
                    RenderPassthrough(v, frames);
                }
            }
        }
    }
    static const float silence[GRAIN * MAX_CH]{};
    int written[16] = {0};
    for (Rack *r = s->racks; r; r = r->next) {
        if (r->kind != KIND_MASTERING) {
            continue;
        }
        for (uint32_t i = 0; i < r->num_voices; ++i) {
            Voice *v = &r->voices[i];
            const uint32_t target = (i < count) ? i : 0;
            if (target >= 16 || !buffers[target].buffer) {
                continue;
            }
            const int live = (v->state & STATE_PLAYING) && v->input_used;
            if (live || !written[target]) {
                WriteOutput(&buffers[target], first, live ? v->input : silence, frames);
                written[target] = 1;
            }
        }
    }
    for (uint32_t b = 0; b < count && b < 16; ++b) {
        if (!written[b] && buffers[b].buffer) {
            WriteOutput(&buffers[b], first, silence, frames);
        }
    }
}

/* ---- Parameters -------------------------------------------------------------------------- */

void SamplerSetup(Voice *v, const uint8_t *p) {
    uint32_t format[6];
    std::memcpy(format, p + 8, sizeof(format));
    ResetDecoder(v);
    v->waveform = format[0];
    v->channels = (format[1] <= MAX_CH) ? format[1] : MAX_CH;
    v->rate = format[2] ? format[2] : SYSTEM_RATE;
    v->config = format[3];
    v->head = v->count = 0;
    v->offset = v->emitted = v->skip_left = v->repeats_left = 0;
    v->cache_frames = v->cache_pos = 0;
    v->decoded_samples = v->decoded_bytes = 0;
    v->pitch = 1.0f;
    v->primed = 0;
    v->state = 0;
    if (Trace()) {
        std::printf("NGS2 trace: sampler %u setup: waveform %#x, %u channels, %u Hz, config %#x\n", v->index,
                    v->waveform, format[1], v->rate, v->config);
    }
}

void SamplerBlocks(Voice *v, const uint8_t *p) {
    const uint8_t *data = nullptr;
    uint32_t flags = 0;
    uint32_t n = 0;
    const WaveBlock *blocks = nullptr;
    std::memcpy(&data, p + 8, 8);
    std::memcpy(&flags, p + 16, 4);
    std::memcpy(&n, p + 20, 4);
    std::memcpy(&blocks, p + 24, 8);

    if (Trace()) {
        static int printed = 0;
        if (printed++ < 200) {
            std::printf(
                "NGS2 trace: sampler %u blocks: flags %#x, %u block(s) of %u bytes, %u queued, %llu samples played\n",
                v->index, flags, n, (blocks && n) ? blocks[0].data_size : 0, v->count,
                static_cast<unsigned long long>(v->decoded_samples));
        }
    }
    if (!(flags & 2)) {
        if (v->count) {
            ++s_window_restarts;
            s_window_dropped += v->count;
        }
        v->head = v->count = 0;
        v->offset = v->emitted = 0;
        v->cache_frames = v->cache_pos = 0;
        ResetDecoder(v);
    }
    for (uint32_t i = 0; blocks && data && i < n; ++i) {
        if (v->count == MAX_BLOCKS) {
            std::printf("Runtime: NGS2 sampler %u: block queue full\n", v->index);
            break;
        }
        const int first = (v->count == 0);
        Block *b = &v->blocks[(v->head + v->count++) % MAX_BLOCKS];
        b->data = data + blocks[i].data_offset;
        b->size = blocks[i].data_size;
        b->repeats = blocks[i].repeats;
        b->skip = blocks[i].skip_samples;
        b->samples = blocks[i].samples;
        b->user_data = blocks[i].user_data;
        if (first) {
            v->skip_left = b->skip;
            v->repeats_left = b->repeats;
            v->offset = v->emitted = 0;
        }
        s_blocks_added.fetch_add(1, std::memory_order_relaxed);
    }
}

void VoiceEvent(Voice *v, uint32_t event) {
    switch (event) {
    case EVENT_PLAY:
        v->state = STATE_INUSE | STATE_PLAYING;
        v->primed = 0;
        break;
    case EVENT_STOP:
    case EVENT_STOP_IMM:
    case EVENT_KILL:
        v->state = 0;
        v->primed = 0;
        if (event != EVENT_STOP) {
            v->head = v->count = 0;
            v->cache_frames = v->cache_pos = 0;
        }
        break;
    case EVENT_PAUSE:
        if (v->state & STATE_PLAYING) {
            v->state |= STATE_PAUSED;
        }
        break;
    case EVENT_RESUME:
        v->state &= ~static_cast<uint32_t>(STATE_PAUSED);
        break;
    default:
        break;
    }
}

void VoiceParam(Voice *v, const ParamHeader *h) {
    const auto *p = reinterpret_cast<const uint8_t *>(h);
    uint32_t a = 0;
    uint32_t b = 0;
    if (h->size >= 12) std::memcpy(&a, p + 8, 4);
    if (h->size >= 16) std::memcpy(&b, p + 12, 4);
    switch (h->id) {
    case PARAM_MATRIX_LEVELS: {
        const float *levels = nullptr;
        std::memcpy(&levels, p + 16, 8);
        if (a < MAX_MATRICES && levels) {
            const uint32_t n = (b < MAX_CH * MAX_CH) ? b : MAX_CH * MAX_CH;
            std::memcpy(v->matrix[a], levels, n * sizeof(float));
            v->matrix_levels[a] = n;
            if (Trace()) {
                static int printed = 0;
                if (printed++ < 4) {
                    std::printf("NGS2 trace: voice %u matrix %u:", v->index, a);
                    for (uint32_t i = 0; i < n; ++i) {
                        std::printf(" %.2f", levels[i]);
                    }
                    std::putchar('\n');
                }
            }
        }
        break;
    }
    case PARAM_PORT_VOLUME:
        if (a < MAX_PORTS) {
            std::memcpy(&v->port[a].volume, p + 12, 4);
        }
        break;
    case PARAM_PORT_MATRIX:
        if (a < MAX_PORTS) {
            v->port[a].matrix = static_cast<int32_t>(b);
        }
        break;
    case PARAM_PATCH: {
        void *dest = nullptr;
        std::memcpy(&dest, p + 16, 8);
        if (a < MAX_PORTS) {
            v->port[a].dest = VoiceOf(dest);
        }
        break;
    }
    case PARAM_EVENT:
        VoiceEvent(v, a);
        break;
    default:
        if (v->rack->kind == KIND_SAMPLER) {
            if (h->id == SAMPLER_SETUP) {
                SamplerSetup(v, p);
            } else if (h->id == SAMPLER_BLOCKS) {
                SamplerBlocks(v, p);
            } else if (h->id == SAMPLER_EXIT_LOOP) {
                v->repeats_left = 0;
            } else if (h->id == SAMPLER_PITCH) {
                std::memcpy(&v->pitch, p + 8, 4);
            }
        } else if ((h->id & 0xffff) == 0 && h->size >= 12) {
            v->channels = (a && a <= MAX_CH) ? a : MAX_CH;
        }
        break;
    }
}

/* ---- Entry points ------------------------------------------------------------------------ */

ABI int32_t query_size(const void *option, BufferInfo *info) {
    (void)option;
    if (!info) return NGS2_INVALID_ARG;
    info->size = 4096;
    return 0;
}

ABI int32_t rack_query_size(uint32_t rack_id, const void *option, BufferInfo *info) {
    (void)rack_id;
    return query_size(option, info);
}

ABI int32_t system_create(const void *option, const void *buffer, System **out) {
    (void)option;
    (void)buffer;
    if (!out) return NGS2_INVALID_ARG;
    auto *s = static_cast<System *>(std::calloc(1, sizeof(System)));
    if (!s) return NGS2_NO_MEMORY;
    s->magic = MAGIC_SYSTEM;
    s->rate = SYSTEM_RATE;
    *out = s;
    std::printf("Runtime: NGS2 system created (mixing samplers to %u Hz)\n", s->rate);
    return 0;
}

ABI int32_t system_destroy(System *s, void *context) {
    (void)context;
    if (!s || s->magic != MAGIC_SYSTEM) return NGS2_INVALID_HANDLE;
    std::lock_guard<std::recursive_mutex> lock(s_lock);
    s->magic = 0;
    return 0;
}

ABI int32_t rack_create(System *s, uint32_t rack_id, const void *option, const void *buffer, Rack **out) {
    (void)buffer;
    if (!s || s->magic != MAGIC_SYSTEM) return NGS2_INVALID_HANDLE;
    if (!out) return NGS2_INVALID_ARG;
    uint32_t voices = 0;
    if (option) {
        std::memcpy(&voices, static_cast<const uint8_t *>(option) + 32, 4);
    }
    if (!voices || voices > 1024) voices = 256;
    auto *r = static_cast<Rack *>(std::calloc(1, sizeof(Rack)));
    auto *v = r ? static_cast<Voice *>(std::calloc(voices, sizeof(Voice))) : nullptr;
    if (!v) {
        std::free(r);
        return NGS2_NO_MEMORY;
    }
    r->magic = MAGIC_RACK;
    r->id = rack_id;
    r->kind = RackKind(rack_id);
    r->num_voices = voices;
    r->system = s;
    r->voices = v;
    for (uint32_t i = 0; i < voices; ++i) {
        v[i].magic = MAGIC_VOICE;
        v[i].index = i;
        v[i].rack = r;
        v[i].pitch = 1.0f;
        for (uint32_t p = 0; p < MAX_PORTS; ++p) {
            v[i].port[p].matrix = -1;
            v[i].port[p].volume = 1.0f;
        }
    }
    std::lock_guard<std::recursive_mutex> lock(s_lock);
    Rack **tail = &s->racks;
    while (*tail) {
        tail = &(*tail)->next;
    }
    *tail = r;
    if (Trace()) {
        std::printf("NGS2 trace: rack %#x created with %u voices\n", rack_id, voices);
    }
    *out = r;
    return 0;
}

ABI int32_t rack_destroy(Rack *r, void *context) {
    (void)context;
    if (!r || r->magic != MAGIC_RACK) return NGS2_INVALID_HANDLE;
    std::lock_guard<std::recursive_mutex> lock(s_lock);
    Rack **link = &r->system->racks;
    while (*link && *link != r) {
        link = &(*link)->next;
    }
    if (*link) {
        *link = r->next;
    }
    for (Rack *o = r->system->racks; o; o = o->next) {
        for (uint32_t i = 0; i < o->num_voices; ++i) {
            for (uint32_t p = 0; p < MAX_PORTS; ++p) {
                if (o->voices[i].port[p].dest && o->voices[i].port[p].dest->rack == r) {
                    o->voices[i].port[p].dest = nullptr;
                }
            }
        }
    }
    for (uint32_t i = 0; i < r->num_voices; ++i) {
        ResetDecoder(&r->voices[i]);
        std::free(r->voices[i].input);
        r->voices[i].magic = 0;
    }
    r->magic = 0;
    std::free(r->voices);
    std::free(r);
    return 0;
}

ABI int32_t voice_handle(Rack *r, uint32_t index, Voice **out) {
    if (!r || r->magic != MAGIC_RACK) return NGS2_INVALID_HANDLE;
    if (!out || index >= r->num_voices) return NGS2_INVALID_ARG;
    *out = &r->voices[index];
    if (Trace()) {
        std::printf("NGS2 trace: rack %#x voice %u = %p\n", r->id, index, static_cast<void *>(*out));
    }
    return 0;
}

ABI int32_t voice_control(void *handle, const ParamHeader *params) {
    Voice *v = VoiceOf(handle);
    if (!v) return NGS2_INVALID_HANDLE;
    std::lock_guard<std::recursive_mutex> lock(s_lock);
    const ParamHeader *h = params;
    for (int n = 0; h && n < 256; ++n) {
        if (Trace() && h->id != PARAM_MATRIX_LEVELS && h->id != SAMPLER_PITCH) {
            std::printf("NGS2 trace: rack %#x voice %u param %#x\n", v->rack->id, v->index, h->id);
        }
        VoiceParam(v, h);
        if (h->next == 0) {
            break;
        }
        h = reinterpret_cast<const ParamHeader *>(reinterpret_cast<const uint8_t *>(h) + h->next);
    }
    return 0;
}

ABI int32_t voice_state(void *handle, void *state, uint64_t size) {
    Voice *v = VoiceOf(handle);
    if (!v) return NGS2_INVALID_HANDLE;
    if (!state || size < 4) return NGS2_INVALID_ARG;
    uint8_t out[48] = {0};
    {
        std::lock_guard<std::recursive_mutex> lock(s_lock);
        uint32_t flags = v->state;
        if (v->rack->kind == KIND_SAMPLER && (flags & STATE_INUSE) && !v->count) {
            flags |= STATE_EMPTY;
        }
        std::memcpy(out, &flags, 4);
        if (v->rack->kind == KIND_SAMPLER) {
            constexpr float one = 1.0f;
            std::memcpy(out + 4, &one, 4);
            std::memcpy(out + 16, &v->decoded_samples, 8);
            std::memcpy(out + 24, &v->decoded_bytes, 8);
            if (v->count) {
                const Block *b = &v->blocks[v->head];
                std::memcpy(out + 32, &b->user_data, 8);
                std::memcpy(out + 40, &b->data, 8);
            }
        }
    }
    std::memcpy(state, out, (size < sizeof(out)) ? size : sizeof(out));
    if (size > sizeof(out)) {
        std::memset(static_cast<uint8_t *>(state) + sizeof(out), 0, size - sizeof(out));
    }
    return 0;
}

ABI int32_t system_render(System *s, const RenderBuffer *buffers, uint32_t count) {
    if (!s || s->magic != MAGIC_SYSTEM) return NGS2_INVALID_HANDLE;
    if (!buffers || !count) return NGS2_INVALID_ARG;
    uint64_t frames = UINT64_MAX;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t bytes =
            SampleBytes(buffers[i].waveform) * (buffers[i].channels ? buffers[i].channels : 2);
        if (!bytes) return NGS2_INVALID_ARG;
        if (buffers[i].size / bytes < frames) {
            frames = buffers[i].size / bytes;
        }
    }
    std::lock_guard<std::recursive_mutex> lock(s_lock);
    for (uint64_t first = 0; first < frames; first += GRAIN) {
        RenderGrain(s, buffers, count, first,
                    static_cast<uint32_t>((frames - first < GRAIN) ? frames - first : GRAIN));
    }
    s_renders.fetch_add(1, std::memory_order_relaxed);
    if (Trace() && ++s_window_renders * frames >= 10ull * s->rate) {
        std::printf(
            "NGS2 stats: %zu renders; queues restarted %zu (%zu blocks dropped), samplers dry %zu, peak %.2f, "
            "limited frames %zu\n",
            s_window_renders, s_window_restarts, s_window_dropped, s_window_starved, s_window_peak, s_window_limited);
        s_window_renders = s_window_restarts = s_window_dropped = s_window_starved = s_window_limited = 0;
        s_window_peak = 0.0f;
    }
    return 0;
}

ABI int32_t system_lock(System *s) {
    return (s && s->magic == MAGIC_SYSTEM) ? 0 : NGS2_INVALID_HANDLE;
}
ABI int32_t rack_lock(Rack *r) {
    return (r && r->magic == MAGIC_RACK) ? 0 : NGS2_INVALID_HANDLE;
}
ABI int32_t system_set_value(System *s, uint32_t value) {
    (void)value;
    return (s && s->magic == MAGIC_SYSTEM) ? 0 : NGS2_INVALID_HANDLE;
}

const RuntimeExport ngs2_exports[] = {
    {"sceNgs2SystemQueryBufferSize", reinterpret_cast<void *>(query_size)},
    {"sceNgs2SystemCreate", reinterpret_cast<void *>(system_create)},
    {"sceNgs2SystemCreateWithAllocator", reinterpret_cast<void *>(system_create)},
    {"sceNgs2SystemDestroy", reinterpret_cast<void *>(system_destroy)},
    {"sceNgs2RackQueryBufferSize", reinterpret_cast<void *>(rack_query_size)},
    {"sceNgs2RackCreate", reinterpret_cast<void *>(rack_create)},
    {"sceNgs2RackCreateWithAllocator", reinterpret_cast<void *>(rack_create)},
    {"sceNgs2RackDestroy", reinterpret_cast<void *>(rack_destroy)},
    {"sceNgs2RackGetVoiceHandle", reinterpret_cast<void *>(voice_handle)},
    {"sceNgs2VoiceControl", reinterpret_cast<void *>(voice_control)},
    {"sceNgs2VoiceGetState", reinterpret_cast<void *>(voice_state)},
    {"sceNgs2SystemRender", reinterpret_cast<void *>(system_render)},
    {"sceNgs2SystemLock", reinterpret_cast<void *>(system_lock)},
    {"sceNgs2SystemUnlock", reinterpret_cast<void *>(system_lock)},
    {"sceNgs2RackLock", reinterpret_cast<void *>(rack_lock)},
    {"sceNgs2RackUnlock", reinterpret_cast<void *>(rack_lock)},
    {"sceNgs2SystemSetGrainSamples", reinterpret_cast<void *>(system_set_value)},
};

} // namespace

extern "C" {

void runtime_ngs2_register(void) {
    runtime_register_exports(ngs2_exports, sizeof(ngs2_exports) / sizeof(ngs2_exports[0]));
}

uintptr_t runtime_ngs2_resolve(const char *name) {
    return runtime_registry_resolve(name, EXPORT_FUNCTION);
}

void runtime_ngs2_report(void) {
    std::printf("Runtime: NGS2 renders=%zu, blocks added=%zu, played=%zu, samplers out of blocks=%zu\n",
                s_renders.load(std::memory_order_relaxed),
                s_blocks_added.load(std::memory_order_relaxed),
                s_blocks_played.load(std::memory_order_relaxed),
                s_starved.load(std::memory_order_relaxed));
}

} // extern "C"
