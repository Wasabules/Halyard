---
name: feedback_log_segment_per_launch
description: The log's timestamps restart from zero on every launch — segment before any temporal correlation
metadata:
  type: feedback
---

The network log is accumulated by appending: one file contains several
launches, and the timestamps **restart from zero** each time. Two
different sessions therefore have lines at the same timestamps.

**Why:** I concluded that a user click had cut the video stream, when
on the strength of a timestamp coincidence between two distinct launches.
Correctly segmented, the clicks arrived **7 seconds after** the freeze — the
causality was inverted. A temporal correlation on an unsegmented log
unsegmented is worth nothing.

**How to apply:** before any temporal analysis, split on the markers of a
start (`grep -n '^\[0\.000\] reseau:'`) and work on one launch only. The same
reflex applies to cumulative counters (`nals_total`, the G44 summaries): they
cross sessions and say nothing about an isolated one.

Voir KB.md §3.34.
