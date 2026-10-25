#ifndef BB_PLATFORM_H
#define BB_PLATFORM_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Target operating system detection */
#if defined(_WIN32) || defined(_WIN64)
#define BB_PLATFORM_WINDOWS 1
#elif defined(__APPLE__) && defined(__MACH__)
#define BB_PLATFORM_MACOS 1
#define BB_PLATFORM_POSIX 1
#elif defined(__linux__)
#define BB_PLATFORM_LINUX 1
#define BB_PLATFORM_POSIX 1
#elif defined(__unix__)
#define BB_PLATFORM_POSIX 1
#else
#warning "Unrecognized host platform; defaulting to generic POSIX environment"
#define BB_PLATFORM_POSIX 1
#endif

/* Target architecture detection */
#if defined(__x86_64__) || defined(_M_X64)
#define BB_ARCH_X86_64 1
#elif defined(__aarch64__) || defined(_M_ARM64)
#define BB_ARCH_ARM64 1
#endif

/* Standard attributes and calling conventions */
#if defined(__GNUC__) || defined(__clang__)
#define BB_SYSV_ABI __attribute__((sysv_abi))
#define BB_FORCE_INLINE inline __attribute__((always_inline))
#define BB_NORETURN _Noreturn
#define BB_EXPORT __attribute__((visibility("default")))
#elif defined(_MSC_VER)
#define BB_SYSV_ABI
#define BB_FORCE_INLINE __forceinline
#define BB_NORETURN __declspec(noreturn)
#define BB_EXPORT __declspec(dllexport)
#else
#define BB_SYSV_ABI
#define BB_FORCE_INLINE inline
#define BB_NORETURN _Noreturn
#define BB_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Returns human-readable platform identification string. */
const char *bb_platform_name(void);

/* Returns human-readable architecture identification string. */
const char *bb_platform_arch(void);

#ifdef __cplusplus
}
#endif

#endif /* BB_PLATFORM_H */
