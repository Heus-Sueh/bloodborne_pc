#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "runtime_ajm.hpp"
#include "runtime.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

extern "C" {
#include "third_party/LibAtrac9/C/src/libatrac9.h"
}

#define ERR_INVALID_CONTEXT ((int32_t)0x80930002)
#define ERR_INVALID_INSTANCE ((int32_t)0x80930003)
#define ERR_INVALID_BATCH ((int32_t)0x80930004)
#define ERR_INVALID_PARAMETER ((int32_t)0x80930005)
#define ERR_OUT_OF_RESOURCES ((int32_t)0x80930007)
#define ERR_ALREADY_REGISTERED ((int32_t)0x80930009)
#define ERR_NOT_REGISTERED ((int32_t)0x8093000A)
#define ERR_WRONG_REVISION ((int32_t)0x8093000B)
#define ERR_MALFORMED_BATCH ((int32_t)0x80930011)
#define ERR_CANCELLED ((int32_t)0x80930017)
#define RESULT_NOT_INITIALIZED 0x1
#define RESULT_INVALID_PARAMETER 0x4
#define RESULT_PARTIAL_INPUT 0x8
#define RESULT_NOT_ENOUGH_ROOM 0x10
#define RESULT_CODEC_ERROR 0x40000000u
#define RESULT_FATAL 0x80000000u
#define STATISTICS_INSTANCE 0x80000u
#define MAX_INSTANCES 0x3fff
#define MAX_CONTEXTS 8
#define MAX_BATCHES 256

namespace {

enum {
    IDENT_JOB = 0,
    IDENT_INPUT_RUN = 1,
    IDENT_INPUT_CONTROL = 2,
    IDENT_CONTROL_FLAGS = 3,
    IDENT_RUN_FLAGS = 4,
    IDENT_RETURN_ADDRESS = 6,
    IDENT_INLINE = 7,
    IDENT_OUTPUT_RUN = 17,
    IDENT_OUTPUT_CONTROL = 18
};
enum { FORMAT_S16 = 0, FORMAT_S32 = 1, FORMAT_FLOAT = 2 };

#define RUN_CODEC_INFO(f) (((f) >> 11) & 1)
#define RUN_MULTIPLE_FRAMES(f) (((f) >> 12) & 1)
#define CONTROL_RESET(f) (((f) >> 13) & 1)
#define CONTROL_INITIALIZE(f) (((f) >> 14) & 1)
#define CONTROL_RESAMPLE(f) (((f) >> 15) & 1)
#define SIDEBAND_GAPLESS(f) (((f) >> 45) & 1)
#define SIDEBAND_FORMAT(f) (((f) >> 46) & 1)
#define SIDEBAND_STREAM(f) (((f) >> 47) & 1)

struct Chunk {
    uint32_t word;
    uint32_t size;
}; /* ident:6 payload:20 reserved:6 */
struct ChunkBuffer {
    uint32_t word;
    uint32_t size;
    void *address;
};
struct SidebandResult {
    int32_t result, internal_result;
};
struct SidebandStream {
    int32_t input_consumed, output_written;
    uint64_t total_decoded_samples;
};
struct SidebandFormat {
    uint32_t channels, channel_mask, sample_rate, encoding, bitrate, reserved;
};
struct SidebandGapless {
    uint32_t total_samples;
    uint16_t skip_samples, skipped_samples;
};
struct At9Info {
    uint32_t super_frame_size, frames_in_super_frame, next_frame_size, frame_samples;
};
struct BatchError {
    int error_code;
    const void *job_address;
    uint32_t command_offset;
    const void *job_return_address;
};

static_assert(sizeof(ChunkBuffer) == 16, "Ajm buffer chunk");
static_assert(sizeof(SidebandFormat) == 24 && sizeof(SidebandGapless) == 8 && sizeof(SidebandStream) == 16,
              "Ajm sideband layout");

struct Instance {
    int used, codec, format, channels_hint, gapless_loop;
    uint32_t codec_flags;
    void *handle;
    unsigned char config[4];
    int initialized;
    Atrac9CodecInfo info;
    uint32_t superframe_remain, frames;
    SidebandGapless gapless_init, gapless;
    uint64_t total_samples;
};

struct Context {
    int used, registered[24];
    Instance instances[MAX_INSTANCES + 1];
};

struct Batch {
    int used, context, canceled;
};

std::recursive_mutex s_lock;
Context *s_contexts[MAX_CONTEXTS + 1]{};
Batch s_batches[MAX_BATCHES]{};
std::atomic<size_t> s_jobs_run{0};
std::atomic<size_t> s_frames_decoded{0};
std::atomic<size_t> s_batches_run{0};

int s_trace_left = -1;
bool AjmTrace() {
    if (s_trace_left < 0) {
        const char *e = std::getenv("BB_AUDIO_TRACE");
        s_trace_left = (e && e[0] == '1') ? 400 : 0;
    }
    return s_trace_left > 0 && s_trace_left--;
}

uint32_t Ident(uint32_t word) {
    return word & 0x3f;
}
uint32_t Payload(uint32_t word) {
    return (word >> 6) & 0xfffff;
}
uint32_t Header(uint32_t id, uint32_t value) {
    return (id & 0x3f) | ((value & 0xfffff) << 6);
}

uint32_t ChannelMask(int channels) {
    switch (channels) {
    case 1:
        return 4;
    case 2:
        return 3;
    case 4:
        return 0x33;
    case 6:
        return 0x60f;
    case 8:
        return 0x63f;
    default:
        return 0;
    }
}

int PcmSize(int format) {
    return (format == FORMAT_S16) ? 2 : 4;
}

/* ---- batch builders ---- */
unsigned char *PutBuffer(unsigned char *p, uint32_t id, const void *address, uint64_t size) {
    ChunkBuffer c = {Header(id, 0), static_cast<uint32_t>(size), const_cast<void *>(address)};
    std::memcpy(p, &c, sizeof(c));
    return p + sizeof(c);
}

unsigned char *PutFlags(unsigned char *p, uint32_t id, uint64_t flags) {
    Chunk c = {Header(id, static_cast<uint32_t>(flags >> 32)), static_cast<uint32_t>(flags)};
    std::memcpy(p, &c, sizeof(c));
    return p + sizeof(c);
}

void *FinishJob(unsigned char *start, unsigned char *end, uint32_t instance) {
    Chunk job = {Header(IDENT_JOB, instance), static_cast<uint32_t>(end - start - sizeof(Chunk))};
    std::memcpy(start, &job, sizeof(job));
    return end;
}

ABI void *job_control(void *buffer, uint32_t instance, uint64_t flags, void *in, uint64_t in_size, void *out,
                      uint64_t out_size, void *return_address) {
    auto *p = static_cast<unsigned char *>(buffer) + sizeof(Chunk);
    if (return_address) {
        p = PutBuffer(p, IDENT_RETURN_ADDRESS, return_address, 0);
    }
    p = PutBuffer(p, IDENT_INPUT_CONTROL, in, in_size);
    flags &= (instance == STATISTICS_INSTANCE) ? UINT64_C(0xC0018007) : UINT64_C(0x60000000E7FF);
    p = PutFlags(p, IDENT_CONTROL_FLAGS, flags);
    p = PutBuffer(p, IDENT_OUTPUT_CONTROL, out, out_size);
    return FinishJob(static_cast<unsigned char *>(buffer), p, instance);
}

ABI void *job_run(void *buffer, uint32_t instance, uint64_t flags, void *in, uint64_t in_size, void *out,
                  uint64_t out_size, void *sideband, uint64_t sideband_size, void *return_address) {
    auto *p = static_cast<unsigned char *>(buffer) + sizeof(Chunk);
    if (return_address) {
        p = PutBuffer(p, IDENT_RETURN_ADDRESS, return_address, 0);
    }
    p = PutBuffer(p, IDENT_INPUT_RUN, in, in_size);
    p = PutFlags(p, IDENT_RUN_FLAGS, flags & UINT64_C(0xE00000001FFF));
    p = PutBuffer(p, IDENT_OUTPUT_RUN, out, out_size);
    p = PutBuffer(p, IDENT_OUTPUT_CONTROL, sideband, sideband_size);
    return FinishJob(static_cast<unsigned char *>(buffer), p, instance);
}

struct AjmBuffer {
    void *address;
    uint64_t size;
};

ABI void *job_run_split(void *buffer, uint32_t instance, uint64_t flags, const AjmBuffer *in, uint64_t in_count,
                        const AjmBuffer *out, uint64_t out_count, void *sideband, uint64_t sideband_size,
                        void *return_address) {
    auto *p = static_cast<unsigned char *>(buffer) + sizeof(Chunk);
    if (return_address) {
        p = PutBuffer(p, IDENT_RETURN_ADDRESS, return_address, 0);
    }
    for (uint64_t i = 0; i < in_count; ++i) {
        p = PutBuffer(p, IDENT_INPUT_RUN, in[i].address, in[i].size);
    }
    p = PutFlags(p, IDENT_RUN_FLAGS, flags & UINT64_C(0xE00000001FFF));
    for (uint64_t i = 0; i < out_count; ++i) {
        p = PutBuffer(p, IDENT_OUTPUT_RUN, out[i].address, out[i].size);
    }
    p = PutBuffer(p, IDENT_OUTPUT_CONTROL, sideband, sideband_size);
    return FinishJob(static_cast<unsigned char *>(buffer), p, instance);
}

ABI void *job_inline(void *buffer, const void *data, uint64_t size, const void **address) {
    Chunk c = {Header(IDENT_INLINE, 0), static_cast<uint32_t>((size + 7) & ~UINT64_C(7))};
    std::memcpy(buffer, &c, sizeof(c));
    auto *p = static_cast<unsigned char *>(buffer) + sizeof(c);
    std::memcpy(p, data, size);
    if (address) {
        *address = p;
    }
    return p + c.size;
}

/* ---- ATRAC9 decoding ---- */
void At9Reset(Instance *in) {
    if (in->handle) {
        Atrac9ReleaseHandle(in->handle);
    }
    in->handle = Atrac9GetHandle();
    in->initialized = (Atrac9InitDecoder(in->handle, in->config) == 0);
    if (in->initialized) {
        Atrac9GetCodecInfo(in->handle, &in->info);
    }
    in->frames = 0;
    in->superframe_remain = static_cast<uint32_t>(in->info.superframeSize);
    if (AjmTrace()) {
        std::printf(
            "Audio trace: ATRAC9 init %s: %d ch, %d Hz, frame %d samples, %d frames/superframe, superframe %d bytes, "
            "format %d, flags %#x\n",
            in->initialized ? "ok" : "FAILED", in->info.channels, in->info.samplingRate, in->info.frameSamples,
            in->info.framesInSuperframe, in->info.superframeSize, in->format, in->codec_flags);
    }
}

void GaplessReset(Instance *in) {
    in->gapless.total_samples = in->gapless_init.total_samples;
    in->gapless.skip_samples = in->gapless_init.skip_samples;
    in->gapless.skipped_samples = 0;
}

int GaplessEnd(const Instance *in) {
    return in->gapless_init.total_samples && !in->gapless.total_samples;
}

uint32_t NextFrameBytes(const Instance *in) {
    const uint32_t frame = static_cast<uint32_t>(in->info.frameSamples);
    const uint32_t skip = (in->gapless.skip_samples < frame) ? in->gapless.skip_samples : frame;
    uint32_t samples = frame - skip;
    if (in->gapless_init.total_samples && in->gapless.total_samples < samples) {
        samples = in->gapless.total_samples;
    }
    return samples * static_cast<uint32_t>(in->info.channels) * static_cast<uint32_t>(PcmSize(in->format));
}

struct Output {
    ChunkBuffer *chunks;
    int count, current;
    uint32_t offset;
};

uint64_t OutputRoom(const Output *o) {
    uint64_t room = 0;
    for (int i = o->current; i < o->count; ++i) {
        room += o->chunks[i].size - (i == o->current ? o->offset : 0);
    }
    return room;
}

void OutputWrite(Output *o, const unsigned char *data, uint64_t size) {
    while (size && o->current < o->count) {
        ChunkBuffer *c = &o->chunks[o->current];
        uint64_t n = c->size - o->offset;
        if (n > size) {
            n = size;
        }
        std::memcpy(static_cast<unsigned char *>(c->address) + o->offset, data, n);
        data += n;
        size -= n;
        o->offset += static_cast<uint32_t>(n);
        if (o->offset == c->size) {
            ++o->current;
            o->offset = 0;
        }
    }
}

/* One frame; returns result flags. */
uint32_t At9Frame(Instance *in, const unsigned char **input, uint64_t *input_size, Output *out,
                  uint32_t *samples_written, int32_t *internal) {
    const int channels = in->info.channels;
    const int frame = in->info.frameSamples;
    int used = 0;
    int16_t pcm[256 * 8];
    if (frame * channels > static_cast<int>(sizeof(pcm) / sizeof(*pcm))) {
        return RESULT_CODEC_ERROR | RESULT_FATAL;
    }
    const int e = Atrac9Decode(in->handle, *input, pcm, &used);
    if (e) {
        *internal = e;
        return RESULT_CODEC_ERROR | RESULT_FATAL;
    }
    *input += used;
    *input_size -= static_cast<uint64_t>(used);
    in->superframe_remain -= static_cast<uint32_t>(used);
    s_frames_decoded.fetch_add(1, std::memory_order_relaxed);

    uint32_t skip = 0;
    if (in->gapless.skip_samples) {
        skip = (in->gapless.skip_samples < static_cast<uint32_t>(frame)) ? in->gapless.skip_samples
                                                                        : static_cast<uint32_t>(frame);
        in->gapless.skip_samples -= static_cast<uint16_t>(skip);
    }
    uint32_t samples = static_cast<uint32_t>(frame) - skip;
    if (in->gapless_init.total_samples && samples > in->gapless.total_samples) {
        samples = in->gapless.total_samples;
    }
    const uint64_t room = OutputRoom(out) / static_cast<uint64_t>(channels * PcmSize(in->format));
    if (samples > room) {
        samples = static_cast<uint32_t>(room);
    }

    /* Convert (and optionally de-interleave) into the requested encoding. */
    unsigned char converted[256 * 8 * 4];
    const int non_interleaved = (in->codec_flags >> 8) & 1;
    for (uint32_t s = 0; s < samples; ++s) {
        for (int c = 0; c < channels; ++c) {
            const int16_t v = pcm[(skip + s) * static_cast<uint32_t>(channels) + static_cast<uint32_t>(c)];
            const size_t index = non_interleaved ? static_cast<size_t>(c) * samples + s
                                                 : static_cast<size_t>(s) * static_cast<size_t>(channels) + c;
            if (in->format == FORMAT_S16) {
                std::memcpy(converted + index * 2, &v, 2);
            } else if (in->format == FORMAT_S32) {
                const int32_t w = static_cast<int32_t>(v) << 16;
                std::memcpy(converted + index * 4, &w, 4);
            } else {
                const float f = static_cast<float>(v) / 32768.0f;
                std::memcpy(converted + index * 4, &f, 4);
            }
        }
    }
    OutputWrite(out, converted, static_cast<uint64_t>(samples) * channels * PcmSize(in->format));
    *samples_written = samples;
    in->gapless.skipped_samples += static_cast<uint16_t>(static_cast<uint32_t>(frame) - samples);
    if (in->gapless_init.total_samples) {
        in->gapless.total_samples -= samples;
    }

    if (++in->frames % static_cast<uint32_t>(in->info.framesInSuperframe) == 0) {
        const uint64_t skip_bytes =
            (in->superframe_remain < *input_size) ? in->superframe_remain : *input_size;
        *input += skip_bytes;
        *input_size -= skip_bytes;
        in->superframe_remain = static_cast<uint32_t>(in->info.superframeSize);
        in->frames = 0;
    } else if (GaplessEnd(in)) {
        /* Drain the rest of the superframe so the next stream starts aligned. */
        while (in->frames % static_cast<uint32_t>(in->info.framesInSuperframe) && *input_size) {
            if (Atrac9Decode(in->handle, *input, pcm, &used)) {
                break;
            }
            *input += used;
            *input_size -= static_cast<uint64_t>(used);
            in->superframe_remain -= static_cast<uint32_t>(used);
            ++in->frames;
        }
        const uint64_t skip_bytes =
            (in->superframe_remain < *input_size) ? in->superframe_remain : *input_size;
        *input += skip_bytes;
        *input_size -= skip_bytes;
        in->superframe_remain = static_cast<uint32_t>(in->info.superframeSize);
        in->frames = 0;
    }
    return 0;
}

void ParseRiff(Instance *in, const unsigned char **input, uint64_t *size) {
    const unsigned char *p = *input;
    const unsigned char *end = p + *size;
    p += 12;
    while (p + 8 <= end) {
        uint32_t tag = 0;
        uint32_t length = 0;
        std::memcpy(&tag, p, 4);
        std::memcpy(&length, p + 4, 4);
        p += 8;
        if (tag == 0x61746164) {
            break; /* "data" */
        }
        if (tag == 0x20746d66 && length >= 52 && p + 52 <= end) { /* "fmt " */
            std::memcpy(in->config, p + 44, 4);
            At9Reset(in);
        } else if (tag == 0x74636166 && length >= 12 && p + 12 <= end) { /* "fact" */
            uint32_t total = 0;
            uint32_t delay = 0;
            std::memcpy(&total, p, 4);
            std::memcpy(&delay, p + 4, 4);
            in->gapless_init.total_samples = total;
            in->gapless_init.skip_samples = static_cast<uint16_t>(delay);
            GaplessReset(in);
        }
        p += length;
    }
    *size -= static_cast<uint64_t>(p - *input);
    *input = p;
}

/* ---- job execution ---- */
struct Job {
    uint64_t flags;
    int have_flags;
    ChunkBuffer inputs[16];
    int input_count;
    ChunkBuffer outputs[16];
    int output_count;
    ChunkBuffer input_control, output_control;
};

int ParseJob(const unsigned char *p, uint32_t size, Job *job) {
    std::memset(job, 0, sizeof(*job));
    for (const unsigned char *end = p + size; p < end;) {
        uint32_t word = 0;
        std::memcpy(&word, p, 4);
        switch (Ident(word)) {
        case IDENT_CONTROL_FLAGS:
        case IDENT_RUN_FLAGS: {
            Chunk c{};
            std::memcpy(&c, p, 8);
            job->flags = (static_cast<uint64_t>(Payload(c.word)) << 32) | c.size;
            job->have_flags = 1;
            p += 8;
            break;
        }
        case IDENT_INPUT_RUN:
            if (job->input_count == 16) return -1;
            std::memcpy(&job->inputs[job->input_count++], p, 16);
            p += 16;
            break;
        case IDENT_OUTPUT_RUN:
            if (job->output_count == 16) return -1;
            std::memcpy(&job->outputs[job->output_count++], p, 16);
            p += 16;
            break;
        case IDENT_INPUT_CONTROL:
            std::memcpy(&job->input_control, p, 16);
            p += 16;
            break;
        case IDENT_OUTPUT_CONTROL:
            std::memcpy(&job->output_control, p, 16);
            p += 16;
            break;
        case IDENT_RETURN_ADDRESS:
            p += 16;
            break;
        default:
            return -1;
        }
    }
    return job->have_flags ? 0 : -1;
}

void RunJob(Context *ctx, uint32_t id, Job *job) {
    s_jobs_run.fetch_add(1, std::memory_order_relaxed);
    auto *side = job->output_control.size ? static_cast<unsigned char *>(job->output_control.address) : nullptr;
    const auto *side_end = side ? side + job->output_control.size : nullptr;
    SidebandResult *result = nullptr;
    if (side && side + sizeof(SidebandResult) <= side_end) {
        result = reinterpret_cast<SidebandResult *>(side);
        side += sizeof(*result);
        std::memset(result, 0, sizeof(*result));
    }
    if (id == STATISTICS_INSTANCE) {
        if (side) {
            std::memset(side, 0, static_cast<size_t>(side_end - side));
        }
        return;
    }
    Instance *in = &ctx->instances[id & MAX_INSTANCES];
    if ((id & MAX_INSTANCES) == 0 || !in->used) {
        if (result) {
            result->result = static_cast<int32_t>(RESULT_FATAL | RESULT_INVALID_PARAMETER);
        }
        return;
    }
    const uint64_t f = job->flags;
    uint32_t flags_result = 0;
    const auto *control =
        job->input_control.size ? static_cast<const unsigned char *>(job->input_control.address) : nullptr;
    const auto *control_end = control ? control + job->input_control.size : nullptr;

    if (CONTROL_RESET(f)) {
        in->total_samples = 0;
        GaplessReset(in);
        if (in->initialized) {
            At9Reset(in);
        }
    }
    if (control && SIDEBAND_FORMAT(f) && control + 24 <= control_end) {
        control += 24; /* output format fixed per instance */
    }
    if (control && SIDEBAND_GAPLESS(f) && control + 8 <= control_end) {
        SidebandGapless g{};
        std::memcpy(&g, control, 8);
        control += 8;
        const uint32_t processed = in->gapless_init.total_samples - in->gapless.total_samples;
        if (g.total_samples || !g.skip_samples) {
            if (g.total_samples >= processed) {
                const int64_t diff = static_cast<int64_t>(in->gapless_init.total_samples) - g.total_samples;
                in->gapless_init.total_samples = g.total_samples;
                in->gapless.total_samples = static_cast<uint32_t>(in->gapless.total_samples - diff);
            } else {
                flags_result |= RESULT_INVALID_PARAMETER;
            }
        }
        const uint32_t skipped = static_cast<uint32_t>(in->gapless_init.skip_samples) - in->gapless.skip_samples;
        if (g.skip_samples || !g.total_samples) {
            if (g.skip_samples >= skipped) {
                const int32_t diff = static_cast<int32_t>(in->gapless_init.skip_samples) - g.skip_samples;
                in->gapless_init.skip_samples = g.skip_samples;
                in->gapless.skip_samples = static_cast<uint16_t>(in->gapless.skip_samples - diff);
            } else {
                flags_result |= RESULT_INVALID_PARAMETER;
            }
        }
    }
    if (control && CONTROL_RESAMPLE(f) && control + 8 <= control_end) {
        control += 8;
    }
    if (control && CONTROL_INITIALIZE(f) && control + 8 <= control_end) {
        std::memcpy(in->config, control, 4);
        At9Reset(in);
    }
    uint64_t in_size = 0;
    for (int i = 0; i < job->input_count; ++i) {
        in_size += job->inputs[i].size;
    }
    unsigned char *joined = nullptr;
    const unsigned char *input = nullptr;
    if (job->input_count == 1) {
        input = static_cast<const unsigned char *>(job->inputs[0].address);
    } else if (job->input_count > 1) {
        joined = static_cast<unsigned char *>(std::malloc(in_size ? in_size : 1));
        uint64_t at = 0;
        for (int i = 0; i < job->input_count; ++i) {
            std::memcpy(joined + at, job->inputs[i].address, job->inputs[i].size);
            at += job->inputs[i].size;
        }
        input = joined;
    }

    Output out = {job->outputs, job->output_count, 0, 0};
    const uint64_t in_start = in_size;
    const uint64_t out_start = OutputRoom(&out);
    uint32_t frames = 0;
    int32_t internal = 0;

    if (in_size) {
        for (;;) {
            if (in->gapless_loop && GaplessEnd(in)) {
                GaplessReset(in);
                in->total_samples = 0;
            }
            if ((in->codec_flags & 1) && in_size >= 4 && !std::memcmp(input, "RIFF", 4)) {
                ParseRiff(in, &input, &in_size);
                in->total_samples = 0;
            }
            if (!in->initialized) {
                flags_result |= RESULT_NOT_INITIALIZED;
                break;
            }
            if (!GaplessEnd(in) && OutputRoom(&out) < NextFrameBytes(in)) {
                flags_result |= RESULT_NOT_ENOUGH_ROOM;
            }
            if (in_size < in->superframe_remain) {
                flags_result |= RESULT_PARTIAL_INPUT;
            }
            if (flags_result) {
                break;
            }
            uint32_t written = 0;
            const uint32_t r = At9Frame(in, &input, &in_size, &out, &written, &internal);
            in->total_samples += written;
            ++frames;
            if (r) {
                flags_result |= r;
                break;
            }
            if (!RUN_MULTIPLE_FRAMES(f)) {
                break;
            }
        }
    }
    std::free(joined);

    if (flags_result && AjmTrace()) {
        std::printf(
            "Audio trace: Ajm job instance %u flags %#llx -> result %#x internal %d; input %llu/%llu used, output "
            "%llu/%llu written, frames %u\n",
            id, static_cast<unsigned long long>(f), flags_result, internal,
            static_cast<unsigned long long>(in_start - in_size), static_cast<unsigned long long>(in_start),
            static_cast<unsigned long long>(out_start - OutputRoom(&out)), static_cast<unsigned long long>(out_start),
            frames);
    }
    if (result) {
        result->result = static_cast<int32_t>(flags_result);
        result->internal_result = internal;
    }

    /* Output sideband order: stream, format, gapless, multi-frame, codec info. */
    if (side && SIDEBAND_STREAM(f) && side + 16 <= side_end) {
        SidebandStream s = {static_cast<int32_t>(in_start - in_size),
                            static_cast<int32_t>(out_start - OutputRoom(&out)), in->total_samples};
        std::memcpy(side, &s, 16);
        side += 16;
    }
    if (side && SIDEBAND_FORMAT(f) && side + 24 <= side_end) {
        SidebandFormat s = {static_cast<uint32_t>(in->info.channels), ChannelMask(in->info.channels),
                            static_cast<uint32_t>(in->info.samplingRate), static_cast<uint32_t>(in->format), 0, 0};
        if (in->info.framesInSuperframe && in->info.frameSamples) {
            s.bitrate = static_cast<uint32_t>(
                static_cast<uint64_t>(in->info.samplingRate) * static_cast<uint64_t>(in->info.superframeSize) * 8 /
                (static_cast<uint64_t>(in->info.framesInSuperframe) * static_cast<uint64_t>(in->info.frameSamples)));
        }
        std::memcpy(side, &s, 24);
        side += 24;
    }
    if (side && SIDEBAND_GAPLESS(f) && side + 8 <= side_end) {
        std::memcpy(side, &in->gapless, 8);
        side += 8;
    }
    if (side && RUN_MULTIPLE_FRAMES(f) && side + 8 <= side_end) {
        uint32_t m[2] = {frames, 0};
        std::memcpy(side, m, 8);
        side += 8;
    }
    if (side && RUN_CODEC_INFO(f) && side + 16 <= side_end) {
        At9Info info = {static_cast<uint32_t>(in->info.superframeSize),
                        static_cast<uint32_t>(in->info.framesInSuperframe), in->superframe_remain,
                        static_cast<uint32_t>(in->info.frameSamples)};
        std::memcpy(side, &info, 16);
    }
}

/* ---- API ---- */
Context *GetContext(uint32_t id) {
    return (id && id <= MAX_CONTEXTS) ? s_contexts[id] : nullptr;
}

ABI int32_t ajm_initialize(int64_t reserved, uint32_t *out) {
    if (!out || reserved) {
        return ERR_INVALID_PARAMETER;
    }
    std::lock_guard<std::recursive_mutex> lock(s_lock);
    for (uint32_t i = 1; i <= MAX_CONTEXTS; ++i) {
        if (!s_contexts[i]) {
            s_contexts[i] = static_cast<Context *>(std::calloc(1, sizeof(Context)));
            if (!s_contexts[i]) {
                return ERR_OUT_OF_RESOURCES;
            }
            *out = i;
            std::puts("Runtime: Ajm context initialized (ATRAC9 via LibAtrac9)");
            return 0;
        }
    }
    return ERR_OUT_OF_RESOURCES;
}

ABI int32_t ajm_finalize(uint32_t id) {
    std::lock_guard<std::recursive_mutex> lock(s_lock);
    Context *c = GetContext(id);
    if (!c) {
        return ERR_INVALID_CONTEXT;
    }
    for (int i = 0; i <= MAX_INSTANCES; ++i) {
        if (c->instances[i].handle) {
            Atrac9ReleaseHandle(c->instances[i].handle);
        }
    }
    std::free(c);
    s_contexts[id] = nullptr;
    return 0;
}

ABI int32_t ajm_module_register(uint32_t id, uint32_t codec, int64_t reserved) {
    if (reserved || codec >= 24) {
        return ERR_INVALID_PARAMETER;
    }
    std::lock_guard<std::recursive_mutex> lock(s_lock);
    Context *c = GetContext(id);
    const int32_t r = !c ? ERR_INVALID_CONTEXT : c->registered[codec] ? ERR_ALREADY_REGISTERED : 0;
    if (!r) {
        c->registered[codec] = 1;
    }
    return r;
}

ABI int32_t ajm_module_unregister(uint32_t id, uint32_t codec) {
    std::lock_guard<std::recursive_mutex> lock(s_lock);
    Context *c = GetContext(id);
    const int32_t r = !c ? ERR_INVALID_CONTEXT
                         : (codec >= 24) ? ERR_INVALID_PARAMETER : !c->registered[codec] ? ERR_NOT_REGISTERED : 0;
    if (!r) {
        c->registered[codec] = 0;
    }
    return r;
}

ABI int32_t ajm_instance_create(uint32_t id, uint32_t codec, uint64_t flags, uint32_t *out) {
    if (!out || codec >= 24) {
        return ERR_INVALID_PARAMETER;
    }
    if (!(flags & 7)) {
        return ERR_WRONG_REVISION;
    }
    if (codec != 1) {
        std::fprintf(stderr, "STOP: Ajm codec %u (0=MP3, 2=AAC) is not implemented\n", codec);
        std::exit(21);
    }
    if (AjmTrace()) {
        std::printf("Audio trace: Ajm instance create codec %u flags %#llx\n", codec,
                    static_cast<unsigned long long>(flags));
    }
    std::lock_guard<std::recursive_mutex> lock(s_lock);
    Context *c = GetContext(id);
    int32_t r = !c ? ERR_INVALID_CONTEXT : !c->registered[codec] ? ERR_NOT_REGISTERED : ERR_OUT_OF_RESOURCES;
    if (c && c->registered[codec]) {
        for (uint32_t i = 1; i <= MAX_INSTANCES; ++i) {
            if (!c->instances[i].used) {
                Instance *in = &c->instances[i];
                std::memset(in, 0, sizeof(*in));
                in->used = 1;
                in->codec = static_cast<int>(codec);
                in->channels_hint = static_cast<int>((flags >> 3) & 15);
                in->format = static_cast<int>((flags >> 7) & 7);
                in->gapless_loop = static_cast<int>((flags >> 10) & 1);
                in->codec_flags = static_cast<uint32_t>(flags >> 32);
                if (in->format > FORMAT_FLOAT) {
                    in->used = 0;
                    r = ERR_INVALID_PARAMETER;
                    break;
                }
                *out = i | (codec << 14);
                r = 0;
                break;
            }
        }
    }
    return r;
}

ABI int32_t ajm_instance_destroy(uint32_t id, uint32_t instance) {
    std::lock_guard<std::recursive_mutex> lock(s_lock);
    Context *c = GetContext(id);
    Instance *in = c ? &c->instances[instance & MAX_INSTANCES] : nullptr;
    const int32_t r =
        !c ? ERR_INVALID_CONTEXT : !(instance & MAX_INSTANCES) || !in->used ? ERR_INVALID_INSTANCE : 0;
    if (!r) {
        if (in->handle) {
            Atrac9ReleaseHandle(in->handle);
        }
        std::memset(in, 0, sizeof(*in));
    }
    return r;
}

ABI uint32_t ajm_instance_codec(uint32_t instance) {
    return (instance >> 14) & 0x1f;
}

ABI int32_t ajm_batch_start(uint32_t id, unsigned char *buffer, uint32_t size, int priority, BatchError *error,
                            uint32_t *out) {
    (void)priority;
    if (size & 7) {
        return ERR_MALFORMED_BATCH;
    }
    std::lock_guard<std::recursive_mutex> lock(s_lock);
    Context *c = GetContext(id);
    if (!c) {
        return ERR_INVALID_CONTEXT;
    }
    int slot = -1;
    for (int i = 0; i < MAX_BATCHES; ++i) {
        if (!s_batches[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return ERR_OUT_OF_RESOURCES;
    }
    for (unsigned char *p = buffer, *end = buffer + size; p < end;) {
        Chunk chunk{};
        std::memcpy(&chunk, p, 8);
        unsigned char *body = p + 8;
        p = body + chunk.size;
        if (Ident(chunk.word) == IDENT_INLINE) {
            continue;
        }
        Job job{};
        if (Ident(chunk.word) != IDENT_JOB || p > end || ParseJob(body, chunk.size, &job)) {
            if (error) {
                error->error_code = ERR_MALFORMED_BATCH;
                error->job_address = body - 8;
                error->command_offset = static_cast<uint32_t>(body - 8 - buffer);
                error->job_return_address = nullptr;
            }
            return ERR_MALFORMED_BATCH;
        }
        RunJob(c, Payload(chunk.word), &job);
    }
    s_batches[slot] = Batch{1, static_cast<int>(id), 0};
    s_batches_run.fetch_add(1, std::memory_order_relaxed);
    *out = static_cast<uint32_t>(slot + 1);
    return 0;
}

ABI int32_t ajm_batch_wait(uint32_t id, uint32_t batch, uint32_t timeout, BatchError *error) {
    (void)timeout;
    (void)error;
    std::lock_guard<std::recursive_mutex> lock(s_lock);
    int32_t r = !GetContext(id)                                             ? ERR_INVALID_CONTEXT
                : (!batch || batch > MAX_BATCHES || !s_batches[batch - 1].used) ? ERR_INVALID_BATCH
                                                                               : 0;
    if (!r) {
        if (s_batches[batch - 1].canceled) {
            r = ERR_CANCELLED;
        }
        s_batches[batch - 1].used = 0;
    }
    return r;
}

ABI int32_t ajm_batch_cancel(uint32_t id, uint32_t batch) {
    std::lock_guard<std::recursive_mutex> lock(s_lock);
    const int32_t r = !GetContext(id)                                             ? ERR_INVALID_CONTEXT
                      : (!batch || batch > MAX_BATCHES || !s_batches[batch - 1].used) ? ERR_INVALID_BATCH
                                                                                     : 0;
    return r; /* already processed at submission: nothing to cancel */
}

ABI int32_t ajm_error_dump(void) {
    return 0;
}

ABI int32_t ajm_memory_register(uint32_t id, void *p, uint64_t pages) {
    (void)p;
    (void)pages;
    return GetContext(id) ? 0 : ERR_INVALID_CONTEXT;
}

const RuntimeExport ajm_exports[] = {
    {"sceAjmInitialize", reinterpret_cast<void *>(ajm_initialize)},
    {"sceAjmFinalize", reinterpret_cast<void *>(ajm_finalize)},
    {"sceAjmModuleRegister", reinterpret_cast<void *>(ajm_module_register)},
    {"sceAjmModuleUnregister", reinterpret_cast<void *>(ajm_module_unregister)},
    {"sceAjmInstanceCreate", reinterpret_cast<void *>(ajm_instance_create)},
    {"sceAjmInstanceDestroy", reinterpret_cast<void *>(ajm_instance_destroy)},
    {"sceAjmInstanceCodecType", reinterpret_cast<void *>(ajm_instance_codec)},
    {"sceAjmBatchJobControlBufferRa", reinterpret_cast<void *>(job_control)},
    {"sceAjmBatchJobRunBufferRa", reinterpret_cast<void *>(job_run)},
    {"sceAjmBatchJobRunSplitBufferRa", reinterpret_cast<void *>(job_run_split)},
    {"sceAjmBatchJobInlineBuffer", reinterpret_cast<void *>(job_inline)},
    {"sceAjmBatchStartBuffer", reinterpret_cast<void *>(ajm_batch_start)},
    {"sceAjmBatchWait", reinterpret_cast<void *>(ajm_batch_wait)},
    {"sceAjmBatchCancel", reinterpret_cast<void *>(ajm_batch_cancel)},
    {"sceAjmBatchErrorDump", reinterpret_cast<void *>(ajm_error_dump)},
    {"sceAjmMemoryRegister", reinterpret_cast<void *>(ajm_memory_register)},
};

} // namespace

extern "C" {

void runtime_ajm_register(void) {
    runtime_register_exports(ajm_exports, sizeof(ajm_exports) / sizeof(ajm_exports[0]));
}

uintptr_t runtime_ajm_resolve(const char *name) {
    return runtime_registry_resolve(name, EXPORT_FUNCTION);
}

void runtime_ajm_report(void) {
    std::printf("Runtime: Ajm batches=%zu, jobs=%zu, ATRAC9 frames decoded=%zu\n",
                s_batches_run.load(std::memory_order_relaxed),
                s_jobs_run.load(std::memory_order_relaxed),
                s_frames_decoded.load(std::memory_order_relaxed));
}

} // extern "C"
