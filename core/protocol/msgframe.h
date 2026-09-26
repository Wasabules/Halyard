/* msgframe.h - length-prefix message framing, as a PURE function.
 *
 * The input channel (`:base+14`) carries length-prefixed FlatBuffers:
 * `[size u32 LE][payload]`, total length = size + 4. The convention is
 * attested on BOTH sides of the wire: our Connect blob starts with
 * `5c 00 00 00` (= 92 = 96-4) and the server echo with `64 00 00 00` (= 100 =
 * 104-4).
 *
 * TCP is a stream with no message boundaries: one read can deliver several
 * messages, or half of one. This function is what puts the boundaries back.
 * It is pure so it can be checked offline by tests/test_msgframe.c - a framing
 * that slips by a single byte desynchronises the connection for good.
 */
#ifndef MSGFRAME_H
#define MSGFRAME_H

#include <stddef.h>
#include <stdint.h>

#define MSGFRAME_PREFIX_LEN 4

/* Length of the next complete message at the head of `acc`, prefix included.
 *   > 0 : that many bytes form one whole message
 *     0 : still incomplete, read more
 *    -1 : the announced size cannot possibly fit in `cap` - the stream is
 *         desynchronised or the peer is misbehaving; the caller must resync.
 * `cap` is the capacity of the accumulation buffer: a size that will never fit
 * is reported immediately, rather than waiting forever for bytes that cannot
 * be held. */
int msgframe_next(const uint8_t *acc, size_t acc_len, size_t cap);

#endif /* MSGFRAME_H */
