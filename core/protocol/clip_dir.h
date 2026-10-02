/* clip_dir - which way the clipboard is allowed to travel.
 *
 * === CLIP6 2026-10-02 — WHY FOUR PREDICATES EARN A MODULE ==================
 *
 * `SHADOW_CLIPBOARD` carries four values (0 off, 1 both, 2 PC→VM, 3 VM→PC) and
 * the whole behaviour of the channel hangs on reading them right. The logic was
 * four static functions inside `ctrl_session.c` — a 6000-line file that no test
 * can include, because it pulls wolfSSL, pthreads, sockets and the journal.
 *
 * So the one piece of this feature that CAN be tested offline was the one piece
 * that was not. That matters more here than the line count suggests: the two
 * failure modes are both silent.
 *
 *   - Get `to_pc` wrong in the permissive direction and the VM's clipboard is
 *     pulled across the network on a session the user set to one-way, which is
 *     the exact thing the setting is chosen to prevent.
 *   - Get the CLAMP wrong and a typo in `env.txt` disables a feature the user
 *     believes they configured. `SHADOW_CLIPBOARD=O` (letter O) must not read
 *     as 0; it must fall back to "both ways", because a value we cannot
 *     understand is not an instruction to stop sharing.
 *
 * Pure: no globals, no I/O, no `getenv`. The environment is read by the caller
 * and the STRING is passed in, which is what makes the clamp testable — and
 * which is also the rule `tests/run_tests.sh` imposes on anything it compiles.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CLIP_MODE_OFF   0
#define CLIP_MODE_BOTH  1
#define CLIP_MODE_TO_VM 2   /* this machine -> the VM only */
#define CLIP_MODE_TO_PC 3   /* the VM -> this machine only */

/* The mode `s` asks for, clamped to the four values above.
 *
 * `s` is the raw environment string, or NULL when the variable is unset. An
 * absent, empty or unreadable value gives CLIP_MODE_BOTH — see the header
 * comment for why the fallback is "both ways" and not "off".
 *
 * Leading spaces and a sign are accepted the way `atoi` accepts them, because
 * that is what every other toggle in this repo does and a value that works for
 * one key must work for the next. */
static inline int clip_dir_from_env(const char *s)
{
    if (!s) return CLIP_MODE_BOTH;

    /* Skip blanks, then an optional sign, then read digits. Written out rather
     * than calling `atoi`, because `atoi("")` and `atoi("abc")` both answer 0 -
     * which is CLIP_MODE_OFF, the single most harmful answer to guess. Here a
     * value with no digits at all is refused and falls back. */
    const char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    int sign = 1;
    if (*p == '+' || *p == '-') { if (*p == '-') sign = -1; p++; }
    if (*p < '0' || *p > '9') return CLIP_MODE_BOTH;   /* no number here */

    int v = 0;
    for (; *p >= '0' && *p <= '9'; p++) {
        v = v * 10 + (*p - '0');
        if (v > 9999) break;            /* long enough to be out of range */
    }
    v *= sign;

    return (v >= CLIP_MODE_OFF && v <= CLIP_MODE_TO_PC) ? v : CLIP_MODE_BOTH;
}

/* True when the VM's clipboard may reach this machine.
 *
 * The caller must ALSO stop asking for it (`clip_tcp_set_auto_request`): a
 * direction enforced only at delivery still sends the text across the network.
 * That is stated in clip_tcp.h and repeated here because this is the function
 * someone will reach for. */
static inline bool clip_dir_to_pc(int m)
{
    return m == CLIP_MODE_BOTH || m == CLIP_MODE_TO_PC;
}

/* True when this machine's clipboard may reach the VM. */
static inline bool clip_dir_to_vm(int m)
{
    return m == CLIP_MODE_BOTH || m == CLIP_MODE_TO_VM;
}

/* True when nothing travels and the channel should not even be opened. */
static inline bool clip_dir_off(int m) { return m == CLIP_MODE_OFF; }

/* A name for the log and for the end-of-session summary. Never NULL. */
static inline const char *clip_dir_name(int m)
{
    switch (m) {
        case CLIP_MODE_OFF:   return "off";
        case CLIP_MODE_TO_VM: return "PC -> VM only";
        case CLIP_MODE_TO_PC: return "VM -> PC only";
        case CLIP_MODE_BOTH:  return "both ways";
        default:              return "both ways";
    }
}

#ifdef __cplusplus
}
#endif
