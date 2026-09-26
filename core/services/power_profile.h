/* power_profile - ask the console what it is actually capable of.
 *
 * One concept, one entry point, and TWO OPPOSITE ANSWERS -- which is the whole
 * reason this is a shared file rather than a Vita one. The PS Vita starts an
 * application well below its own ceiling and has to be asked; the Switch does
 * not, and its equivalent lever would make things worse. Either way the caller
 * has no business knowing which: it asks once, at startup.
 *
 * Call it BEFORE any measurement. Measuring a console that is still throttled
 * produces numbers that describe nothing that runs afterwards. No effect on a
 * desktop.
 */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
void shadow_power_profile_apply(void);
/* LAT-V1 (2026-09-26): reads the clocks back and logs them when they differ
 * from what apply() set - the system can move them behind our back (the
 * question this was written to answer). Call it every few seconds from a
 * running stream; a no-op where there is nothing to read. */
void shadow_power_profile_check(void);
#ifdef __cplusplus
}
#endif
