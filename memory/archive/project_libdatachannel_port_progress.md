---
name: libdatachannel port Switch — bootstrap progress 2026-05-05
description: M-WebRTC-1 started. MbedTLS 3.6 LTS compiles, the libdatachannel CMake is OK, the libsrtp build fails (ntohl). The remaining estimate = ~13 days minimum.
type: project
---

> **ARCHIVED 2026-09-13 — a state, not a finding.** This records the libdatachannel path, abandoned 2026-05-06.
> It was true when written. Nothing here should be acted on: read `KB.md`
> for what holds today. Kept because knowing what was tried, and when, is
> what stops it being tried again.

## State at 2026-05-05 (≈45 min of work)

### ✅ Acquis
- libdatachannel cloned with its submodules (libsrtp, usrsctp, plog, libjuice, json) into `library/libdatachannel/`
- MbedTLS 3.6 LTS cloned at v3.6.4 + the framework submodule
- MbedTLS compiles for the Switch aarch64 in `library/libdatachannel/build_switch/mbedtls/build_switch/`
  - Patches applied in `mbedtls_config.h`:
    - `MBEDTLS_NO_PLATFORM_ENTROPY` enabled
    - `MBEDTLS_TIMING_C` disabled
    - `MBEDTLS_NET_C` disabled
    - `MBEDTLS_FS_IO` disabled
    - `MBEDTLS_PSA_ITS_FILE_C` disabled
    - `MBEDTLS_PSA_CRYPTO_STORAGE_C` disabled
    - `MBEDTLS_PSA_INJECT_ENTROPY` disabled
    - `MBEDTLS_PSA_CRYPTO_SE_C` disabled
    - `MBEDTLS_HAVE_TIME_DATE` disabled
    - `MBEDTLS_SSL_DTLS_SRTP` enabled (required by libdatachannel)
  - Patch `library/platform_util.c` : ajout `defined(__SWITCH__)` dans condition POSIX
  - Build : `libmbedtls.a`, `libmbedx509.a`, `libmbedcrypto.a` dans `mbedtls/build_switch/library/`
- libdatachannel CMake configure OK avec custom MbedTLS path

### ❌ Left to do
- libsrtp's missing ntohl/htonl: add `#include <arpa/inet.h>` to `srtp.c`
- The usrsctp aarch64 build (never attempted, probably +2 days of HOS patches)
- libdatachannel core code Switch HOS patches (estimation +3-5 jours)
- Integration with our Shadow stream (rewrite webrtc.c as a C++ wrapper, ~600 LoC)
- Tests + debugging the t=120 s freeze

### Estimation totale restante
- **~13 days** according to the original agent plan (14.5 estimated in total, 1.5 days already done)

### Risques majeurs
1. usrsctp has never been compiled for aarch64 Switch homebrew. Plan B: fork libdatachannel to swap usrsctp → our current `sctp.c`.
2. **The t=120 s freeze may not be resolved** — the server cap is applied outside the stack (28 hypotheses proving that the server ignores the client's signals). libdatachannel will change the structural fingerprint but that may not be enough.

### Commands pour reprendre
```bash
cd $REPO/05-shadow-client-borealis/library/libdatachannel/build_switch
# Fix libsrtp ntohl
sed -i '20i\#include <arpa/inet.h>' $REPO/05-shadow-client-borealis/library/libdatachannel/deps/libsrtp/srtp/srtp.c
make -j4
```

## Why

The prerequisite to validate before finishing the port: test libdatachannel on **Linux x86_64** (1-2 h) to see whether the stack resolves the t=120 s freeze. If the freeze persists on Linux too → a server-side cap confirmed independent of the stack → abandon B and accept the workaround. If the freeze disappears → finish the Switch port (12-13 days justified).
