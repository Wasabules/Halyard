/* test_atomic_file - AF4 (2026-09-10): the token and the settings were written
 * by truncating in place, so a cut mid-write left an empty file - a rejected
 * token, i.e. a new pairing. These checks pin the replacement scheme, and above
 * all its counter-cases: the fallback's window where only "<path>.new" exists,
 * a failed write that must leave the original untouched, and a deletion that
 * must not leave a ".new" behind to resurrect a forgotten token. */
#include "../core/services/atomic_file.h"

#include <stdio.h>
#include <string.h>

static int g_checks = 0, g_fail = 0;
#define CHECK(c, msg) do { g_checks++; if (!(c)) { g_fail++; printf("  FAIL %s:%d - %s\n", __FILE__, __LINE__, msg); } } while (0)

static const char *P = "atomic_file_test.tmp";
static const char *N = "atomic_file_test.tmp.new";

static int read_all(const char *path, char *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = 0;
    return (int)n;
}

static int exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static int write_raw(const char *path, const char *s)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fputs(s, f);
    fclose(f);
    return 1;
}

static int commit_text(const char *s, int keep)
{
    char tmp[256];
    FILE *f = atomic_file_open(P, tmp, sizeof tmp);
    if (!f) return -1;
    fputs(s, f);
    return atomic_file_commit(f, tmp, P, keep);
}

int main(void)
{
    char buf[256];
    printf("== atomic file writes (AF4) ==\n");
    remove(P); remove(N);

    /* 1. A first write creates the file and leaves no temporary. */
    CHECK(commit_text("v1", 1) == 1, "commit of a first file");
    CHECK(read_all(P, buf, sizeof buf) == 2 && strcmp(buf, "v1") == 0, "read back: v1");
    CHECK(!exists(N), "no .new left after a successful commit");

    /* 2. Overwriting an EXISTING file: the path Windows and the Switch refuse
     *    to rename onto, hence the fallback. */
    CHECK(commit_text("version-2", 1) == 1, "replacing an existing file");
    CHECK(read_all(P, buf, sizeof buf) > 0 && strcmp(buf, "version-2") == 0, "read back: version-2");
    CHECK(!exists(N), "no .new left after a replacement");

    /* 3. COUNTER-CASE - a failed write (keep = 0) must leave the original
     *    exactly as it was: before AF4 the file was ALREADY truncated here. */
    CHECK(commit_text("BROKEN", 0) == 0, "a failed write returns 0");
    CHECK(read_all(P, buf, sizeof buf) > 0 && strcmp(buf, "version-2") == 0,
          "the original survives a failed write");
    CHECK(!exists(N), "a failed write's .new is deleted");

    /* 4. COUNTER-CASE - the fallback's window: the original removed, the new
     *    contents only in ".new". A cut there used to read as "no file". */
    remove(P);
    CHECK(write_raw(N, "v3"), "setup: only the .new exists");
    {
        FILE *f = atomic_file_open_read(P, "rb");
        CHECK(f != NULL, "the read falls back to the .new");
        if (f) {
            size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f);
            CHECK(strcmp(buf, "v3") == 0, "and reads v3 from it");
        }
    }

    /* 5. Both exist: the complete file wins - a ".new" beside it is a leftover
     *    of an interrupted write, not the latest word. */
    CHECK(write_raw(P, "complete"), "setup: both exist");
    {
        FILE *f = atomic_file_open_read(P, "rb");
        if (f) {
            size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f);
        } else buf[0] = 0;
        CHECK(strcmp(buf, "complete") == 0, "the complete file wins over the .new");
    }

    /* 6. COUNTER-CASE - deleting must take the ".new" too, or a forgotten
     *    token would come back from it at the next launch. */
    CHECK(atomic_file_remove(P) == 1, "removal");
    CHECK(!exists(P) && !exists(N), "neither the file nor its .new remains");
    CHECK(atomic_file_open_read(P, "rb") == NULL, "nothing left to read after removal");
    CHECK(atomic_file_remove(P) == 1, "removing what no longer exists is not an error");

    /* 7. A temporary-path buffer too small is refused, nothing is written. */
    {
        char tiny[8];
        CHECK(atomic_file_open(P, tiny, sizeof tiny) == NULL, "a buffer too small is refused");
        CHECK(!exists(N), "and nothing is created");
    }

    remove(P); remove(N);
    printf("%d checks, %d failure(s)\n", g_checks, g_fail);
    if (!g_fail) printf("OK\n");
    return g_fail ? 1 : 0;
}
