/* gmp/c/stream.h - incremental frame extraction from a byte stream (port of gmp::StreamParser).
 *
 * The caller owns the buffer (any size >= the protocol's max frame). Generated protocols wrap gmsg_stream in a
 * struct whose first member is the gmsg_stream, and provide the scan / process / on_error functions; the
 * functions receive the gmsg_stream pointer, so the wrapper can be found by a cast (no stored self pointer, so
 * copying an idle stream is harmless). Behaviour, statistics and resync match the C++ StreamParser:
 *
 *   gmsg_stream_push(s, data, n)   copy as much as fits; returns bytes accepted
 *   gmsg_stream_next(s)            one step: deliver one frame or report one error; false when more input is needed
 *   gmsg_stream_feed(s, data, n)   push everything and process every complete frame; returns frames delivered
 *   gmsg_stream_step(s)            one step without reporting errors (pull API: the caller handles the result)
 *   gmsg_stream_pull(s)            step until a frame was accepted (true) or more input is needed; errors go to
 *                                  on_error. The accepted frame stays buffered until the next call.
 *
 * Views handed to callbacks point into the buffer and are valid only during the callback. Views returned by the
 * pull API stay valid until the next call that changes the stream (push / feed / next / step / pull / reset);
 * gmsg_stream_generation() changes whenever that happens, so callers can check a view is still current.
 */
#ifndef GMSG_C_STREAM_H
#define GMSG_C_STREAM_H

#include "gmp/c/config.h"
#include "gmp/c/error.h"
#include "gmp/c/framing.h"
#include "gmp/c/mem.h"

GMSG_C_BEGIN

typedef struct gmsg_stream_stats {
    uint32_t frames;         /* frames delivered successfully */
    uint32_t frame_errors;   /* frames rejected (checksum, unknown message type, decode error) */
    uint32_t bytes_skipped;  /* bytes discarded while resynchronising */
    uint32_t overflows;      /* times the buffer filled without a complete frame */
} gmsg_stream_stats;

struct gmsg_stream;
typedef gmsg_frame_scan (*gmsg_stream_scan_fn)(const uint8_t* buf, size_t n);
/* Validate a complete frame and deliver its message; returns GMSG_OK or the reason it was rejected. */
typedef gmsg_error (*gmsg_stream_process_fn)(struct gmsg_stream* s, uint8_t* frame, size_t n);
/* Dropped bytes / rejected frame (data valid only during the call). May be NULL. */
typedef void (*gmsg_stream_error_fn)(struct gmsg_stream* s, gmsg_error e, const uint8_t* data, size_t n);

typedef struct gmsg_stream {
    uint8_t* buf;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t held;       /* bytes of the frame kept for the current step (released on the next call) */
    bool holding;
    uint32_t generation;  /* changes whenever buffered frame bytes are released (expires pulled views) */
    gmsg_error last_error;
    gmsg_stream_stats stats;
    gmsg_stream_scan_fn scan;
    gmsg_stream_process_fn process;
    gmsg_stream_error_fn on_error;
} gmsg_stream;

/* GMSG_ERR_BUFFER_FULL when capacity < min_capacity (the protocol's max frame). */
GMSG_API gmsg_error gmsg_stream_init(gmsg_stream* s, uint8_t* buf, size_t capacity, size_t min_capacity,
                                     gmsg_stream_scan_fn scan, gmsg_stream_process_fn process, gmsg_stream_error_fn on_error);

GMSG_API void gmsg_stream_reset(gmsg_stream* s);
GMSG_API size_t gmsg_stream_buffered(const gmsg_stream* s);

/* Changes whenever the bytes a pulled view points into are released. */
GMSG_API uint32_t gmsg_stream_generation(const gmsg_stream* s);

/* Copy as much of data[0..n) as fits. Returns bytes accepted. */
GMSG_API size_t gmsg_stream_push(gmsg_stream* s, const uint8_t* data, size_t n);

/* Result of a generated <protocol>_stream_poll_event(). */
typedef enum gmsg_poll { GMSG_POLL_NEED_MORE = 0, GMSG_POLL_MESSAGE = 1, GMSG_POLL_ERROR = 2 } gmsg_poll;

typedef enum gmsg_step_kind { GMSG_STEP_NEED_MORE = 0, GMSG_STEP_FRAME = 1, GMSG_STEP_ERROR = 2 } gmsg_step_kind;

typedef struct gmsg_step {
    gmsg_step_kind kind;
    gmsg_error error;
    const uint8_t* data;  /* the frame, or the dropped bytes (valid until the next call) */
    size_t size;
} gmsg_step;

/* Scan to the next frame or reportable error; a frame is validated (and delivered or captured) by s->process.
 * The frame / dropped bytes stay buffered until the next call. Silent skips are consumed directly. Errors are
 * counted and kept in last_error but not passed to on_error (gmsg_stream_next / gmsg_stream_pull do that). */
GMSG_API gmsg_step gmsg_stream_step(gmsg_stream* s);

/* Step until a frame is accepted (true) or more input is needed (false); errors go to on_error. The accepted
 * frame stays buffered until the next call. */
GMSG_API bool gmsg_stream_pull(gmsg_stream* s);

/* Deliver the next frame (or report dropped data). Returns false when more input is required. */
GMSG_API bool gmsg_stream_next(gmsg_stream* s);

/* Push data[0..n) and process every complete frame. Returns the number of frames delivered. */
GMSG_API size_t gmsg_stream_feed(gmsg_stream* s, const uint8_t* data, size_t n);

/* ---- implementation (header-only build, or runtime-c/src/stream.c of the library build) -------------------------------- */

#if GMSG_DEFINE_FUNCTIONS_ || defined(GMSG_IMPL_STREAM_)

GMSG_API gmsg_error gmsg_stream_init(gmsg_stream* s, uint8_t* buf, size_t capacity, size_t min_capacity,
                                     gmsg_stream_scan_fn scan, gmsg_stream_process_fn process, gmsg_stream_error_fn on_error) {
    gmsg_memset(s, 0, sizeof *s);
    s->scan = scan;
    s->process = process;
    s->on_error = on_error;
    if (buf == NULL || capacity == 0 || capacity < min_capacity) return GMSG_ERR_BUFFER_FULL;
    s->buf = buf;
    s->capacity = capacity;
    return GMSG_OK;
}

GMSG_API void gmsg_stream_reset(gmsg_stream* s) {
    ++s->generation;
    s->head = s->tail = 0;
    s->held = 0;
    s->holding = false;
    s->last_error = GMSG_OK;
    gmsg_memset(&s->stats, 0, sizeof s->stats);
}

GMSG_API size_t gmsg_stream_buffered(const gmsg_stream* s) { return s->tail - s->head; }

GMSG_API uint32_t gmsg_stream_generation(const gmsg_stream* s) { return s->generation; }

GMSG_PRIV_ void gmsg_stream_consume_(gmsg_stream* s, size_t n) {
    const size_t avail = s->tail - s->head;
    s->head += n < avail ? n : avail;
    if (s->head == s->tail) s->head = s->tail = 0;
}

GMSG_PRIV_ void gmsg_stream_hold_(gmsg_stream* s, size_t n) {
    s->held = n;
    s->holding = true;
}

GMSG_PRIV_ void gmsg_stream_release_(gmsg_stream* s) {
    if (!s->holding) return;
    gmsg_stream_consume_(s, s->held);
    s->held = 0;
    s->holding = false;
    ++s->generation;
}

GMSG_PRIV_ void gmsg_stream_compact_(gmsg_stream* s) {
    if (s->head == 0) return;
    gmsg_memmove(s->buf, s->buf + s->head, s->tail - s->head);
    s->tail -= s->head;
    s->head = 0;
}

GMSG_API size_t gmsg_stream_push(gmsg_stream* s, const uint8_t* data, size_t n) {
    size_t k;
    gmsg_stream_release_(s);
    if (s->head > 0 && (s->capacity - s->tail) < n) gmsg_stream_compact_(s);
    k = n < s->capacity - s->tail ? n : s->capacity - s->tail;
    gmsg_memcpy(s->buf + s->tail, data, k);
    s->tail += k;
    return k;
}

GMSG_PRIV_ gmsg_step gmsg_step_make_(gmsg_step_kind k, gmsg_error e, const uint8_t* data, size_t size) {
    gmsg_step st;
    st.kind = k;
    st.error = e;
    st.data = data;
    st.size = size;
    return st;
}

GMSG_API gmsg_step gmsg_stream_step(gmsg_stream* s) {
    gmsg_stream_release_(s);
    for (;;) {
        const uint8_t* pending;
        size_t avail;
        gmsg_frame_scan sc;
        if (s->head == s->tail) {
            s->head = s->tail = 0;
            return gmsg_step_make_(GMSG_STEP_NEED_MORE, GMSG_OK, NULL, 0);
        }
        pending = s->buf + s->head;
        avail = s->tail - s->head;
        sc = s->scan(pending, avail);
        if (sc.kind == GMSG_SCAN_FRAME) {
            uint8_t* frame = s->buf + s->head;
            const gmsg_error e = s->process(s, frame, sc.size);
            if (e == GMSG_OK) {
                ++s->stats.frames;
                gmsg_stream_hold_(s, sc.consume);
                return gmsg_step_make_(GMSG_STEP_FRAME, GMSG_OK, frame, sc.size);
            }
            ++s->stats.frame_errors;
            s->stats.bytes_skipped += (uint32_t)sc.consume_on_error;
            gmsg_stream_hold_(s, sc.consume_on_error);
            s->last_error = e;
            return gmsg_step_make_(GMSG_STEP_ERROR, e, frame, sc.size);
        }
        if (sc.kind == GMSG_SCAN_SKIP) {
            size_t k = sc.size < avail ? sc.size : avail;
            if (k < 1) k = 1;
            s->stats.bytes_skipped += (uint32_t)k;
            if (sc.error == GMSG_OK) {
                gmsg_stream_consume_(s, k);
                continue;
            }
            gmsg_stream_hold_(s, k);
            s->last_error = sc.error;
            return gmsg_step_make_(GMSG_STEP_ERROR, sc.error, pending, k);
        }
        if (avail >= s->capacity) {
            /* buffer full but no frame: drop one byte and resync */
            ++s->stats.overflows;
            ++s->stats.bytes_skipped;
            gmsg_stream_hold_(s, 1);
            s->last_error = GMSG_ERR_BUFFER_FULL;
            return gmsg_step_make_(GMSG_STEP_ERROR, GMSG_ERR_BUFFER_FULL, pending, 1);
        }
        if (s->head > 0) gmsg_stream_compact_(s);
        return gmsg_step_make_(GMSG_STEP_NEED_MORE, GMSG_OK, NULL, 0);
    }
}

GMSG_API bool gmsg_stream_pull(gmsg_stream* s) {
    for (;;) {
        const gmsg_step st = gmsg_stream_step(s);
        if (st.kind == GMSG_STEP_FRAME) return true;
        if (st.kind == GMSG_STEP_NEED_MORE) return false;
        if (s->on_error) s->on_error(s, st.error, st.data, st.size);
    }
}

GMSG_API bool gmsg_stream_next(gmsg_stream* s) {
    const gmsg_step st = gmsg_stream_step(s);
    if (st.kind == GMSG_STEP_ERROR && s->on_error) s->on_error(s, st.error, st.data, st.size);
    gmsg_stream_release_(s);
    return st.kind != GMSG_STEP_NEED_MORE;
}

GMSG_API size_t gmsg_stream_feed(gmsg_stream* s, const uint8_t* data, size_t n) {
    const uint32_t before = s->stats.frames;
    do {
        const size_t k = gmsg_stream_push(s, data, n);
        data += k;
        n -= k;
        while (gmsg_stream_next(s)) {
        }
    } while (n > 0);
    return (size_t)(s->stats.frames - before);
}

#endif /* implementation */

GMSG_C_END

#endif /* GMSG_C_STREAM_H */
