/* Linux desktop port stubs for Switch HOS APIs.
 * Included by the files that call svcSleepThread/applet/etc. without being
 * Switch-specific themselves (they just use a few HOS functions).
 */
#pragma once

#ifndef __SWITCH__
#include <unistd.h>
#include <stdint.h>

/* svcSleepThread : nanoseconds → usleep microseconds. */
static inline void svcSleepThread(uint64_t ns) {
    usleep((useconds_t)(ns / 1000ULL));
}

/* libnx integer type aliases */
typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t  u8;
typedef int64_t  s64;
typedef int32_t  s32;
typedef int16_t  s16;
typedef int8_t   s8;
typedef uint32_t Result;
#endif /* !__SWITCH__ */
