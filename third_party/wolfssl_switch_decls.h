// A forward declaration for CUSTOM_RAND_GENERATE_SEED, injected as a pre-include
// into the Switch wolfSSL build (implemented in core/services/quic_rand_switch.c).
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
extern int switch_rand_seed(unsigned char *output, unsigned int sz);
#ifdef __cplusplus
}
#endif
