#ifndef __SKOOMA_COMMON_H__
#define __SKOOMA_COMMON_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Bit manipulation
 *
 * Return bit at 'bit' (0 or 1) from 'x'. */
#define BIT_CHECK(x, bit) ((uint64_t)(x) & ((uint64_t)1 << (bit)))
// Set 'bit' from 'x'
#define BIT_SET(x, bit) ((x) |= ((uint64_t)1 << (bit)))
// Clear 'bit' from 'x'
#define BIT_CLEAR(x, bit) ((x) &= ~((uint64_t)1 << (bit)))
// Generate uint with 'bits' all set to 1
#define MAXBITS(bits) (~((uint64_t)0) >> ((sizeof(size_t) * 8) - 1 - bits))
// Wrapper around MAXBITS, uses 'bytes' instead
#define MAXBYTES(bytes) MAXBITS(bytes * 8)

/* Leave defined for debug build */
#define DEBUG_TRACE_EXECUTION

typedef uint8_t Byte;
typedef uint32_t UInt;

/* Compiler builtins (attributes) */
#if defined(__GLIBC__)
#define __likely(cond) __glibc_likely(cond)
#define __unlikely(cond) __glibc_unlikely(cond)
#define __unused __attribute__((unused))
#define __unreachable __builtin_unreachable()
#elif defined(__clang__)
#define __likely(cond) [[likely]] cond
#define __unlikely(cond) [[unlikely]] cond
#define __unused [[maybe_unused]]
#define __unreachable
#else
#define __likely(cond) cond
#define __unlikely(cond) cond
#define __unused
#define __unreachable
#endif

#define _likely(cond) __likely(cond)
#define _unlikely(cond) __unlikely(cond)
#define _unused __unused
#define _unreachable __unreachable

#endif
