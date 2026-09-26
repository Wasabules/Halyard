---
name: feedback-automation-priority
description: Automate the testing/RE loop end to end and RUN it, rather than handing the user a recipe. The one exception is the interactive OAuth login.
metadata:
  node_type: memory
  type: feedback
---

# Automate and run it, do not delegate the running

**Rule**: for any testing, RE or measurement task, automate end to end with a
script rather than asking the user to run commands by hand.

**Why**: the owner asked for this explicitly ("you normally have everything you
need to automate all that", 2026-05-22) in reaction to a manual recipe being
proposed. The headless CLI, `bench_runner.py` and the campaign scripts exist for
that. Asking for a manual run is friction and it costs iterations.

**How to apply**:

- To measure an effect while streaming, drive the headless runner rather than
  the GUI, and analyse its dump offline.
- To A/B a patch: `tools/client/bench_runner.py --treatment-patch X.patch --runs 3`.
  **Always three runs or more** - single-run variance is large here.
- To validate an RE hypothesis: instrument a dump (`SHADOW_DUMP_*`) and analyse
  it offline in python.
- **Exception**: the GUI OAuth login, once. The token then persists in the data
  directory and everything after it is automatable.

**Anti-patterns**:

- "Run this and paste me the output" - no, run it.
- "Try this flag and tell me what you see" - no, test it and report the verdict.
- "Can you try X?" to validate a hypothesis - no, try it.

## The devui trap (G47, 2026-08-22)

`MenuBar::draw()` calls `layout()` on EVERY draw, and `layout()` evaluates
`Info::measure()` for every item of every menu, **including closed ones**. So an
`info()` callback runs on the RENDER thread at 60 Hz and must be pure memory
access. A system probe there collapsed rendering to 5 fps - it was scanning 26
`/dev/input/event*` per frame.

**Cross-ref**: [[reference_switch_dev_workflow]],
[[project_iter_2026-05-06_bandwidth_caps]], [[feedback_verify_grep_measurements]]
