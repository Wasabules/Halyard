/* Prototype for the seed function wolfSSL is told to call through
 * CUSTOM_RAND_GENERATE_SEED, force-included into every wolfSSL translation
 * unit at build time.
 *
 * WHY THIS FILE EXISTS. wolfSSL's `random.c` writes
 * `return CUSTOM_RAND_GENERATE_SEED(output, sz);` and deliberately declares
 * nothing - the comment right above it tells the integrator to supply
 * `int rand_gen_seed(byte* output, word32 sz);` themselves. Under GCC 15,
 * whose default is C23, an implicit declaration is an ERROR, so without this
 * the library simply does not compile with the macro set.
 *
 * WHAT IT COST TO NOT HAVE IT (2026-09-13). `core/services/wolfssl_rand_switch.c`
 * has provided `switch_rand_seed` for the Vita since the port began, and its
 * very first line says it is "handed to wolfSSL through
 * CUSTOM_RAND_GENERATE_SEED" - but no build passed that macro, so the symbol
 * was never referenced by anything. wolfSSL kept its own `wc_GenerateSeed`,
 * which on this platform reads `/dev/urandom`; the Vita has none, `wc_InitRng`
 * failed, and `wolfSSL_new` returned NULL on every attempt. The console showed
 * fifteen of those on a socket that had connected.
 *
 * The check that finds this class of defect is not reading the code - it is
 * `nm`: the object file defined `switch_rand_seed` and NOT `wc_GenerateSeed`,
 * while `libwolfssl.a` defined `wc_GenerateSeed`. Two symbols that were
 * supposed to be the same thing, and nothing in either build complained.
 * `strings libwolfssl.a | grep /dev/urandom` says it just as fast: the Switch
 * archive has zero matches, and the Vita one had one. */
#pragma once
int switch_rand_seed(unsigned char *output, unsigned int sz);
