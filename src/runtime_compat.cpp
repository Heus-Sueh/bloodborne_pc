#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "runtime_compat.hpp"
#include "runtime.h"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <pthread.h>
#include <sys/random.h>
#include <time.h>

#define ERR(n) ORBIS_ERR(n)
#define KERNEL_EINVAL ERR(22)
#define KERNEL_ESRCH ERR(3)
#define KERNEL_EBUSY ERR(16)
#define KERNEL_ETIMEDOUT ERR(60)
#define KERNEL_ECANCELED ERR(85)
#define KERNEL_EACCES ERR(13)
#define NP_SIGNED_OUT ((int32_t)0x80550006)
#define NET_CTL_NOT_CONNECTED ((int32_t)0x80412108)
#define NET_ENOTCONN ((int32_t)0x80410139)
#define HTTP_NETWORK ((int32_t)0x80431063)
#define APPCONTENT_NOT_FOUND ((int32_t)0x80D90005)
#define SSL_NOT_FOUND ((int32_t)0x8095F00B)
#define RTC_INVALID ((int32_t)0x80B50002)
#define TROPHY_INVALID ((int32_t)0x80551604)
#define VIDEODEC_UNSUPPORTED ((int32_t)0x80C10003)
#define DISC_MAP_NO_BITMAP ((int32_t)0x81100004)

namespace {

ABI int32_t ok(void) {
    return 0;
}

/* ---- Event flags ---------------------------------------------------------------------- */

enum { WAIT_AND = 0x01, WAIT_OR = 0x02, CLEAR_ALL = 0x10, CLEAR_PAT = 0x20 };

struct EventFlag {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    uint64_t pattern;
    uint32_t attr;
    unsigned waiters;
    uint64_t generation; ///< bumped by cancel and delete: waiters leave with an error
    int deleted;
    char name[32];
};

std::atomic<size_t> s_flags_created{0};
std::atomic<size_t> s_flag_waits{0};

bool Matches(uint64_t pattern, uint64_t bits, uint32_t mode) {
    return (mode & WAIT_AND) ? ((pattern & bits) == bits) : ((pattern & bits) != 0);
}

ABI int32_t evf_create(EventFlag **out, const char *name, uint32_t attr, uint64_t init, const void *options) {
    (void)options;
    if (!out) {
        return KERNEL_EINVAL;
    }
    auto *f = static_cast<EventFlag *>(std::calloc(1, sizeof(EventFlag)));
    if (!f) {
        return ERR(12);
    }
    pthread_mutex_init(&f->lock, nullptr);
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, RUNTIME_COND_CLOCK);
    pthread_cond_init(&f->changed, &ca);
    pthread_condattr_destroy(&ca);
    f->pattern = init;
    f->attr = attr;
    if (name) {
        std::snprintf(f->name, sizeof(f->name), "%s", name);
    }
    *out = f;
    s_flags_created.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

ABI int32_t evf_delete(EventFlag *f) {
    if (!f) {
        return KERNEL_ESRCH;
    }
    pthread_mutex_lock(&f->lock);
    f->deleted = 1;
    ++f->generation;
    const unsigned waiting = f->waiters;
    pthread_cond_broadcast(&f->changed);
    pthread_mutex_unlock(&f->lock);
    if (!waiting) { /* waiters free nothing; a flag deleted under them is leaked (rare) */
        pthread_cond_destroy(&f->changed);
        pthread_mutex_destroy(&f->lock);
        std::free(f);
    }
    return 0;
}

ABI int32_t evf_set(EventFlag *f, uint64_t bits) {
    if (!f) {
        return KERNEL_ESRCH;
    }
    pthread_mutex_lock(&f->lock);
    f->pattern |= bits;
    pthread_cond_broadcast(&f->changed);
    pthread_mutex_unlock(&f->lock);
    return 0;
}

ABI int32_t evf_clear(EventFlag *f, uint64_t bits) {
    if (!f) {
        return KERNEL_ESRCH;
    }
    pthread_mutex_lock(&f->lock);
    f->pattern &= bits; /* the bits kept, as the kernel's clear takes them */
    pthread_mutex_unlock(&f->lock);
    return 0;
}

int32_t evf_take(EventFlag *f, uint64_t bits, uint32_t mode, uint64_t *result) {
    if (result) {
        *result = f->pattern;
    }
    if (mode & CLEAR_ALL) {
        f->pattern = 0;
    } else if (mode & CLEAR_PAT) {
        f->pattern &= ~bits;
    }
    return 0;
}

ABI int32_t evf_wait(EventFlag *f, uint64_t bits, uint32_t mode, uint64_t *result, uint32_t *timeout_us) {
    if (!f) {
        return KERNEL_ESRCH;
    }
    if (!bits || !(mode & (WAIT_AND | WAIT_OR)) || ((mode & WAIT_AND) && (mode & WAIT_OR))) {
        return KERNEL_EINVAL;
    }
    pthread_mutex_lock(&f->lock);
    struct timespec deadline;
    if (timeout_us) {
        clock_gettime(RUNTIME_COND_CLOCK, &deadline);
        const uint64_t ns = static_cast<uint64_t>(deadline.tv_nsec) + static_cast<uint64_t>(*timeout_us) * 1000;
        deadline.tv_sec += static_cast<time_t>(ns / 1000000000);
        deadline.tv_nsec = static_cast<long>(ns % 1000000000);
    }
    const uint64_t generation = f->generation;
    const uint64_t start = runtime_wait_clock();
    int32_t status = 0;
    ++f->waiters;
    while (!Matches(f->pattern, bits, mode)) {
        if (f->generation != generation) {
            status = f->deleted ? KERNEL_EACCES : KERNEL_ECANCELED;
            break;
        }
        const int e = timeout_us ? pthread_cond_timedwait(&f->changed, &f->lock, &deadline)
                                 : pthread_cond_wait(&f->changed, &f->lock);
        if (e == ETIMEDOUT && !Matches(f->pattern, bits, mode)) {
            status = KERNEL_ETIMEDOUT;
            break;
        }
    }
    --f->waiters;
    if (status == 0) {
        evf_take(f, bits, mode, result);
    } else if (result) {
        *result = f->pattern;
    }
    if (timeout_us) {
        const uint64_t waited_us = (runtime_wait_clock() - start) / 1000;
        *timeout_us = (status == KERNEL_ETIMEDOUT || waited_us >= *timeout_us)
                          ? 0
                          : *timeout_us - static_cast<uint32_t>(waited_us);
    }
    const int free_it = f->deleted && !f->waiters;
    pthread_mutex_unlock(&f->lock);
    if (free_it) {
        pthread_cond_destroy(&f->changed);
        pthread_mutex_destroy(&f->lock);
        std::free(f);
    }
    s_flag_waits.fetch_add(1, std::memory_order_relaxed);
    runtime_wait_note(2, runtime_wait_clock() - start);
    return status;
}

ABI int32_t evf_poll(EventFlag *f, uint64_t bits, uint32_t mode, uint64_t *result) {
    if (!f) {
        return KERNEL_ESRCH;
    }
    if (!bits || !(mode & (WAIT_AND | WAIT_OR))) {
        return KERNEL_EINVAL;
    }
    pthread_mutex_lock(&f->lock);
    int32_t status = Matches(f->pattern, bits, mode) ? evf_take(f, bits, mode, result) : KERNEL_EBUSY;
    if (status && result) {
        *result = f->pattern;
    }
    pthread_mutex_unlock(&f->lock);
    return status;
}

ABI int32_t evf_cancel(EventFlag *f, uint64_t pattern, int32_t *waiting) {
    if (!f) {
        return KERNEL_ESRCH;
    }
    pthread_mutex_lock(&f->lock);
    if (waiting) {
        *waiting = static_cast<int32_t>(f->waiters);
    }
    f->pattern = pattern;
    ++f->generation;
    pthread_cond_broadcast(&f->changed);
    pthread_mutex_unlock(&f->lock);
    return 0;
}

/* ---- Time zones, UUIDs, memory hints ---------------------------------------------------- */

struct Timesec {
    int64_t t;
    uint32_t west_sec, dst_sec;
};
struct Timezone {
    int32_t minutes_west, dst_time;
};

long HostOffset(time_t at, int *dst) {
    struct tm local {};
    localtime_r(&at, &local);
    if (dst) {
        *dst = local.tm_isdst > 0;
    }
    return RUNTIME_TM_GMTOFF(local); /* east of UTC, seconds */
}

ABI int32_t utc_to_local(int64_t utc, int64_t *local, Timesec *st, uint64_t *dst_sec) {
    int dst = 0;
    const long offset = HostOffset(static_cast<time_t>(utc), &dst);
    if (local) {
        *local = utc + offset;
    }
    if (st) {
        st->t = utc;
        st->west_sec = static_cast<uint32_t>(offset - (dst ? 3600 : 0));
        st->dst_sec = dst ? 3600 : 0;
    }
    if (dst_sec) {
        *dst_sec = dst ? 3600 : 0;
    }
    return 0;
}

ABI int32_t local_to_utc(int64_t local, int64_t unused, int64_t *utc, Timezone *tz, int32_t *dst_sec) {
    (void)unused;
    int dst = 0;
    const long offset = HostOffset(static_cast<time_t>(local), &dst);
    if (utc) {
        *utc = local - offset;
    }
    if (tz) {
        tz->minutes_west = static_cast<int32_t>(-offset / 60);
        tz->dst_time = dst;
    }
    if (dst_sec) {
        *dst_sec = dst ? 3600 : 0;
    }
    return 0;
}

ABI int32_t uuid_create(uint8_t *uuid) {
    if (!uuid) {
        return KERNEL_EINVAL;
    }
    if (getrandom(uuid, 16, 0) != 16) {
        for (int i = 0; i < 16; ++i) {
            uuid[i] = static_cast<uint8_t>(rand());
        }
    }
    uuid[6] = static_cast<uint8_t>((uuid[6] & 0x0f) | 0x40); /* version 4 */
    uuid[8] = static_cast<uint8_t>((uuid[8] & 0x3f) | 0x80);
    return 0;
}

/* Condition variable attributes: the runtime's conditions take none (CLOCK_REALTIME waits). */
ABI int32_t condattr_init(void **attr) {
    if (!attr) {
        return KERNEL_EINVAL;
    }
    *attr = std::calloc(1, 16);
    return *attr ? 0 : ERR(12);
}

ABI int32_t condattr_destroy(void **attr) {
    if (!attr) {
        return KERNEL_EINVAL;
    }
    std::free(*attr);
    *attr = nullptr;
    return 0;
}

/* ---- Dialogs that finish at once (cancelled) ------------------------------------------------ */

enum { DIALOG_FINISHED = 3, DIALOG_RESULT_CANCELED = 1 };
ABI int32_t dialog_finished(void) {
    return DIALOG_FINISHED;
}
ABI int32_t dialog_cancelled(int32_t *result) {
    if (result) {
        *result = DIALOG_RESULT_CANCELED;
    }
    return 0;
}

/* SceSaveDataDialogResult: mode, result (0: OK), button. */
ABI int32_t save_dialog_result(int32_t *result) {
    if (!result) {
        return KERNEL_EINVAL;
    }
    result[1] = 0;
    result[2] = 0;
    return 0;
}

/* ---- Small services -------------------------------------------------------------------------- */

ABI int32_t np_signed_out(void) {
    return NP_SIGNED_OUT;
}
ABI int32_t netctl_not_connected(void) {
    return NET_CTL_NOT_CONNECTED;
}
ABI int32_t net_not_connected(void) {
    return NET_ENOTCONN;
}
ABI int32_t http_unavailable(void) {
    return HTTP_NETWORK;
}
ABI int32_t appcontent_not_found(void) {
    return APPCONTENT_NOT_FOUND;
}
ABI int32_t ssl_not_found(void) {
    return SSL_NOT_FOUND;
}
ABI int32_t rtc_invalid(void) {
    return RTC_INVALID;
}
ABI int32_t trophy_invalid(void) {
    return TROPHY_INVALID;
}
ABI int32_t safe_area(float *info) {
    if (!info) {
        return KERNEL_EINVAL;
    }
    info[0] = 1.0f;
    return 0;
}
ABI int32_t user_color(int32_t user, int32_t *color) {
    (void)user;
    if (color) {
        *color = 0;
    }
    return 0;
}

/* PlayGo: everything installed; progress complete. */
struct PlayGoProgress {
    uint64_t progress, total;
};
ABI int32_t playgo_progress(uint32_t handle, const uint16_t *chunks, uint32_t count, PlayGoProgress *out) {
    (void)handle;
    (void)chunks;
    (void)count;
    if (!out) {
        return KERNEL_EINVAL;
    }
    out->progress = out->total = 1;
    return 0;
}

/* Save data: plenty of room; parameters read as empty. */
struct SaveMountInfo {
    uint64_t blocks, free_blocks;
    uint8_t reserved[32];
};
ABI int32_t save_mount_info(const void *mount, SaveMountInfo *info) {
    (void)mount;
    if (!info) {
        return KERNEL_EINVAL;
    }
    std::memset(info, 0, sizeof(*info));
    info->blocks = 1u << 20;
    info->free_blocks = 1u << 19;
    return 0;
}

ABI int32_t save_get_param(const void *mount, int32_t type, void *buffer, uint64_t size, uint64_t *got) {
    (void)mount;
    (void)type;
    if (buffer) {
        std::memset(buffer, 0, size);
    }
    if (got) {
        *got = size;
    }
    return 0;
}

/* Temporary data: /temp0 in the user folder. */
ABI int32_t temporary_mount(int32_t option, char *mount_point) {
    (void)option;
    if (!mount_point) {
        return KERNEL_EINVAL;
    }
    char host[1024];
    std::snprintf(host, sizeof(host), "%s/temp0", runtime_file_user_dir());
    runtime_file_mount("/temp0", host);
    std::strcpy(mount_point, "/temp0");
    return 0;
}

/* ---- What another game asks beyond the others ------------------------------------------- */

ABI int32_t not_neo(void) {
    return 0;
}
ABI int32_t prt_aperture(int32_t index, uint64_t *address, uint64_t *size) {
    (void)index;
    if (address) {
        *address = 0;
    }
    if (size) {
        *size = 0;
    }
    return 0;
}

ABI int32_t system_param_string(int32_t id, char *buffer, uint64_t size) {
    (void)id;
    if (!buffer || !size) {
        return KERNEL_EINVAL;
    }
    buffer[0] = 0;
    return 0;
}

ABI int32_t playgo_eta(uint32_t handle, const uint16_t *chunks, uint32_t count, int64_t *eta) {
    (void)handle;
    (void)chunks;
    (void)count;
    if (eta) {
        *eta = 0;
    }
    return 0;
}

ABI int32_t playgo_todo(uint32_t handle, void *list, uint32_t count, uint32_t *out_count) {
    (void)handle;
    (void)list;
    (void)count;
    if (out_count) {
        *out_count = 0;
    }
    return 0;
}

ABI int32_t callback_id(void) {
    return 1;
}
ABI int32_t disc_map_no_bitmap(void) {
    return DISC_MAP_NO_BITMAP;
}
ABI int32_t videodec_unsupported(void) {
    static int said = 0;
    if (!said++) {
        std::printf("Runtime: sceVideodec asked for: no video decoder yet (videos are skipped)\n");
    }
    return VIDEODEC_UNSUPPORTED;
}

ABI int32_t error_dialog_open(const void *param) {
    (void)param;
    std::printf("Runtime: ErrorDialog opened; finished at once (no dialog UI)\n");
    return 0;
}

/* libc's heap tracing (sceLibcHeapGetTraceInfo, during libc's start) */
struct HeapTraceInfo {
    uint64_t size;
    uint32_t flag, get_segment_info;
    uint64_t *atomic_id_mask, *mstate_table;
};
uint64_t s_heap_trace_mask = 0;
uint64_t s_heap_trace_table[64] = {0};

ABI void heap_trace_info(HeapTraceInfo *info) {
    if (!info) {
        return;
    }
    info->get_segment_info = 0;
    info->atomic_id_mask = &s_heap_trace_mask;
    info->mstate_table = s_heap_trace_table;
}

const RuntimeExport compat_exports[] = {
    {"sceKernelCreateEventFlag", reinterpret_cast<void *>(evf_create)},
    {"sceKernelDeleteEventFlag", reinterpret_cast<void *>(evf_delete)},
    {"sceKernelSetEventFlag", reinterpret_cast<void *>(evf_set)},
    {"sceKernelClearEventFlag", reinterpret_cast<void *>(evf_clear)},
    {"sceKernelWaitEventFlag", reinterpret_cast<void *>(evf_wait)},
    {"sceKernelPollEventFlag", reinterpret_cast<void *>(evf_poll)},
    {"sceKernelCancelEventFlag", reinterpret_cast<void *>(evf_cancel)},
    {"sceKernelConvertUtcToLocaltime", reinterpret_cast<void *>(utc_to_local)},
    {"sceKernelConvertLocaltimeToUtc", reinterpret_cast<void *>(local_to_utc)},
    {"sceKernelUuidCreate", reinterpret_cast<void *>(uuid_create)},
    {"sceKernelMlock", reinterpret_cast<void *>(ok)},
    {"msync", reinterpret_cast<void *>(ok)},
    {"scePthreadCondattrInit", reinterpret_cast<void *>(condattr_init)},
    {"scePthreadCondattrDestroy", reinterpret_cast<void *>(condattr_destroy)},
    {"sceCoredumpRegisterCoredumpHandler", reinterpret_cast<void *>(ok)},
    {"sceCoredumpWriteUserData", reinterpret_cast<void *>(ok)},
    {"sceGameCustomDataDialogInitialize", reinterpret_cast<void *>(ok)},
    {"sceGameCustomDataDialogOpen", reinterpret_cast<void *>(ok)},
    {"sceGameCustomDataDialogUpdateStatus", reinterpret_cast<void *>(dialog_finished)},
    {"sceGameCustomDataDialogGetResult", reinterpret_cast<void *>(dialog_cancelled)},
    {"sceGameCustomDataDialogTerminate", reinterpret_cast<void *>(ok)},
    {"sceInvitationDialogInitialize", reinterpret_cast<void *>(ok)},
    {"sceInvitationDialogOpen", reinterpret_cast<void *>(ok)},
    {"sceInvitationDialogUpdateStatus", reinterpret_cast<void *>(dialog_finished)},
    {"sceInvitationDialogGetResult", reinterpret_cast<void *>(dialog_cancelled)},
    {"sceInvitationDialogTerminate", reinterpret_cast<void *>(ok)},
    {"sceNpSnsFacebookDialogInitialize", reinterpret_cast<void *>(ok)},
    {"sceNpSnsFacebookDialogOpen", reinterpret_cast<void *>(ok)},
    {"sceNpSnsFacebookDialogUpdateStatus", reinterpret_cast<void *>(dialog_finished)},
    {"sceNpSnsFacebookDialogGetResult", reinterpret_cast<void *>(dialog_cancelled)},
    {"sceNpSnsFacebookDialogTerminate", reinterpret_cast<void *>(ok)},
    {"sceWebBrowserDialogInitialize", reinterpret_cast<void *>(ok)},
    {"sceWebBrowserDialogOpen", reinterpret_cast<void *>(ok)},
    {"sceWebBrowserDialogUpdateStatus", reinterpret_cast<void *>(dialog_finished)},
    {"sceWebBrowserDialogTerminate", reinterpret_cast<void *>(ok)},
    {"sceNpCommerceDialogGetResult", reinterpret_cast<void *>(dialog_cancelled)},
    {"sceSaveDataDialogClose", reinterpret_cast<void *>(ok)},
    {"sceSaveDataDialogGetResult", reinterpret_cast<void *>(save_dialog_result)},
    {"sceSaveDataDialogProgressBarSetValue", reinterpret_cast<void *>(ok)},
    {"sceSaveDataGetMountInfo", reinterpret_cast<void *>(save_mount_info)},
    {"sceSaveDataGetParam", reinterpret_cast<void *>(save_get_param)},
    {"sceAppContentTemporaryDataMount2", reinterpret_cast<void *>(temporary_mount)},
    {"sceAppContentGetAddcontInfo", reinterpret_cast<void *>(appcontent_not_found)},
    {"sceAppContentAddcontMount", reinterpret_cast<void *>(appcontent_not_found)},
    {"sceAppContentAddcontUnmount", reinterpret_cast<void *>(appcontent_not_found)},
    {"sceAppContentAddcontDelete", reinterpret_cast<void *>(appcontent_not_found)},
    {"sceAppContentAddcontEnqueueDownload", reinterpret_cast<void *>(appcontent_not_found)},
    {"sceAppContentGetEntitlementKey", reinterpret_cast<void *>(appcontent_not_found)},
    {"scePlayGoGetProgress", reinterpret_cast<void *>(playgo_progress)},
    {"scePlayGoPrefetch", reinterpret_cast<void *>(ok)},
    {"scePlayGoClose", reinterpret_cast<void *>(ok)},
    {"scePlayGoTerminate", reinterpret_cast<void *>(ok)},
    {"scePlayGoGetEta", reinterpret_cast<void *>(playgo_eta)},
    {"scePlayGoGetToDoList", reinterpret_cast<void *>(playgo_todo)},
    {"scePlayGoSetToDoList", reinterpret_cast<void *>(ok)},
    {"scePlayGoSetLanguageMask", reinterpret_cast<void *>(ok)},
    {"sceKernelIsNeoMode", reinterpret_cast<void *>(not_neo)},
    {"sceKernelGetPrtAperture", reinterpret_cast<void *>(prt_aperture)},
    {"sceSystemServiceParamGetString", reinterpret_cast<void *>(system_param_string)},
    {"sceHttpSetChunkedTransferEnabled", reinterpret_cast<void *>(ok)},
    {"sceHttpSetRecvTimeOut", reinterpret_cast<void *>(ok)},
    {"sceHttpSetSendTimeOut", reinterpret_cast<void *>(ok)},
    {"sceHttpsLoadCert", reinterpret_cast<void *>(ok)},
    {"sceHttpGetAllResponseHeaders", reinterpret_cast<void *>(http_unavailable)},
    {"sceErrorDialogInitialize", reinterpret_cast<void *>(ok)},
    {"sceErrorDialogOpen", reinterpret_cast<void *>(error_dialog_open)},
    {"sceErrorDialogUpdateStatus", reinterpret_cast<void *>(dialog_finished)},
    {"sceErrorDialogTerminate", reinterpret_cast<void *>(ok)},
    {"sceNpCheckNpAvailabilityA", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpGetAccountCountryA", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpGetAccountIdA", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpGetGamePresenceStatusA", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpRegisterGamePresenceCallbackA", reinterpret_cast<void *>(callback_id)},
    {"sceNpRegisterStateCallbackA", reinterpret_cast<void *>(callback_id)},
    {"sceDiscMap_7C980FFB0AA27E7A", reinterpret_cast<void *>(disc_map_no_bitmap)},
    {"sceVideodecQueryResourceInfo", reinterpret_cast<void *>(videodec_unsupported)},
    {"sceVideodecCreateDecoder", reinterpret_cast<void *>(videodec_unsupported)},
    {"sceVideodecDecode", reinterpret_cast<void *>(videodec_unsupported)},
    {"sceVideodecFlush", reinterpret_cast<void *>(videodec_unsupported)},
    {"sceVideodecReset", reinterpret_cast<void *>(videodec_unsupported)},
    {"sceVideodecDeleteDecoder", reinterpret_cast<void *>(ok)},
    {"__pthread_cxa_finalize", reinterpret_cast<void *>(ok)},
    {"sceLibcHeapGetTraceInfo", reinterpret_cast<void *>(heap_trace_info)},
    {"scePadSetLightBar", reinterpret_cast<void *>(ok)},
    {"scePadResetLightBar", reinterpret_cast<void *>(ok)},
    {"sceRemoteplayInitialize", reinterpret_cast<void *>(ok)},
    {"sceRemoteplayProhibit", reinterpret_cast<void *>(ok)},
    {"sceScreenShotEnable", reinterpret_cast<void *>(ok)},
    {"sceScreenShotDisable", reinterpret_cast<void *>(ok)},
    {"sceVideoRecordingSetInfo", reinterpret_cast<void *>(ok)},
    {"sceGameLiveStreamingPermitServerSideRecording", reinterpret_cast<void *>(ok)},
    {"sceSystemServiceGetDisplaySafeAreaInfo", reinterpret_cast<void *>(safe_area)},
    {"sceUserServiceGetUserColor", reinterpret_cast<void *>(user_color)},
    {"sceRtcParseDateTime", reinterpret_cast<void *>(rtc_invalid)},
    {"sceRtcParseRFC3339", reinterpret_cast<void *>(rtc_invalid)},
    {"sceSslGetSerialNumber", reinterpret_cast<void *>(ssl_not_found)},
    {"sceNetCtlInit", reinterpret_cast<void *>(ok)},
    {"sceNetCtlTerm", reinterpret_cast<void *>(ok)},
    {"sceNetCtlGetResult", reinterpret_cast<void *>(netctl_not_connected)},
    {"sceNetGetpeername", reinterpret_cast<void *>(net_not_connected)},
    {"sceHttpCreateRequestWithURL2", reinterpret_cast<void *>(http_unavailable)},
    {"sceHttpsSetSslCallback", reinterpret_cast<void *>(ok)},
    {"sceHttpUriEscape", reinterpret_cast<void *>(http_unavailable)},
    {"sceNpTrophyDestroyContext", reinterpret_cast<void *>(ok)},
    {"sceNpTrophyDestroyHandle", reinterpret_cast<void *>(ok)},
    {"sceNpTrophyGetGameIcon", reinterpret_cast<void *>(trophy_invalid)},
    {"sceNpTrophyGetGroupIcon", reinterpret_cast<void *>(trophy_invalid)},
    {"sceNpTrophyGetGroupInfo", reinterpret_cast<void *>(trophy_invalid)},
    {"sceNpTrophyGetTrophyIcon", reinterpret_cast<void *>(trophy_invalid)},
    {"sceNpTrophyGetTrophyUnlockState", reinterpret_cast<void *>(trophy_invalid)},
    {"sceNpAuthCreateRequest", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpCreateRequest", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpGetAccountDateOfBirth", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpLookupCreateRequest", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpBandwidthTestInitStart", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpBandwidthTestGetStatus", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpBandwidthTestShutdown", reinterpret_cast<void *>(ok)},
    {"sceNpInGameMessageInitialize", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpInGameMessageTerminate", reinterpret_cast<void *>(ok)},
    {"sceNpInGameMessageCreateHandle", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpInGameMessageDeleteHandle", reinterpret_cast<void *>(ok)},
    {"sceNpInGameMessagePrepare", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpInGameMessageSendData", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpMatching2GetRoomDataExternalList", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpMatching2GetRoomDataInternal", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpMatching2GetRoomMemberDataInternal", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpMatching2GetUserInfoList", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpMatching2RegisterRoomMessageCallback", reinterpret_cast<void *>(ok)},
    {"sceNpMatching2SendRoomChatMessage", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpMatching2SendRoomMessage", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpMatching2SetUserInfo", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpMatching2SignalingGetConnectionInfo", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpScoreGetFriendsRanking", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpScoreGetFriendsRankingAsync", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpScoreGetGameDataAsync", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpScoreGetRankingByNpId", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpScoreGetRankingByNpIdAsync", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpScoreGetRankingByRangeAsync", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpScorePollAsync", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpScoreRecordGameDataAsync", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpScoreRecordScoreAsync", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpSnsFacebookCreateRequest", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpSnsFacebookDeleteRequest", reinterpret_cast<void *>(ok)},
    {"sceNpSnsFacebookGetAccessToken", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpTssGetData", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpTusCreateNpTitleCtx", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpTusDeleteNpTitleCtx", reinterpret_cast<void *>(ok)},
    {"sceNpTusCreateRequest", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpTusDeleteRequest", reinterpret_cast<void *>(ok)},
    {"sceNpTusAddAndGetVariable", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpTusAddAndGetVariableVUser", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpTusDeleteMultiSlotData", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpTusDeleteMultiSlotDataVUser", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpTusGetData", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpTusGetDataVUser", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpTusGetMultiSlotVariable", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpTusGetMultiSlotVariableVUser", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpTusSetData", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpTusSetDataVUser", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpTusSetMultiSlotVariable", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpTusSetMultiSlotVariableVUser", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpWebApiCreateHandle", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpWebApiDeleteHandle", reinterpret_cast<void *>(ok)},
    {"sceNpWebApiCreateServicePushEventFilter", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpWebApiDeleteServicePushEventFilter", reinterpret_cast<void *>(ok)},
    {"sceNpWebApiRegisterServicePushEventCallback", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpWebApiUnregisterServicePushEventCallback", reinterpret_cast<void *>(ok)},
    {"sceNpWebApiSendRequest2", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpWordFilterCreateTitleCtx", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpWordFilterDeleteTitleCtx", reinterpret_cast<void *>(ok)},
    {"sceNpWordFilterCreateRequest", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpWordFilterCreateAsyncRequest", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpWordFilterDeleteRequest", reinterpret_cast<void *>(ok)},
    {"sceNpWordFilterAbortRequest", reinterpret_cast<void *>(ok)},
    {"sceNpWordFilterPollAsync", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpWordFilterCensorComment", reinterpret_cast<void *>(np_signed_out)},
    {"sceNpWordFilterSanitizeComment", reinterpret_cast<void *>(np_signed_out)},
};

} // namespace

extern "C" {

void runtime_compat_register(void) {
    runtime_register_exports(compat_exports, sizeof(compat_exports) / sizeof(compat_exports[0]));
}

uintptr_t runtime_compat_resolve(const char *name) {
    return runtime_registry_resolve(name, EXPORT_FUNCTION);
}

void runtime_compat_report(void) {
    std::printf("Runtime: event flags created=%zu, waits=%zu\n",
                s_flags_created.load(std::memory_order_relaxed),
                s_flag_waits.load(std::memory_order_relaxed));
}

} // extern "C"
