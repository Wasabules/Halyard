> **The base is `3ecf2de10226392ecb071c470bea9758a24cd6b1`** of
> `xfangfang/borealis` (2025-02-25), established on 2026-09-26 by comparing the
> git hash of EVERY file of the vendored copy - all 2878 under `library/`,
> submodules included - against the 1310 upstream commits. A fresh clone of that
> commit with `submodule update --init --recursive` differs from what we build
> on exactly nine files, and those nine are our local fixes. `local-fixes.patch`
> was regenerated against it and PROVEN on a fresh clone: it applies cleanly and
> reproduces all nine byte for byte.
>
> What it replaces, and why it matters for anyone cloning this repository: the
> previous pin read `5000a1e5`, a commit that **does not exist** upstream (the
> real one is `5000a1e4...` - a typo in a short SHA), identified on 2026-09-12
> from eight files only. So `tools/bootstrap-libs.sh` could not fetch Borealis at
> all, and the patch had stopped applying ("four hunks fail") because it had
> been made against the wrong base. A clone of the public repository could not
> have been built. The pin is now a FULL SHA: a short one is how the typo went
> unseen.
>
> The tenth fix, GXM-1, is why this matters. `third_party/borealis/` is in
> `.gitignore`, so nothing in git records its contents: a line deleted there is
> invisible to `git status`, survives every commit, and makes rebuilding "the
> same commit" produce a DIFFERENT binary. That is exactly what happened — the
> Vita build crashed two seconds into its main loop while the repository was
> clean, and checking out the previous commit reproduced the crash, which
> wrongly cleared the code under suspicion.
>
> So the fixes live in `local-fixes.patch` (548 lines, nine files), and
> `tools/bootstrap-libs.sh` clones the right commit and applies it. The
> `.extract` files stay because they carry the REASONING — a patch shows what
> changed, they say why.

# Local Borealis patches

`third_party/borealis/` is **ignored by git**. So the patches below survive
neither a fresh clone nor a re-vendoring. They are archived here so they can be
re-applied - `tools/bootstrap-libs.sh` does it, from `local-fixes.patch` - and
above all so that their existence is VISIBLE: without this directory, they
disappear silently.

`./apply.sh` puts them back and reports the ones that are already there.

## S63 (2026-08-27) — `giveFocus(nullptr)` was a complete no-op

`Application::giveFocus` (`library/lib/core/application.cpp`) resolves its target
through `getDefaultFocus()`, and then the `newFocus != nullptr` guard skips the
whole body. For `giveFocus(nullptr)`, `newFocus` is nullptr: **nothing** ran, and
`Application::currentFocus` was therefore NEVER reset.

Consequence: the "Focus sanity check" in `View`'s destructor (`view.cpp:1486`),
which calls precisely `giveFocus(nullptr)`, was dead code. Any view destroyed
while holding focus left `currentFocus` dangling, and the next virtual call on it
(`oldFocus->onFocusLost()`, reached from
`ScrollingFrame::naturalScrollingBehaviour()`) jumped into a freed vtable — an
Atmosphère "Instruction Abort" report.

This patch is a SAFETY NET. The cause of the reported crash is fixed in our own
code (`clients/borealis/activity/vm_list_activity.cpp`, S63), which git does track.

## Null-focus guard in both `ScrollingFrame`s

`naturalScrollingBehaviour()` dereferenced `Application::getCurrentFocus()` with no
null check. Before S63 that pointer was never null, so the guard was unreachable;
since S63 it is reachable, and it is necessary. Present identically in
`scrolling_frame.cpp` and `h_scrolling_frame.cpp`.

## K20 (2026-09-10) — the desktop keyboard, displayed and finally mapped

Two patches that go together, and a document that was lying.

`docs/DESKTOP_KEYS.md` had been announcing eleven keyboard mappings for months.
**Three existed**: Enter, Escape and the arrows (`glfw_input.cpp`). X, Y, L, R, F1,
F2, Q and P were mapped NOWHERE — neither in the polling nor in the
`keyboardCallback`, which only broadcasts a generic event. Anyone reading
"(Y) Settings" in the bottom bar pressed Y and nothing happened.

- **`glfw_input.cpp`** (`glfw_input_keys.extract`) adds the eight missing keys,
  **behind a safeguard**: during a streaming session they belong to the VM.
  `stream_view.cpp` already sends the physical keyboard there, and letting Borealis
  act on it AS WELL would open a menu in the middle of a game (`stream_activity`
  binds BUTTON_Y and BUTTON_START). The safeguard is a **weak** symbol
  (`glfw_input_hook.extract`) that `stream_view.cpp` defines: an unpatched Borealis
  links and behaves exactly as before.
- **`hint.cpp`** (`hint.extract`) names the KEY instead of the button, and only
  when **no gamepad is plugged in** — with a gamepad, the glyphs are the correct
  thing. The A/B swap is read before `mapControllerState`, otherwise Enter and
  Escape are inverted half the time.

Known limit, written in the code: the polling does not know whether text entry is
in progress, so typing "x" into a field also triggers the X action. The defect
already existed for Enter and Escape; these keys extend it.

## INJ-1 (2026-09-12) — input injection, the lowest layer

`btn`, `nav` and `tap` were documented in `DEVLINK.md`, parsed by `devcmd.h` and
sent by `devlink.py` since 2026-08-27 — and **nothing served them**: `devlink.cpp`
returned `false` saying that injection "belongs to the input module", and that
module had never been written. The channel could observe, not act; `state` was a
position report with no way to move. Found by navigating on the console and
watching `state` not flinch.

Two **weak** symbols, called by both platforms' input handlers
(`glfw_input_inject.extract`, `switch_input_inject.extract`):

```c
extern "C" __attribute__((weak)) void halyard_inject_controller(bool*, size_t, float*, size_t);
extern "C" __attribute__((weak)) bool halyard_inject_touch(float*, float*);
```

An unpatched Borealis sees them as null and behaves exactly as before.

**Why this layer and not another.** Injecting higher up —
`Application::onControllerButtonPressed`, or a screen's virtual method — would have
covered the menus and silently missed three things: holds (`ui/hold_exit.hpp` reads
the STATE frame by frame, not events), the stream (which forwards the gamepad to
the VM), and touch gestures. Here, everything downstream sees a press it cannot
distinguish from a real one.

**By an OR, never a replacement**: what is injected adds to the real gamepad. A
script driving the console must not prevent a human from taking over — on a device
where the two share a screen, that is not a courtesy.

On the GLFW side there are **two** call sites: the function returns early when the
window does not have focus, and a test bench driven from a terminal almost never
does. Returning without injecting would make the engine useless on desktop —
exactly where this repo insists on reproducing before blaming the console.

The duration logic is NOT here: it lives in `clients/borealis/devlink/inject.h`, pure and
covered by `tests/test_inject.c` — the hard part of injection is not pressing, it
is pressing long enough for a frame to see it, and no longer.
