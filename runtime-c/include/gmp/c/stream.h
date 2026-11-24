/* gmp/c/stream.h - incremental frame extraction from a byte stream (port of gmsg::StreamParser).
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
 *   gmsg_stream_resync(s)          give up on an incomplete frame at the head (reported to on_error as TRUNCATED)
 *   gmsg_stream_flush(s)           drop everything buffered (reported to on_error as TRUNCATED)
 *   gmsg_stream_set_idle_timeout(s, t) / gmsg_stream_tick(s, now)
 *                                  resync automatically after `t` without new bytes (`now`: any monotonic
 *                                  unsigned time the application passes; wraparound is fine)
 *   resync / flush / tick only drop data; frames that become complete are delivered by the next
 *   gmsg_stream_next / gmsg_stream_feed / poll call.
 *   gmsg_stream_push_from(s, read, ctx, max)
 *                                  let read(ctx, out, n) write input straight into the buffer (a byte source)
 *   gmsg_stream_push_gap(s)        input was lost after the bytes pushed so far (e.g. a UART overrun flag): the
 *                                  frame in progress is dropped and reported as GMSG_ERR_OVERRUN
 *   gmsg_stream_push_ring(s, ring, max) / gmsg_stream_feed_ring(s, ring, max)
 *                                  move bytes from a gmsg_ring (gmp/c/ring.h) filled by an interrupt, at most
 *                                  `max` (SIZE_MAX: all). Bytes the full ring dropped are an overrun: the frames
 *                                  complete before the gap are delivered, then the frame in progress is dropped
 *                                  and reported to on_error as GMSG_ERR_OVERRUN.
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
#include "gmp/c/ring.h"

GMSG_C_BEGIN

typedef struct gmsg_stream_stats {
    uint32_t frames;         /* frames delivered successfully */
    uint32_t frame_errors;   /* frames rejected (checksum, unknown message type, decode error) */
    uint32_t bytes_skipped;  /* bytes discarded while resynchronising */
    uint32_t overflows;      /* times the buffer filled without a complete frame */
    uint32_t resyncs;        /* incomplete frames given up by gmsg_stream_resync (also from gmsg_stream_tick) */
    uint32_t flushes;        /* gmsg_stream_flush calls that dropped buffered bytes */
    uint32_t frames_with_extra; /* delivered frames with data after their message (ignored) */
    uint32_t overruns;       /* gaps in the input taken from a gmsg_ring (each drops the frame in progress) */
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
    uint32_t received;       /* bytes pushed (wraps; only compared for change) */
    uint32_t seen;           /* `received` at the last tick */
    uint32_t last_activity;  /* time of the last tick that saw new bytes (or of the last idle resync) */
    uint32_t idle_timeout;   /* 0 = off */
    bool ticked;
    size_t last_extra;       /* set by `process`: data after the message of the frame being accepted */
    bool gap_pending;        /* input was lost at buf[gap] (taken from a gmsg_ring); reported by the next step */
    size_t gap;
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

/* Free room in the buffer (bytes gmsg_stream_push can take now). */
GMSG_API size_t gmsg_stream_space(const gmsg_stream* s);

/* Let read(ctx, out, n) write up to n input bytes straight into the buffer (n: all the room there is, at most
 * `max`); it returns the number written. Returns the bytes added (0 when the buffer is full). */
GMSG_API size_t gmsg_stream_push_from(gmsg_stream* s, size_t (*read)(void* ctx, uint8_t* out, size_t n), void* ctx,
                                      size_t max);

/* Mark that input was lost after the bytes pushed so far. Frames complete before that point are delivered; the
 * frame in progress is dropped and reported as GMSG_ERR_OVERRUN. One gap at a time: returns false (and does
 * nothing) while an earlier gap has not been reported yet. */
GMSG_API bool gmsg_stream_push_gap(gmsg_stream* s);

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

/* Give up on the frame at the head of the buffer when it is incomplete (e.g. a corrupted length claims more bytes
 * than will arrive): report it to on_error as GMSG_ERR_TRUNCATED with its buffered bytes, drop its first byte and
 * let the next scan resynchronise. Returns false (and does nothing) when the buffer is empty or its head frame is
 * complete. */
GMSG_API bool gmsg_stream_resync(gmsg_stream* s);

/* Drop every buffered byte, reported to on_error as one GMSG_ERR_TRUNCATED. Returns false when nothing was buffered. */
GMSG_API bool gmsg_stream_flush(gmsg_stream* s);

/* Resync from gmsg_stream_tick once the link has been idle for `t` (unit of the tick times); 0 turns it off. */
GMSG_API void gmsg_stream_set_idle_timeout(gmsg_stream* s, uint32_t t);

/* Call periodically with the current time. Bytes pushed since the previous tick count as activity. After
 * idle_timeout without new bytes an incomplete head frame is given up as by gmsg_stream_resync (returns true);
 * the next resync needs another idle period. */
GMSG_API bool gmsg_stream_tick(gmsg_stream* s, uint32_t now);

/* Push data[0..n) and process every complete frame. Returns the number of frames delivered. */
GMSG_API size_t gmsg_stream_feed(gmsg_stream* s, const uint8_t* data, size_t n);

/* Move as many bytes from `ring` as fit (at most `max`; SIZE_MAX for no limit) into the buffer without processing
 * them (pull API). Returns bytes moved. When the ring has dropped input, its gap is taken over once everything
 * before it has been moved; the next step reports it as GMSG_ERR_OVERRUN. */
GMSG_API size_t gmsg_stream_push_ring(gmsg_stream* s, gmsg_ring* ring, size_t max);

/* Drain `ring` (at most `max` bytes; SIZE_MAX for no limit) and process every complete frame. Returns the number of
 * frames delivered. */
GMSG_API size_t gmsg_stream_feed_ring(gmsg_stream* s, gmsg_ring* ring, size_t max);

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
    s->ticked = false;
    s->gap_pending = false;
    s->gap = 0;
    gmsg_memset(&s->stats, 0, sizeof s->stats);
}

GMSG_API size_t gmsg_stream_buffered(const gmsg_stream* s) { return s->tail - s->head; }

GMSG_API uint32_t gmsg_stream_generation(const gmsg_stream* s) { return s->generation; }

GMSG_PRIV_ void gmsg_stream_consume_(gmsg_stream* s, size_t n) {
    const size_t avail = s->tail - s->head;
    s->head += n < avail ? n : avail;
    if (s->head == s->tail) s->head = s->tail = s->gap = 0; /* a gap left can only be at the end */
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
    if (s->gap_pending) s->gap -= s->head;
    s->head = 0;
}

GMSG_API size_t gmsg_stream_push(gmsg_stream* s, const uint8_t* data, size_t n) {
    size_t k;
    gmsg_stream_release_(s);
    if (s->head > 0 && (s->capacity - s->tail) < n) gmsg_stream_compact_(s);
    k = n < s->capacity - s->tail ? n : s->capacity - s->tail;
    if (k > 0) gmsg_memcpy(s->buf + s->tail, data, k);
    s->tail += k;
    s->received += (uint32_t)k;
    return k;
}

GMSG_API size_t gmsg_stream_space(const gmsg_stream* s) { return s->capacity - (s->tail - s->head); }

GMSG_API size_t gmsg_stream_push_from(gmsg_stream* s, size_t (*read)(void* ctx, uint8_t* out, size_t n), void* ctx,
                                      size_t max) {
    size_t room, k;
    gmsg_stream_release_(s);
    gmsg_stream_compact_(s);
    room = s->capacity - s->tail;
    if (room > max) room = max;
    if (room == 0) return 0;
    k = read(ctx, s->buf + s->tail, room);
    if (k > room) k = room;
    s->tail += k;
    s->received += (uint32_t)k;
    return k;
}

GMSG_API bool gmsg_stream_push_gap(gmsg_stream* s) {
    gmsg_stream_release_(s);
    if (s->gap_pending) return s->gap == s->tail; /* the same position: nothing to add */
    s->gap_pending = true;
    s->gap = s->tail;
    return true;
}

GMSG_PRIV_ gmsg_step gmsg_step_make_(gmsg_step_kind k, gmsg_error e, const uint8_t* data, size_t size) {
    gmsg_step st;
    st.kind = k;
    st.error = e;
    st.data = data;
    st.size = size;
    return st;
}

/* Drop the n bytes before the gap (the frame in progress when input was lost) as GMSG_ERR_OVERRUN. */
GMSG_PRIV_ gmsg_step gmsg_stream_overrun_(gmsg_stream* s, size_t n) {
    s->gap_pending = false;
    ++s->stats.overruns;
    s->stats.bytes_skipped += (uint32_t)n;
    s->last_error = GMSG_ERR_OVERRUN;
    gmsg_stream_hold_(s, n);
    return gmsg_step_make_(GMSG_STEP_ERROR, GMSG_ERR_OVERRUN, s->buf + s->head, n);
}

GMSG_API gmsg_step gmsg_stream_step(gmsg_stream* s) {
    gmsg_stream_release_(s);
    for (;;) {
        const uint8_t* pending;
        size_t avail;
        gmsg_frame_scan sc;
        if (s->gap_pending && s->head == s->gap) return gmsg_stream_overrun_(s, 0);
        if (s->head == s->tail) {
            s->head = s->tail = 0;
            return gmsg_step_make_(GMSG_STEP_NEED_MORE, GMSG_OK, NULL, 0);
        }
        pending = s->buf + s->head;
        avail = (s->gap_pending ? s->gap : s->tail) - s->head; /* never scan across a gap */
        sc = s->scan(pending, avail);
        if (sc.kind == GMSG_SCAN_FRAME) {
            uint8_t* frame = s->buf + s->head;
            gmsg_error e;
            s->last_extra = 0;
            e = s->process(s, frame, sc.size);
            if (e == GMSG_OK) {
                ++s->stats.frames;
                if (s->last_extra != 0) ++s->stats.frames_with_extra;
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
        if (s->gap_pending) return gmsg_stream_overrun_(s, avail); /* the frame in progress lost bytes */
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

GMSG_API bool gmsg_stream_resync(gmsg_stream* s) {
    const uint8_t* pending;
    size_t avail;
    gmsg_stream_release_(s);
    if (s->head == s->tail || s->gap_pending) return false; /* empty, or the next step drops it as an overrun */
    pending = s->buf + s->head;
    avail = s->tail - s->head;
    if (s->scan(pending, avail).kind != GMSG_SCAN_NEED_MORE) return false; /* nothing stuck at the head */
    ++s->stats.resyncs;
    ++s->stats.bytes_skipped;
    s->last_error = GMSG_ERR_TRUNCATED;
    if (s->on_error) s->on_error(s, GMSG_ERR_TRUNCATED, pending, avail);
    gmsg_stream_consume_(s, 1);
    ++s->generation;
    return true;
}

GMSG_API bool gmsg_stream_flush(gmsg_stream* s) {
    size_t avail;
    gmsg_stream_release_(s);
    if (s->gap_pending) { /* report the lost input first, for the bytes before the gap */
        const gmsg_step st = gmsg_stream_overrun_(s, s->gap - s->head);
        if (s->on_error) s->on_error(s, st.error, st.data, st.size);
        gmsg_stream_release_(s);
        if (s->head == s->tail) return true;
    }
    if (s->head == s->tail) return false;
    avail = s->tail - s->head;
    ++s->stats.flushes;
    s->stats.bytes_skipped += (uint32_t)avail;
    s->last_error = GMSG_ERR_TRUNCATED;
    if (s->on_error) s->on_error(s, GMSG_ERR_TRUNCATED, s->buf + s->head, avail);
    s->head = s->tail = 0;
    ++s->generation;
    return true;
}

GMSG_API void gmsg_stream_set_idle_timeout(gmsg_stream* s, uint32_t t) { s->idle_timeout = t; }

GMSG_API bool gmsg_stream_tick(gmsg_stream* s, uint32_t now) {
    if (!s->ticked || s->received != s->seen) { /* first tick, or bytes arrived since the last one */
        s->ticked = true;
        s->seen = s->received;
        s->last_activity = now;
        return false;
    }
    if (s->idle_timeout == 0 || (uint32_t)(now - s->last_activity) < s->idle_timeout) return false;
    s->last_activity = now; /* a further resync needs another idle period */
    return gmsg_stream_resync(s);
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

GMSG_API size_t gmsg_stream_push_ring(gmsg_stream* s, gmsg_ring* ring, size_t max) {
    size_t total = 0;
    gmsg_stream_release_(s);
    while (total < max) {
        size_t avail, k;
        const uint8_t* part = gmsg_ring_readable(ring, &avail);
        if (avail == 0) {
            if (s->gap_pending || !gmsg_ring_overrun_pending(ring)) break; /* one gap at a time in the buffer */
            (void)gmsg_ring_readable(ring, &avail);
            if (avail != 0) continue; /* bytes that arrived before the gap became visible */
            gmsg_ring_clear_overrun(ring);
            s->gap_pending = true;
            s->gap = s->tail;
            break;
        }
        if (s->head > 0 && (s->capacity - s->tail) < avail) gmsg_stream_compact_(s);
        k = avail < s->capacity - s->tail ? avail : s->capacity - s->tail;
        if (k > max - total) k = max - total;
        if (k == 0) break;
        gmsg_memcpy(s->buf + s->tail, part, k);
        s->tail += k;
        s->received += (uint32_t)k;
        gmsg_ring_consume(ring, k);
        total += k;
    }
    return total;
}

GMSG_API size_t gmsg_stream_feed_ring(gmsg_stream* s, gmsg_ring* ring, size_t max) {
    const uint32_t before = s->stats.frames;
    size_t total = 0;
    for (;;) {
        const size_t k = gmsg_stream_push_ring(s, ring, max - total);
        total += k;
        while (gmsg_stream_next(s)) { /* also reports a gap the push took over */
        }
        if (k == 0) break;
    }
    return (size_t)(s->stats.frames - before);
}

#endif /* implementation */

GMSG_C_END

#endif /* GMSG_C_STREAM_H */
