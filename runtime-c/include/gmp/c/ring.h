/* gmp/c/ring.h - lock-free single-producer / single-consumer byte ring for interrupt-driven input
 * (port of gmsg::ByteRing).
 *
 * The producer (typically a UART RX interrupt) calls gmsg_ring_push(); the consumer (the main loop) drains the
 * ring into a stream with gmsg_stream_feed_ring() (or <protocol>_stream_feed_ring()), so decoding and callbacks
 * never run in the interrupt. One producer and one consumer at a time; neither side blocks or disables
 * interrupts.
 *
 *     static uint8_t rx_buf[256];                  // a power of two
 *     static gmsg_ring rx;                         // gmsg_ring_init(&rx, rx_buf, sizeof rx_buf) at start-up
 *     void USART1_IRQHandler(void) { gmsg_ring_push(&rx, (uint8_t)USART1->DR); }
 *     for (;;) { link_stream_feed_ring(&stream, &rx, SIZE_MAX); ... }
 *
 * Overflow: when the ring is full the newest byte is dropped and counted (gmsg_ring_dropped), and an overrun is
 * marked. From then on the producer drops every byte until the consumer has read everything that arrived before
 * the gap, so a ring holds at most one gap, always at its end. The stream that drains the ring delivers the
 * frames complete before the gap, drops the frame in progress and reports it as GMSG_ERR_OVERRUN.
 *
 * Options (define before including the runtime headers):
 *   GMSG_RING_INDEX_BITS  32 (default), 16 or 8: width of the free-running indices. Use 8 or 16 on targets where
 *                         a 32-bit load / store is not a single access (8- and 16-bit MCUs). The capacity must be
 *                         at most half the index range (128 for 8 bits, 32768 for 16 bits).
 *   GMSG_RING_ATOMICS     1: indices are C11 _Atomic objects (acquire / release). Default when compiling C11 or
 *                         later with <stdatomic.h> available.
 *                         0: volatile indices ordered by GMSG_RING_FENCE() (C99, compilers without atomics,
 *                         AVR). Default otherwise, and when the header is compiled as C++.
 *   GMSG_RING_FENCE()     memory barrier used when GMSG_RING_ATOMICS is 0. Default: __sync_synchronize() with
 *                         GCC / Clang; define it for other compilers (a compiler barrier is enough on a
 *                         single-core MCU where the producer is an interrupt).
 */
#ifndef GMSG_C_RING_H
#define GMSG_C_RING_H

#include "gmp/c/config.h"
#include "gmp/c/error.h"

#ifndef GMSG_RING_INDEX_BITS
#define GMSG_RING_INDEX_BITS 32
#endif
#if GMSG_RING_INDEX_BITS == 32
typedef uint32_t gmsg_ring_index;
#elif GMSG_RING_INDEX_BITS == 16
typedef uint16_t gmsg_ring_index;
#elif GMSG_RING_INDEX_BITS == 8
typedef uint8_t gmsg_ring_index;
#else
#error "GMSG_RING_INDEX_BITS must be 8, 16 or 32"
#endif

#ifndef GMSG_RING_ATOMICS
#if !defined(__cplusplus) && GMSG_C11 && !defined(__STDC_NO_ATOMICS__)
#define GMSG_RING_ATOMICS 1
#else
#define GMSG_RING_ATOMICS 0
#endif
#endif

#if GMSG_RING_ATOMICS
#include <stdatomic.h>
typedef _Atomic gmsg_ring_index gmsg_ring_atomic_;
typedef _Atomic uint32_t gmsg_ring_atomic32_;
#else
#ifndef GMSG_RING_FENCE
#if defined(__GNUC__) || defined(__clang__)
#define GMSG_RING_FENCE() __sync_synchronize()
#else
#error "gmp/c/ring.h: define GMSG_RING_FENCE() (a memory barrier) or use C11 atomics (GMSG_RING_ATOMICS=1)"
#endif
#endif
typedef volatile gmsg_ring_index gmsg_ring_atomic_;
typedef volatile uint32_t gmsg_ring_atomic32_;
#endif

GMSG_C_BEGIN

typedef struct gmsg_ring {
    uint8_t* buf;
    size_t capacity;              /* a power of two */
    gmsg_ring_atomic_ w;          /* written by the producer */
    gmsg_ring_atomic_ r;          /* written by the consumer */
    gmsg_ring_atomic_ overrun;    /* 1: input was dropped after the buffered bytes (set by the producer) */
    gmsg_ring_atomic32_ dropped;  /* bytes dropped (producer) */
    gmsg_ring_atomic_ high_water; /* highest fill level (producer) */
} gmsg_ring;

/* ---- index access ------------------------------------------------------------------------------------------ */

#if GMSG_RING_ATOMICS
GMSG_PRIV_ gmsg_ring_index gmsg_ring_acquire_(const gmsg_ring_atomic_* p) {
    return atomic_load_explicit((gmsg_ring_atomic_*)p, memory_order_acquire);
}
GMSG_PRIV_ gmsg_ring_index gmsg_ring_relaxed_(const gmsg_ring_atomic_* p) {
    return atomic_load_explicit((gmsg_ring_atomic_*)p, memory_order_relaxed);
}
GMSG_PRIV_ void gmsg_ring_release_(gmsg_ring_atomic_* p, gmsg_ring_index v) {
    atomic_store_explicit(p, v, memory_order_release);
}
GMSG_PRIV_ void gmsg_ring_set_(gmsg_ring_atomic_* p, gmsg_ring_index v) {
    atomic_store_explicit(p, v, memory_order_relaxed);
}
GMSG_PRIV_ uint32_t gmsg_ring_get32_(const gmsg_ring_atomic32_* p) {
    return atomic_load_explicit((gmsg_ring_atomic32_*)p, memory_order_relaxed);
}
GMSG_PRIV_ void gmsg_ring_set32_(gmsg_ring_atomic32_* p, uint32_t v) { atomic_store_explicit(p, v, memory_order_relaxed); }
#else
GMSG_PRIV_ gmsg_ring_index gmsg_ring_acquire_(const gmsg_ring_atomic_* p) {
    const gmsg_ring_index v = *p;
    GMSG_RING_FENCE();
    return v;
}
GMSG_PRIV_ gmsg_ring_index gmsg_ring_relaxed_(const gmsg_ring_atomic_* p) { return *p; }
GMSG_PRIV_ void gmsg_ring_release_(gmsg_ring_atomic_* p, gmsg_ring_index v) {
    GMSG_RING_FENCE();
    *p = v;
}
GMSG_PRIV_ void gmsg_ring_set_(gmsg_ring_atomic_* p, gmsg_ring_index v) { *p = v; }
GMSG_PRIV_ uint32_t gmsg_ring_get32_(const gmsg_ring_atomic32_* p) { return *p; }
GMSG_PRIV_ void gmsg_ring_set32_(gmsg_ring_atomic32_* p, uint32_t v) { *p = v; }
#endif

/* ---- set-up ---------------------------------------------------------------------------------------------------- */

/* Use buf[0..capacity) as the ring storage. GMSG_ERR_OUT_OF_RANGE unless capacity is a power of two >= 2 and at
 * most half the index range (see GMSG_RING_INDEX_BITS). Call before the producer starts. */
GMSG_INLINE gmsg_error gmsg_ring_init(gmsg_ring* r, uint8_t* buf, size_t capacity) {
    const size_t max_capacity = (size_t)1 << (GMSG_RING_INDEX_BITS - 1);
    r->buf = buf;
    r->capacity = 0;
    gmsg_ring_set_(&r->w, 0);
    gmsg_ring_set_(&r->r, 0);
    gmsg_ring_set_(&r->overrun, 0);
    gmsg_ring_set32_(&r->dropped, 0);
    gmsg_ring_set_(&r->high_water, 0);
    if (buf == NULL || capacity < 2 || (capacity & (capacity - 1)) != 0 || capacity > max_capacity)
        return GMSG_ERR_OUT_OF_RANGE;
    r->capacity = capacity;
    return GMSG_OK;
}

GMSG_INLINE size_t gmsg_ring_capacity(const gmsg_ring* r) { return r->capacity; }

/* ---- producer (interrupt) side ---------------------------------------------------------------------------------- */

GMSG_PRIV_ bool gmsg_ring_drop_(gmsg_ring* r, size_t n) {
    gmsg_ring_set32_(&r->dropped, gmsg_ring_get32_(&r->dropped) + (uint32_t)n);
    return false;
}

GMSG_PRIV_ void gmsg_ring_note_fill_(gmsg_ring* r, gmsg_ring_index fill) {
    if (fill > gmsg_ring_relaxed_(&r->high_water)) gmsg_ring_set_(&r->high_water, fill);
}

/* Append one byte. Returns false (and drops it) when the ring is full or an overrun is waiting to be drained. */
GMSG_INLINE bool gmsg_ring_push(gmsg_ring* r, uint8_t b) {
    gmsg_ring_index w, rd;
    if (gmsg_ring_acquire_(&r->overrun)) return gmsg_ring_drop_(r, 1);
    w = gmsg_ring_relaxed_(&r->w);
    rd = gmsg_ring_acquire_(&r->r);
    if ((size_t)(gmsg_ring_index)(w - rd) == r->capacity) {
        gmsg_ring_drop_(r, 1);
        gmsg_ring_release_(&r->overrun, 1); /* after the last w store: the consumer sees every byte before it */
        return false;
    }
    r->buf[w & (r->capacity - 1)] = b;
    gmsg_ring_release_(&r->w, (gmsg_ring_index)(w + 1));
    gmsg_ring_note_fill_(r, (gmsg_ring_index)(w + 1 - rd));
    return true;
}

/* Append as many of data[0..n) as fit; the rest are dropped (an overrun). Returns the number appended. */
GMSG_INLINE size_t gmsg_ring_push_n(gmsg_ring* r, const uint8_t* data, size_t n) {
    gmsg_ring_index w, rd;
    size_t room, k, i;
    if (n == 0) return 0;
    if (gmsg_ring_acquire_(&r->overrun)) {
        gmsg_ring_drop_(r, n);
        return 0;
    }
    w = gmsg_ring_relaxed_(&r->w);
    rd = gmsg_ring_acquire_(&r->r);
    room = r->capacity - (size_t)(gmsg_ring_index)(w - rd);
    k = n < room ? n : room;
    for (i = 0; i < k; ++i) r->buf[(gmsg_ring_index)(w + i) & (r->capacity - 1)] = data[i];
    if (k > 0) {
        gmsg_ring_release_(&r->w, (gmsg_ring_index)(w + k));
        gmsg_ring_note_fill_(r, (gmsg_ring_index)(w + k - rd));
    }
    if (k < n) {
        gmsg_ring_drop_(r, n - k);
        gmsg_ring_release_(&r->overrun, 1);
    }
    return k;
}

/* Free room for gmsg_ring_push_n (producer side): bytes that can be appended now without an overrun. */
GMSG_INLINE size_t gmsg_ring_room(const gmsg_ring* r) {
    if (gmsg_ring_acquire_(&r->overrun)) return 0;
    return r->capacity - (size_t)(gmsg_ring_index)(gmsg_ring_relaxed_(&r->w) - gmsg_ring_acquire_(&r->r));
}

/* ---- consumer side ------------------------------------------------------------------------------------------- */

/* The oldest buffered bytes that are contiguous in storage (all of them, or the part up to the end of the storage
 * when they wrap): returns a pointer and sets *n (0 when empty). Valid until gmsg_ring_consume. */
GMSG_INLINE const uint8_t* gmsg_ring_readable(const gmsg_ring* r, size_t* n) {
    const gmsg_ring_index w = gmsg_ring_acquire_(&r->w);
    const gmsg_ring_index rd = gmsg_ring_relaxed_(&r->r);
    const size_t avail = (size_t)(gmsg_ring_index)(w - rd);
    const size_t start = rd & (r->capacity - 1);
    *n = avail < r->capacity - start ? avail : r->capacity - start;
    return r->buf + start;
}

/* Release the first n bytes of gmsg_ring_readable to the producer. */
GMSG_INLINE void gmsg_ring_consume(gmsg_ring* r, size_t n) {
    gmsg_ring_release_(&r->r, (gmsg_ring_index)(gmsg_ring_relaxed_(&r->r) + n));
}

/* Copy up to n bytes out (raw use without a stream). Returns the number copied. An overrun is not cleared here:
 * check gmsg_ring_overrun_pending and call gmsg_ring_clear_overrun once the ring is empty. */
GMSG_INLINE size_t gmsg_ring_read(gmsg_ring* r, uint8_t* out, size_t n) {
    size_t total = 0;
    while (total < n) {
        size_t avail, k, i;
        const uint8_t* part = gmsg_ring_readable(r, &avail);
        if (avail == 0) break;
        k = avail < n - total ? avail : n - total;
        for (i = 0; i < k; ++i) out[total + i] = part[i];
        gmsg_ring_consume(r, k);
        total += k;
    }
    return total;
}

/* Bytes buffered (consumer side; the producer may add more at any time). */
GMSG_INLINE size_t gmsg_ring_size(const gmsg_ring* r) {
    return (size_t)(gmsg_ring_index)(gmsg_ring_acquire_(&r->w) - gmsg_ring_relaxed_(&r->r));
}
GMSG_INLINE bool gmsg_ring_empty(const gmsg_ring* r) { return gmsg_ring_size(r) == 0; }

/* True when bytes were dropped after the buffered ones; the producer drops input until gmsg_ring_clear_overrun. */
GMSG_INLINE bool gmsg_ring_overrun_pending(const gmsg_ring* r) { return gmsg_ring_acquire_(&r->overrun) != 0; }
/* Accept new input again. Call only when the ring is empty (every byte before the gap has been read). */
GMSG_INLINE void gmsg_ring_clear_overrun(gmsg_ring* r) { gmsg_ring_release_(&r->overrun, 0); }

/* Bytes dropped because the ring was full (wraps). */
GMSG_INLINE uint32_t gmsg_ring_dropped(const gmsg_ring* r) { return gmsg_ring_get32_(&r->dropped); }
/* Highest fill level seen by the producer: size the ring from it. */
GMSG_INLINE size_t gmsg_ring_high_water(const gmsg_ring* r) { return gmsg_ring_relaxed_(&r->high_water); }

GMSG_C_END

#endif /* GMSG_C_RING_H */
