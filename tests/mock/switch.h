/* tests/mock/switch.h - a MOCK of the few libnx declarations ui/sfx.cpp uses.
 *
 * Test-only, never part of the application. tests/test_sfx_handoff.cpp compiles
 * the REAL ui/sfx.cpp with -D__SWITCH__, so its `audout` branch - the console's -
 * is the one that runs, against the implementation of these functions that the
 * test provides.
 *
 * What the implementation models is what libnx does (audout.c and
 * service_guard.h, read for AUDC-1 on 2026-09-11): ONE IAudioOut for the whole
 * process behind a refcounted guard - audoutInitialize/audoutExit are counted,
 * Start (command 1) and Stop (command 2) are plain commands and are not - and one
 * released-buffer list, so audoutWaitPlayFinish and
 * audoutGetReleasedAudioOutBuffer hand back ANY caller's buffer. Every call takes
 * no session handle: the UI sounds and the stream necessarily drive the same
 * output. The signatures and the buffer layout below are libnx's.
 *
 * What a second Start on an output already started does on the console is not
 * known: yuzu and Ryujinx refuse it ([C80]). The test runs both models. */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef uint32_t u32;
typedef uint64_t u64;
typedef u32      Result;

#define R_FAILED(res)    ((res) != 0)
#define R_SUCCEEDED(res) ((res) == 0)

typedef struct AudioOutBuffer AudioOutBuffer;
struct AudioOutBuffer {
    AudioOutBuffer *next;   /* unused by the service */
    void *buffer;           /* 0x1000-aligned on console; the mock does not care */
    u64 buffer_size;
    u64 data_size;
    u64 data_offset;
};

#ifdef __cplusplus
extern "C" {
#endif

Result audoutInitialize(void);
void   audoutExit(void);
Result audoutStartAudioOut(void);
Result audoutStopAudioOut(void);
Result audoutAppendAudioOutBuffer(AudioOutBuffer *Buffer);
Result audoutGetReleasedAudioOutBuffer(AudioOutBuffer **Buffer, u32 *ReleasedBuffersCount);
Result audoutWaitPlayFinish(AudioOutBuffer **released, u32 *released_count, u64 timeout);

#ifdef __cplusplus
}
#endif
