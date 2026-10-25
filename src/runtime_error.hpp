#pragma once

#include "runtime_error.h"
#include <cstdint>

namespace bb::error {

constexpr int32_t HostToGuestErrno(int host_errno) noexcept;

} // namespace bb::error

#ifdef __cplusplus
extern "C" {
#endif

int32_t runtime_guest_errno(int host_errno);
int32_t orbis_error_from_errno(int host_errno);

#ifdef __cplusplus
}
#endif
