// Minimal protobuf encoder/decoder for Shadow's CtrlChanV2 messages.
//
// nanopb is avoided so this stays dependency-free: the protobuf wire format is
// simple (varint + length-delimited), and that is enough for Auth, Encryption
// and RegisterSession, which only use wire type 0 (varint) and 2 (length-delim).
//
// Reference: https://protobuf.dev/programming-guides/encoding/

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PB_WIRE_VARINT  0
#define PB_WIRE_FIXED64 1
#define PB_WIRE_LENDELIM 2
#define PB_WIRE_FIXED32 5

/* --- Encoder --------------------------------------------------------- */

/* Appends a varint at buf[off..]. Returns the new offset, or -1 on overflow. */
int pb_write_varint(uint8_t *buf, size_t cap, int off, uint64_t value);

/* Appends a tag (field_number<<3 | wire_type). */
int pb_write_tag(uint8_t *buf, size_t cap, int off,
                 uint32_t field_number, uint32_t wire_type);

/* Appends a varint field (uint32/uint64/bool). */
int pb_write_uint(uint8_t *buf, size_t cap, int off,
                   uint32_t field_number, uint64_t value);
int pb_write_bool(uint8_t *buf, size_t cap, int off,
                   uint32_t field_number, bool value);

/* Appends a string field (length-delimited UTF-8). */
int pb_write_string(uint8_t *buf, size_t cap, int off,
                     uint32_t field_number, const char *str);

/* Appends a bytes field (length-delimited binary). */
int pb_write_bytes(uint8_t *buf, size_t cap, int off,
                    uint32_t field_number,
                    const uint8_t *data, size_t data_len);

/* Appends an embedded message field. The caller has already encoded the
 * sub-message into `submsg`+`submsg_len`. */
int pb_write_submsg(uint8_t *buf, size_t cap, int off,
                     uint32_t field_number,
                     const uint8_t *submsg, size_t submsg_len);

/* --- Decoder --------------------------------------------------------- */

/* Reads a varint from buf+off. On success writes the value into *value and
 * returns the new offset. Returns -1 on error. */
int pb_read_varint(const uint8_t *buf, size_t len, int off, uint64_t *value);

/* Reads a tag (a varint) and splits it into field_number and wire_type. */
int pb_read_tag(const uint8_t *buf, size_t len, int off,
                 uint32_t *field_number, uint32_t *wire_type);

/* Skips an unknown field, according to its wire type. */
int pb_skip_field(const uint8_t *buf, size_t len, int off, uint32_t wire_type);

/* Reads a length-delimited field (string/bytes/submsg) - returns a pointer
 * and a length into the source buffer (zero-copy). */
int pb_read_lendelim(const uint8_t *buf, size_t len, int off,
                      const uint8_t **out_data, size_t *out_len);

/* Helper: iterates over every field of a message. The callback returns false
 * to stop the iteration (i.e. on error). */
typedef bool (*pb_field_cb)(uint32_t field_number, uint32_t wire_type,
                             const uint8_t *buf, size_t len, int off,
                             int *next_off, void *user);

bool pb_iter_fields(const uint8_t *buf, size_t len, pb_field_cb cb, void *user);

#ifdef __cplusplus
}
#endif
