#include "proto.h"

#include <limits.h>
#include <string.h>

/* --- Encoder --------------------------------------------------------- */

int pb_write_varint(uint8_t *buf, size_t cap, int off, uint64_t value) {
    while (value >= 0x80) {
        if ((size_t)off >= cap) return -1;
        buf[off++] = (uint8_t)((value & 0x7f) | 0x80);
        value >>= 7;
    }
    if ((size_t)off >= cap) return -1;
    buf[off++] = (uint8_t)(value & 0x7f);
    return off;
}

int pb_write_tag(uint8_t *buf, size_t cap, int off,
                  uint32_t field_number, uint32_t wire_type) {
    uint64_t tag = ((uint64_t)field_number << 3) | (wire_type & 0x7);
    return pb_write_varint(buf, cap, off, tag);
}

int pb_write_uint(uint8_t *buf, size_t cap, int off,
                   uint32_t field_number, uint64_t value) {
    off = pb_write_tag(buf, cap, off, field_number, PB_WIRE_VARINT);
    if (off < 0) return -1;
    return pb_write_varint(buf, cap, off, value);
}

int pb_write_bool(uint8_t *buf, size_t cap, int off,
                   uint32_t field_number, bool value) {
    return pb_write_uint(buf, cap, off, field_number, value ? 1 : 0);
}

int pb_write_string(uint8_t *buf, size_t cap, int off,
                     uint32_t field_number, const char *str) {
    if (!str) str = "";
    size_t slen = strlen(str);
    off = pb_write_tag(buf, cap, off, field_number, PB_WIRE_LENDELIM);
    if (off < 0) return -1;
    off = pb_write_varint(buf, cap, off, slen);
    if (off < 0) return -1;
    if ((size_t)off + slen > cap) return -1;
    memcpy(buf + off, str, slen);
    return off + (int)slen;
}

int pb_write_bytes(uint8_t *buf, size_t cap, int off,
                    uint32_t field_number,
                    const uint8_t *data, size_t data_len) {
    off = pb_write_tag(buf, cap, off, field_number, PB_WIRE_LENDELIM);
    if (off < 0) return -1;
    off = pb_write_varint(buf, cap, off, data_len);
    if (off < 0) return -1;
    if ((size_t)off + data_len > cap) return -1;
    if (data_len > 0) memcpy(buf + off, data, data_len);
    return off + (int)data_len;
}

int pb_write_submsg(uint8_t *buf, size_t cap, int off,
                     uint32_t field_number,
                     const uint8_t *submsg, size_t submsg_len) {
    return pb_write_bytes(buf, cap, off, field_number, submsg, submsg_len);
}

/* --- Decoder --------------------------------------------------------- */

/* This decoder handles SERVER-CONTROLLED input: every bound has to hold against
 * a malformed message, not merely against an honest one.
 *
 * Checks that a field of `dlen` bytes fits from `off` inside `len`.
 * We subtract rather than add: `off + dlen > len` OVERFLOWS once dlen nears
 * 2^64, and the check then wrongly passes (see tests/test_proto.c). */
static bool pb_fits(size_t len, int off, uint64_t dlen) {
    if (off < 0 || (size_t)off > len) return false;
    return dlen <= (uint64_t)(len - (size_t)off);
}

/* A Shadow control message fits far below INT_MAX; beyond that, this
 * interface's `int` offsets can no longer represent the positions. */
static bool pb_len_ok(size_t len) { return len <= (size_t)INT_MAX; }

int pb_read_varint(const uint8_t *buf, size_t len, int off, uint64_t *value) {
    if (!buf || !value || off < 0 || !pb_len_ok(len)) return -1;
    uint64_t v = 0;
    int shift = 0;
    while ((size_t)off < len) {
        uint8_t b = buf[off++];
        /* The 10th byte carries bit 63 and nothing else: any other bit would
         * overflow the 64-bit integer and be lost silently. A conforming
         * encoder never emits one, so rejecting it only discards malformed
         * input. */
        if (shift == 63 && (b & 0x7e) != 0) return -1;
        v |= (uint64_t)(b & 0x7f) << shift;
        if ((b & 0x80) == 0) {
            *value = v;
            return off;
        }
        shift += 7;
        if (shift >= 64) return -1;
    }
    return -1;
}

int pb_read_tag(const uint8_t *buf, size_t len, int off,
                 uint32_t *field_number, uint32_t *wire_type) {
    uint64_t tag = 0;
    off = pb_read_varint(buf, len, off, &tag);
    if (off < 0) return -1;
    if (field_number) *field_number = (uint32_t)(tag >> 3);
    if (wire_type)    *wire_type    = (uint32_t)(tag & 0x7);
    return off;
}

int pb_skip_field(const uint8_t *buf, size_t len, int off, uint32_t wire_type) {
    if (!buf || off < 0 || !pb_len_ok(len)) return -1;
    switch (wire_type) {
        case PB_WIRE_VARINT: {
            uint64_t dummy;
            return pb_read_varint(buf, len, off, &dummy);
        }
        case PB_WIRE_FIXED64:
            if (!pb_fits(len, off, 8)) return -1;
            return off + 8;
        case PB_WIRE_LENDELIM: {
            uint64_t dlen = 0;
            off = pb_read_varint(buf, len, off, &dlen);
            if (off < 0) return -1;
            if (!pb_fits(len, off, dlen)) return -1;
            return off + (int)dlen;
        }
        case PB_WIRE_FIXED32:
            if (!pb_fits(len, off, 4)) return -1;
            return off + 4;
        default:
            return -1;
    }
}

int pb_read_lendelim(const uint8_t *buf, size_t len, int off,
                      const uint8_t **out_data, size_t *out_len) {
    uint64_t dlen = 0;
    if (!buf || off < 0 || !pb_len_ok(len)) return -1;
    off = pb_read_varint(buf, len, off, &dlen);
    if (off < 0) return -1;
    if (!pb_fits(len, off, dlen)) return -1;
    if (out_data) *out_data = buf + off;
    if (out_len)  *out_len  = (size_t)dlen;
    return off + (int)dlen;
}

bool pb_iter_fields(const uint8_t *buf, size_t len, pb_field_cb cb, void *user) {
    if (!buf || !pb_len_ok(len)) return false;
    int off = 0;
    while ((size_t)off < len) {
        uint32_t fn = 0, wt = 0;
        int new_off = pb_read_tag(buf, len, off, &fn, &wt);
        if (new_off < 0) return false;
        int after = new_off;
        if (cb) {
            if (!cb(fn, wt, buf, len, new_off, &after, user)) return false;
        } else {
            after = pb_skip_field(buf, len, new_off, wt);
            if (after < 0) return false;
        }
        /* Safety net: the offset must STRICTLY advance. Without it a malformed
         * field - or a misbehaving callback - spins this loop forever. On
         * Switch a stuck thread stops polling its abort_flag: process_exit
         * kills it brutally, HOS leaks its handles, and the console has to be
         * rebooted. */
        if (after <= off) return false;
        off = after;
    }
    return true;
}
