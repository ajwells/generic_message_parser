/* gmp/c/mem.h - byte copy / fill / compare: <string.h> with GMSG_USE_LIBC, small loops without it. */
#ifndef GMSG_C_MEM_H
#define GMSG_C_MEM_H

#include "gmp/c/config.h"

#if GMSG_USE_LIBC
#include <string.h>
#endif

GMSG_C_BEGIN

GMSG_INLINE void gmsg_memcpy(void* dst, const void* src, size_t n) {
#if GMSG_USE_LIBC
    if (n) memcpy(dst, src, n);
#else
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    while (n--) *d++ = *s++;
#endif
}

/* Like memmove: the ranges may overlap. */
GMSG_INLINE void gmsg_memmove(void* dst, const void* src, size_t n) {
#if GMSG_USE_LIBC
    if (n) memmove(dst, src, n);
#else
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else if (d > s) {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
#endif
}

GMSG_INLINE void gmsg_memset(void* dst, uint8_t value, size_t n) {
#if GMSG_USE_LIBC
    if (n) memset(dst, value, n);
#else
    uint8_t* d = (uint8_t*)dst;
    while (n--) *d++ = value;
#endif
}

/* <0, 0 or >0 like memcmp (bytes compared as unsigned). */
GMSG_INLINE int gmsg_memcmp(const void* a, const void* b, size_t n) {
#if GMSG_USE_LIBC
    return n ? memcmp(a, b, n) : 0;
#else
    const uint8_t* x = (const uint8_t*)a;
    const uint8_t* y = (const uint8_t*)b;
    for (; n; --n, ++x, ++y)
        if (*x != *y) return *x < *y ? -1 : 1;
    return 0;
#endif
}

GMSG_C_END

#endif /* GMSG_C_MEM_H */
