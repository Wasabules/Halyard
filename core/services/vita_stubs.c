/* vita_stubs.c - what the PS Vita's newlib declares but does not provide.
 *
 * `stdio.h` on this SDK declares `flockfile` / `funlockfile`, so anything that
 * uses them compiles; nothing implements them, so the link fails. FFmpeg's
 * libavformat reaches for them, which is how this surfaced.
 *
 * WHY NO-OPS ARE THE HONEST ANSWER HERE, and where they would not be. These
 * lock a FILE* against concurrent use from several threads. This application
 * never shares one: the journal owns its handle and writes under its own mutex
 * (`shadow/journal.c`), the settings and the token go through
 * `atomic_file.c`'s write-beside-then-rename, and the decoders read from
 * memory. FFmpeg only takes these locks around demuxer I/O we do not run - we
 * feed it Annex-B buffers directly, never a file.
 *
 * So: no-ops with the reason written down, rather than a real lock nobody
 * needs. If a Vita build ever grows a second thread on one FILE*, this is the
 * file that must stop being a stub - and the comment says so.
 *
 * Compiled everywhere, empty off the Vita, exactly like `win_stubs.c` and
 * `media/audio_out_vita.c`.
 */
#if defined(__vita__) || defined(__psp2__)

#include <stdio.h>

void flockfile(FILE *f)    { (void)f; }
void funlockfile(FILE *f)  { (void)f; }
int  ftrylockfile(FILE *f) { (void)f; return 0; }   /* 0 = the lock was taken */

#else
/* Not a Vita: empty on purpose. ISO C forbids an empty translation unit, and
 * a `static const` placed here to satisfy it is DEFINED and unused, which the
 * other console's build reports as a warning. A typedef declares nothing and
 * emits no symbol - the idiom `audio_out_win.c` already uses. */
typedef int vita_stubs_vita_only;
#endif

/* === THE SCE METADATA'S ROOM IS THE LINKER'S JOB, NOT A PADDING ARRAY ===
 *
 * A 32 KB `shadow_sce_headroom` array used to sit here to push the end of the
 * code away from the data segment, so `vita-elf-create` would have room for its
 * 3 368 bytes of SCE data. It was the FIRST attempt; the 64 KB segment
 * alignment set in CMakeLists.txt is the one that actually fixes it, and the
 * array was never removed afterwards.
 *
 * Measured 2026-09-13, same tree, one flag apart:
 *
 *     with the 32 KB array   code ends 0x8185a5ec, data at 0x81860000 -> gap 23 060
 *     without it             code ends 0x818525ec, data at 0x81870000 -> gap 121 364
 *
 * So it was not merely redundant, it made the gap FIVE TIMES SMALLER while
 * costing 32 KB of binary - padding to a 64 KB boundary just lands you nearer
 * the next one.
 *
 * What replaces it is not another workaround but a guard: `tools/vita-check-gap.py`
 * runs right after the link and fails the build with both numbers. The gap is
 * alignment - (code size mod alignment), so it moves with every size change;
 * that dice roll is acceptable only while losing is LOUD, and it was not -
 * vita-elf-create prints no "error:" and then segfaults, leaving the previous
 * `.self` in place. */
