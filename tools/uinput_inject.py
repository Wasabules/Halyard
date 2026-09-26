#!/usr/bin/env python3
"""uinput_inject.py — injecte souris/clavier via /dev/uinput (niveau noyau).

Why uinput and not xdotool/wtype: the session is GNOME/**Wayland**, which
forbids one application from synthesising events for another. uinput
creates a virtual device on the kernel side; the compositor treats it as a real
keyboard/mouse and routes to the focused window. Indifferent to X11/Wayland.

Prerequisite: write access to /dev/uinput (here through the `shadow-input` group,
so no sudo). Check with `--check`.

CAREFUL: the events go to the window that has FOCUS, not to a chosen process.
Focus the right window first, and prefer `--delay` to leave
temps de basculer.

Usage :
    uinput_inject.py --check
    uinput_inject.py moveto 3840 720   # absolu, bureau virtuel multi-ecran
    uinput_inject.py move 40 0
    uinput_inject.py click left
    uinput_inject.py key 125              # KEY_LEFTMETA
    uinput_inject.py scroll -3
    uinput_inject.py script seq.txt       # one command per line, '# ' = comment
                                          # 'sleep 0.5' is supported
"""
import ctypes, fcntl, os, struct, sys, time

UI_DEV_CREATE, UI_DEV_DESTROY = 0x5501, 0x5502
UI_SET_EVBIT, UI_SET_KEYBIT, UI_SET_RELBIT = 0x40045564, 0x40045565, 0x40045566
UI_SET_ABSBIT = 0x40045567
EV_SYN, EV_KEY, EV_REL, EV_ABS = 0x00, 0x01, 0x02, 0x03
SYN_REPORT = 0
REL_X, REL_Y, REL_WHEEL = 0x00, 0x01, 0x08
ABS_X, ABS_Y = 0x00, 0x01


class Pointer:
    """Reads the cursor position through XQueryPointer (libX11, XWayland).

    Indispensable: Mutter IGNORES the ABS events from our virtual
    virtual device (tested: no movement at all), and applies ACCELERATION to
    REL events — move(100,0) moves 75 px, move(-100,-100) moves 109.
    Positioning blind is therefore impossible; we close the loop on the
    measurement."""

    def __init__(self):
        import ctypes, ctypes.util
        self.ct = ctypes
        lib = ctypes.util.find_library('X11')
        self.x = ctypes.CDLL(lib) if lib else None
        self.d = self.x.XOpenDisplay(None) if self.x else None
        self.root = self.x.XDefaultRootWindow(self.d) if self.d else None

    def ok(self):
        return bool(self.d)

    def settled(self, timeout=0.25, quiet=0.03):
        """Waits for the position to settle, then returns it.

        Mutter applies our events asynchronously: reading back just after
        a move() returns a STALE position, and closing the loop on it diverges
        the loop (measured: 100-140 px errors, landing on values
        rondes = artefacts de lecture en retard)."""
        import time as _t
        end = _t.time() + timeout
        last = self.get(); stable = _t.time()
        while _t.time() < end:
            _t.sleep(0.008)
            cur = self.get()
            if cur != last:
                last = cur; stable = _t.time()
            elif _t.time() - stable >= quiet:
                return cur
        return last

    def get(self):
        c = self.ct
        a = [c.c_ulong(), c.c_ulong(), c.c_int(), c.c_int(),
             c.c_int(), c.c_int(), c.c_uint()]
        self.x.XQueryPointer(self.d, self.root, *[c.byref(v) for v in a])
        return a[2].value, a[3].value

    def child(self):
        """The X window under the cursor (0 if there is none) — used to CHECK that we
        is really aiming at the stream window and not another panel."""
        c = self.ct
        a = [c.c_ulong(), c.c_ulong(), c.c_int(), c.c_int(),
             c.c_int(), c.c_int(), c.c_uint()]
        self.x.XQueryPointer(self.d, self.root, *[c.byref(v) for v in a])
        return a[1].value


def virtual_screen():
    """The virtual desktop's extent (every screen), through xrandr.

    Indispensable on multi-screen: REL_X/REL_Y events are RELATIVE, so without
    an absolute position there is no telling which screen is being clicked. Here:
    2 2560x1440 panels side by side => 5120x1440."""
    import subprocess, re
    try:
        out = subprocess.run(['xrandr', '--listmonitors'],
                             capture_output=True, text=True, timeout=5).stdout
    except Exception:
        return 1920, 1080
    w = h = 0
    for m in re.finditer(r'(\d+)/\d+x(\d+)/\d+\+(\d+)\+(\d+)', out):
        mw, mh, mx, my = (int(g) for g in m.groups())
        w = max(w, mx + mw); h = max(h, my + mh)
    return (w or 1920), (h or 1080)
BTN = {"left": 0x110, "right": 0x111, "middle": 0x112}

class Injector:
    """Peripherique virtuel clavier + pointeur.

    The pointer exposes both REL (relative movement) and ABS (absolute position
    on the virtual desktop). The absolute is what allows aiming at a precise
    window on multi-screen; the relative stays useful for producing
    petits mouvements naturels une fois place."""

    def __init__(self, name=b"halyard-virtual-input", screen=None):
        self.ptr = Pointer()
        self.fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
        self.sw, self.sh = screen if screen else virtual_screen()
        # A PURELY RELATIVE MOUSE, deliberately WITHOUT absolute axes.
        # Measured: the keyboard and this device's MOVEMENTS do reach the Shadow
        # client (144B messages correlated with the keys), but the BUTTONS do
        # not. A mixed REL+ABS device can be classified as a tablet by libinput,
        # and a BTN_LEFT without BTN_TOOL_PEN is ignored there. The ABS axes were
        # useless anyway: Mutter ignored them (tested, no movement at all), and
        # the positioning is done in a closed loop on relative motion.
        for ev in (EV_KEY, EV_REL, EV_SYN):
            fcntl.ioctl(self.fd, UI_SET_EVBIT, ev)
        for rel in (REL_X, REL_Y, REL_WHEEL):
            fcntl.ioctl(self.fd, UI_SET_RELBIT, rel)
        for b in BTN.values():
            fcntl.ioctl(self.fd, UI_SET_KEYBIT, b)
        for k in range(1, 249):                      # the whole usual keyboard
            fcntl.ioctl(self.fd, UI_SET_KEYBIT, k)
        # struct uinput_user_dev : name[80] + input_id(4xu16) + ff_max
        #                          + absmax[64] + absmin[64] + absfuzz[64] + absflat[64]
        dev = name[:79].ljust(80, b"\0")
        dev += struct.pack("HHHH", 0x03, 0x1234, 0x5678, 1)   # BUS_USB
        dev += struct.pack("I", 0)
        dev += b"\0" * (4 * 64 * 4)                  # absmax/min/fuzz/flat inutilises
        os.write(self.fd, dev)
        fcntl.ioctl(self.fd, UI_DEV_CREATE)
        time.sleep(0.35)                             # laisser udev enregistrer

    def moveto(self, x, y, tol=2, max_iter=40):
        """Positionne le curseur en absolu, EN BOUCLE FERMEE.

        Mutter ignores our ABS events and accelerates the REL ones: so we can
        neither jump to a coordinate nor trust a delta. We read the real position
        after each step and correct until it converges. With no reading possible
        (no X11), we fall back to a best-effort ABS send."""
        x, y = int(x), int(y)
        if not (self.ptr and self.ptr.ok()):
            return None          # with no X11 reading, no reliable positioning
        # 1) slam into a corner to restart from a known bound.
        #    BOTTOM-RIGHT and not top-left: (0,0) is GNOME's *hot corner*, which
        #    opens the overview and STEALS FOCUS. Measured: with a pass through
        #    (0,0) before each click, the Shadow client emitted
        #    no click message at all — they were swallowed by the overview.
        for _ in range(3):
            self.move(20000, 20000); time.sleep(0.02)
        # 2) convergence asservie, sur position STABILISEE
        for _ in range(max_iter):
            cx, cy = self.ptr.settled()
            dx, dy = x - cx, y - cy
            if abs(dx) <= tol and abs(dy) <= tol:
                break
            # small steps on approach: the acceleration is negligible below
            # ~10 px, which gives a usable 1:1 ratio
            step = 150 if max(abs(dx), abs(dy)) > 200 else 6
            self.move(max(-step, min(step, dx)), max(-step, min(step, dy)))
        return self.ptr.settled()

    def _ev(self, typ, code, val):
        os.write(self.fd, struct.pack("llHHi", 0, 0, typ, code, val))

    def _syn(self):
        self._ev(EV_SYN, SYN_REPORT, 0)

    def move(self, dx, dy):
        if dx: self._ev(EV_REL, REL_X, int(dx))
        if dy: self._ev(EV_REL, REL_Y, int(dy))
        self._syn()

    def click(self, button="left", hold=0.06):
        code = BTN[button]
        self._ev(EV_KEY, code, 1); self._syn()
        time.sleep(hold)
        self._ev(EV_KEY, code, 0); self._syn()

    def key(self, code, hold=0.06):
        self._ev(EV_KEY, int(code), 1); self._syn()
        time.sleep(hold)
        self._ev(EV_KEY, int(code), 0); self._syn()

    def scroll(self, n):
        self._ev(EV_REL, REL_WHEEL, int(n)); self._syn()

    def close(self):
        try: fcntl.ioctl(self.fd, UI_DEV_DESTROY)
        except OSError: pass
        os.close(self.fd)

def run_cmd(inj, parts):
    c = parts[0]
    if   c == "move":   inj.move(int(parts[1]), int(parts[2]))
    elif c == "moveto": inj.moveto(int(parts[1]), int(parts[2]))
    elif c == "click":  inj.click(parts[1] if len(parts) > 1 else "left")
    elif c == "key":    inj.key(parts[1])
    elif c == "scroll": inj.scroll(int(parts[1]))
    elif c == "sleep":  time.sleep(float(parts[1]))
    else: raise SystemExit(f"commande inconnue : {c}")

def main():
    a = sys.argv[1:]
    if not a or a[0] == "--check":
        try:
            fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK); os.close(fd)
            print("OK: /dev/uinput is writable (no sudo required)")
        except Exception as e:
            print("ECHEC :", e); sys.exit(1)
        return
    inj = Injector()
    try:
        if a[0] == "script":
            for raw in open(a[1]):
                line = raw.split("#", 1)[0].strip()
                if line:
                    print(f"  [inject] {line}", flush=True)
                    run_cmd(inj, line.split())
        else:
            run_cmd(inj, a)
    finally:
        inj.close()

if __name__ == "__main__":
    main()
