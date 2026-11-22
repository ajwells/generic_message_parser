/* Interface between crosscheck_c.c (C11) and test_c_vs_cpp.cpp (C++). */
#ifndef GMPCT_CROSSCHECK_C_H
#define GMPCT_CROSSCHECK_C_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Checksums in the order of gmp/c/checksum.h: 16 CRC presets, then xor8 sum8 lrc8 sum16 fletcher16 fletcher8_ubx. */
#define GMPCT_CHECKSUM_COUNT 22
int gmpct_checksum_count(void);
uint64_t gmpct_checksum(int algo, const uint8_t* d, size_t n);
uint64_t gmpct_checksum_split(int algo, const uint8_t* d, size_t n, size_t split);
uint64_t gmpct_crc_params(unsigned width, bool refin, bool refout, uint64_t poly, uint64_t init, uint64_t xorout,
                          const uint8_t* d, size_t n);

enum { GMPCT_U8, GMPCT_U16, GMPCT_U32, GMPCT_U64, GMPCT_I8, GMPCT_I16, GMPCT_I32, GMPCT_I64, GMPCT_F32, GMPCT_F64 };
/* Loaded value as a bit pattern (signed: sign-extended to 64 bits; floats: IEEE bits). */
uint64_t gmpct_load(int type, const uint8_t* p, int big);
void gmpct_store(int type, uint8_t* p, uint64_t bits, int big);

uint64_t gmpct_get_bits(const uint8_t* p, size_t off, unsigned n, int msb);
int64_t gmpct_get_bits_signed(const uint8_t* p, size_t off, unsigned n, int msb);
void gmpct_set_bits(uint8_t* p, size_t off, unsigned n, uint64_t v, int msb);

int gmpct_error_count(void);
const char* gmpct_error_str(int e);

/* ---- framing (crosscheck_framing_c.c) ---- */

/* Scanner configs 0..GMPCT_SCAN_COUNT-1, mirrored in test_c_vs_cpp.cpp.
 * out: kind, size, consume, consume_on_error, error. */
#define GMPCT_SCAN_COUNT 9
void gmpct_scan(int which, const uint8_t* buf, size_t n, size_t out[5]);

/* Escape configs 0..GMPCT_ESCAPE_COUNT-1: xor (HDLC), index, map (SLIP). */
#define GMPCT_ESCAPE_COUNT 3
int gmpct_unescape(int which, uint8_t* buf, size_t n, size_t* out_n);
int gmpct_escape(int which, uint8_t* buf, size_t cap, size_t at, size_t len, size_t* out_len);
/* out: kind, wire, error */
void gmpct_walk(int which, const uint8_t* buf, size_t n, size_t at, size_t count, size_t pick_at, uint8_t* pick,
                size_t pick_n, size_t out[3]);

/* Stream over scanner config `which` with a buffer of `capacity` (<= 256) bytes, fed in chunks of `chunk` bytes.
 * A frame is accepted when its last byte is even, otherwise rejected with GMSG_ERR_BAD_CHECKSUM. Records one
 * event per delivered frame or reported error (3 values: 0 = frame / 1 = error, error code, size) and returns
 * the event count. stats: frames, frame_errors, bytes_skipped, overflows. */
size_t gmpct_stream(int which, size_t capacity, const uint8_t* data, size_t n, size_t chunk, size_t* events,
                    size_t max_events, uint32_t stats[4]);

/* ---- text and float text (crosscheck_text_c.c) ---- */
int gmpct_format_double(double v, int precision, char* out, size_t cap, size_t* n);
int gmpct_format_float(float v, int precision, char* out, size_t cap, size_t* n);
int gmpct_parse_double(const char* s, size_t n, double* out);
int gmpct_parse_float(const char* s, size_t n, float* out);
int gmpct_parse_i64(const char* s, size_t n, int base, int64_t min, int64_t max, int64_t* out);
int gmpct_parse_u64(const char* s, size_t n, int base, uint64_t max, uint64_t* out);
/* which: 0 i64 token, 1 u64 token, 2 f64 token, 3 f32 token; then a char token 'x' (tests the separator) */
int gmpct_text_tokens(int which, int64_t iv, uint64_t uv, double dv, float fv, int base, int precision, int width,
                      char* out, size_t cap, size_t* n);
int gmpct_format_general(double v, int digits, char* out, size_t cap, size_t* n);

/* ---- print (crosscheck_text_c.c) ---- */
/* Runs a fixed sequence of printer calls (the same as print_scenario in test_text_c_vs_cpp.cpp) into out[0..cap)
 * with options (style, indent, max_items, max_bytes, precision); returns the gmsg_printer_finish() error and the
 * characters written in *n. */
int gmpct_print_scenario(int style, unsigned indent, size_t max_items, size_t max_bytes, int precision, char* out,
                         size_t cap, size_t* n);

#ifdef __cplusplus
}
#endif

#endif
