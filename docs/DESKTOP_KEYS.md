# Simulating a Switch from the keyboard — what works, and what does not

The desktop client exists to film and measure the UI without a console. This
document says **exactly** what can be reproduced there, because the honest answer
is not "everything".

## The keys, as Borealis translates them

The mapping comes from
`third_party/borealis/library/lib/platforms/glfw/glfw_input.cpp` — it is **in the
vendored library**, not in our code, so it cannot be adjusted from the app.

> **K20, 2026-09-10 — this table was wrong, it is now right.** Until that date,
> only **three** rows existed: `Enter`, `Escape` and the arrows. `X`, `Y`, `L`,
> `R`, `F1`, `F2`, `Q` and `P` were mapped nowhere — neither in the polling nor in
> the `keyboardCallback`, which only broadcasts a generic event. Reading
> "Ⓨ Settings" at the bottom of the screen, pressing `Y` and seeing nothing happen
> was the normal behaviour. The eight keys were added by a local Borealis patch
> (`patches/borealis/`, to be re-applied after a fresh clone).
>
> **They stay silent during a streaming session**, and that is deliberate: the
> physical keyboard belongs to the VM then, where `stream_view.cpp` already sends
> it. Without that safeguard, typing `Y` in a game would also open the pause menu.
> `Enter`, `Escape` and the arrows do stay active in session as before —
> neutralising them would have been a change nothing has measured.
>
> **The bottom bar now names the key** (`Enter`, `Esc`, `X`, `F2`…) instead of the
> gamepad glyph, *except* when a gamepad is plugged in: there, the glyph is the
> correct thing.

| Keyboard | Switch button | What it does in our app |
|---|---|---|
| `Enter` | **A** | confirm, open, toggle a setting |
| `Escape` | **B** | back, close a mode |
| `X` | **X** | refresh (machine list), reload (log) |
| `Y` | **Y** | Settings (machine list), a section's detail (pause menu), previous session (log) |
| `L` | **L** | previous section |
| `R` | **R** | next section |
| `F1` | **−** | virtual keyboard (long press in session) |
| `F2` | **+** | pause menu (long press), confirming block placement |
| `Q` | **L3** | left stick click |
| `P` | **R3** | right stick click |
| `↑ ↓ ← →` | d-pad | navigation, adjusting a value |

**Careful with A/B**: Borealis swaps the two according to the system setting of the
console it believes it is imitating (`glfw_input.cpp:294-301`). If `Enter` confirms
for you, `Escape` goes back; if the opposite happens, that setting is why.

## What has NO keyboard equivalent

**The analogue sticks.** No key simulates them. The pause menu now accepts the left
stick (S97): that part can only be tested with a gamepad plugged in over USB, which
GLFW recognises. The d-pad performs the same navigation, so nothing is unreachable
— but the *stick path* is not covered.

**ZL / ZR.** Absent from the table. They do nothing in our UI — they are only
forwarded to the remote machine — so they block no journey.

**Touch.** That is the real limit, and it is a wide one. Everything that reads
`hidGetTouchScreenStates` is compiled under `__SWITCH__` and **does not exist** on
desktop:

- placing the measurement blocks (drag, snap, detach, double tap);
- tapping the footer hints (`A Connect`, `X Settings`);
- tapping a machine thumbnail;
- the grabbable scrollbar;
- the stream's gestures (touch mouse, two-finger scrolling).

Those journeys **cannot be filmed from the desktop**. The demo pointer shows the
mouse, not a finger: the mouse drives the Borealis views, it produces no touch
event.

## Filming a complete journey

`Settings › Account` carries two actions made for this:

- **Replay from the beginning** — returns to the boot screen without closing the
  window. The session stays open: you replay the loading.
- **Replay, pairing included** — forgets the token, so the code to scan reappears.
  That is the one that lets you film from pairing to disconnection in one take.

`Settings › Advanced` carries the **demo pointer**: a disc that marks the click,
with a ripple that stays visible for half a second. Without it, a capture shows a
menu opening without showing the gesture that opened it.

## What differs from real hardware, and must be known before measuring

- **Sound** goes out through ALSA and not `audout`: the queue measured is not the
  same one (see `KB.md`, L11/L12).
- **Decoding** goes through NVDEC/CUDA and not `nvtegra`. The timings do not carry
  over.
- **Haptic feedback** cannot be felt: no motors.
- **The UI's scale** is the window's; at exactly 720p it matches handheld mode.
