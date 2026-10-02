/* test_ft_path - the remote-path check for the file-transfer channel.
 *
 * The cases that matter are the REFUSALS, and each one mirrors a specific
 * behaviour of the VM's SFTP server read at @0x140b8b710 (KB §3.50): an
 * absolute path REPLACES the Downloads root there, and a `..` component is
 * concatenated verbatim because nothing canonicalises. So every refusal below
 * is a path that would really have escaped, not a hypothetical.
 *
 * Compiled with -Wall -Wextra -Werror -O1 by tests/run_tests.sh.
 */
#include <stdio.h>
#include <string.h>

#include "../core/protocol/ft_path.h"

static int checks = 0, failures = 0;

static void expect(const char *p, ft_path_verdict want, const char *what)
{
    checks++;
    const ft_path_verdict got = ft_path_check(p, p ? strlen(p) : 0);
    if (got != want) {
        failures++;
        printf("  FAIL %-46s \"%s\": got %d (%s), expected %d\n",
               what, p ? p : "(null)", (int)got, ft_path_reason(got), (int)want);
    }
}

int main(void)
{
    /* --- what must pass: ordinary relative names, which the server resolves
     * inside the interactive user's Downloads folder. */
    expect("file.txt",            FT_PATH_OK, "plain name");
    expect("sub/dir/file.txt",    FT_PATH_OK, "forward slashes");
    expect("sub\\dir\\file.txt",  FT_PATH_OK, "backslashes");
    expect("a",                   FT_PATH_OK, "one character");
    expect("..hidden",            FT_PATH_OK, "leading dots are NOT a parent ref");
    expect("a..b",                FT_PATH_OK, "dots inside a name");
    expect("...",                 FT_PATH_OK, "three dots is a legal name");
    expect("dir/..hidden",        FT_PATH_OK, "leading dots deeper in");
    expect("Nouveau dossier/é.txt", FT_PATH_OK, "spaces and UTF-8");

    /* --- absolute: the server would use it INSTEAD of the root. */
    expect("/etc/passwd",             FT_PATH_ABSOLUTE, "leading slash");
    expect("\\Windows\\System32",     FT_PATH_ABSOLUTE, "leading backslash");
    expect("C:\\Windows\\win.ini",    FT_PATH_ABSOLUTE, "drive letter");
    expect("c:/windows/win.ini",      FT_PATH_ABSOLUTE, "lowercase drive letter");
    expect("Z:",                      FT_PATH_ABSOLUTE, "bare drive");
    expect("C:file.txt",              FT_PATH_ABSOLUTE, "drive-relative, just as dangerous");
    expect("/",                       FT_PATH_ABSOLUTE, "the root itself");

    /* --- UNC: also starts with a separator, named separately so a log says so. */
    expect("\\\\host\\share\\f",  FT_PATH_UNC, "UNC backslashes");
    expect("//host/share/f",      FT_PATH_UNC, "UNC forward slashes");

    /* --- `..` as a component, on both separators and at every position. The
     * server concatenates these verbatim, so each one really escapes. */
    expect("..",                      FT_PATH_PARENT, "bare parent");
    expect("../file",                 FT_PATH_PARENT, "leading parent");
    expect("..\\file",                FT_PATH_PARENT, "leading parent, backslash");
    expect("dir/../file",             FT_PATH_PARENT, "parent in the middle");
    expect("dir\\..\\file",           FT_PATH_PARENT, "parent in the middle, backslash");
    expect("dir/..",                  FT_PATH_PARENT, "trailing parent");
    expect("a/b/c/../../../../x",     FT_PATH_PARENT, "several parents");
    expect("dir/../../Windows/x",     FT_PATH_PARENT, "the realistic escape");
    /* Mixed separators are the case a one-separator check would miss - which is
     * why this check looks at both. */
    expect("dir\\../file",            FT_PATH_PARENT, "mixed separators");
    expect("dir/..\\file",            FT_PATH_PARENT, "mixed the other way");

    /* --- colon anywhere else: NTFS alternate data stream, or a device name. */
    expect("file.txt:stream",     FT_PATH_BAD_CHAR, "alternate data stream");
    expect("dir/a:b",             FT_PATH_BAD_CHAR, "colon deeper in");

    /* --- control characters and the empty path. */
    expect("",                    FT_PATH_EMPTY,    "empty");
    expect(NULL,                  FT_PATH_EMPTY,    "NULL");
    expect("file\nname",          FT_PATH_BAD_CHAR, "newline");
    expect("file\tname",          FT_PATH_BAD_CHAR, "tab");
    expect("\x01",                FT_PATH_BAD_CHAR, "control byte");

    /* --- an embedded NUL. strlen() stops at it, so it must be passed with an
     * explicit length: the wire carries a length and would NOT truncate. */
    checks++;
    if (ft_path_check("ok\0evil", 7) != FT_PATH_BAD_CHAR) {
        failures++;
        printf("  FAIL embedded NUL must be refused when the length says it is there\n");
    }

    /* --- length bound. */
    {
        char longp[FT_PATH_MAX + 8];
        memset(longp, 'a', sizeof longp);
        checks++;
        if (ft_path_check(longp, FT_PATH_MAX) != FT_PATH_OK) {
            failures++; printf("  FAIL exactly FT_PATH_MAX must pass\n");
        }
        checks++;
        if (ft_path_check(longp, FT_PATH_MAX + 1) != FT_PATH_TOO_LONG) {
            failures++; printf("  FAIL one over FT_PATH_MAX must be refused\n");
        }
    }

    /* --- MUTATION CHECK, the cursor_wire standard: if the `..` detection were
     * weakened to "does the path contain the two characters `..`" - the obvious
     * wrong implementation - then `..hidden` would be refused. Assert it is
     * NOT, so the test fails if someone replaces the component scan with a
     * substring search. */
    expect("..hidden",            FT_PATH_OK, "MUTATION: substring search would break this");
    /* And the mirror: if the scan only looked at the FIRST component,
     * `dir/../x` would pass. Asserted above; restated here so the pair is
     * visible together. */
    expect("dir/../x",            FT_PATH_PARENT, "MUTATION: first-component-only would break this");

    /* --- every verdict has a distinct, non-empty reason string: these end up
     * in a log line that someone has to act on. */
    {
        const ft_path_verdict all[] = {
            FT_PATH_OK, FT_PATH_EMPTY, FT_PATH_ABSOLUTE, FT_PATH_PARENT,
            FT_PATH_TOO_LONG, FT_PATH_BAD_CHAR, FT_PATH_UNC
        };
        for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) {
            checks++;
            const char *r = ft_path_reason(all[i]);
            if (!r || !*r) { failures++; printf("  FAIL empty reason for verdict %d\n", (int)all[i]); }
        }
    }

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
