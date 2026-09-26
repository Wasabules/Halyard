---
name: feedback-verify-grep-measurements
description: A count produced by a grep is wrong about three times in four in this repository. Use one to build a candidate LIST, never to state a fact.
metadata:
  node_type: memory
  type: feedback
---

# Verify a measurement before reporting it, especially one from a grep

A count that comes out of a grep must be verified **before** it is reported or
allowed to steer a decision. Four errors in a single session (2026-08-25), and a
fifth on 2026-09-02:

- *"dead code: sufp, smoke_test, shadowusb"* - all three were live (cursor,
  headless, connect). A deletion was avoided by a hair.
- *"0 compiler warnings"* - the command had lost its input file. Real: 12.
- *"28 untested allocations"* - the pattern matched `plain_buf` instead of
  `ctx.plain_buf`; corrected, a trailing `\b` then failed after a `]`. Real: 12,
  of which exactly one was a real defect.
- *"5 unbounded string operations"* - all five are bounded by their caller.

The fifth was reported **to the user** before being checked:

- *"the Joy-Con mouse page has 11 entries, the busiest in the menu, it needs a
  second level"* - the count was of i18n keys matching `menu/pad_mouse*`, six of
  which are only VALUE LABELS (`gyro_left`, `gyro_right`...) inside a single
  `choice` entry. Real: 5 entries, 8 at most on any page. The work announced had
  no reason to exist.

**Why here in particular**: this code is dense with macros, member access
(`r->chunks[i]`), guards placed on the following line, and conditionally
compiled paths. A naive pattern is nearly always wrong, and a wrong count either
hides a real bug or launches a destructive cleanup.

**How to apply it**: a grep produces a **candidate list**, never a finding.
Counting ENTITIES (menu entries, fields, calls) means counting the construction
of the entity, not the tokens that mention it. Open each case, or validate the
pattern against a known example before running it at scale. Report "N
candidates, M verified" rather than a raw total. When a measurement will drive a
deletion or a fix, verify all of it.

See [[feedback_automation_priority]]: automating stays the priority, but an
uncalibrated automatic tool produces falsehoods with authority.
