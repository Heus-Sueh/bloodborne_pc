#ifndef BB_RUNTIME_ERROR_H
#define BB_RUNTIME_ERROR_H

#include <stdint.h>

#define ORBIS_ERROR_BASE UINT32_C(0x80020000)
#define ORBIS_ERR(code) ((int32_t)(ORBIS_ERROR_BASE | (uint32_t)(code)))

enum {
    ORBIS_OK = 0,
    ORBIS_EPERM = ORBIS_ERR(1),
    ORBIS_ENOENT = ORBIS_ERR(2),
    ORBIS_ESRCH = ORBIS_ERR(3),
    ORBIS_EINTR = ORBIS_ERR(4),
    ORBIS_EIO = ORBIS_ERR(5),
    ORBIS_ENXIO = ORBIS_ERR(6),
    ORBIS_E2BIG = ORBIS_ERR(7),
    ORBIS_ENOEXEC = ORBIS_ERR(8),
    ORBIS_EBADF = ORBIS_ERR(9),
    ORBIS_ECHILD = ORBIS_ERR(10),
    ORBIS_EDEADLK = ORBIS_ERR(11),
    ORBIS_ENOMEM = ORBIS_ERR(12),
    ORBIS_EACCES = ORBIS_ERR(13),
    ORBIS_EFAULT = ORBIS_ERR(14),
    ORBIS_EBUSY = ORBIS_ERR(16),
    ORBIS_EEXIST = ORBIS_ERR(17),
    ORBIS_EXDEV = ORBIS_ERR(18),
    ORBIS_ENODEV = ORBIS_ERR(19),
    ORBIS_ENOTDIR = ORBIS_ERR(20),
    ORBIS_EISDIR = ORBIS_ERR(21),
    ORBIS_EINVAL = ORBIS_ERR(22),
    ORBIS_ENFILE = ORBIS_ERR(23),
    ORBIS_EMFILE = ORBIS_ERR(24),
    ORBIS_ENOTTY = ORBIS_ERR(25),
    ORBIS_EFBIG = ORBIS_ERR(27),
    ORBIS_ENOSPC = ORBIS_ERR(28),
    ORBIS_ESPIPE = ORBIS_ERR(29),
    ORBIS_EROFS = ORBIS_ERR(30),
    ORBIS_EMLINK = ORBIS_ERR(31),
    ORBIS_EPIPE = ORBIS_ERR(32),
    ORBIS_ERANGE = ORBIS_ERR(34),
    ORBIS_EAGAIN = ORBIS_ERR(35),
    ORBIS_ETIMEDOUT = ORBIS_ERR(60),
    ORBIS_ELOOP = ORBIS_ERR(62),
    ORBIS_ENAMETOOLONG = ORBIS_ERR(63),
    ORBIS_ENOTEMPTY = ORBIS_ERR(66),
    ORBIS_ENOSYS = ORBIS_ERR(78),
    ORBIS_EOVERFLOW = ORBIS_ERR(84),
    ORBIS_ECANCELED = ORBIS_ERR(85),
};

#ifdef __cplusplus
extern "C" {
#endif

/* Convert host errno to guest FreeBSD errno (1..85) */
int32_t runtime_guest_errno(int host_errno);

/* Convert host errno or pthread error return value to Orbis error (0x80020000 | guest_errno) */
int32_t orbis_error_from_errno(int host_errno);

/* Convert timed wait return status to Orbis error (maps ETIMEDOUT to ORBIS_ETIMEDOUT) */
static inline int32_t orbis_error_from_timed(int status) {
    return orbis_error_from_errno(status);
}

#ifdef __cplusplus
}
#endif

#endif
