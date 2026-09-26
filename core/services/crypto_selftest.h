/* See crypto_selftest.c: an offline bench that says whether the libcrypto
 * computes CORRECTLY and HOW FAST, with no network and no session. Triggered
 * by SHADOW_CRYPTO_SELFTEST=1 in env.txt; returns the number of failures, and
 * 0 when the bench was not asked for. */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
int shadow_crypto_selftest(void);
#ifdef __cplusplus
}
#endif
