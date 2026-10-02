/* launch_uri - see launch_uri.h, in particular why this does not look for
 * winscp.exe. */
#include "launch_uri.h"

#include <stddef.h>
#include <string.h>

#if defined(_WIN32)

#include <windows.h>
#include <shellapi.h>
#include <stdlib.h>

bool launch_uri_available(void) { return true; }

bool launch_uri(const char *uri)
{
    if (!uri || !uri[0]) return false;

    /* Widened before the call. `ShellExecuteA` exists, but it takes the ANSI
     * code page: a URI holding a percent-escape is pure ASCII today, and will
     * not be the day a host name is not. Converting here costs nothing and
     * removes the question. */
    const int wn = MultiByteToWideChar(CP_UTF8, 0, uri, -1, NULL, 0);
    if (wn <= 0) return false;
    wchar_t *w = (wchar_t *)malloc((size_t)wn * sizeof(wchar_t));
    if (!w) return false;
    if (MultiByteToWideChar(CP_UTF8, 0, uri, -1, w, wn) <= 0) { free(w); return false; }

    /* `ShellExecuteEx` rather than `ShellExecute`, for ONE flag:
     * SEE_MASK_FLAG_NO_UI. Without it, a machine with no `sftp://` handler gets
     * Windows' own "how do you want to open this?" or "look for an app in the
     * Store" dialog - on top of a full-screen stream, where it is at best
     * confusing and at worst invisible. With it, the call simply fails and the
     * caller gets to say something useful instead.
     *
     * The verb is NULL, not "open": for a scheme handler the default verb is
     * whatever the registry says, and naming one overrides a choice the user
     * may have configured. SW_SHOWNORMAL so the file manager comes up visible
     * rather than behind our window. */
    SHELLEXECUTEINFOW ei;
    memset(&ei, 0, sizeof ei);
    ei.cbSize = sizeof ei;
    ei.fMask  = SEE_MASK_FLAG_NO_UI;
    ei.lpVerb = NULL;
    ei.lpFile = w;
    ei.nShow  = SW_SHOWNORMAL;
    const BOOL ok = ShellExecuteExW(&ei);
    free(w);
    return ok ? true : false;
}

#elif defined(__SWITCH__) || defined(__vita__) || defined(__psp2__)

/* No second application to hand a URI to. */
bool launch_uri_available(void) { return false; }
bool launch_uri(const char *uri) { (void)uri; return false; }

#else   /* Linux and the other desktops */

#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>

bool launch_uri_available(void) { return true; }

bool launch_uri(const char *uri)
{
    if (!uri || !uri[0]) return false;

    /* `fork` + `execvp`, NOT `system()`: the URI holds a password, and
     * `system()` would hand it to a shell that expands `$`, backticks and `;`.
     * An argv entry is passed through untouched. */
    const pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        /* The child. A double fork so the grandchild is reparented to init and
         * we never have to wait for the file manager to exit - this function
         * must not block a UI callback, and an unreaped child would otherwise
         * sit as a zombie for the rest of the session. */
        const pid_t pid2 = fork();
        if (pid2 == 0) {
            char *const argv[] = { (char *)"xdg-open", (char *)uri, NULL };
            execvp(argv[0], argv);
            _exit(127);          /* xdg-open absent: nothing more to try */
        }
        _exit(pid2 < 0 ? 127 : 0);
    }
    /* Reap the middle child only. It exits immediately, so this does not wait
     * on anything the user is doing. */
    int st = 0;
    (void)waitpid(pid, &st, 0);
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

#endif
