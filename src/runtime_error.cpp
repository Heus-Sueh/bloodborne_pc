#include "runtime_error.h"
#include <cerrno>

namespace bb::error {

constexpr int32_t HostToGuestErrno(int host_errno) noexcept {
    switch (host_errno) {
    case 0: return 0;
    case EPERM: return 1;
    case ENOENT: return 2;
    case ESRCH: return 3;
    case EINTR: return 4;
    case EIO: return 5;
    case ENXIO: return 6;
    case E2BIG: return 7;
    case ENOEXEC: return 8;
    case EBADF: return 9;
    case ECHILD: return 10;
    case EDEADLK: return 11;
    case ENOMEM: return 12;
    case EACCES: return 13;
    case EFAULT: return 14;
    case EBUSY: return 16;
    case EEXIST: return 17;
    case EXDEV: return 18;
    case ENODEV: return 19;
    case ENOTDIR: return 20;
    case EISDIR: return 21;
    case EINVAL: return 22;
    case ENFILE: return 23;
    case EMFILE: return 24;
    case ENOTTY: return 25;
    case EFBIG: return 27;
    case ENOSPC: return 28;
    case ESPIPE: return 29;
    case EROFS: return 30;
    case EMLINK: return 31;
    case EPIPE: return 32;
    case ERANGE: return 34;
    case EAGAIN: return 35;
    case ETIMEDOUT: return 60;
    case ELOOP: return 62;
    case ENAMETOOLONG: return 63;
    case ENOTEMPTY: return 66;
    case ENOSYS: return 78;
    case EOVERFLOW: return 84;
    case ECANCELED: return 85;
    default: return 5; // EIO: unmapped host error
    }
}

} // namespace bb::error

extern "C" {

int32_t runtime_guest_errno(int host_errno) {
    return bb::error::HostToGuestErrno(host_errno);
}

int32_t orbis_error_from_errno(int host_errno) {
    if (!host_errno) return 0;
    return ORBIS_ERR(bb::error::HostToGuestErrno(host_errno));
}

} // extern "C"
