# Controls

During a stream, Halyard presents your console to the machine as **a gamepad, a
mouse and a keyboard** at once. The buttons go to the gamepad; the touchscreen
and the controller mouse drive the mouse; the on-screen keyboard types. What the
game reacts to is up to the game.

## Nintendo Switch

### The gamepad

Every button and stick goes to a virtual gamepad on your machine — an
**Xbox 360 controller** by default, which the most Windows games understand.
Buttons are sent **by position**: the Switch's A, on the right of the diamond, is
the Xbox B, also on the right. The mapping, the controller type and the stick
dead zone are all in Settings › Controls › [Gamepad](settings.md#gamepad).

Two buttons have a second role when **held for half a second**:

| Button | Short press | Held half a second |
|---|---|---|
| **+** | the gamepad's Start (Menu) | opens the [pause menu](using.md#the-pause-menu) |
| **−** | the gamepad's Select (View) | shows or hides the on-screen keyboard |

A long press of + releases the game's Start as the menu opens, so the game sees a
half-second press.

### The controller mouse

**Click both sticks at once (L3 + R3)** to turn the controllers into a mouse, and
again to go back. While it is on, the gamepad is not sent:

| Input | Mouse |
|---|---|
| Right stick (or the gyroscope) | moves the pointer |
| Left stick | the wheel |
| **ZR** | left click |
| **ZL** | right click |
| **R** | middle click |

Pointing with the gyroscope, which Joy-Con aims and the sensitivity are in the
pause menu › Controls › Controller mouse, with a tester that shows the figures
without sending anything.

### The touchscreen

By default the screen works like a **laptop trackpad** (*relative* mode): slide
one finger to move the pointer. In *absolute* mode the pointer jumps to where you
touch. The mode, the sensitivity and the scroll direction are in the pause menu ›
Controls.

| Gesture | Default action |
|---|---|
| Tap, one finger | left click |
| Long press, one finger | drag — the button stays down until you lift |
| Tap, two fingers | right click |
| Slide two fingers up or down | the wheel |
| Tap, three fingers | on-screen keyboard |
| Long press, three fingers | pause menu |

Every tap, double tap and long press, for one, two and three fingers, can be
given another action in Settings › Controls › [Touch gestures](settings.md#touch-gestures):
a click, a double click, a drag, the keyboard, the pause menu, Escape, the
Windows key or Alt+Tab. A double tap left on *Automatic* simply repeats the tap,
so a single tap clicks at once.

### The on-screen keyboard

It opens over the bottom half of the screen (hold **−**, or tap with three
fingers); the part of the picture above it still works as a mouse.

- Four pages: the **letters** (AZERTY or QWERTY, set in Settings › Controls ›
  Layout), **navigation** keys, **F1–F12** and a **numeric pad**.
- A key is sent when you **lift** your finger; slide off it to cancel.
- **Shift, Ctrl and Alt** stay down for the next key.
- On the AZERTY letters, hold **a, e, i, o, u, c** or **y** to pick an accented
  letter (your machine must use a French keyboard layout).

### Games that capture the mouse

A game that locks the pointer (most shooters) expects **movement**, not
positions — otherwise the view spins away. Turn on **Game mouse mode** (pause menu
› Controls, or Settings › Controls) while you play it, and off again on the
desktop. Lowering the pointing sensitivity to 20–35 % makes aiming easier.

### Rumble

The game's vibration plays on the Joy-Cons or the Pro Controller. Its strength
is set in Settings › Controls › Gamepad › Motor intensity, which also has a test.

## PS Vita and PS TV

The same logic, with the Vita's buttons:

| Switch | PS Vita |
|---|---|
| **+** | **START** — hold it half a second for the pause menu |
| **−** | **SELECT** — hold it half a second for the on-screen keyboard |
| A B X Y | the face buttons, sent by position |
| ZL / ZR | none on a handheld Vita — see the rear touch panel below |
| L3 / R3 (stick clicks) | only on a PS TV controller |

- The **front touchscreen** works exactly as on the Switch, gestures included.
- The **rear touch panel** can stand in for the missing buttons: turn it on in
  Settings › Controls › Gamepad › *Use the rear panel*. Each quarter of the panel
  can be ZL, ZR, L3, R3 or an analogue trigger that you pull by sliding. By
  default the top two quarters are the left and right triggers.
- There is **no rumble and no gyroscope** on a Vita.
- On a handheld Vita, without stick clicks, turn the controller mouse on from
  the pause menu rather than with L3 + R3.

## Linux and Windows (development builds)

The computer's own **keyboard and mouse** go straight to the machine while the
stream has the focus; the pointer maps onto the picture. There is no long press
to open the pause menu: it is in the **developer menu bar** at the top of the
window (*Actions › Open Shadow menu*), which also sends the keys a computer keeps
for itself — Windows, Ctrl+Alt+Del, Alt+Tab, Win+D. In the pause menu, the arrow
keys move and a click selects.

These builds are made for developing and testing Halyard, not for everyday use.
