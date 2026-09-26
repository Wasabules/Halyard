/* atomic_file - write a file so that a crash or a power cut leaves either the
 * old contents or the new ones, never a truncated mix.
 *
 * === AF4 2026-09-10 - WHY THIS EXISTS ===
 *
 * The refresh token and the settings were written with fopen("w"/"wb") IN
 * PLACE: the file is truncated first, then filled. A cut between the two -
 * forced shutdown, flat battery, card pulled - left an empty or partial file.
 * An empty token is rejected (a v3 token fails its Poly1305 tag), which means a
 * new Device Grant pairing - on console, getting a computer out. Empty settings
 * load as factory settings without a word. `applock_store_save` already had the
 * right scheme; this module gives it to the others.
 *
 * The scheme: write "<path>.new", flush, sync to storage, rename over <path>.
 * Two of our three platforms - Windows, and the Switch through Nintendo's FS -
 * refuse to rename onto an existing name, so the fallback removes <path>
 * first. That opens a window where ONLY "<path>.new" exists, which is why
 * `atomic_file_open_read` falls back to it - and why deleting such a file must
 * go through `atomic_file_remove`, or a leftover ".new" would bring back a
 * token the user asked to forget.
 */
#pragma once

#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opens "<path>.new" for writing ("wb"). `tmp` receives that path and needs
 * strlen(path) + 5 bytes. NULL on failure, nothing touched. */
FILE *atomic_file_open(const char *path, char *tmp, size_t tmp_cap);

/* Ends a write started by atomic_file_open.
 *   keep != 0: flush, sync, close, move over `path`. 1 on success.
 *   keep == 0: the caller's own write failed - close and delete the
 *              temporary, leave `path` exactly as it was. Returns 0.
 * On failure the original is never lost: if even the fallback rename fails,
 * the temporary is KEPT, so the next read still finds the new contents. */
int atomic_file_commit(FILE *f, const char *tmp, const char *path, int keep);

/* Opens `path` for reading; if it is missing but "<path>.new" exists - the
 * fallback's window - opens that instead. When both exist, `path` wins: a
 * ".new" beside a complete file is a leftover, not the latest word. */
FILE *atomic_file_open_read(const char *path, const char *mode);

/* Deletes `path` AND "<path>.new". Returns 1 when neither remains. */
int atomic_file_remove(const char *path);

/* === RENAME AND REMOVE, WHICH ARE NOT PORTABLE THE WAY THEY LOOK ===========
 *
 * On the PS Vita a path carries a DEVICE prefix (`ux0:`), and newlib's POSIX
 * wrappers do not resolve it: `rename` and `remove` return without touching
 * anything and without saying so. That is the same defect that stopped
 * `mkdir` from creating the data directory - and it had a second victim
 * nobody connected to it at first: the refresh token never landed under its
 * final name, so the console asked to be paired again at EVERY launch, and
 * the settings did not persist either.
 *
 * Every rename and remove of an application file goes through these, so the
 * next platform has one place to fix rather than a search across the tree.
 * Semantics are `rename(3)` and `remove(3)`: 0 on success, -1 otherwise, and
 * `errno` set to ENOENT when the target was simply not there. */
/* Creates SHADOW_DATA_DIR if it is not there. 1 when it exists afterwards.
 *
 * ONE implementation, because there were two and they DISAGREED: `main.cpp`
 * made `ux0:data/halyard/` at startup while `oauth.c` carried its own
 * copy of the platform ladder - `#else` meaning `/tmp/halyard` - and
 * tried to create THAT. The token therefore had nowhere to land and the
 * console asked to be paired again at every launch, with the reason printed
 * only to stderr: `mkdir /tmp/halyard: No such file or directory`.
 *
 * A path that is "the same as SHADOW_DATA_DIR" must be DERIVED from it, never
 * retyped: the second copy is where the drift lives. */
int shadow_ensure_data_dir(void);

int shadow_file_rename(const char *from, const char *to);
int shadow_file_remove(const char *path);

#ifdef __cplusplus
}
#endif
