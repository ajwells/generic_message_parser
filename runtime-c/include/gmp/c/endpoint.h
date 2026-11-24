/* gmp/c/endpoint.h - a protocol stream joined to a byte source, a byte sink and a clock (port of gmsg::Endpoint).
 *
 * A byte source reads input that has arrived (never blocking), a byte sink sends output:
 *
 *   gmsg_source  { read(ctx, out, n) -> bytes read (0: nothing yet), take_overrun(ctx) (optional), ctx, ring }
 *   gmsg_sink    { write(ctx, data, n) -> bytes taken, writable(ctx) (optional: room right now), ctx, ring }
 *
 * Set `ring` instead of the functions to drain a gmsg_ring filled by an RX interrupt (overruns included) or to
 * queue output in a ring a TX interrupt sends from. Adapters: gmsg_source_ring / gmsg_sink_ring,
 * gmsg_source_fn / gmsg_sink_fn, gmsg_buffer_source / gmsg_buffer_sink (memory), gmsg_source_file /
 * gmsg_sink_file (FILE*, with GMSG_USE_LIBC) and gmsg_dma_source (a circular DMA receive buffer).
 *
 * Generated protocols wrap gmsg_endpoint in <protocol>_endpoint (the protocol stream, the I/O and a transmit
 * buffer of the protocol's max frame) with _service, _send_<message>, _send, _send_raw, _wait_for and _transact.
 * The generic part here:
 *
 *   gmsg_endpoint_set_clock(e, fn, ctx)     fn(ctx) -> any monotonic uint32 time (wraparound is fine); timeouts
 *                                           are in its unit. Without a clock a wait looks once (timeout 0).
 *   gmsg_endpoint_set_idle(e, fn, ctx)      fn(ctx) runs whenever a wait or a retried send has nothing to do
 *   gmsg_endpoint_set_send_policy(e, p, t)  GMSG_SEND_WHOLE_FRAME (default: all or nothing; a sink whose
 *                                           writable() is too small fails with BUFFER_FULL and nothing is
 *                                           written; a sink without writable() is assumed to take whole frames)
 *                                           or GMSG_SEND_RETRY (write the rest until it is out, or TIMEOUT after
 *                                           t; GMSG_FOREVER: no limit)
 *   gmsg_endpoint_service(e, max)           read what the source has (at most max bytes), deliver every frame to
 *                                           the callbacks, run the idle resync (with a clock)
 *   gmsg_endpoint_write(e, data, n)         write bytes with the send policy
 *   gmsg_endpoint_wait(e, timeout, pull, ctx)
 *                                           the wait loop of the generated _wait_for: pull(ctx) until it returns
 *                                           true, reading input, ticking and idling in between; TIMEOUT after
 *                                           `timeout`
 *
 * Single-threaded: only the source and sink may be fed from interrupts (gmsg_ring, gmsg_dma_source_on_half).
 */
#ifndef GMSG_C_ENDPOINT_H
#define GMSG_C_ENDPOINT_H

#include "gmp/c/config.h"
#include "gmp/c/error.h"
#include "gmp/c/ring.h"
#include "gmp/c/stream.h"

#if GMSG_USE_LIBC
#include <stdio.h>
#endif

GMSG_C_BEGIN

#define GMSG_FOREVER 0xFFFFFFFFu /* no time limit (gmsg_endpoint_set_send_policy) */

typedef struct gmsg_source {
    size_t (*read)(void* ctx, uint8_t* out, size_t n); /* copy up to n bytes that have arrived; never block */
    bool (*take_overrun)(void* ctx);                    /* optional: true once when input was lost before the next read */
    void* ctx;
    gmsg_ring* ring; /* instead of read: drain this ring */
} gmsg_source;

typedef struct gmsg_sink {
    size_t (*write)(void* ctx, const uint8_t* data, size_t n); /* send what it can now; returns bytes taken */
    size_t (*writable)(void* ctx);                             /* optional: room for write right now */
    void* ctx;
    gmsg_ring* ring; /* instead of write: push into this ring (for a TX interrupt) */
} gmsg_sink;

typedef uint32_t (*gmsg_clock_fn)(void* ctx);

typedef enum gmsg_send_policy {
    GMSG_SEND_WHOLE_FRAME = 0, /* all or nothing: BUFFER_FULL (nothing written) when the sink cannot take it now */
    GMSG_SEND_RETRY = 1        /* write the rest until it is out, or TIMEOUT after the send timeout */
} gmsg_send_policy;

/* State of a generated <protocol>_request (requests that do not block). */
typedef enum gmsg_request_state {
    GMSG_REQUEST_IDLE = 0,    /* not started, or cancelled */
    GMSG_REQUEST_WAITING = 1, /* sent (or expected), waiting for the reply */
    GMSG_REQUEST_DONE = 2     /* finished: GMSG_OK with the reply, or GMSG_ERR_TIMEOUT */
} gmsg_request_state;

typedef struct gmsg_endpoint {
    gmsg_stream* stream;
    gmsg_source source;
    gmsg_sink sink;
    gmsg_clock_fn clock;
    void* clock_ctx;
    void (*idle)(void* ctx);
    void* idle_ctx;
    gmsg_send_policy policy;
    uint32_t send_timeout; /* GMSG_FOREVER: no limit */
    /* set by the generated endpoint: after every stream step (run finished requests' callbacks), and after every
     * tick (time requests out); NULL when unused */
    void (*on_step)(struct gmsg_endpoint* e);
    void (*on_tick)(struct gmsg_endpoint* e);
    int depth; /* nested service / waits (a request started from a callback does not drain input first) */
} gmsg_endpoint;

/* ---- adapters -------------------------------------------------------------------------------------------------- */

GMSG_INLINE gmsg_source gmsg_source_ring(gmsg_ring* ring) {
    gmsg_source s;
    s.read = NULL;
    s.take_overrun = NULL;
    s.ctx = NULL;
    s.ring = ring;
    return s;
}
GMSG_INLINE gmsg_sink gmsg_sink_ring(gmsg_ring* ring) {
    gmsg_sink k;
    k.write = NULL;
    k.writable = NULL;
    k.ctx = NULL;
    k.ring = ring;
    return k;
}
GMSG_INLINE gmsg_source gmsg_source_fn(size_t (*read)(void* ctx, uint8_t* out, size_t n), void* ctx) {
    gmsg_source s;
    s.read = read;
    s.take_overrun = NULL;
    s.ctx = ctx;
    s.ring = NULL;
    return s;
}
GMSG_INLINE gmsg_sink gmsg_sink_fn(size_t (*write)(void* ctx, const uint8_t* data, size_t n), void* ctx) {
    gmsg_sink k;
    k.write = write;
    k.writable = NULL;
    k.ctx = ctx;
    k.ring = NULL;
    return k;
}

/* Memory source (a capture, a test vector): at most `chunk` bytes per read (0 = no limit). */
typedef struct gmsg_buffer_source {
    const uint8_t* data;
    size_t size;
    size_t pos;
    size_t chunk;
} gmsg_buffer_source;

GMSG_PRIV_ size_t gmsg_buffer_source_read_(void* ctx, uint8_t* out, size_t n) {
    gmsg_buffer_source* b = (gmsg_buffer_source*)ctx;
    size_t k = b->size - b->pos;
    if (k > n) k = n;
    if (b->chunk != 0 && k > b->chunk) k = b->chunk;
    if (k > 0) gmsg_memcpy(out, b->data + b->pos, k);
    b->pos += k;
    return k;
}
GMSG_INLINE gmsg_source gmsg_buffer_source_init(gmsg_buffer_source* b, const uint8_t* data, size_t size, size_t chunk) {
    b->data = data;
    b->size = size;
    b->pos = 0;
    b->chunk = chunk;
    return gmsg_source_fn(gmsg_buffer_source_read_, b);
}

/* Memory sink: written bytes are buf[0..used). */
typedef struct gmsg_buffer_sink {
    uint8_t* buf;
    size_t capacity;
    size_t used;
} gmsg_buffer_sink;

GMSG_PRIV_ size_t gmsg_buffer_sink_writable_(void* ctx) {
    const gmsg_buffer_sink* b = (const gmsg_buffer_sink*)ctx;
    return b->capacity - b->used;
}
GMSG_PRIV_ size_t gmsg_buffer_sink_write_(void* ctx, const uint8_t* data, size_t n) {
    gmsg_buffer_sink* b = (gmsg_buffer_sink*)ctx;
    size_t k = b->capacity - b->used;
    if (k > n) k = n;
    if (k > 0) gmsg_memcpy(b->buf + b->used, data, k);
    b->used += k;
    return k;
}
GMSG_INLINE gmsg_sink gmsg_buffer_sink_init(gmsg_buffer_sink* b, uint8_t* buf, size_t capacity) {
    gmsg_sink k;
    b->buf = buf;
    b->capacity = capacity;
    b->used = 0;
    k.write = gmsg_buffer_sink_write_;
    k.writable = gmsg_buffer_sink_writable_;
    k.ctx = b;
    k.ring = NULL;
    return k;
}

#if GMSG_USE_LIBC
GMSG_PRIV_ size_t gmsg_file_read_(void* ctx, uint8_t* out, size_t n) { return n == 0 ? 0 : fread(out, 1, n, (FILE*)ctx); }
GMSG_PRIV_ size_t gmsg_file_write_(void* ctx, const uint8_t* data, size_t n) {
    const size_t k = n == 0 ? 0 : fwrite(data, 1, n, (FILE*)ctx);
    fflush((FILE*)ctx);
    return k;
}
/* Reads a FILE* (e.g. a recorded capture). fread blocks on pipes and terminals: use gmsg_source_fn for live input. */
GMSG_INLINE gmsg_source gmsg_source_file(FILE* f) { return gmsg_source_fn(gmsg_file_read_, f); }
/* Writes to a FILE* and flushes after every frame. */
GMSG_INLINE gmsg_sink gmsg_sink_file(FILE* f) { return gmsg_sink_fn(gmsg_file_write_, f); }
#endif

/* Circular DMA receive buffer of `size` bytes (even) that the hardware fills continuously. pos(ctx) returns the
 * index the DMA writes next (0..size-1), e.g. size - DMA counter. Start the DMA at index 0 before the first read,
 * or call gmsg_dma_source_restart once it runs. With track_laps, the DMA half-transfer and transfer-complete
 * interrupts call gmsg_dma_source_on_half: a lap over unread input is then reported as an overrun (and bytes
 * overwritten while they were copied are dropped). Without it the reader must read at least once per `size`
 * bytes; a lap is invisible. The half counter is 8 bits (a single access on every target): read at least once
 * per 127 laps. */
typedef struct gmsg_dma_source {
    const volatile uint8_t* buf;
    size_t size;
    size_t (*pos)(void* ctx);
    void* pos_ctx;
    bool track;
    volatile uint8_t halves; /* written by the DMA interrupts */
    uint8_t h;               /* halves at the last sample */
    size_t k;                /* bytes past the half boundary at the last sample */
    size_t p;                /* write index at the last sample */
    size_t r;                /* read index */
    size_t unread;
    bool lost;
    uint32_t overruns;
} gmsg_dma_source;

/* From the DMA half-transfer and transfer-complete interrupts (lap tracking). */
GMSG_INLINE void gmsg_dma_source_on_half(gmsg_dma_source* d) {
    d->halves = (uint8_t)(d->halves + 1u);
}

GMSG_API gmsg_source gmsg_dma_source_init(gmsg_dma_source* d, const volatile uint8_t* buf, size_t size,
                                          size_t (*pos)(void* ctx), void* pos_ctx, bool track_laps);
/* Forget everything buffered and continue from the DMA's current position. */
GMSG_API void gmsg_dma_source_restart(gmsg_dma_source* d);
GMSG_API size_t gmsg_dma_source_read(void* d, uint8_t* out, size_t n);
GMSG_API bool gmsg_dma_source_take_overrun(void* d);

/* ---- endpoint -------------------------------------------------------------------------------------------------- */

/* Join `stream` to a source and a sink (NULL: none; a NULL sink fails every send with BUFFER_FULL). */
GMSG_API void gmsg_endpoint_init(gmsg_endpoint* e, gmsg_stream* stream, const gmsg_source* source, const gmsg_sink* sink);
GMSG_API void gmsg_endpoint_set_clock(gmsg_endpoint* e, gmsg_clock_fn clock, void* ctx);
GMSG_API void gmsg_endpoint_set_idle(gmsg_endpoint* e, void (*idle)(void* ctx), void* ctx);
GMSG_API void gmsg_endpoint_set_send_policy(gmsg_endpoint* e, gmsg_send_policy policy, uint32_t timeout);
/* The clock's time (0 without a clock). */
GMSG_API uint32_t gmsg_endpoint_now(const gmsg_endpoint* e);
/* Move source bytes into the stream without processing them (at most max). Returns bytes moved. */
GMSG_API size_t gmsg_endpoint_read(gmsg_endpoint* e, size_t max);
/* Read and deliver everything available (at most max bytes; SIZE_MAX: all). Returns frames delivered. */
GMSG_API size_t gmsg_endpoint_service(gmsg_endpoint* e, size_t max);
/* Write data[0..n) to the sink with the send policy. */
GMSG_API gmsg_error gmsg_endpoint_write(gmsg_endpoint* e, const uint8_t* data, size_t n);
/* Call pull(ctx) until it returns true (GMSG_OK), reading input, ticking the idle resync and idling in between;
 * GMSG_ERR_TIMEOUT after `timeout` (clock units). */
GMSG_API gmsg_error gmsg_endpoint_wait(gmsg_endpoint* e, uint32_t timeout, bool (*pull)(void* ctx), void* ctx);

/* ---- implementation (header-only build, or runtime-c/src/endpoint.c of the library build) ---------------------- */

#if GMSG_DEFINE_FUNCTIONS_ || defined(GMSG_IMPL_ENDPOINT_)

GMSG_PRIV_ size_t gmsg_dma_index_(gmsg_dma_source* d) { return d->pos(d->pos_ctx) % d->size; }
GMSG_PRIV_ size_t gmsg_dma_base_(const gmsg_dma_source* d, uint8_t halves) { return (halves & 1u) ? d->size / 2 : 0; }

/* Add the bytes written since the last sample to `unread`. */
GMSG_PRIV_ void gmsg_dma_sample_(gmsg_dma_source* d) {
    size_t delta;
    const size_t cap = 4 * d->size;
    if (d->track) {
        const uint8_t h = d->halves;
        const size_t k = (gmsg_dma_index_(d) + d->size - gmsg_dma_base_(d, h)) % d->size;
        const size_t whole = (size_t)(uint8_t)(h - d->h) * (d->size / 2);
        delta = whole + k >= d->k ? whole + k - d->k : 0;
        if (delta > cap) delta = cap;
        d->h = h;
        d->k = k;
        d->p = (gmsg_dma_base_(d, h) + k) % d->size;
    } else {
        const size_t p = gmsg_dma_index_(d);
        delta = (p + d->size - d->p) % d->size;
        d->p = p;
    }
    d->unread += delta;
    if (d->unread > cap) d->unread = cap;
}

GMSG_PRIV_ size_t gmsg_dma_overrun_(gmsg_dma_source* d) {
    d->lost = true;
    ++d->overruns;
    d->r = d->p; /* continue at the write position: everything buffered is suspect */
    d->unread = 0;
    return 0;
}

GMSG_API void gmsg_dma_source_restart(gmsg_dma_source* d) {
    const size_t p = gmsg_dma_index_(d);
    d->h = d->halves;
    d->k = (p + d->size - gmsg_dma_base_(d, d->h)) % d->size;
    d->p = p;
    d->r = p;
    d->unread = 0;
    d->lost = false;
}

GMSG_API gmsg_source gmsg_dma_source_init(gmsg_dma_source* d, const volatile uint8_t* buf, size_t size,
                                          size_t (*pos)(void* ctx), void* pos_ctx, bool track_laps) {
    gmsg_source s;
    gmsg_memset((void*)d, 0, sizeof *d);
    d->buf = buf;
    d->size = size;
    d->pos = pos;
    d->pos_ctx = pos_ctx;
    d->track = track_laps;
    s.read = gmsg_dma_source_read;
    s.take_overrun = gmsg_dma_source_take_overrun;
    s.ctx = d;
    s.ring = NULL;
    return s;
}

GMSG_API size_t gmsg_dma_source_read(void* ctx, uint8_t* out, size_t n) {
    gmsg_dma_source* d = (gmsg_dma_source*)ctx;
    size_t k, i;
    if (d->lost) return 0; /* take_overrun first */
    gmsg_dma_sample_(d);
    if (d->unread > d->size) return gmsg_dma_overrun_(d);
    k = n < d->unread ? n : d->unread;
    for (i = 0; i < k; ++i) out[i] = d->buf[(d->r + i) % d->size];
    if (d->track) {
        gmsg_dma_sample_(d);
        if (d->unread > d->size) return gmsg_dma_overrun_(d); /* the DMA overwrote bytes while they were copied */
    }
    d->r = (d->r + k) % d->size;
    d->unread -= k;
    return k;
}

GMSG_API bool gmsg_dma_source_take_overrun(void* ctx) {
    gmsg_dma_source* d = (gmsg_dma_source*)ctx;
    const bool l = d->lost;
    d->lost = false;
    return l;
}

GMSG_API void gmsg_endpoint_init(gmsg_endpoint* e, gmsg_stream* stream, const gmsg_source* source, const gmsg_sink* sink) {
    gmsg_memset(e, 0, sizeof *e);
    e->stream = stream;
    if (source) e->source = *source;
    if (sink) e->sink = *sink;
    e->policy = GMSG_SEND_WHOLE_FRAME;
    e->send_timeout = GMSG_FOREVER;
}

GMSG_API void gmsg_endpoint_set_clock(gmsg_endpoint* e, gmsg_clock_fn clock, void* ctx) {
    e->clock = clock;
    e->clock_ctx = ctx;
}

GMSG_API void gmsg_endpoint_set_idle(gmsg_endpoint* e, void (*idle)(void* ctx), void* ctx) {
    e->idle = idle;
    e->idle_ctx = ctx;
}

GMSG_API void gmsg_endpoint_set_send_policy(gmsg_endpoint* e, gmsg_send_policy policy, uint32_t timeout) {
    e->policy = policy;
    e->send_timeout = timeout;
}

GMSG_PRIV_ uint32_t gmsg_endpoint_now_(const gmsg_endpoint* e) { return e->clock ? e->clock(e->clock_ctx) : 0; }
GMSG_API uint32_t gmsg_endpoint_now(const gmsg_endpoint* e) { return gmsg_endpoint_now_(e); }
GMSG_PRIV_ void gmsg_endpoint_step_hook_(gmsg_endpoint* e) {
    if (e->on_step) e->on_step(e);
}
GMSG_PRIV_ void gmsg_endpoint_tick_(gmsg_endpoint* e) {
    if (!e->clock) return;
    (void)gmsg_stream_tick(e->stream, gmsg_endpoint_now_(e));
    if (e->on_tick) e->on_tick(e);
}
GMSG_PRIV_ bool gmsg_endpoint_elapsed_(const gmsg_endpoint* e, uint32_t start, uint32_t limit) {
    if (!e->clock) return true;
    return (uint32_t)(gmsg_endpoint_now_(e) - start) >= limit;
}
GMSG_PRIV_ void gmsg_endpoint_idle_(const gmsg_endpoint* e) {
    if (e->idle) e->idle(e->idle_ctx);
}

GMSG_API size_t gmsg_endpoint_read(gmsg_endpoint* e, size_t max) {
    size_t k;
    if (e->source.ring) return gmsg_stream_push_ring(e->stream, e->source.ring, max);
    if (!e->source.read) return 0;
    if (e->source.take_overrun && e->source.take_overrun(e->source.ctx)) (void)gmsg_stream_push_gap(e->stream);
    k = gmsg_stream_push_from(e->stream, e->source.read, e->source.ctx, max);
    if (k == 0 && e->source.take_overrun && e->source.take_overrun(e->source.ctx)) {
        (void)gmsg_stream_push_gap(e->stream); /* the read found the loss: mark it now, then read what follows */
        k = gmsg_stream_push_from(e->stream, e->source.read, e->source.ctx, max);
    }
    return k;
}

GMSG_API size_t gmsg_endpoint_service(gmsg_endpoint* e, size_t max) {
    const uint32_t before = e->stream->stats.frames;
    size_t total = 0;
    ++e->depth;
    while (gmsg_stream_next(e->stream)) gmsg_endpoint_step_hook_(e);
    gmsg_endpoint_step_hook_(e);
    while (total < max) {
        const size_t k = gmsg_endpoint_read(e, max - total);
        total += k;
        while (gmsg_stream_next(e->stream)) gmsg_endpoint_step_hook_(e); /* also reports an overrun the read took over */
        gmsg_endpoint_step_hook_(e);
        if (k == 0) break;
    }
    gmsg_endpoint_tick_(e);
    --e->depth;
    return (size_t)(e->stream->stats.frames - before);
}

GMSG_PRIV_ size_t gmsg_endpoint_room_(const gmsg_endpoint* e) {
    if (e->sink.ring) return gmsg_ring_room(e->sink.ring);
    if (e->sink.writable) return e->sink.writable(e->sink.ctx);
    if (e->sink.write) return SIZE_MAX;
    return 0;
}

GMSG_PRIV_ size_t gmsg_endpoint_put_(const gmsg_endpoint* e, const uint8_t* data, size_t n) {
    size_t k;
    if (e->sink.ring) {
        k = gmsg_ring_room(e->sink.ring);
        return gmsg_ring_push_n(e->sink.ring, data, n < k ? n : k);
    }
    if (!e->sink.write) return 0;
    k = e->sink.write(e->sink.ctx, data, n);
    return k < n ? k : n;
}

GMSG_API gmsg_error gmsg_endpoint_write(gmsg_endpoint* e, const uint8_t* data, size_t n) {
    uint32_t start = 0;
    if (e->policy == GMSG_SEND_WHOLE_FRAME) {
        if (gmsg_endpoint_room_(e) < n) return GMSG_ERR_BUFFER_FULL;
        return gmsg_endpoint_put_(e, data, n) == n ? GMSG_OK : GMSG_ERR_BUFFER_FULL;
    }
    if (e->send_timeout != GMSG_FOREVER) start = gmsg_endpoint_now_(e);
    for (;;) {
        const size_t k = gmsg_endpoint_put_(e, data, n);
        data += k;
        n -= k;
        if (n == 0) return GMSG_OK;
        if (e->send_timeout != GMSG_FOREVER && gmsg_endpoint_elapsed_(e, start, e->send_timeout)) return GMSG_ERR_TIMEOUT;
        gmsg_endpoint_idle_(e);
    }
}

GMSG_API gmsg_error gmsg_endpoint_wait(gmsg_endpoint* e, uint32_t timeout, bool (*pull)(void* ctx), void* ctx) {
    const uint32_t start = gmsg_endpoint_now_(e);
    bool expired = false; /* after the limit: one last look at what the final read brought in */
    gmsg_error result;
    ++e->depth;
    for (;;) {
        size_t k;
        if (pull(ctx)) {
            result = GMSG_OK;
            break;
        }
        if (expired) {
            result = GMSG_ERR_TIMEOUT;
            break;
        }
        k = gmsg_endpoint_read(e, SIZE_MAX);
        gmsg_endpoint_tick_(e);
        if (gmsg_endpoint_elapsed_(e, start, timeout)) expired = true;
        else if (k == 0) gmsg_endpoint_idle_(e);
    }
    --e->depth;
    return result;
}

#endif /* implementation */

GMSG_C_END

#endif /* GMSG_C_ENDPOINT_H */
