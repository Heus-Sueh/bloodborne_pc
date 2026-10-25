#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "runtime_toypad.hpp"
#include "runtime.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>

#define USBD_ERROR_NO_DEVICE ((int32_t)0x80240004)
#define USBD_ERROR_INVALID_PARAM ((int32_t)0x80240002)

namespace {

enum { VENDOR = 0x0E6F, PRODUCT = 0x0241, ENDPOINT_IN = 0x81, ENDPOINT_OUT = 0x01, REPORT = 32 };
enum { TRANSFER_COMPLETED = 0, TRANSFER_TIMED_OUT = 2, TRANSFER_CANCELLED = 3 };

/* libusb 1.0's public structures, as the PS4's library lays them out. */
struct UsbTransfer;
using TransferCallback = ABI void (*)(UsbTransfer *);
struct UsbTransfer {
    void *handle;
    uint8_t flags, endpoint, type;
    uint32_t timeout;
    int32_t status, length, actual_length;
    TransferCallback callback;
    void *user_data;
    uint8_t *buffer;
    int32_t num_iso_packets;
};
static_assert(offsetof(UsbTransfer, callback) == 32, "libusb_transfer layout");
static_assert(offsetof(UsbTransfer, buffer) == 48, "libusb_transfer layout");

struct EndpointDescriptor {
    uint8_t length, type, endpoint, attributes;
    uint16_t max_packet;
    uint8_t interval, refresh, synch_address;
    const uint8_t *extra;
    int32_t extra_length;
};
struct InterfaceDescriptor {
    uint8_t length, type, number, alternate, num_endpoints, class_, subclass, protocol, name;
    const EndpointDescriptor *endpoint;
    const uint8_t *extra;
    int32_t extra_length;
};
struct Interface {
    const InterfaceDescriptor *altsetting;
    int32_t num_altsetting;
};
struct ConfigDescriptor {
    uint8_t length, type;
    uint16_t total_length;
    uint8_t num_interfaces, value, name, attributes, max_power;
    const Interface *interface;
    const uint8_t *extra;
    int32_t extra_length;
};
struct DeviceDescriptor {
    uint8_t length, type;
    uint16_t usb;
    uint8_t class_, subclass, protocol, max_packet0;
    uint16_t vendor, product, device;
    uint8_t manufacturer, product_name, serial, num_configurations;
};
static_assert(sizeof(DeviceDescriptor) == 18, "libusb_device_descriptor layout");
static_assert(offsetof(ConfigDescriptor, interface) == 16, "libusb_config_descriptor layout");
static_assert(offsetof(InterfaceDescriptor, endpoint) == 16, "libusb_interface_descriptor layout");

const EndpointDescriptor kEndpoints[2] = {
    {7, 5, ENDPOINT_IN, 3, REPORT, 1, 0, 0, nullptr, 0},
    {7, 5, ENDPOINT_OUT, 3, REPORT, 1, 0, 0, nullptr, 0},
};
const InterfaceDescriptor kInterfaceDescriptor = {9, 4, 0, 0, 2, 3, 0, 0, 0, kEndpoints, nullptr, 0};
const Interface kInterfaces[1] = {{&kInterfaceDescriptor, 1}};
const ConfigDescriptor kConfig = {9, 2, 0x29, 1, 1, 0, 0x80, 0xFA, kInterfaces, nullptr, 0};

/* The device, its handle and the list holding it: objects of our own. */
struct UsbObject {
    uint32_t magic;
};
UsbObject s_device = {0x544f5950};
UsbObject s_handle = {0x48414e44};
void *s_device_list[2] = {&s_device, nullptr};

/* ---- The pad's protocol ---------------------------------------------------------------------- */

const uint32_t kCommandKey[4] = {0xB0F6FE55, 0x410BBF62, 0xB47CB3C9, 0x7B293E97};

void TeaEncrypt(uint8_t data[8]) {
    uint32_t a = 0;
    uint32_t b = 0;
    uint32_t sum = 0;
    std::memcpy(&a, data, 4);
    std::memcpy(&b, data + 4, 4);
    for (int i = 0; i < 32; ++i) {
        sum += 0x9E3779B9;
        a += ((b << 4) + kCommandKey[0]) ^ (b + sum) ^ ((b >> 5) + kCommandKey[1]);
        b += ((a << 4) + kCommandKey[2]) ^ (a + sum) ^ ((a >> 5) + kCommandKey[3]);
    }
    std::memcpy(data, &a, 4);
    std::memcpy(data + 4, &b, 4);
}

void TeaDecrypt(uint8_t data[8]) {
    uint32_t a = 0;
    uint32_t b = 0;
    uint32_t sum = 0xC6EF3720;
    std::memcpy(&a, data, 4);
    std::memcpy(&b, data + 4, 4);
    for (int i = 0; i < 32; ++i) {
        b -= ((a << 4) + kCommandKey[2]) ^ (a + sum) ^ ((a >> 5) + kCommandKey[3]);
        a -= ((b << 4) + kCommandKey[0]) ^ (b + sum) ^ ((b >> 5) + kCommandKey[1]);
        sum -= 0x9E3779B9;
    }
    std::memcpy(data, &a, 4);
    std::memcpy(data + 4, &b, 4);
}

uint32_t Rotl32(uint32_t v, int n) {
    return (v << n) | (v >> (32 - n));
}

/* The pad's random generator (seeded by the game through command 0xB1). */
uint32_t s_rng[4]{};
uint32_t RngNext() {
    const uint32_t e = s_rng[0] - Rotl32(s_rng[1], 21);
    s_rng[0] = s_rng[1] ^ Rotl32(s_rng[2], 19);
    s_rng[1] = s_rng[2] + Rotl32(s_rng[3], 6);
    s_rng[2] = s_rng[3] + e;
    s_rng[3] = e + s_rng[0];
    return s_rng[3];
}
void RngSeed(uint32_t seed) {
    s_rng[0] = 0xF1EA5EED;
    s_rng[1] = s_rng[2] = s_rng[3] = seed;
    for (int i = 0; i < 42; ++i) {
        RngNext();
    }
}

uint8_t Checksum(const uint8_t *data, int count) {
    unsigned sum = 0;
    for (int i = 0; i < count; ++i) {
        sum += data[i];
    }
    return static_cast<uint8_t>(sum);
}

/* ---- Figures ----------------------------------------------------------------------------- */

enum { PAGES = 45, SLOTS = 7 };
struct Figure {
    int present;
    uint8_t zone, index; ///< zone 1..3, the pad's reader index 1..7
    uint32_t id, copy;
    uint8_t data[PAGES * 4];
};
Figure s_figures[SLOTS]{};

uint32_t Rotr32(uint32_t v, int n) {
    return (v >> n) | (v << (32 - n));
}

/* The tags' hash over little-endian words of a key. */
uint32_t TagHash(const uint8_t *key, int words) {
    uint32_t h = 0;
    for (int i = 0; i < words; ++i) {
        uint32_t w = 0;
        std::memcpy(&w, key + i * 4, 4);
        h = w + Rotr32(h, 25) + Rotr32(h, 10) - h;
    }
    return h;
}

void TagUid(const uint8_t *data, uint8_t uid[7]) {
    uid[0] = data[0];
    uid[1] = data[1];
    uid[2] = data[2];
    std::memcpy(uid + 3, data + 4, 4);
}

/* A character's key: four hashes of the UID followed by the tags' constant. */
void FigureKey(const uint8_t *data, uint32_t key[4]) {
    static const uint8_t constant[17] = {0xB7, 0xD5, 0xD7, 0xE6, 0xE7, 0xBA, 0x3C, 0xA8, 0xD8,
                                         0x75, 0x47, 0x68, 0xCF, 0x23, 0xE9, 0xFE, 0xAA};
    for (int n = 3; n <= 6; ++n) {
        uint8_t buffer[24];
        TagUid(data, buffer);
        std::memcpy(buffer + 7, constant, sizeof(constant));
        buffer[n * 4 - 1] = 0xAA;
        key[n - 3] = TagHash(buffer, n);
    }
}

/* Builds a tag: a UID made from the figure id and copy, the id encrypted into pages 36 and 37
 * for characters (written in plain for vehicles and gadgets, ids from 1000), the password. */
void FigureCreate(Figure *f) {
    std::memset(f->data, 0, sizeof(f->data));
    const uint32_t h = f->id * 2654435761u ^ (f->copy + 1) * 40503u;
    const uint8_t uid[7] = {0x04, static_cast<uint8_t>(h), static_cast<uint8_t>(h >> 8),
                            static_cast<uint8_t>(h >> 16), static_cast<uint8_t>(h >> 24),
                            static_cast<uint8_t>(f->copy * 37 + 11), 0x80};
    f->data[0] = uid[0];
    f->data[1] = uid[1];
    f->data[2] = uid[2];
    f->data[3] = static_cast<uint8_t>(0x88 ^ uid[0] ^ uid[1] ^ uid[2]);
    std::memcpy(f->data + 4, uid + 3, 4);
    f->data[8] = static_cast<uint8_t>(uid[3] ^ uid[4] ^ uid[5] ^ uid[6]);
    if (f->id < 1000) {
        uint32_t key[4];
        FigureKey(f->data, key);
        uint8_t value[8];
        std::memcpy(value, &f->id, 4);
        std::memcpy(value + 4, &f->id, 4);
        uint32_t a = 0;
        uint32_t b = 0;
        uint32_t sum = 0;
        std::memcpy(&a, value, 4);
        std::memcpy(&b, value + 4, 4);
        for (int i = 0; i < 32; ++i) {
            sum += 0x9E3779B9;
            a += ((b << 4) + key[0]) ^ (b + sum) ^ ((b >> 5) + key[1]);
            b += ((a << 4) + key[2]) ^ (a + sum) ^ ((a >> 5) + key[3]);
        }
        std::memcpy(f->data + 36 * 4, &a, 4);
        std::memcpy(f->data + 37 * 4, &b, 4);
    } else {
        std::memcpy(f->data + 36 * 4, &f->id, 4);
        f->data[38 * 4 + 1] = 1;
    }
    static const char copyright[] = "(c) Copyright LEGO 2014";
    uint8_t buffer[32];
    std::memcpy(buffer, uid, 7);
    std::memcpy(buffer + 7, copyright, 23);
    buffer[30] = buffer[31] = 0xAA;
    const uint32_t password = TagHash(buffer, 8);
    std::memcpy(f->data + 43 * 4, &password, 4);
}

void FigurePath(const Figure *f, char *path, size_t size) {
    std::snprintf(path, size, "%s/toypad/%u-%u.bin", runtime_file_user_dir(), f->id, f->copy);
}

void FigureSave(const Figure *f) {
    char path[512];
    std::snprintf(path, sizeof(path), "%s/toypad", runtime_file_user_dir());
    mkdir(path, 0755);
    FigurePath(f, path, sizeof(path));
    FILE *file = std::fopen(path, "wb");
    if (!file) {
        std::printf("Toy Pad: cannot write %s\n", path);
        return;
    }
    std::fwrite(f->data, 1, sizeof(f->data), file);
    std::fclose(file);
}

void FigureLoad(Figure *f) {
    char path[512];
    FigurePath(f, path, sizeof(path));
    FILE *file = std::fopen(path, "rb");
    const int loaded = file && std::fread(f->data, 1, sizeof(f->data), file) == sizeof(f->data);
    if (file) {
        std::fclose(file);
    }
    if (!loaded) {
        FigureCreate(f);
        FigureSave(f);
    }
}

/* Replies and figure reports waiting for an IN transfer. */
pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t s_changed = PTHREAD_COND_INITIALIZER;
enum { QUEUE = 64 };
uint8_t s_replies[QUEUE][REPORT]{};
unsigned s_reply_head = 0;
unsigned s_reply_count = 0;
size_t s_commands = 0;
size_t s_unknown_commands = 0;

/* Figure reports, sent once the game has woken the pad (after its first challenge). */
uint8_t s_events[QUEUE][REPORT]{};
unsigned s_event_head = 0;
unsigned s_event_count = 0;
int s_awake = 0;

void QueueEvent(const Figure *f, int removed) {
    if (s_event_count == QUEUE) {
        return;
    }
    uint8_t *e = s_events[(s_event_head + s_event_count++) % QUEUE];
    std::memset(e, 0, REPORT);
    const uint8_t report[13] = {0x56,
                                0x0b,
                                f->zone,
                                0,
                                f->index,
                                static_cast<uint8_t>(removed),
                                f->data[0],
                                f->data[1],
                                f->data[2],
                                f->data[4],
                                f->data[5],
                                f->data[6],
                                f->data[7]};
    std::memcpy(e, report, sizeof(report));
    e[13] = Checksum(e, 13);
}

Figure *FigureAtIndex(uint8_t index) {
    return (index >= 1 && index <= SLOTS && s_figures[index - 1].present) ? &s_figures[index - 1] : nullptr;
}

void QueueReply(const uint8_t reply[REPORT]) {
    if (s_reply_count == QUEUE) {
        return; /* the game stopped reading: dropped */
    }
    std::memcpy(s_replies[(s_reply_head + s_reply_count++) % QUEUE], reply, REPORT);
}

/* The three zones' lights as the game last set them (0xRRGGBB) */
uint32_t s_lights[3]{};
uint32_t Rgb(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) << 16 | static_cast<uint32_t>(p[1]) << 8 | p[2];
}

void SetZone(unsigned zone, uint32_t colour) {
    if (zone == 0) {
        s_lights[0] = s_lights[1] = s_lights[2] = colour;
    } else if (zone <= 3) {
        s_lights[zone - 1] = colour;
    }
}

void SetLights(uint8_t command, const uint8_t *p) {
    switch (command) {
    case 0xC0:
        SetZone(p[0], Rgb(p + 1));
        break;
    case 0xC2:
        SetZone(p[0], Rgb(p + 3));
        break;
    case 0xC3:
        SetZone(p[0], Rgb(p + 4));
        break;
    case 0xC6:
        for (int z = 0; z < 3; ++z) {
            if (p[z * 6]) s_lights[z] = Rgb(p + z * 6 + 3);
        }
        break;
    case 0xC7:
        for (int z = 0; z < 3; ++z) {
            if (p[z * 7]) s_lights[z] = Rgb(p + z * 7 + 4);
        }
        break;
    case 0xC8:
        for (int z = 0; z < 3; ++z) {
            if (p[z * 4]) s_lights[z] = Rgb(p + z * 4 + 1);
        }
        break;
    default:
        break;
    }
}

/* One request from the game (an OUT report), answered into the reply queue. */
void HandleRequest(const uint8_t *request) {
    if (request[0] != 0x55) {
        return;
    }
    const uint8_t command = request[2];
    const uint8_t sequence = request[3];
    uint8_t reply[REPORT] = {0x55};
    uint8_t value[8];
    ++s_commands;

    switch (command) {
    case 0xB0: { /* wake: the pad's banner, "(c) LEGO 2014" */
        static const uint8_t banner[] = {0x55, 0x0e, 0x01, 0x28, 0x63, 0x29, 0x20, 0x4c, 0x45,
                                         0x47, 0x4f, 0x20, 0x32, 0x30, 0x31, 0x34, 0x46};
        std::memcpy(reply, banner, sizeof(banner));
        break;
    }
    case 0xB1: { /* seed: the generator's seed and a confirmation, echoed back encrypted */
        std::memcpy(value, request + 4, 8);
        TeaDecrypt(value);
        uint32_t seed = 0;
        std::memcpy(&seed, value, 4);
        RngSeed(seed);
        uint8_t answer[8] = {value[4], value[5], value[6], value[7], 0, 0, 0, 0};
        TeaEncrypt(answer);
        reply[1] = 0x09;
        reply[2] = sequence;
        std::memcpy(reply + 3, answer, 8);
        reply[11] = Checksum(reply, 11);
        break;
    }
    case 0xB3: { /* challenge: the generator's next number and the confirmation */
        std::memcpy(value, request + 4, 8);
        TeaDecrypt(value);
        const uint32_t next = RngNext();
        uint8_t answer[8];
        std::memcpy(answer, &next, 4);
        std::memcpy(answer + 4, value, 4);
        TeaEncrypt(answer);
        reply[1] = 0x09;
        reply[2] = sequence;
        std::memcpy(reply + 3, answer, 8);
        reply[11] = Checksum(reply, 11);
        s_awake = 1;
        break;
    }
    case 0xC0:
    case 0xC1:
    case 0xC2:
    case 0xC3:
    case 0xC4:
    case 0xC6:
    case 0xC7:
    case 0xC8:
        /* lights: their colours kept for the menu */
        SetLights(command, request + 4);
        reply[1] = 0x01;
        reply[2] = sequence;
        reply[3] = Checksum(reply, 3);
        break;
    case 0xD2: { /* read 4 pages of the figure at index [4] from page [5] */
        const Figure *f = FigureAtIndex(request[4]);
        reply[1] = 0x12;
        reply[2] = sequence;
        reply[3] = 0;
        for (int i = 0; f && i < 16; ++i) {
            if (request[5] * 4 + i < PAGES * 4) {
                reply[4 + i] = f->data[request[5] * 4 + i];
            }
        }
        reply[20] = Checksum(reply, 20);
        break;
    }
    case 0xD3: { /* write page [5] of the figure at index [4] */
        Figure *f = FigureAtIndex(request[4]);
        if (f && request[5] < PAGES) {
            std::memcpy(f->data + request[5] * 4, request + 6, 4);
            FigureSave(f);
        }
        reply[1] = 0x02;
        reply[2] = sequence;
        reply[3] = 0;
        reply[4] = Checksum(reply, 4);
        break;
    }
    case 0xD4: { /* the model (id) of the figure at the encrypted index, and the confirmation */
        std::memcpy(value, request + 4, 8);
        TeaDecrypt(value);
        const Figure *f = FigureAtIndex(value[0]);
        uint32_t id = 0;
        if (f && f->id < 1000) {
            id = f->id;
        } else if (f) {
            std::memcpy(&id, f->data + 36 * 4, 4);
        }
        uint8_t answer[8] = {static_cast<uint8_t>(id), static_cast<uint8_t>(id >> 8),
                             static_cast<uint8_t>(id >> 16), static_cast<uint8_t>(id >> 24),
                             value[4], value[5], value[6], value[7]};
        if (!f) {
            std::memset(answer, 0, 4);
        }
        TeaEncrypt(answer);
        reply[1] = 0x0a;
        reply[2] = sequence;
        reply[3] = 0;
        std::memcpy(reply + 4, answer, 8);
        reply[12] = Checksum(reply, 12);
        break;
    }
    default: /* tag list, passwords, activity, LED queries: an empty acknowledgement */
        ++s_unknown_commands;
        if (s_unknown_commands <= 8) {
            std::printf("Toy Pad: command %#x answered empty\n", command);
        }
        reply[1] = 0x01;
        reply[2] = sequence;
        reply[3] = Checksum(reply, 3);
        break;
    }
    QueueReply(reply);
}

void Trace(const char *what, long a, long b);

/* ---- Transfers: OUT completes at once, IN when a report is there --------------------------- */

struct Pending {
    UsbTransfer *transfer;
    uint64_t deadline_ns;
};
enum { MAX_PENDING = 32 };
Pending s_pending[MAX_PENDING]{};
unsigned s_num_pending = 0;
UsbTransfer *s_completed[MAX_PENDING * 2]{};
unsigned s_num_completed = 0;

uint64_t NowNs() {
    struct timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return static_cast<uint64_t>(t.tv_sec) * 1000000000 + static_cast<uint64_t>(t.tv_nsec);
}

/* Under the lock: IN transfers that can complete, and those whose time is up. */
void SettleLocked() {
    const uint64_t now = NowNs();
    for (unsigned i = 0; i < s_num_pending;) {
        UsbTransfer *t = s_pending[i].transfer;
        int done = 0;
        if (s_reply_count || (s_awake && s_event_count)) {
            const int32_t n = t->length < REPORT ? t->length : REPORT;
            if (s_reply_count) {
                std::memcpy(t->buffer, s_replies[s_reply_head], static_cast<size_t>(n));
                s_reply_head = (s_reply_head + 1) % QUEUE;
                --s_reply_count;
            } else {
                std::memcpy(t->buffer, s_events[s_event_head], static_cast<size_t>(n));
                s_event_head = (s_event_head + 1) % QUEUE;
                --s_event_count;
            }
            t->actual_length = n;
            t->status = TRANSFER_COMPLETED;
            done = 1;
        } else if (s_pending[i].deadline_ns && now >= s_pending[i].deadline_ns) {
            t->actual_length = 0;
            t->status = TRANSFER_TIMED_OUT;
            done = 1;
        }
        if (done) {
            if (s_num_completed < sizeof(s_completed) / sizeof(*s_completed)) {
                s_completed[s_num_completed++] = t;
            }
            s_pending[i] = s_pending[--s_num_pending];
        } else {
            ++i;
        }
    }
}

ABI int32_t submit_transfer(UsbTransfer *t) {
    Trace(t && t->endpoint == ENDPOINT_OUT ? "submit OUT" : "submit IN", t ? static_cast<long>(t->endpoint) : 0,
          t && t->buffer && t->endpoint == ENDPOINT_OUT ? static_cast<long>(t->buffer[2]) : -1);
    if (!t || t->handle != &s_handle) {
        return USBD_ERROR_NO_DEVICE;
    }
    pthread_mutex_lock(&s_lock);
    if (t->endpoint == ENDPOINT_OUT) {
        if (t->buffer && t->length >= 4) {
            HandleRequest(t->buffer);
        }
        t->actual_length = t->length;
        t->status = TRANSFER_COMPLETED;
        if (s_num_completed < sizeof(s_completed) / sizeof(*s_completed)) {
            s_completed[s_num_completed++] = t;
        }
    } else {
        if (s_num_pending == MAX_PENDING) {
            pthread_mutex_unlock(&s_lock);
            return USBD_ERROR_INVALID_PARAM;
        }
        s_pending[s_num_pending++] = Pending{t, t->timeout ? NowNs() + static_cast<uint64_t>(t->timeout) * 1000000 : 0};
    }
    SettleLocked();
    pthread_cond_broadcast(&s_changed);
    pthread_mutex_unlock(&s_lock);
    return 0;
}

/* ---- The figures file --------------------------------------------------------------------- */

const char *FiguresFile() {
    static char path[512]{};
    if (!path[0]) {
        const char *env = std::getenv("BB_TOYPAD_FILE");
        if (env && *env) {
            std::snprintf(path, sizeof(path), "%s", env);
        } else {
            std::snprintf(path, sizeof(path), "%s/toypad.txt", runtime_file_user_dir());
        }
    }
    return path;
}

void SyncFiguresLocked() {
    static uint64_t last_check = 0;
    static struct timespec last_mtime{};
    static int had_file = -1;
    const uint64_t now = NowNs();
    if (now - last_check < 300000000ull) {
        return;
    }
    last_check = now;
    struct stat st{};
    const int has_file = stat(FiguresFile(), &st) == 0;
    if (has_file == had_file && (!has_file || (st.st_mtim.tv_sec == last_mtime.tv_sec &&
                                               st.st_mtim.tv_nsec == last_mtime.tv_nsec))) {
        return;
    }
    if (had_file < 0 && !has_file) {
        std::printf("Toy Pad: no figures file (%s)\n", FiguresFile());
    }
    had_file = has_file;
    if (has_file) {
        last_mtime = st.st_mtim;
    }
    struct Wanted {
        uint8_t zone;
        uint32_t id, copy;
    };
    Wanted wanted[SLOTS];
    unsigned num_wanted = 0;
    unsigned per_zone[4] = {0};
    FILE *file = has_file ? std::fopen(FiguresFile(), "r") : nullptr;
    char line[256];
    while (file && std::fgets(line, sizeof(line), file)) {
        char *hash = std::strchr(line, '#');
        if (hash) {
            *hash = 0;
        }
        unsigned zone = 0;
        unsigned id = 0;
        if (std::sscanf(line, "%u %u", &zone, &id) != 2) {
            continue;
        }
        const unsigned capacity = (zone == 1) ? 1 : 3;
        if (zone < 1 || zone > 3 || !id || per_zone[zone] == capacity || num_wanted == SLOTS) {
            std::printf("Toy Pad: figure %u in zone %u does not fit (zone 1 holds 1, zones 2 and 3 hold 3)\n", id,
                        zone);
            continue;
        }
        ++per_zone[zone];
        uint32_t copy = 0;
        for (unsigned i = 0; i < num_wanted; ++i) {
            if (wanted[i].id == id) {
                ++copy;
            }
        }
        wanted[num_wanted++] = Wanted{static_cast<uint8_t>(zone), id, copy};
    }
    if (file) {
        std::fclose(file);
    }
    int kept[SLOTS] = {0};
    for (unsigned s = 0; s < SLOTS; ++s) {
        Figure *f = &s_figures[s];
        if (!f->present) {
            continue;
        }
        int found = 0;
        for (unsigned i = 0; i < num_wanted && !found; ++i) {
            if (!kept[i] && wanted[i].zone == f->zone && wanted[i].id == f->id && wanted[i].copy == f->copy) {
                kept[i] = found = 1;
            }
        }
        if (!found) {
            QueueEvent(f, 1);
            std::printf("Toy Pad: figure %u lifted from zone %u\n", f->id, f->zone);
            f->present = 0;
        }
    }
    for (unsigned i = 0; i < num_wanted; ++i) {
        if (kept[i]) {
            continue;
        }
        for (unsigned s = 0; s < SLOTS; ++s) {
            Figure *f = &s_figures[s];
            if (f->present) {
                continue;
            }
            f->present = 1;
            f->zone = wanted[i].zone;
            f->index = static_cast<uint8_t>(s + 1);
            f->id = wanted[i].id;
            f->copy = wanted[i].copy;
            FigureLoad(f);
            QueueEvent(f, 0);
            std::printf("Toy Pad: figure %u placed on zone %u (reader index %u)\n", f->id, f->zone, f->index);
            break;
        }
    }
}

/* The game's event loop: up to 10 ms for something to complete, then its callbacks. */
ABI int32_t handle_events(void) {
    UsbTransfer *done[MAX_PENDING * 2];
    unsigned count = 0;
    pthread_mutex_lock(&s_lock);
    SyncFiguresLocked();
    SettleLocked();
    if (!s_num_completed) {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_nsec += 10000000;
        if (deadline.tv_nsec >= 1000000000) {
            deadline.tv_nsec -= 1000000000;
            ++deadline.tv_sec;
        }
        pthread_cond_timedwait(&s_changed, &s_lock, &deadline);
        SettleLocked();
    }
    count = s_num_completed;
    std::memcpy(done, s_completed, count * sizeof(*done));
    s_num_completed = 0;
    pthread_mutex_unlock(&s_lock);
    for (unsigned i = 0; i < count; ++i) {
        if (done[i]->callback) {
            done[i]->callback(done[i]);
        }
    }
    return 0;
}

/* ---- sceUsbd -------------------------------------------------------------------------------- */

void Trace(const char *what, long a, long b) {
    static int on = -1;
    static int counts[64]{};
    if (on < 0) {
        const char *env = std::getenv("BB_TOYPAD_TRACE");
        on = (env && env[0] == '1') ? 1 : 0;
    }
    if (!on) {
        return;
    }
    unsigned h = 0;
    for (const char *c = what; *c; ++c) {
        h = h * 31 + static_cast<unsigned char>(*c);
    }
    if (counts[h % 64]++ < 20) {
        std::printf("Toy Pad trace: %s(%#lx, %#lx)\n", what, a, b);
    }
}

int IsEnabled() {
    static int on = -1;
    if (on < 0) {
        const char *env = std::getenv("BB_TOYPAD");
        on = !(env && env[0] == '0');
    }
    return on;
}

ABI int32_t usbd_init(void) {
    if (IsEnabled()) {
        setenv("BB_TOYPAD_FILE", FiguresFile(), 0); /* the menu's Toy Pad section edits it */
        std::printf("Toy Pad: connected (emulated; figures from %s)\n", FiguresFile());
    } else {
        std::printf("Toy Pad: not connected (BB_TOYPAD=0)\n");
    }
    return 0;
}

ABI void nothing(void) {}

ABI int64_t get_device_list(void ***list) {
    Trace("GetDeviceList", 0, 0);
    static void *none[1]{};
    if (!list) {
        return USBD_ERROR_INVALID_PARAM;
    }
    *list = IsEnabled() ? s_device_list : none;
    return IsEnabled() ? 1 : 0;
}

ABI int32_t get_device_descriptor(void *dev, DeviceDescriptor *out) {
    Trace("GetDeviceDescriptor", static_cast<long>(reinterpret_cast<uintptr_t>(dev)), 0);
    if (dev != &s_device || !out) {
        return USBD_ERROR_NO_DEVICE;
    }
    *out = DeviceDescriptor{18, 1, 0x0200, 0, 0, 0, REPORT, VENDOR, PRODUCT, 0x0200, 1, 2, 0, 1};
    return 0;
}

ABI int32_t get_config_descriptor(void *dev, uint8_t index, const ConfigDescriptor **out) {
    if (dev != &s_device || index != 0 || !out) {
        return USBD_ERROR_NO_DEVICE;
    }
    *out = &kConfig;
    return 0;
}

ABI void *open_vid_pid(uint16_t vendor, uint16_t product) {
    Trace("OpenDeviceWithVidPid", vendor, product);
    return (IsEnabled() && vendor == VENDOR && product == PRODUCT) ? &s_handle : nullptr;
}

ABI int32_t device_ok(void *h) {
    Trace("ResetDevice", static_cast<long>(reinterpret_cast<uintptr_t>(h)),
          static_cast<long>(reinterpret_cast<uintptr_t>(__builtin_return_address(0)) - 0x800000000ull));
    return (h == &s_handle || h == &s_device) ? 0 : USBD_ERROR_NO_DEVICE;
}

ABI int32_t connected(void *h) {
    Trace("CheckConnected", static_cast<long>(reinterpret_cast<uintptr_t>(h)), 0);
    return (h == &s_handle) ? 0 : USBD_ERROR_NO_DEVICE;
}

ABI int32_t driver_active(void *h, int32_t iface) {
    Trace("KernelDriverActive", static_cast<long>(reinterpret_cast<uintptr_t>(h)), iface);
    return (h == &s_handle) ? 1 : USBD_ERROR_NO_DEVICE;
}

ABI int32_t claim(void *h, int32_t iface) {
    (void)iface;
    return (h == &s_handle) ? 0 : USBD_ERROR_NO_DEVICE;
}

ABI int32_t bus_number(void *dev) {
    return (dev == &s_device) ? 1 : USBD_ERROR_NO_DEVICE;
}

ABI int32_t max_packet(void *dev, uint8_t endpoint) {
    (void)endpoint;
    return (dev == &s_device) ? REPORT : USBD_ERROR_NO_DEVICE;
}

ABI UsbTransfer *alloc_transfer(int32_t iso_packets) {
    return static_cast<UsbTransfer *>(std::calloc(
        1, sizeof(UsbTransfer) + 16 + static_cast<size_t>(iso_packets > 0 ? iso_packets : 0) * 12));
}

ABI void fill_interrupt(UsbTransfer *t, void *h, uint8_t endpoint, uint8_t *buffer, int32_t length,
                               TransferCallback callback, void *user_data, uint32_t timeout) {
    if (!t) {
        return;
    }
    t->handle = h;
    t->endpoint = endpoint;
    t->type = 3; /* interrupt */
    t->timeout = timeout;
    t->buffer = buffer;
    t->length = length;
    t->callback = callback;
    t->user_data = user_data;
}

const RuntimeExport toypad_exports[] = {
    {"sceUsbdInit", reinterpret_cast<void *>(usbd_init)},
    {"sceUsbdExit", reinterpret_cast<void *>(nothing)},
    {"sceUsbdGetDeviceList", reinterpret_cast<void *>(get_device_list)},
    {"sceUsbdFreeDeviceList", reinterpret_cast<void *>(nothing)},
    {"sceUsbdGetDeviceDescriptor", reinterpret_cast<void *>(get_device_descriptor)},
    {"sceUsbdGetConfigDescriptor", reinterpret_cast<void *>(get_config_descriptor)},
    {"sceUsbdFreeConfigDescriptor", reinterpret_cast<void *>(nothing)},
    {"sceUsbdOpenDeviceWithVidPid", reinterpret_cast<void *>(open_vid_pid)},
    {"sceUsbdCheckConnected", reinterpret_cast<void *>(connected)},
    {"sceUsbdKernelDriverActive", reinterpret_cast<void *>(driver_active)},
    {"sceUsbdDetachKernelDriver", reinterpret_cast<void *>(claim)},
    {"sceUsbdClaimInterface", reinterpret_cast<void *>(claim)},
    {"sceUsbdResetDevice", reinterpret_cast<void *>(device_ok)},
    {"sceUsbdGetBusNumber", reinterpret_cast<void *>(bus_number)},
    {"sceUsbdGetDeviceAddress", reinterpret_cast<void *>(bus_number)},
    {"sceUsbdGetMaxPacketSize", reinterpret_cast<void *>(max_packet)},
    {"sceUsbdAllocTransfer", reinterpret_cast<void *>(alloc_transfer)},
    {"sceUsbdFillInterruptTransfer", reinterpret_cast<void *>(fill_interrupt)},
    {"sceUsbdSubmitTransfer", reinterpret_cast<void *>(submit_transfer)},
    {"sceUsbdHandleEvents", reinterpret_cast<void *>(handle_events)},
};

} // namespace

extern "C" {

void runtime_toypad_lights(uint32_t out[3]) {
    pthread_mutex_lock(&s_lock);
    std::memcpy(out, s_lights, sizeof(s_lights));
    pthread_mutex_unlock(&s_lock);
}

void runtime_toypad_register(void) {
    runtime_register_exports(toypad_exports, sizeof(toypad_exports) / sizeof(toypad_exports[0]));
}

uintptr_t runtime_toypad_resolve(const char *name) {
    return runtime_registry_resolve(name, EXPORT_FUNCTION);
}

void runtime_toypad_report(void) {
    if (s_commands) {
        std::printf("Toy Pad: %zu commands (%zu answered empty)\n", s_commands, s_unknown_commands);
    }
}

} // extern "C"
