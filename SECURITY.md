# Security

## Reporting a vulnerability

Report privately through GitHub's **Report a vulnerability** button (Security →
Advisories) rather than in a public issue. Please include what you did, what
happened, and what you expected. Expect an acknowledgement within a week.

This is a hobby project with no service behind it and no bug bounty. There is
no deadline you have to respect, but a fix may take a while.

## What this project handles, and what that means for you

Halyard speaks to your Shadow account. It therefore holds, at one moment or
another:

- an **OAuth refresh token** (`refresh_token` in the data directory);
- three short-lived **JWTs** and a streaming token;
- a per-session **ChaCha20-Poly1305 key** and a 20-byte authentication hash,
  both handed over by the server during the bootstrap.

The refresh token is stored XOR-obfuscated. **That is obfuscation, not
encryption** — it stops a casual look at the SD card, and nothing more. Anyone
with physical access to the card can recover it. The optional app lock (PIN,
pattern or password) protects *opening the application*; it explicitly does not
protect the data beside it.

## Never publish a raw log

**This is the rule that matters most here, and it has been broken before.**

Log lines are mirrored over the network when `logsink.txt` is present, and they
get copied off the card to be read. Until 2026-09-11 the session's ChaCha20 key,
the authentication hash and a streaming token were written to them whole. Worse,
an old hex dump of one server reply wrote a **live private key** into the log in
the clear.

Since then:

- every secret goes through `core/services/log_mask.h` — first and last bytes
  only, never more than a quarter of the value (`tests/test_log_mask.c` pins
  this);
- `journal.c`'s `redact()` is a second line of defence over `token=`,
  `Bearer `, `chacha20_key=` and `auth_hash=`;
- `.gitleaks.toml` scans the tree;
- and the journal redacts by **shape** on its way out
  (`core/services/log_redact.h`): a JWT, a PEM private-key body and any
  contiguous run of 32+ hex characters are masked wherever they appear, whatever
  label precedes them.

That last one replaced a list of four literal markers, which is worth
explaining because it is the general lesson. A marker list is a list of the
mistakes you already know about: `smoke_test.c` wrote `tok=` instead of
`token=`, and its own labels around its own spaced hex, so every one of its five
leaks walked straight past. A shape rule does not need to have anticipated the
label.

**It is deliberately incomplete, and you should know where.** Spaced hex
(`41 01 00 14 00`) is left alone, because that is how this project logs protocol
wire and masking it would blind the video and framing diagnosis entirely. So a
secret printed as spaced hex still gets through — which is exactly how
`smoke_test.c`'s 20-byte auth hash escaped, and it was fixed at the call site,
not here. **Nothing replaces masking where the value is known**
(`core/services/log_mask.h`). `tests/test_log_redact.c` pins both halves: what
must be masked, and the eight real log lines that must come through untouched.

None of that makes an old log safe. **Before attaching a log to an issue, read
it.** If you are unsure, do not attach it — describe the symptom instead, and
say which build you were running.

Packet captures are worse than logs and must never be published at all.

## Scope

Halyard reimplements a protocol from observation of the official client. It
needs your own, valid Shadow subscription: it does not bypass authentication,
does not circumvent any access control, and gives you nothing you were not
already entitled to. Reports about how to obtain service you have not paid for
are out of scope and will not be acted on.
