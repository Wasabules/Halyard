/* msgframe.c - see msgframe.h. Pure function, zero dependencies. */
#include "msgframe.h"

int msgframe_next(const uint8_t *acc, size_t acc_len, size_t cap)
{
    if (!acc || cap < MSGFRAME_PREFIX_LEN) return -1;
    if (acc_len < MSGFRAME_PREFIX_LEN) return 0;      /* not even the prefix */

    uint32_t size = (uint32_t)acc[0]        | ((uint32_t)acc[1] << 8)
                    | ((uint32_t)acc[2] << 16) | ((uint32_t)acc[3] << 24);

    /* Compare by SUBTRACTION: `size + 4 > cap` would overflow for a size near
     * 2^32 and let the nonsense through. Same trap as the one fixed in proto.c
     * the same day. */
    if (size > (uint32_t)(cap - MSGFRAME_PREFIX_LEN)) return -1;

    size_t total = (size_t)size + MSGFRAME_PREFIX_LEN;
    if (acc_len < total) return 0;                    /* incomplet */
    return (int)total;
}
