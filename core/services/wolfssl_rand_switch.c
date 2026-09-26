// RNG seed source handed to wolfSSL through CUSTOM_RAND_GENERATE_SEED.
// wolfSSL has no /dev/urandom to read on Switch, so wc_InitRng fails with
// WC_INIT_E (-228). We give it libnx's randomGet() (the kernel-level HOS RNG),
// which provides cryptographically sound randomness.
/* Each platform NAMES its source. The `#else` used to mean `sys/random.h`,
 * which is true of Linux and of nothing else: a PS Vita build stopped here.
 * A platform this ladder does not know fails to compile ON PURPOSE -- unlike
 * the audio, where no output is a workable answer, a missing seed source is
 * not: silently returning zeros would hand wolfSSL a predictable RNG, which is
 * the worst possible way for this file to degrade. */
#include <stddef.h>
#ifdef __SWITCH__
#  include <switch.h>
#elif defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <bcrypt.h>
#elif defined(__vita__) || defined(__psp2__)
#  include <psp2/kernel/rng.h>
#elif defined(__linux__) || defined(__unix__)
#  include <sys/random.h>
#else
#  error "no RNG seed source for this platform - see wolfssl_rand_switch.c"
#endif

typedef unsigned char  byte;
typedef unsigned int   word32;

int switch_rand_seed(byte *output, word32 sz) {
#ifdef __SWITCH__
    randomGet(output, sz);
#elif defined(_WIN32)
    /* Windows : BCryptGenRandom (BCRYPT_USE_SYSTEM_PREFERRED_RNG = CNG kernel RNG).
     * Necessite linker bcrypt.lib (cf. CMakeLists.txt WIN32 branch). */
    NTSTATUS st = BCryptGenRandom(NULL, output, (ULONG)sz,
                                  BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (st != 0) return -1;
#elif defined(__vita__) || defined(__psp2__)
    /* PS Vita: the kernel RNG. Like libnx's randomGet on Switch, there is no
     * /dev/urandom to read, and wc_InitRng would fail with WC_INIT_E (-228).
     *
     * CHUNKED TO 64 BYTES, and that is the whole point. `sceKernelGetRandomNumber`
     * is documented "64 bytes maximum" (psp2/kernel/rng.h); this used to pass
     * `sz` straight through, under a comment admitting it had never run on
     * hardware. Measured 2026-09-13, first console session that ever reached
     * the TLS layer: `wolfSSL_CTX_new` succeeded and `wolfSSL_new` returned
     * NULL on all 15 attempts, on a socket that had CONNECTED - wolfSSL was
     * seeding its DRBG and got nothing.
     *
     * Note what this file already had three lines below: the Linux branch
     * loops over `getrandom`. The same author wrote both and looped only one -
     * a per-platform limit is exactly the thing a single unchecked call
     * hides. */
    {
        const size_t VITA_RNG_MAX = 64;   /* psp2/kernel/rng.h, hard limit */
        size_t off = 0;
        while (off < sz) {
            const size_t want = (sz - off) > VITA_RNG_MAX ? VITA_RNG_MAX
                                                          : (sz - off);
            if (sceKernelGetRandomNumber((unsigned char *)output + off,
                                         (SceSize)want) < 0) return -1;
            off += want;
        }
    }
#else
    /* Linux desktop : /dev/urandom via getrandom syscall */
    size_t off = 0;
    while (off < sz) {
        ssize_t r = getrandom(output + off, sz - off, 0);
        if (r < 0) return -1;
        off += (size_t)r;
    }
#endif
    return 0;
}
