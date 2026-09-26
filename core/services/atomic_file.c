/* atomic_file - see the header. */
#include "atomic_file.h"
#include "config.h"
#include <sys/stat.h>
#ifdef _WIN32
#  include <direct.h>
#endif
#include <string.h>

#include <errno.h>
#include <string.h>

#if defined(_WIN32)
#  include <io.h>       /* _commit, the Windows fsync */
#else
#  include <unistd.h>   /* fsync */
#endif

static int make_tmp(const char *path, char *tmp, size_t cap)
{
    const size_t n = strlen(path);
    if (n + 5 > cap) return 0;
    memcpy(tmp, path, n);
    memcpy(tmp + n, ".new", 5);          /* the terminating NUL included */
    return 1;
}

FILE *atomic_file_open(const char *path, char *tmp, size_t tmp_cap)
{
    if (!path || !tmp || !make_tmp(path, tmp, tmp_cap)) return NULL;
    return fopen(tmp, "wb");
}

static void sync_to_storage(FILE *f)
{
    /* The bytes out of our buffer are not the bytes on the card: without this
     * the rename can land before the data, and a cut between the two leaves an
     * empty file under the right name - the very case the rename is for. A
     * refused sync is not fatal (some filesystems refuse it): the write still
     * happened, we simply cannot promise it survived a power cut. */
#if defined(_WIN32)
    (void)_commit(_fileno(f));
#else
    (void)fsync(fileno(f));
#endif
}

#if defined(__vita__) || defined(__psp2__)
#  include <psp2/io/stat.h>
#  include <psp2/io/fcntl.h>
#endif

int shadow_ensure_data_dir(void)
{
    char path[256];
    snprintf(path, sizeof path, "%s", SHADOW_DATA_DIR);
    size_t n = strlen(path);
    while (n > 1 && (path[n - 1] == '/' || path[n - 1] == '\\')) path[--n] = 0;

    struct stat st;
    if (stat(path, &st) == 0) return 1;

#if defined(__vita__) || defined(__psp2__)
    /* `sceIoMkdir` and not `mkdir`: newlib's wrapper does not resolve the
     * device prefix, and makes ONE level - so the parent first. */
    {
        char parent[256];
        snprintf(parent, sizeof parent, "%s", path);
        char *slash = strrchr(parent, '/');
        if (slash) { *slash = 0; sceIoMkdir(parent, 0777); }
    }
    sceIoMkdir(path, 0777);
#elif defined(_WIN32)
    _mkdir(path);
#else
    mkdir(path, 0755);
#endif
    return stat(path, &st) == 0;
}

/* See the header for WHY these exist. */
#if defined(__vita__) || defined(__psp2__)
#  include <psp2/io/fcntl.h>
#  include <errno.h>
/* The Sce code for "no such file". Mapped so that callers can keep testing
 * `errno == ENOENT`, which is what `atomic_file_remove` already did. */
#  define SHADOW_SCE_ENOENT 0x80010002

int shadow_file_rename(const char *from, const char *to)
{
    const int rc = sceIoRename(from, to);
    if (rc >= 0) return 0;
    errno = (rc == (int)SHADOW_SCE_ENOENT) ? ENOENT : EIO;
    return -1;
}

int shadow_file_remove(const char *path)
{
    const int rc = sceIoRemove(path);
    if (rc >= 0) return 0;
    errno = (rc == (int)SHADOW_SCE_ENOENT) ? ENOENT : EIO;
    return -1;
}
#else
int shadow_file_rename(const char *from, const char *to) { return rename(from, to); }
int shadow_file_remove(const char *path)                 { return remove(path); }
#endif

int atomic_file_commit(FILE *f, const char *tmp, const char *path, int keep)
{
    if (!f || !tmp || !path) return 0;
    int ok = keep != 0;
    if (ok && fflush(f) != 0) ok = 0;
    if (ok) sync_to_storage(f);
    if (fclose(f) != 0) ok = 0;
    if (!ok) { shadow_file_remove(tmp); return 0; }

    if (shadow_file_rename(tmp, path) == 0) return 1;   /* POSIX: atomic */
    /* Windows and the Switch refuse to rename onto an existing name. */
    shadow_file_remove(path);
    if (shadow_file_rename(tmp, path) == 0) return 1;
    /* Both refused. `path` is gone but `tmp` is complete: keep it, and
     * `atomic_file_open_read` will find it. Deleting it too would lose both. */
    return 0;
}

FILE *atomic_file_open_read(const char *path, const char *mode)
{
    if (!path) return NULL;
    FILE *f = fopen(path, mode);
    if (f) return f;
    char tmp[1024];
    if (!make_tmp(path, tmp, sizeof tmp)) return NULL;
    return fopen(tmp, mode);
}

int atomic_file_remove(const char *path)
{
    if (!path) return 0;
    char tmp[1024];
    int ok = 1;
    if (shadow_file_remove(path) != 0 && errno != ENOENT) ok = 0;
    if (make_tmp(path, tmp, sizeof tmp) && shadow_file_remove(tmp) != 0 && errno != ENOENT) ok = 0;
    return ok;
}
