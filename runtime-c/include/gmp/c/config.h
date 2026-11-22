/* gmp/c/config.h - build configuration of the GMP C runtime.
 *
 * Every public name of the C runtime uses the prefix gmsg_ / GMSG_ ("generic message"), not gmp_, so it never
 * clashes with GNU MP, which uses gmp_ functions and GMP_ macros in its <gmp.h>.
 *
 * Every option can be set before including any runtime header (or on the command line, -DGMSG_USE_LIBC=0).
 *
 *   GMSG_USE_LIBC     1 (default): use <string.h> (memcpy, memset, memcmp) and <assert.h>.
 *                     0: the runtime uses its own small loops and needs no C library at all; only the
 *                     freestanding headers <stdint.h>, <stddef.h> and <stdbool.h> are included.
 *   GMSG_FLOAT_TEXT   how floats are turned into text and back (print, text messages):
 *                     GMSG_FLOAT_TEXT_BUILTIN (default): built-in shortest round-trip formatter and strict,
 *                         locale-free parser; output matches the C++ runtime exactly.
 *                     GMSG_FLOAT_TEXT_LIBC: snprintf / strtod (smaller if the C library is linked anyway).
 *                     GMSG_FLOAT_TEXT_NONE: no float text support (saves flash).
 *   GMSG_RUNTIME_LIBRARY
 *                     0 (default): header-only. Every runtime function is `static inline` in the headers.
 *                     1: compiled library. The larger functions (checksums, framing, stream, text, float text,
 *                     print) are only declared in the headers and compiled once, in runtime-c/src/ (one .c file
 *                     per header, so a static library links only the parts in use; CMake target
 *                     gmp::runtime_c_lib, which defines this option for its users). Small functions
 *                     (endian, bits, reader / writer) stay inline. Every option in this file must have the same value
 *                     for the library and the code using it. Header-only and library code may be mixed in one
 *                     program (the header-only copies are static).
 *   GMSG_CRC_TABLES   1: the CRC presets (gmsg_crc32_compute, ...) use 256-entry lookup tables (4 to 8 times
 *                     faster on x86 at -O2; 256 to 2048 bytes of constant data for each table used).
 *                     0: bit by bit, no tables. Default: 1 in the library build, 0 header-only.
 *                     The generic gmsg_crc_compute (any parameters) is always bit by bit.
 *   GMSG_API          storage class of the runtime's library functions: `static inline` header-only, `extern`
 *                     in the library build (define it to add, e.g., a DLL export attribute).
 *   GMSG_ASSERT(x)    debug check used by the runtime; default assert() with GMSG_USE_LIBC, otherwise no-op.
 *
 * Language level: C99 or later (C11 recommended). C11-only features are used only through the macros below.
 * GMSG_C11 is 1 for C11 or later (and for C++); generated code uses it to add C11-only checks of user functions
 * (`_Generic` signature checks), which C99 builds skip (the calls are still type checked by the compiler).
 */
#ifndef GMSG_C_CONFIG_H
#define GMSG_C_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if !defined(__cplusplus) && (!defined(__STDC_VERSION__) || __STDC_VERSION__ < 199901L)
#error "the GMP C runtime needs C99 or later (-std=c99, -std=c11; MSVC: /std:c11)"
#endif

#if defined(__cplusplus) || __STDC_VERSION__ >= 201112L
#define GMSG_C11 1
#else
#define GMSG_C11 0
#endif

#ifndef GMSG_USE_LIBC
#define GMSG_USE_LIBC 1
#endif

#define GMSG_FLOAT_TEXT_NONE 0
#define GMSG_FLOAT_TEXT_BUILTIN 1
#define GMSG_FLOAT_TEXT_LIBC 2
#ifndef GMSG_FLOAT_TEXT
#define GMSG_FLOAT_TEXT GMSG_FLOAT_TEXT_BUILTIN
#endif
#if GMSG_FLOAT_TEXT == GMSG_FLOAT_TEXT_LIBC && !GMSG_USE_LIBC
#error "GMSG_FLOAT_TEXT_LIBC needs GMSG_USE_LIBC=1"
#endif

#ifndef GMSG_RUNTIME_LIBRARY
#define GMSG_RUNTIME_LIBRARY 0
#endif

#ifndef GMSG_CRC_TABLES
#define GMSG_CRC_TABLES GMSG_RUNTIME_LIBRARY
#endif

/* GMSG_DEFINE_FUNCTIONS_: the headers contain the function bodies (header-only build). In the library build each
 * file of runtime-c/src/ defines GMSG_IMPL_<PART>_ to compile the bodies of one header. */
#if !GMSG_RUNTIME_LIBRARY
#ifndef GMSG_API
#define GMSG_API static inline
#endif
#define GMSG_DEFINE_FUNCTIONS_ 1
#else
#ifndef GMSG_API
#define GMSG_API extern
#endif
#define GMSG_DEFINE_FUNCTIONS_ 0
#endif

/* Small functions that are always inline (endian, bits, reader / writer, ...), and internal helpers. */
#define GMSG_INLINE static inline
#define GMSG_PRIV_ static inline

#ifndef GMSG_ASSERT
#if GMSG_USE_LIBC
#include <assert.h>
#define GMSG_ASSERT(x) assert(x)
#else
#define GMSG_ASSERT(x) ((void)0)
#endif
#endif

/* C11 features with C99 fallbacks. GMSG_STATIC_ASSERT may be used at file scope only. The C99 version declares
 * an array whose size is negative when the condition fails (the message is not shown; the line is). */
#if defined(__cplusplus)
#define GMSG_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#define GMSG_ALIGNOF(t) alignof(t)
#elif GMSG_C11
#define GMSG_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#define GMSG_ALIGNOF(t) _Alignof(t)
#else
#define GMSG_STATIC_ASSERT(cond, msg) extern char gmsg_static_assert_[(cond) ? 1 : -1]
#define GMSG_ALIGNOF(t) offsetof(struct { char c_; t v_; }, v_)
#endif

/* C linkage when the headers are used from C++ (needed for the library build). */
#ifdef __cplusplus
#define GMSG_C_BEGIN extern "C" {
#define GMSG_C_END }
#else
#define GMSG_C_BEGIN
#define GMSG_C_END
#endif

GMSG_STATIC_ASSERT(sizeof(float) == 4, "GMP needs a 32-bit IEEE float");
GMSG_STATIC_ASSERT(sizeof(double) == 8, "GMP needs a 64-bit IEEE double");

#endif /* GMSG_C_CONFIG_H */
