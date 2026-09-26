---
name: project-shadow-dual-key-crypto
description: "DECISIVE 2026-08-21 — Shadow's encryption is asymmetric in keys: client→server uses the key the CLIENT sends in its Encryption request (which we were discarding), server→client the one from the reply."
metadata:
  type: project
---

Shadow uses **two distinct chacha20-poly1305 keys, one per direction**.

- **server → client**: the 32 B key from the `Encryption` **reply**.
  A positive control: a `:base+10` video packet decrypts to 1241 B
  starting with `02 a4 aa ad` (the documented `VideoFrame` header), and a cursor
  packet on `:base+30` to 271 B. **[Label SUPERSEDED on 2026-09-11: that plaintext of
  271 B is an Opus session's stream descriptor (type `0x02`), not a
  cursor packet — `:base+30` is AudioOut alone (KB §3.37), see KB §3.26. The
  the positive decryption control still holds.]**
- **client → server**: the 32 B key **the client itself sends** in
  its `Encryption` **request** (the same f12 structure: keylen 32, noncelen 12,
  taglen 16, then the key). Our code generated it with `rand()` and then
  was **throwing away** — the comment described it as a "client_random / DH share"
  sans importance.

Proof: the 42 B packet emitted on `:base+13` does **not** decrypt with the
reply's key (neither as AEAD nor as a raw stream) but decrypts perfectly with
the request's. A converging clue: video and cursor share the suffix
nonce `e04dc3a3b0230db0558a1c`, and `:base+13` has its own (`b7d54df1…`).

The `:base+13` keepalive's content: a **constant** 14 B payload
`04 00 05 00 00 00 00 00 00 00 00 00 00 00`, every ~7 s, with only the counter
du nonce change.

`encryption.h` already provided for two Tx/Rx keys ("the Linux binary's CryptoCipher
layout"): the API was right, only the caller was wrong
(`shadow_cipher_create(server_key, server_key)`).

**Did NOT unblock input injection** — see [[project-input-port-not-base14]].

`KB.md` §3.23.
