/* gmp/c/error.h - error codes shared by the C runtime and generated C code.
 * Values and texts match gmsg::Error / gmsg::to_string in the C++ runtime (checked by tests). */
#ifndef GMSG_C_ERROR_H
#define GMSG_C_ERROR_H

#include "gmp/c/config.h"

GMSG_C_BEGIN

typedef enum gmsg_error {
    GMSG_OK = 0,             /* no error (gmsg::Error::None) */
    GMSG_ERR_TRUNCATED,      /* input ended before the message/frame was complete */
    GMSG_ERR_BAD_SYNC,       /* sync bytes / start delimiter not found (garbage skipped) */
    GMSG_ERR_BAD_CHECKSUM,   /* checksum / CRC mismatch */
    GMSG_ERR_BAD_LENGTH,     /* length field invalid or frame exceeds max size */
    GMSG_ERR_BAD_DELIMITER,  /* unexpected delimiter inside a frame */
    GMSG_ERR_BAD_CONST,      /* constant / magic field did not match */
    GMSG_ERR_OUT_OF_RANGE,   /* value does not fit (count > capacity, etc.) */
    GMSG_ERR_UNKNOWN_TYPE,   /* the frame's type id matched no message */
    GMSG_ERR_BUFFER_FULL,    /* output buffer too small / stream buffer full */
    GMSG_ERR_PARSE,          /* text field could not be parsed */
    GMSG_ERR_MISSING_FIELD,  /* required (text) field was empty or absent */
    GMSG_ERR_INVALID_VALUE,  /* generic semantic failure (used by custom codecs) */
    GMSG_ERR_CUSTOM,         /* reserved for user codecs */
    GMSG_ERR_UNKNOWN_ENUM,   /* a `strict` enum field held a value that is not one of the enum's values */
    GMSG_ERR_VALUE_OUT_OF_LIMITS, /* a field value is outside its min_value / max_value / min_raw / max_raw limits */
    GMSG_ERR_OVERRUN             /* input bytes were lost before parsing (a full gmsg_ring dropped them) */
} gmsg_error;

#define GMSG_ERROR_COUNT 17

/* Short description, e.g. "bad checksum". */
GMSG_INLINE const char* gmsg_error_str(gmsg_error e) {
    switch (e) {
        case GMSG_OK: return "none";
        case GMSG_ERR_TRUNCATED: return "truncated";
        case GMSG_ERR_BAD_SYNC: return "bad sync";
        case GMSG_ERR_BAD_CHECKSUM: return "bad checksum";
        case GMSG_ERR_BAD_LENGTH: return "bad length";
        case GMSG_ERR_BAD_DELIMITER: return "bad delimiter";
        case GMSG_ERR_BAD_CONST: return "bad constant";
        case GMSG_ERR_OUT_OF_RANGE: return "out of range";
        case GMSG_ERR_UNKNOWN_TYPE: return "unknown message type";
        case GMSG_ERR_BUFFER_FULL: return "buffer full";
        case GMSG_ERR_PARSE: return "parse error";
        case GMSG_ERR_MISSING_FIELD: return "missing field";
        case GMSG_ERR_INVALID_VALUE: return "invalid value";
        case GMSG_ERR_CUSTOM: return "custom error";
        case GMSG_ERR_UNKNOWN_ENUM: return "unknown enum value";
        case GMSG_ERR_VALUE_OUT_OF_LIMITS: return "value out of limits";
        case GMSG_ERR_OVERRUN: return "overrun";
    }
    return "unknown";
}

GMSG_C_END

#endif /* GMSG_C_ERROR_H */
