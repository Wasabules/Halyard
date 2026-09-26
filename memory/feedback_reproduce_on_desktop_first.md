---
name: feedback_reproduce_on_desktop_first
description: Check that a defect reproduces on Linux BEFORE treating it as Switch-specific
metadata:
  type: feedback
---

The silent audio channel was treated for hours as a console problem:
seven hypotheses, as many round trips with the user, a
manipulation humaine par essai (quitter l'app, pousser 22 Mo, relancer).

It reproduced **identically on Linux**, same VM, same native path:
`cursor=0` with the video at 86,745 packets. Thirty seconds to observe it,
une fois l'idee venue.

**Why:** the native path is the SAME code on both platforms. So a protocol defect
reproduces there, and on a desktop the loop costs a command instead of a manual
operation. Treating a defect as "console-specific" from the outset without having
tried it on a desktop is choosing the slowest loop
sans raison.

**How to apply:** before any campaign on the console, run
`SHADOW_NATIVE=1 ./shadow-client` on Linux against the same VM. If the defect is
there, the whole diagnosis happens locally. Only go back to the console for what
est reellement propre a HOS (pile reseau, decodage materiel, threads).

A corollary seen the same day: `ss -unap` compares a healthy client's sockets
against a broken one's in one line — that is what showed the cursor's socket as
`UNCONN *:*` where video and input were `ESTAB`. See KB.md §3.29.
