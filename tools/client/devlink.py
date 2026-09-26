#!/usr/bin/env python3
"""devlink - drives the Shadow client's interface from the development machine.

Checking a visual change used to cost a human gesture: launch the application,
look at the screen, describe what you see. This tool removes that gesture - it
captures the screen, presses buttons and reads the current state, which lets an
assistant close its own loop on the interface.

THE DIRECTION OF THE CONNECTION - the point that surprises everyone:
it is the APPLICATION that connects to US. It reads `logsink.txt` on the SD card
(`address:port`), opens an outbound socket to this machine, pours its log into it
and READS its commands from it (core/services/journal.c). So this tool is a
SERVER: it listens, waits for the connection, then talks. Nothing listens on the
console side; there is nothing to reach from outside.

Practical consequences, all handled here:
  - a command sent before the application is connected WAITS (a guard delay plus
    a clear message) instead of failing;
  - since DEVL-7 the application RETRIES, with a backoff from 1 s to 30 s, so the
    listener no longer has to be armed before the application - and a channel
    lost mid-session comes back on its own;
  - only one process can hold the port. `listen` therefore acts as a RELAY for
    the other invocations (a local socket), and a second attempt to take the port
    says so plainly instead of leaking "address already in use".

Usage:
    tools/client/devlink.py listen [journal.log]   listen, print the log, act as relay
    tools/client/devlink.py shot [file.png]        capture the screen
    tools/client/devlink.py state                  describe the current screen
    tools/client/devlink.py btn a                  press and release a button
    tools/client/devlink.py nav down               navigate (the d-pad)
    tools/client/devlink.py tap 640 360            touch at the given position
    tools/client/devlink.py cmd "<text>"           send a raw line (quit, ping...)
    tools/client/devlink.py hold b 3000            hold a button (the "hold to" pages)
    tools/client/devlink.py stick left 0 -100      push a stick
    tools/client/devlink.py swipe 100 200 900 200  drag a finger
    tools/client/devlink.py release                let go of everything
    tools/client/devlink.py version                which build is running, from which .nro
    tools/client/devlink.py env [SHADOW_X=1]       read/change a toggle (next launch)
    tools/client/devlink.py quit                   close the application (to push a .nro)
    tools/client/devlink.py relaunch [path]        close AND load a .nro instead
    tools/client/devlink.py script plan.txt        play a list of commands
    tools/client/devlink.py --autotest             check the tool itself, offline

Exit codes (an assistant tests these):
    0 success
    1 the application answered `err`, or the capture is invalid
    2 an argument refused HERE, before anything was sent
    3 no application connected within the delay
    4 the port is already taken by something else

Python 3, standard library only.
"""

import argparse
import base64
import binascii
import errno
import os
import re
import select
import socket
import struct
import sys
import tempfile
import threading
import time
import zlib
from datetime import datetime

# ── Reglages ────────────────────────────────────────────────────────────────

# The same default port as tools/switch-logsink.sh: both tools speak to the same
# socket, and a different port would make `setup` useless here.
PORT_DEFAULT = int(os.environ.get("LOGSINK_PORT", "9999"))

# The relay's local entry point. A UNIX socket rather than a TCP port: it is not
# exposed to the network and disappears with the temporary directory.
RELAY_DEFAULT = os.environ.get(
    "DEVLINK_RELAIS", os.path.join(tempfile.gettempdir(), "shadow-devlink.sock"))

MARKER = "[devlink]"

# journal.c prefixes EVERY line with its timestamp relative to startup
# ("[12.345]", added by S27 on 2026-08-22, also sent over the network since
# 2026-08-25). Without stripping it, NO reply is recognised: the first real run
# would have ended in "timed out" on every command.
#
# 2026-09-11 - AND THE SEVERITY/CATEGORY COLUMN. Since S81 (2026-08-29),
# shadow/journal.c writes "[12.345] I/systeme  [devlink] state ...": a severity
# letter, a slash, a category padded to eight characters. This stripping knew
# only the timestamp, so the line began with "I/systeme" and no reply was
# recognised any more - devlink stopped working entirely, "timed out" on every
# command, while the application had answered in 0.35 s (seen on the Windows
# desktop). The column stays optional so pre-S81 logs can still be read. See
# shadow/journal_line.h.
RE_TIMESTAMP = re.compile(r"^\[\d+\.\d{3}\]\s*(?:[A-Z]/\S+\s+)?")

BUTTONS = ("a", "b", "x", "y", "l", "r", "zl", "zr", "plus", "minus")
# The words the APP parses (devcmd.h). They were French until the 2026-09-12
# migration renamed them on the app's side only: from then on the tool refused
# `nav down` and sent `nav bas`, which the app rejects - nav and stick were dead
# for two weeks while --autotest, which checks this file against itself, stayed
# green. tests/test_devcmd.c is the app's side of the same list.
DIRECTIONS = ("up", "down", "left", "right")
STICK_SIDES = ("left", "right")

# Bounds. These lines come from the network: no value is trusted.
MAX_LINE = 8192              # a shot-data line is at most 512 characters
MAX_BUFFER = 4 * 1024 * 1024  # beyond that, someone has stopped sending newlines
MAX_PNG = 64 * 1024 * 1024
MAX_DIM = 16384
MAX_TAP = 8192
# Ceiling on a hold. Beyond it this is forgetfulness, not intent - and a button
# pinned for the whole session looks like broken hardware.
MAX_MS = 100000

# journal.c reads 255 bytes per 100 ms tick and splits on newlines. A longer
# command would be cut in two and its second half taken for an unknown command:
# we bound it here, and NEVER send two lines at once.
MAX_COMMAND = 200

TIMEOUT_DEFAULT = 10.0       # reply to an ordinary command
TIMEOUT_SHOT = 30.0         # a 1280x720 capture is several hundred KiB
WAIT_DEFAULT = 60.0     # how long to wait for the application to connect

EXIT_OK = 0
EXIT_FAILURE = 1
EXIT_USAGE = 2
EXIT_UNREACHABLE = 3
EXIT_PORT_BUSY = 4


def trace(text, actif=True):
    """Progress messages go to stderr: stdout carries only the RESULT (the PNG's
    path, the state text), so it stays usable as is in a pipe."""
    if actif:
        print(text, file=sys.stderr, flush=True)


# ── Parsing the replies (the PURE part, covered by --autotest) ──────────────

def strip_prefix(line):
    """Strips log.c's timestamp and the surrounding whitespace."""
    return RE_TIMESTAMP.sub("", line.strip())


def png_dimensions(data):
    """Width/height read from the IHDR, or None when the header is not there."""
    if len(data) < 24 or data[12:16] != b"IHDR":
        return None
    return struct.unpack(">II", data[16:24])


class Collector:
    """Tracks ONE command in flight and reassembles what comes back.

    The same code serves both sides of the relay: the server decides there when
    the command is finished, the client redoes the parsing on the relayed lines.
    """

    def __init__(self, command):
        self.command = command.strip()
        words = self.command.split()
        self.mot = words[0] if words else ""
        self.status = None        # None = en vol ; puis "ok" | "err" | "timeout"
        self.reason = ""
        self.state = None
        self.png = None
        self.width = 0
        self.height = 0
        self.nbytes = 0
        self.lines = []          # lines [devlink] vues, pour l'affichage
        self._morceaux = []
        self._b64 = 0
        self._en_capture = False

    # -- fin de vie ---------------------------------------------------------

    def done(self):
        return self.status is not None

    def succeeded(self):
        return self.status == "ok"

    def fail(self, reason):
        self.status = "err"
        self.reason = reason
        self.png = None
        self._morceaux = []
        self._en_capture = False
        return True

    def expired(self):
        if not self.done():
            self.status = "timeout"
            self.reason = "no reply within the delay"
        return self.status

    def summary(self):
        if self.status == "ok" and self.png is not None:
            return "capture %dx%d, %d bytes" % (self.width, self.height, len(self.png))
        if self.status == "ok" and self.state is not None:
            return self.state
        if self.status == "ok":
            return "ok"
        return "%s : %s" % (self.status or "en vol", self.reason or "no reason")

    # -- absorption ---------------------------------------------------------

    def absorb(self, line):
        """Absorbs ONE log line. Returns True when the command is finished.

        Anything without the [devlink] marker is ordinary log and passes through
        unchanged: replies arrive MIXED into the normal log stream, there is no
        separate channel.
        """
        if self.done():
            return True
        if len(line) > MAX_LINE:
            line = line[:MAX_LINE]
        line = strip_prefix(line)
        if not line.startswith(MARKER):
            return False
        reste = line[len(MARKER):].strip()
        if not reste:
            return False
        self.lines.append(reste)
        decoupe = reste.split(" ", 1)
        verbe = decoupe[0]
        arg = decoupe[1].strip() if len(decoupe) > 1 else ""

        if verbe == "shot-begin":
            return self._capture_begin(arg)
        if verbe == "shot-data":
            return self._capture_data(arg)
        if verbe == "shot-end":
            return self._capture_end()
        if verbe == "state":
            self.state = arg
            if self.mot == "state":
                self.status = "ok"
            return self.done()
        # `version` and `env-list` answer with TEXT, like `state`, not with a
        # plain acknowledgement: it is their content we want on stdout. Without
        # these two branches the reply fell into "unknown verb", so it was
        # ignored, so it timed out - while the application had answered.
        if verbe == "version":
            self.state = arg
            if self.mot == "version":
                self.status = "ok"
            return self.done()
        if verbe == "env-list":
            self.state = arg
            if self.mot == "env":
                self.status = "ok"
            return self.done()
        if verbe == "ok":
            return self._acknowledge(arg)
        if verbe == "err":
            return self._refuse(arg)
        # Unknown verb: the application may be newer than this tool. We ignore
        # it rather than fail - the opposite would block an iteration over one
        # extra line.
        return False

    def _acknowledge(self, arg):
        # `ok shot` can precede the transfer: it does NOT end a capture, or we
        # would write an empty file believing we had succeeded.
        if self.mot == "shot" and self.png is None:
            return False
        premier = arg.split(" ")[0] if arg else ""
        if premier and self.mot and premier != self.mot:
            return False      # an earlier command's acknowledgement, not ours
        self.status = "ok"
        return True

    def _refuse(self, arg):
        words = arg.split()
        if words and self.mot and words[0] != self.mot:
            return False
        reason = " ".join(words[1:]) if len(words) > 1 else ""
        return self.fail(reason or "refused with no reason")

    def _capture_begin(self, arg):
        words = arg.split()
        if len(words) < 3:
            return self.fail("header-incomplete")
        try:
            width, height, nbytes = (int(words[0]), int(words[1]), int(words[2]))
        except ValueError:
            return self.fail("header-non-numerique")
        if not (0 < width <= MAX_DIM and 0 < height <= MAX_DIM):
            return self.fail("dimensions-hors-bornes")
        if not (0 < nbytes <= MAX_PNG):
            return self.fail("taille-hors-bornes")
        self.width, self.height, self.nbytes = width, height, nbytes
        self._morceaux = []
        self._b64 = 0
        self._en_capture = True
        return False

    def _capture_data(self, arg):
        if not self._en_capture:
            return False          # an orphan chunk: noise, not to be trusted
        if not arg:
            return False
        # Bound BEFORE keeping anything: a talkative sender must not be able to
        # grow this process without limit.
        self._b64 += len(arg)
        if (self._b64 // 4) * 3 > self.nbytes + 3:
            return self.fail("overflow (more bytes than announced)")
        self._morceaux.append(arg)
        return False

    def _capture_end(self):
        if not self._en_capture:
            return False
        self._en_capture = False
        brut = "".join(self._morceaux)
        try:
            data = base64.b64decode(brut, validate=True)
        except (binascii.Error, ValueError):
            return self.fail("base64-invalide")
        # THE check that matters. A truncated capture written to disk would be
        # worse than an error: we would look at it and blame the rendering, when
        # it is bytes lost on the way.
        if len(data) != self.nbytes:
            return self.fail("truncated %d/%d bytes" % (len(data), self.nbytes))
        if not data.startswith(b"\x89PNG\r\n\x1a\n"):
            return self.fail("not-a-png")
        self.png = data
        self.status = "ok"
        return True


# ── Link TCP avec l'application ─────────────────────────────────────────────

class PortBusy(Exception):
    def __init__(self, port):
        Exception.__init__(self, "port %d already taken" % port)
        self.port = port


class LinkClosed(Exception):
    pass


class Link:
    """A TCP server that WAITS for the application to connect, then talks."""

    def __init__(self, port):
        self.port = port
        self.srv = None
        self.app = None
        self.pair = None
        self._tampon = b""

    def open(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            s.bind(("0.0.0.0", self.port))
        except OSError as e:
            s.close()
            if e.errno in (errno.EADDRINUSE, errno.EACCES):
                raise PortBusy(self.port)
            raise
        s.listen(1)
        self.srv = s
        self.port = s.getsockname()[1]   # useful when port 0 is requested

    def accept(self, timeout):
        """Waits for an outbound connection from the application. Returns the address or None."""
        fin = time.monotonic() + timeout
        while time.monotonic() < fin:
            r, _, _ = select.select([self.srv], [], [], 0.25)
            if r:
                conn, adr = self.srv.accept()
                conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                self.close_app()
                self.app = conn
                self.pair = adr
                self._tampon = b""
                return adr
        return None

    def send_line(self, command):
        """Sends ONE line. Never two: journal.c reads 255 bytes per tick and splits
        on newlines - a bigger batch would be cut mid-line and half of it would
        leave as an unknown command."""
        if self.app is None:
            raise LinkClosed()
        line = command.strip()
        if len(line) > MAX_COMMAND:
            raise ValueError("command too long (%d > %d)" % (len(line), MAX_COMMAND))
        try:
            self.app.sendall((line + "\n").encode("utf-8"))
        except OSError:
            raise LinkClosed()

    def read_lines(self, timeout):
        """Returns the complete lines available ([] if nothing before <delay>)."""
        if self.app is None:
            raise LinkClosed()
        r, _, _ = select.select([self.app], [], [], timeout)
        if not r:
            return []
        try:
            bloc = self.app.recv(65536)
        except OSError:
            raise LinkClosed()
        if not bloc:
            raise LinkClosed()
        self._tampon += bloc
        if len(self._tampon) > MAX_BUFFER:
            # Nobody is sending newlines any more: drop rather than grow
            # without end. A diagnostic tool must not run out of memory because
            # of what it is observing.
            self._tampon = self._tampon[-MAX_LINE:]
        lines = []
        while b"\n" in self._tampon:
            brut, self._tampon = self._tampon.split(b"\n", 1)
            lines.append(brut.decode("utf-8", "replace").rstrip("\r"))
        return lines

    def close_app(self):
        if self.app is not None:
            try:
                self.app.close()
            except OSError:
                pass
            self.app = None
            self.pair = None
            self._tampon = b""

    def close(self):
        self.close_app()
        if self.srv is not None:
            try:
                self.srv.close()
            except OSError:
                pass
            self.srv = None


def run_direct(link, command, timeout, verbose=False):
    """Sends a command on an already established link and waits for its reply."""
    col = Collector(command)
    try:
        link.send_line(command)
    except LinkClosed:
        col.fail("disconnected before the send")
        return col
    except ValueError as e:
        col.fail(str(e))
        return col
    fin = time.monotonic() + timeout
    while not col.done():
        restant = fin - time.monotonic()
        if restant <= 0:
            col.expired()
            break
        try:
            lines = link.read_lines(min(restant, 0.5))
        except LinkClosed:
            if not col.done():
                col.fail("application disconnected")
            break
        for line in lines:
            if verbose:
                print(line, flush=True)
            col.absorb(line)
            if col.done():
                break
    return col


# ── The local relay: one instance holds the port ────────────────────────────
#
# `listen` holds the port AND relays for the other invocations. Without it,
# running `devlink.py shot` while a listener is up would give "address already in
# use" - a message that names neither the cause nor the remedy.

class Request:
    def __init__(self, client, command, timeout):
        self.client = client
        self.command = command
        self.timeout = timeout
        self.echeance = time.monotonic() + timeout
        self.col = Collector(command)
        self.prevenu = False


class Host:
    """The server: it holds the port, prints the log and serves the relay."""

    def __init__(self, port, chemin_relais=None, path_out=None, silencieux=False):
        self.link = Link(port)
        self.chemin_relais = chemin_relais
        self.relay = None
        self.path_out = path_out
        self.silencieux = silencieux
        self.sortie = None
        self.attentes = []
        self.en_cours = None

    # -- ouverture ----------------------------------------------------------

    def open(self):
        self.link.open()
        if self.path_out:
            self.sortie = open(self.path_out, "a", encoding="utf-8")
        if self.chemin_relais:
            self._open_relay()

    def _open_relay(self):
        if not hasattr(socket, "AF_UNIX"):
            trace("The relay is unavailable on this platform (no AF_UNIX).",
                  not self.silencieux)
            return
        if os.path.exists(self.chemin_relais):
            # A socket file is present: either a listener is alive (and then it
            # is not for us to take the port), or it is a leftover.
            t = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            t.settimeout(0.5)
            try:
                t.connect(self.chemin_relais)
                t.close()
                raise PortBusy(self.link.port)
            except (OSError, socket.timeout):
                try:
                    os.unlink(self.chemin_relais)
                except OSError:
                    pass
            finally:
                try:
                    t.close()
                except OSError:
                    pass
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.bind(self.chemin_relais)
        s.listen(8)
        self.relay = s

    # -- boucle -------------------------------------------------------------

    def sert(self, arret=None):
        """The main loop. <stop> is an optional threading.Event."""
        while arret is None or not arret.is_set():
            watched = [self.link.srv]
            if self.relay is not None:
                watched.append(self.relay)
            if self.link.app is not None:
                watched.append(self.link.app)
            try:
                pret, _, _ = select.select(watched, [], [], 0.2)
            except (OSError, ValueError):
                break
            for s in pret:
                if s is self.link.srv:
                    self._new_app()
                elif s is self.relay:
                    self._new_request()
                else:
                    self._app_lines()
            self._deadlines()
            self._dispatch()

    def close(self):
        for d in self.attentes:
            self._conclude(d, "END abandonne")
        self.attentes = []
        if self.en_cours:
            self._conclude(self.en_cours, "END abandonne")
            self.en_cours = None
        self.link.close()
        if self.relay is not None:
            try:
                self.relay.close()
            except OSError:
                pass
            try:
                os.unlink(self.chemin_relais)
            except OSError:
                pass
            self.relay = None
        if self.sortie:
            self.sortie.close()
            self.sortie = None

    # -- evenements ---------------------------------------------------------

    def _new_app(self):
        conn, adr = self.link.srv.accept()
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        if self.link.app is not None:
            trace("--- a new connection replaces the previous one ---",
                  not self.silencieux)
        self.link.close_app()
        self.link.app = conn
        self.link.pair = adr
        trace("--- application connected from %s ---" % (adr[0],), not self.silencieux)

    def _app_lines(self):
        try:
            lines = self.link.read_lines(0)
        except LinkClosed:
            self.link.close_app()
            trace("--- application disconnected, waiting for a new launch ---",
                  not self.silencieux)
            if self.en_cours:
                self.en_cours.col.fail("application disconnected")
                self._conclude(self.en_cours, "END deconnecte")
                self.en_cours = None
            return
        for line in lines:
            self._show(line)
            d = self.en_cours
            if d is None:
                continue
            self._vers_client(d, line)
            if d.col.absorb(line):
                self._conclude(d, self._verdict(d.col))
                self.en_cours = None

    def _show(self, line):
        if not self.silencieux:
            print(line, flush=True)
        if self.sortie:
            self.sortie.write(line + "\n")
            self.sortie.flush()

    def _new_request(self):
        try:
            conn, _ = self.relay.accept()
        except OSError:
            return
        conn.settimeout(2.0)
        try:
            brut = b""
            while b"\n" not in brut and len(brut) < MAX_COMMAND + 64:
                bloc = conn.recv(256)
                if not bloc:
                    break
                brut += bloc
        except (OSError, socket.timeout):
            conn.close()
            return
        line = brut.split(b"\n", 1)[0].decode("utf-8", "replace").strip()
        # Format: CMD <delay> <command>. Everything else is refused - this
        # entry point is local, but it drives the application remotely.
        words = line.split(" ", 2)
        if len(words) < 3 or words[0] != "CMD":
            self._say(conn, "END malformee")
            conn.close()
            return
        try:
            timeout = float(words[1])
        except ValueError:
            self._say(conn, "END malformee")
            conn.close()
            return
        timeout = max(0.5, min(timeout, 300.0))
        command = words[2].strip()
        if not command or len(command) > MAX_COMMAND:
            self._say(conn, "END malformee")
            conn.close()
            return
        conn.settimeout(None)
        d = Request(conn, command, timeout)
        if self.link.app is None:
            self._say(conn, "INFO no application connected - waiting for it to be launched "
                            "(%.0f s)" % timeout)
            d.prevenu = True
        self.attentes.append(d)

    def _dispatch(self):
        # One command in flight at a time: replies arrive mixed into the log,
        # and two at once would make the matching doubtful.
        if self.en_cours is not None or not self.attentes:
            return
        if self.link.app is None:
            return
        d = self.attentes.pop(0)
        try:
            self.link.send_line(d.command)
        except (LinkClosed, ValueError) as e:
            d.col.fail(str(e) or "envoi impossible")
            self._conclude(d, self._verdict(d.col))
            return
        d.deadline = time.monotonic() + d.timeout   # the reply timeout restarts here
        self.en_cours = d

    def _deadlines(self):
        maintenant = time.monotonic()
        restants = []
        for d in self.attentes:
            if maintenant > d.echeance:
                self._conclude(d, "END timeout")
            else:
                restants.append(d)
        self.attentes = restants
        d = self.en_cours
        if d is not None and maintenant > d.echeance:
            d.col.expired()
            self._conclude(d, "END timeout")
            self.en_cours = None

    # -- talking to the relay's client --------------------------------------

    def _verdict(self, col):
        if col.status == "ok":
            return "END ok"
        if col.status == "err":
            return "END err %s" % (col.reason or "no-reason")
        return "END timeout"

    def _say(self, conn, text):
        try:
            conn.sendall((text + "\n").encode("utf-8"))
        except OSError:
            pass

    def _vers_client(self, d, line):
        if len(line) > MAX_LINE:
            line = line[:MAX_LINE]
        try:
            d.client.sendall((line + "\n").encode("utf-8"))
        except OSError:
            pass   # client parti : on continue a lire l'application

    def _conclude(self, d, verdict):
        self._say(d.client, verdict)
        try:
            d.client.close()
        except OSError:
            pass


def relay_running(path):
    if not path or not hasattr(socket, "AF_UNIX") or not os.path.exists(path):
        return False
    t = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    t.settimeout(0.5)
    try:
        t.connect(path)
        return True
    except OSError:
        return False
    finally:
        try:
            t.close()
        except OSError:
            pass


def run_via_relay(path, command, timeout, verbose=False):
    """Hands the command to the listener already in place and reads back its reply."""
    col = Collector(command)
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout + 10.0)
    try:
        s.connect(path)
        s.sendall(("CMD %.1f %s\n" % (timeout, command)).encode("utf-8"))
        tampon = b""
        while True:
            try:
                bloc = s.recv(65536)
            except socket.timeout:
                col.expired()
                break
            if not bloc:
                if not col.done():
                    col.fail("the relay closed without a verdict")
                break
            tampon += bloc
            if len(tampon) > MAX_BUFFER:
                tampon = tampon[-MAX_LINE:]
            done = False
            while b"\n" in tampon:
                brut, tampon = tampon.split(b"\n", 1)
                line = brut.decode("utf-8", "replace").rstrip("\r")
                if line.startswith("INFO "):
                    trace(line[5:], True)
                    continue
                if line.startswith("END "):
                    done = True
                    _verdict_relais(col, line[4:].strip())
                    break
                if verbose:
                    print(line, flush=True)
                col.absorb(line)
            if done:
                break
    except OSError as e:
        col.fail("relay unreachable (%s)" % e)
    finally:
        try:
            s.close()
        except OSError:
            pass
    return col


def _verdict_relais(col, verdict):
    """The relay's verdict must NEVER turn a local failure into a success: if our
    own parsing has already concluded, it wins."""
    if col.done():
        return
    words = verdict.split(" ", 1)
    tete = words[0] if words else ""
    if tete == "ok":
        col.status = "ok"
    elif tete == "err":
        col.fail(words[1] if len(words) > 1 else "no-reason")
    elif tete == "timeout":
        col.expired()
    else:
        col.fail(verdict or "verdict inconnu")


# ── Running a command, whichever path it takes ─────────────────────────────

def play(args, command, timeout, verbose=False):
    """Picks the path (an existing relay, or taking the port) and plays the command."""
    if relay_running(args.socket):
        trace("(via the running listener: %s)" % args.socket, verbose)
        return run_via_relay(args.socket, command, timeout, verbose), None
    link = Link(args.port)
    try:
        link.open()
    except PortBusy:
        trace("Port %d is already taken, and no devlink listener answers on %s."
              % (args.port, args.socket), True)
        trace("Usual causes: a `tools/switch-logsink.sh listen` or an `nc -l` "
              "is still running.", True)
        trace("Remedy: stop that process, then run `tools/client/devlink.py listen` "
              "- it holds the port AND relays the other calls' commands.", True)
        return None, EXIT_PORT_BUSY
    try:
        # A short, silent first attempt: when the application is already
        # running, the tool must not print a wait that is not happening.
        if link.accept(0.3) is None:
            trace("Waiting for the application to connect (%.0f s) - launch "
                  "the application on the console." % args.wait, True)
            if link.accept(args.wait) is None:
                trace("No application connected. Reminder: since DEVL-7 the application "
                      "RETRIES on its own, but only if "
                      "logsink.txt is present on the SD card "
                      "(tools/switch-logsink.sh setup).", True)
                return None, EXIT_UNREACHABLE
        return run_direct(link, command, timeout, verbose), None
    finally:
        link.close()


def verdict(col, what=""):
    """Returns the exit code and says what happened."""
    if col is None:
        return EXIT_FAILURE
    if col.succeeded():
        return EXIT_OK
    trace("FAILED %s - %s" % (what or col.command, col.summary()), True)
    return EXIT_FAILURE


# ── Commandes ───────────────────────────────────────────────────────────────

def why_toggle_refused(assignment):
    """The SAME rule as `devcmd_env_key_ok` / `devcmd_env_val_ok`, replayed here so
    the refusal NAMES the fault. Same reason as for paths, and same duty: any
    change is made on both sides."""
    if "=" not in assignment:
        return "a `=` is required: env SHADOW_X=1 to set, SHADOW_X= to remove"
    key, _, value = assignment.partition("=")
    if not key.startswith("SHADOW_"):
        return ("the key must start with SHADOW_ - the application silently ignores "
                "autres en silence")
    if len(key) <= len("SHADOW_"):
        return "the prefix alone is not a key"
    if len(key) >= 48:
        return "key too long (48 max)"
    for c in key[len("SHADOW_"):]:
        if not (c.isascii() and (c.isupper() or c.isdigit() or c == "_")):
            return "character not allowed in the key: %r (capitals, digits, _)" % c
    if len(value) >= 64:
        return "value too long (64 max)"
    for c in value:
        if not c.isascii() or ord(c) <= 0x20 or ord(c) >= 0x7f or c == "=":
            return "character not allowed in the value: %r" % c
    return None


def why_path_refused(path):
    """The SAME rule as `devcmd_path_ok` (clients/borealis/devlink/devcmd.h), replayed here.

    Why duplicate it: refusing as early as possible gives a message that NAMES
    the fault, whereas the application can only answer an undifferentiated
    `err relaunch path` - it has no channel to explain. And this is the only
    command that names a file to EXECUTE: two guards beat one.

    Returns None when the path passes, otherwise the reason. Any change must be
    made on both sides; `--autotest` compares the two counter-case lists.
    """
    if not path.startswith("/switch/"):
        return "must start with /switch/"
    if not path.endswith(".nro"):
        return "must end with .nro"
    if len(path) <= len("/switch/") + len(".nro"):
        return "a name is missing between /switch/ and .nro"
    if len(path) >= 128:
        return "too long (128 max)"
    # `..` and `//` BEFORE the leading-dot rule: without that order,
    # "/switch/../atmosphere/x.nro" was refused for "a name does not start with a
    # dot", which is true but names the wrong fault - and a message that names
    # the wrong fault costs more than no message at all.
    if ".." in path:
        return "relative segment `..`"
    if "//" in path:
        return "empty segment `//`"
    if path[len("/switch/")] == ".":
        return "a name does not start with a dot"
    for c in path:
        if not (c.isascii() and (c.isalnum() or c in "_-./")):
            return "character not allowed: %r" % c
    return None


def valide_commande(words):
    """Validates BEFORE anything is sent. Returns (command, None) or (None, message)."""
    if not words:
        return None, "empty command"
    verbe = words[0]
    if verbe == "btn":
        if len(words) != 2 or words[1] not in BUTTONS:
            return None, "btn attend un nom parmi : %s" % " ".join(BUTTONS)
        return "btn %s" % words[1], None
    if verbe == "nav":
        if len(words) != 2 or words[1] not in DIRECTIONS:
            return None, "nav expects one of: %s" % " ".join(DIRECTIONS)
        return "nav %s" % words[1], None
    if verbe == "hold":
        if len(words) != 3:
            return None, "hold expects a button and a duration: hold <button> <ms>"
        if words[1] not in BUTTONS:
            return None, "hold attend un button parmi : %s" % " ".join(BUTTONS)
        try:
            ms = int(words[2])
        except ValueError:
            return None, "hold expects a duration in MILLISECONDS (an integer)"
        if not (0 < ms <= MAX_MS):
            return None, "hold out of bounds (1..%d ms)" % MAX_MS
        return "hold %s %d" % (words[1], ms), None
    if verbe == "release":
        return "release", None
    if verbe == "stick":
        if len(words) not in (4, 5):
            return None, "stick expects: stick <left|right> <x> <y> [ms]"
        if words[1] not in STICK_SIDES:
            return None, "stick expects left or right"
        try:
            x, y = int(words[2]), int(words[3])
            ms = int(words[4]) if len(words) == 5 else 0
        except ValueError:
            return None, "stick expects INTEGERS"
        if not (-100 <= x <= 100 and -100 <= y <= 100):
            return None, "a stick out of bounds (-100..100, en pourcentage de course)"
        if ms and not (0 < ms <= MAX_MS):
            return None, "stick : duree hors bornes (1..%d ms)" % MAX_MS
        return ("stick %s %d %d %d" % (words[1], x, y, ms)) if ms else \
               ("stick %s %d %d" % (words[1], x, y)), None
    if verbe == "swipe":
        if len(words) not in (5, 6):
            return None, "swipe attend : swipe <x1> <y1> <x2> <y2> [ms]"
        try:
            v = [int(m) for m in words[1:5]]
            ms = int(words[5]) if len(words) == 6 else 0
        except ValueError:
            return None, "swipe expects INTEGERS"
        if any(not (0 <= n <= MAX_TAP) for n in v):
            return None, "swipe off screen (0..%d)" % MAX_TAP
        if ms and not (0 < ms <= MAX_MS):
            return None, "swipe : duree hors bornes (1..%d ms)" % MAX_MS
        return ("swipe %d %d %d %d %d" % (v[0], v[1], v[2], v[3], ms)) if ms else \
               ("swipe %d %d %d %d" % tuple(v)), None
    if verbe == "tap":
        if len(words) != 3:
            return None, "tap attend deux nombres : tap <x> <y>"
        try:
            x, y = int(words[1]), int(words[2])
        except ValueError:
            return None, "tap attend deux ENTIERS"
        if not (0 <= x <= MAX_TAP and 0 <= y <= MAX_TAP):
            return None, "tap hors bornes (0..%d)" % MAX_TAP
        return "tap %d %d" % (x, y), None
    if verbe == "relaunch":
        if len(words) == 1:
            return "relaunch", None
        if len(words) != 2:
            return None, "relaunch expects at most one path"
        bad = why_path_refused(words[1])
        if bad:
            return None, "relaunch : %s" % bad
        return "relaunch %s" % words[1], None
    if verbe == "env":
        if len(words) == 1:
            return "env", None
        if len(words) != 2:
            return None, "env expects at most one KEY=VALUE (no space)"
        bad = why_toggle_refused(words[1])
        if bad:
            return None, "env : %s" % bad
        return "env %s" % words[1], None
    if verbe in ("shot", "state", "quit", "ping", "version"):
        return verbe, None
    # A free-form command: let it through, but bounded. The application will
    # answer `err` if it does not know it - it decides, not this tool.
    line = " ".join(words)
    if len(line) > MAX_COMMAND:
        return None, "command too long (%d > %d)" % (len(line), MAX_COMMAND)
    if any(ord(c) < 32 for c in line):
        return None, "command with control characters"
    return line, None


def default_capture_name():
    return "devlink-%s.png" % datetime.now().strftime("%Y%m%d-%H%M%S")


def write_capture(col, path):
    """Writes the PNG. NEVER writes it if the reassembly failed."""
    if col.png is None:
        return EXIT_FAILURE
    dims = png_dimensions(col.png)
    if dims and (dims[0] != col.width or dims[1] != col.height):
        # We write it anyway: the pixels are complete and worth looking at. But
        # the gap between the announcement and the IHDR points at a defect on the
        # application side, and hiding it would cost time next iteration.
        trace("WARNING: announced %dx%d but the PNG says %dx%d"
              % (col.width, col.height, dims[0], dims[1]), True)
    with open(path, "wb") as f:
        f.write(col.png)
    trace("capture %dx%d, %d bytes" % (col.width, col.height, len(col.png)), True)
    print(os.path.abspath(path), flush=True)
    return EXIT_OK


def cmd_listen(args):
    poste = Host(args.port, args.socket, args.path_out, silencieux=False)
    try:
        poste.open()
    except PortBusy:
        trace("A devlink listener is already running (port %d, relay %s)."
              % (args.port, args.socket), True)
        trace("Only one instance can hold the port. Use the running one: "
              "`tools/client/devlink.py shot`, `btn a`... go through it automatically.",
              True)
        return EXIT_PORT_BUSY
    except OSError as e:
        trace("Ouverture impossible : %s" % e, True)
        return EXIT_PORT_BUSY
    trace("Listening on port %d - launch the application on the console." % poste.link.port, True)
    if poste.relay is not None:
        trace("Command relay: %s (other devlink calls go through it)."
              % args.socket, True)
    if args.path_out:
        trace("Log also written to %s" % args.path_out, True)
    try:
        poste.sert()
    except KeyboardInterrupt:
        trace("\necoute arretee.", True)
    finally:
        poste.close()
    return EXIT_OK


def cmd_shot(args):
    col, code = play(args, "shot", args.timeout or TIMEOUT_SHOT, args.verbose)
    if code is not None:
        return code
    if not col.succeeded():
        return verdict(col, "shot")
    return write_capture(col, args.path_out or default_capture_name())


def cmd_state(args):
    col, code = play(args, "state", args.timeout or TIMEOUT_DEFAULT, args.verbose)
    if code is not None:
        return code
    if not col.succeeded():
        return verdict(col, "state")
    print(col.state if col.state is not None else "", flush=True)
    return EXIT_OK


def cmd_simple(args, words):
    command, problem = valide_commande(words)
    if problem:
        trace("Refused: %s" % problem, True)
        return EXIT_USAGE
    col, code = play(args, command, args.timeout or TIMEOUT_DEFAULT, args.verbose)
    if code is not None:
        return code
    if col.succeeded():
        trace("ok %s" % command, True)
    return verdict(col, command)


def cmd_text(args, words):
    """Like cmd_simple, but prints the REPLY on stdout rather than the command's
    name: it is the content that matters (the build, the list of toggles), and
    stdout carries only the result so it can be used as is."""
    command, problem = valide_commande(words)
    if problem:
        trace("Refused: %s" % problem, True)
        return EXIT_USAGE
    col, code = play(args, command, args.timeout or TIMEOUT_DEFAULT, args.verbose)
    if code is not None:
        return code
    if col.succeeded() and col.state:
        print(col.state)
        return EXIT_OK
    return verdict(col, command)


def cmd_quit(args):
    """`quit` is the one case where no reply is a SUCCESS: the connection dies with
    the application, often before the acknowledgement."""
    col, code = play(args, "quit", args.timeout or 5.0, args.verbose)
    if code is not None:
        return code
    if col.succeeded():
        trace("ok quit", True)
        return EXIT_OK
    if col.status in ("timeout",) or "deconnect" in (col.reason or ""):
        trace("quit sent; no acknowledgement - that is expected, the application is closing.",
              True)
        return EXIT_OK
    return verdict(col, "quit")


def cmd_relaunch(args):
    """Unlike `quit`, no acknowledgement is a FAILURE here - see the comment below,
    which is where that difference is spelled out. On a refusal the application
    stays alive and answers `err relaunch <reason>`, and that is a real failure,
    not a disconnection."""
    # LOCAL validation first, like cmd_simple - without it a refused path left
    # anyway, the application answered nothing (it does not even parse it), and
    # the silence was read here as "it has exited": a FALSE SUCCESS on a command
    # that did nothing. Seen on 2026-09-12.
    command, problem = valide_commande(
        ["relaunch"] + ([args.path] if args.path else []))
    if problem:
        trace("Refused: %s" % problem, True)
        return EXIT_USAGE
    col, code = play(args, command, args.timeout or 5.0, args.verbose)
    if code is not None:
        return code
    if col.succeeded():
        trace("ok relaunch", True)
        return EXIT_OK
    if col.status == "err":
        # The application REFUSED: it is still running, nothing has changed.
        trace("relaunch refused by the application (%s) - it is still running"
              % (col.reason or "no reason"), True)
        return EXIT_FAILURE
    if col.status in ("timeout",) or "deconnect" in (col.reason or ""):
        # SILENCE IS A FAILURE HERE, unlike `quit`.
        #
        # That is the difference that matters between the two commands, and I got
        # it wrong when writing it: `quit` CANNOT acknowledge, the connection dies
        # with the application. `relaunch` writes its `ok relaunch <path>` BEFORE
        # asking to exit - and that log line leaves through a synchronous
        # `send()`, so it is already on the wire when the application starts
        # stopping. An acknowledgement is therefore ALWAYS expected.
        #
        # Silence then has a far more likely cause, and it is the one that showed
        # up on the very first real run: the running application is a build from
        # BEFORE RELOAD-1. It does not know the verb, does not even parse it, and
        # answers nothing. Reading that silence as success announced a restart
        # that had not happened.
        trace("FAILED relaunch - no acknowledgement.", True)
        trace("  The running application probably does not know `relaunch`:", True)
        trace("  it is a build from before RELOAD-1 (2026-09-12). Once, to", True)
        trace("  bootstrap: devlink.py quit; switch-sync.sh push; launch by hand.", True)
        trace("  After that `switch-sync.sh relance` is self-sufficient.", True)
        return EXIT_FAILURE
    return verdict(col, "relaunch")


def cmd_script(args):
    """Plays a list of commands, one per line. `#` = comment,
    `attends <secondes>` = pause locale, `shot <path_out>` = capture nommee."""
    try:
        with open(args.path_out, "r", encoding="utf-8") as f:
            lines = f.read().splitlines()
        # Every line is validated BEFORE the first send: a half-played plan
        # leaves the interface in an unknown state, which is worse than an
        # immediate refusal.
        plan = []
        for numero, brute in enumerate(lines, 1):
            text = brute.split("#", 1)[0].strip()
            if not text:
                continue
            words = text.split()
            if words[0] in ("attends", "wait"):
                if len(words) != 2:
                    trace("line %d : attends <secondes>" % numero, True)
                    return EXIT_USAGE
                try:
                    pause = float(words[1])
                except ValueError:
                    trace("line %d : duree non numerique" % numero, True)
                    return EXIT_USAGE
                if not (0 <= pause <= 300):
                    trace("line %d : duree hors bornes" % numero, True)
                    return EXIT_USAGE
                plan.append(("pause", pause, None))
                continue
            target = None
            if words[0] == "shot" and len(words) == 2:
                target = words[1]
                words = ["shot"]
            command, problem = valide_commande(words)
            if problem:
                trace("line %d : %s" % (numero, problem), True)
                return EXIT_USAGE
            plan.append(("cmd", command, target))
    except OSError as e:
        trace("Plan illisible : %s" % e, True)
        return EXIT_USAGE

    for genre, value, target in plan:
        if genre == "pause":
            time.sleep(value)
            continue
        timeout = TIMEOUT_SHOT if value == "shot" else (args.timeout or TIMEOUT_DEFAULT)
        col, code = play(args, value, timeout, args.verbose)
        if code is not None:
            return code
        if not col.succeeded():
            return verdict(col, value)
        if value == "shot":
            code = write_capture(col, target or default_capture_name())
            if code != EXIT_OK:
                return code
        else:
            trace("ok %s" % value, True)
        # journal.c only reads its commands on each 100 ms tick of its drain
        # thread: going faster would land two lines in the same tick, which works,
        # but gives the interface no chance to have even redrawn between two
        # gestures.
        time.sleep(max(args.pace, 0.0))
    trace("plan termine : %d commandes" % len([p for p in plan if p[0] == "cmd"]), True)
    return EXIT_OK


# ── Self-test: the tool checks itself, with no application ────────────────

_verifs = 0
_echecs = 0


def check(condition, what):
    global _verifs, _echecs
    _verifs += 1
    if not condition:
        _echecs += 1
        print("  FAILED - %s" % what, flush=True)


def make_png(width, height):
    """A real PNG, produced with zlib (the standard library)."""
    brut = b""
    for y in range(height):
        line = bytearray()
        for x in range(width):
            line += bytes(((x * 37 + y * 11) % 256, (x * 5) % 256, (y * 9) % 256))
        brut += b"\x00" + bytes(line)

    def chunk(typ, data):
        corps = typ + data
        return (struct.pack(">I", len(data)) + corps
                + struct.pack(">I", zlib.crc32(corps) & 0xffffffff))

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(brut))
            + chunk(b"IEND", b""))


def capture_lines(png, width, height, octets_annonces=None, horodate=True):
    """Builds the exact lines the application is supposed to emit."""
    b64 = base64.b64encode(png).decode("ascii")
    dedans = ["shot-begin %d %d %d" % (width, height,
                                       len(png) if octets_annonces is None else octets_annonces)]
    for i in range(0, len(b64), 512):
        dedans.append("shot-data %s" % b64[i:i + 512])
    dedans.append("shot-end")
    sortie = []
    for i, corps in enumerate(dedans):
        tete = "[%d.%03d] " % (i // 10, (i * 7) % 1000) if horodate else ""
        sortie.append("%s%s %s" % (tete, MARKER, corps))
    return sortie


def feed(col, lines):
    for l in lines:
        col.absorb(l)
    return col


def autotest_parsing():
    print("-- reassembling captures --", flush=True)
    png = make_png(16, 9)

    # The nominal case, with ORDINARY log mixed in: replies arrive in the same
    # stream as everything else, there is no separate channel.
    col = Collector("shot")
    lines = capture_lines(png, 16, 9)
    melange = [lines[0], "[1.020] [video] frame 12 keyframe",
               lines[1], "[1.021] [audio] 480 echantillons"] + lines[2:]
    feed(col, melange)
    check(col.succeeded(), "a nominal capture is accepted")
    check(col.png == png, "nbytes identiques a l'emission")
    check((col.width, col.height) == (16, 9), "dimensions relues")

    # COUNTER-CASE (2026-09-11) - THE LOG FORMAT SINCE S81: timestamp THEN the
    # severity/category column ("[1.234] I/systeme  [devlink] ..."). The stripping
    # knew only the timestamp: no reply was recognised, devlink failed on every
    # command while the application was answering.
    col = Collector("shot")
    s81 = ["[1.%03d] I/systeme  %s" % (i, strip_prefix(l))
           for i, l in enumerate(capture_lines(png, 16, 9))]
    feed(col, s81[:1] + ["[1.500] I/video    [G38] an ordinary line"] + s81[1:])
    check(col.succeeded() and col.png == png,
            "COUNTER-CASE: format S81 (severite/categorie) accept, nbytes identiques")

    # COUNTER-CASE - THE TRUNCATED CAPTURE.
    # This is THE defect never to let through: a cut PNG written to disk opens
    # anyway, half grey, and the rendering gets blamed. Here the middle data line
    # disappears, as if one send had been lost.
    col = Collector("shot")
    lines = capture_lines(png, 16, 9)
    ampute = lines[:2] + lines[3:]
    feed(col, ampute)
    check(col.status == "err", "COUNTER-CASE: a truncated capture REFUSED")
    check("truncated" in col.reason, "the reason names the truncation (%s)" % col.reason)
    check(col.png is None, "COUNTER-CASE: nothing to write to disk after truncation")

    # COUNTER-CASE - the announcement lies about the size (one byte more).
    col = Collector("shot")
    feed(col, capture_lines(png, 16, 9, octets_annonces=len(png) + 1))
    check(col.status == "err" and col.png is None,
            "COUNTER-CASE: one missing byte is enough to refuse")

    # COUNTER-CASE - more bytes than announced: the guard fires BEFORE we accumulate.
    col = Collector("shot")
    feed(col, capture_lines(png, 16, 9, octets_annonces=32))
    check(col.status == "err" and "overflow" in col.reason,
            "COUNTER-CASE: an overflow stopped during the transfer")

    # CONTRE-CAS — base64 abime en route.
    col = Collector("shot")
    feed(col, ["%s shot-begin 16 9 %d" % (MARKER, len(png)),
                  "%s shot-data !!!!not-base64!!!!" % MARKER,
                  "%s shot-end" % MARKER])
    check(col.status == "err" and col.png is None, "COUNTER-CASE: invalid base64 refused")

    # COUNTER-CASE - this is not a PNG (right size, wrong content). A file of
    # the right size but unreadable would suggest a write bug.
    bogus = b"THIS-IS-NOT-A-PNG-AT-ALL!!!!!!!"
    col = Collector("shot")
    feed(col, capture_lines(bogus, 16, 9))
    check(col.status == "err" and "png" in col.reason,
            "COUNTER-CASE: non-PNG content refused (%s)" % col.reason)

    # COUNTER-CASE - journal.c's TIMESTAMP.
    # journal.c prefixes every line with "[12.345]" (S27), over the network too.
    # The first version of this parser ignored it: every command timed out while
    # the application was answering perfectly.
    col = Collector("shot")
    feed(col, capture_lines(png, 16, 9, horodate=True))
    check(col.succeeded(), "COUNTER-CASE: lines horodatees reconnues")
    col = Collector("shot")
    feed(col, capture_lines(png, 16, 9, horodate=False))
    check(col.succeeded(), "lines with no timestamp are recognised too")

    # CONTRE-CAS — entetes hors bornes. Ces nombres viennent du reseau.
    for header, what in (("shot-begin 99999 9 100", "absurd width"),
                         ("shot-begin 16 9 0", "taille nulle"),
                         ("shot-begin 16 9 999999999999", "taille enorme"),
                         ("shot-begin seize neuf cent", "header non numerique"),
                         ("shot-begin 16 9", "header incomplete")):
        col = Collector("shot")
        col.absorb("%s %s" % (MARKER, header))
        check(col.status == "err", "COUNTER-CASE: %s refused" % what)

    print("-- acknowledgements and refusals --", flush=True)

    # `ok shot` does NOT end a capture: otherwise we would write an empty file.
    col = Collector("shot")
    col.absorb("%s ok shot" % MARKER)
    check(not col.done(), "COUNTER-CASE: `ok shot` is not a finished capture")
    feed(col, capture_lines(png, 16, 9))
    check(col.succeeded() and col.png == png, "the capture that follows is taken properly")

    col = Collector("btn a")
    col.absorb("[2.000] %s ok btn a" % MARKER)
    check(col.succeeded(), "a button's acknowledgement")

    col = Collector("btn a")
    col.absorb("%s ok nav bas" % MARKER)
    check(not col.done(), "COUNTER-CASE: ANOTHER command's acknowledgement does not count")
    col.absorb("%s ok btn a" % MARKER)
    check(col.succeeded(), "the right acknowledgement concludes")

    col = Collector("btn z")
    col.absorb("%s err btn inconnu" % MARKER)
    check(col.status == "err" and col.reason == "inconnu", "a refusal read with its reason")

    col = Collector("state")
    col.absorb("[9.900] %s state screen=liste-vm focus=2 titre=Mes-machines" % MARKER)
    check(col.succeeded() and "focus=2" in col.state, "the screen state is read")

    col = Collector("nav bas")
    for noise in ("[1.000] [video] nothing to see", "no marker at all",
                  "%s shot-data AAAA" % MARKER, "%s verb-from-the-future 42" % MARKER, ""):
        col.absorb(noise)
    check(not col.done(), "COUNTER-CASE: log noise concludes nothing")
    check(col.png is None, "an orphan shot-data does not fabricate a capture")

    # An outsized line must neither blow up memory nor be taken for a reply.
    col = Collector("state")
    col.absorb("%s state %s" % (MARKER, "x" * (MAX_LINE * 4)))
    check(col.succeeded() and len(col.state) < MAX_LINE, "an outsized line is bounded")

    print("-- validating commands, BEFORE anything is sent --", flush=True)
    for words, attendu in ((["btn", "a"], "btn a"), (["nav", "down"], "nav down"),
                          (["tap", "640", "360"], "tap 640 360"), (["shot"], "shot")):
        rendered, problem = valide_commande(words)
        check(rendered == attendu and problem is None, "command %s accepted" % attendu)
    for words, what in ((["btn", "start"], "a button that does not exist"),
                       (["btn"], "a button with no name"),
                       (["nav", "up-right"], "a direction that does not exist"),
                       (["nav", "bas"], "the French word the app stopped parsing on 2026-09-12"),
                       (["tap", "640"], "tap with no y"),
                       (["tap", "x", "y"], "a non-numeric tap"),
                       (["tap", "-1", "10"], "a negative tap"),
                       (["tap", "99999", "10"], "a tap off screen"),
                       (["quit\nbtn a"], "injecting a second line"),
                       (["z" * 300], "a command longer than journal.c's buffer")):
        rendered, problem = valide_commande(words)
        check(rendered is None and problem, "COUNTER-CASE: %s refused here" % what)

    # relaunch: the ONLY command that names a file to execute. The counter-case
    # list is the one in tests/test_devcmd.c, replayed here - if the two diverge,
    # the tool accepts what the application will refuse, and the diagnosis starts
    # from the wrong end.
    for words, attendu in ((["relaunch"], "relaunch"),
                          (["relaunch", "/switch/halyard.b.nro"],
                           "relaunch /switch/halyard.b.nro"),
                          (["relaunch", "/switch/Halyard.nro"],
                           "relaunch /switch/Halyard.nro")):
        rendered, problem = valide_commande(words)
        check(rendered == attendu and problem is None, "relaunch accepted: %s" % attendu)
    for path, what in (("/atmosphere/x.nro", "outside /switch/"),
                         ("/switch/x.bin", "not a .nro"),
                         ("switch/x.nro", "a relative path"),
                         ("/switch/../atmosphere/x.nro", "a `..` that escapes"),
                         ("/switch/a..b.nro", "a `..` anywhere"),
                         ("/switch//x.nro", "an empty segment"),
                         ("/switch/.nro", "an empty name"),
                         ("/switch/.cache.nro", "a name starting with a dot"),
                         ("/switch/x;reboot.nro", "a character not allowed"),
                         ("/switch/e\u0301.nro", "octet non ASCII"),
                         ("/switch/" + "a" * 200 + ".nro", "too long")):
        rendered, problem = valide_commande(["relaunch", path])
        check(rendered is None and problem, "COUNTER-CASE relaunch: %s refused here" % what)
    rendered, problem = valide_commande(["relaunch", "/switch/a.nro", "/switch/b.nro"])
    check(rendered is None and problem, "CONTRE-CAS relaunch : two paths refused")

    # env: the same counter-case list as tests/test_devcmd.c. If the two diverge,
    # the tool writes a toggle the application will refuse - or worse, one its
    # READER will ignore in silence, and the experiment will not have happened.
    for words, attendu in ((["env"], "env"),
                          (["env", "SHADOW_FPS=60"], "env SHADOW_FPS=60"),
                          (["env", "SHADOW_FPS="], "env SHADOW_FPS=")):
        rendered, problem = valide_commande(words)
        check(rendered == attendu and problem is None, "env accepted: %s" % attendu)
    for assign, what in (("PATH=/tmp", "a key with no SHADOW_ prefix"),
                      ("SHADOW_=1", "the prefix alone"),
                      ("shadow_fps=60", "lowercase (a different variable)"),
                      ("SHADOW_FPS", "with no `=`"),
                      ("SHADOW-FPS=1", "a dash in the key"),
                      ("SHADOW_A=b=c", "a second `=`"),
                      ("SHADOW_A=\u00e9", "a non-ASCII byte in the value"),
                      ("SHADOW_" + "A" * 48 + "=1", "a key that is too long"),
                      ("SHADOW_A=" + "9" * 64, "a value that is too long")):
        rendered, problem = valide_commande(["env", assign])
        check(rendered is None and problem, "COUNTER-CASE env: %s refused here" % what)

    # Injection: the same counter-case list as tests/test_devcmd.c.
    for words, attendu in ((["hold", "b", "3000"], "hold b 3000"),
                          (["release"], "release"),
                          (["stick", "left", "0", "-100"], "stick left 0 -100"),
                          (["stick", "right", "50", "50", "200"], "stick right 50 50 200"),
                          (["swipe", "1", "2", "3", "4"], "swipe 1 2 3 4"),
                          (["swipe", "1", "2", "3", "4", "250"], "swipe 1 2 3 4 250")):
        rendered, problem = valide_commande(words)
        check(rendered == attendu and problem is None, "injection accepted: %s" % attendu)
    for words, what in ((["hold", "b"], "hold with no duration"),
                       (["hold", "start", "100"], "hold on a button that does not exist"),
                       (["hold", "b", "0"], "a zero duration"),
                       (["hold", "b", "999999"], "an absurd duration"),
                       (["stick", "up", "0", "0"], "a side that does not exist"),
                       (["stick", "left", "101", "0"], "a stick out of bounds"),
                       (["stick", "left", "0"], "a stick with only one axis"),
                       (["swipe", "1", "2", "3"], "an incomplete swipe"),
                       (["swipe", "-1", "2", "3", "4"], "a negative coordinate"),
                       (["swipe", "99999", "2", "3", "4"], "off screen")):
        rendered, problem = valide_commande(words)
        check(rendered is None and problem, "COUNTER-CASE injection: %s refused here" % what)


class StubApp(threading.Thread):
    """A stub imitating the application. It CONNECTS to this machine (as journal.c
    does), reads commands line by line and answers with journal.c's timestamp."""

    def __init__(self, port, png=None, truncated_stub=False, mute=False):
        threading.Thread.__init__(self, daemon=True)
        self.port = port
        self.png = png
        self.truncated_stub = truncated_stub
        self.mute = mute
        self.arret = threading.Event()
        self.recues = []
        self.problem = None
        self.t0 = time.monotonic()

    def _say(self, corps):
        ms = int((time.monotonic() - self.t0) * 1000)
        self.s.sendall(("[%d.%03d] %s %s\n" % (ms // 1000, ms % 1000, MARKER, corps))
                       .encode("utf-8"))

    def repond(self, command):
        words = command.split()
        verbe = words[0]
        if verbe == "shot":
            if self.mute:
                return
            png = self.png or make_png(32, 18)
            nbytes = len(png) + 9 if self.truncated_stub else len(png)
            b64 = base64.b64encode(png).decode("ascii")
            self._say("shot-begin 32 18 %d" % nbytes)
            for i in range(0, len(b64), 512):
                self._say("shot-data %s" % b64[i:i + 512])
            self._say("shot-end")
        elif verbe == "state":
            self._say("state screen=liste-vm focus=1 elements=3")
        elif verbe == "btn" and len(words) == 2 and words[1] in BUTTONS:
            self._say("ok btn %s" % words[1])
        elif verbe == "nav" and len(words) == 2 and words[1] in DIRECTIONS:
            self._say("ok nav %s" % words[1])
        elif verbe == "tap" and len(words) == 3:
            self._say("ok tap %s %s" % (words[1], words[2]))
        elif verbe == "quit":
            self._say("ok quit")
        elif verbe == "relaunch":
            self._say("ok " + line.strip())
        elif verbe in ("version", "env", "hold", "release", "stick", "swipe"):
            self._say("ok " + line.strip())
            self.arret.set()
        else:
            self._say("err %s inconnue" % verbe)

    def run(self):
        try:
            self.s = socket.create_connection(("127.0.0.1", self.port), timeout=5)
            self.s.sendall(b"[0.001] [boot] fausse application demarree\n")
            tampon = b""
            while not self.arret.is_set():
                r, _, _ = select.select([self.s], [], [], 0.1)
                if not r:
                    continue
                bloc = self.s.recv(4096)
                if not bloc:
                    break
                tampon += bloc
                while b"\n" in tampon:
                    brut, tampon = tampon.split(b"\n", 1)
                    line = brut.decode("utf-8", "replace").strip()
                    if not line:
                        continue
                    self.recues.append(line)
                    self.repond(line)
        except Exception as e:            # noqa: BLE001 — bouchon de test
            self.problem = e
        finally:
            try:
                self.s.close()
            except Exception:             # noqa: BLE001
                pass


def autotest_end_to_end():
    print("-- end to end: this machine listens, the application connects --", flush=True)
    dossier = tempfile.mkdtemp(prefix="devlink-autotest-")

    # 1. a complete capture, written to disk and read back.
    link = Link(0)
    link.open()
    app = StubApp(link.port)
    app.start()
    check(link.accept(5.0) is not None, "the application connects to this machine")
    col = run_direct(link, "shot", 10.0)
    check(col.succeeded(), "capture received end to end (%s)" % col.summary())
    path = os.path.join(dossier, "screen.png")
    check(write_capture(col, path) == EXIT_OK, "PNG ecrit")
    check(os.path.exists(path) and open(path, "rb").read() == col.png,
            "the file read back is identical to the bytes received")
    col = run_direct(link, "btn a", 5.0)
    check(col.succeeded(), "a button acknowledged")
    col = run_direct(link, "btn unknown-to-the-stub", 5.0)
    check(col.status == "err", "an unknown command -> err, doing nothing")
    app.arret.set()
    link.close()

    # 2. COUNTER-CASE - a capture truncated end to end: NO path_out is written.
    link = Link(0)
    link.open()
    app = StubApp(link.port, truncated_stub=True)
    app.start()
    link.accept(5.0)
    col = run_direct(link, "shot", 10.0)
    check(col.status == "err", "COUNTER-CASE: a truncated capture refused on the real path")
    ampute = os.path.join(dossier, "ampute.png")
    check(write_capture(col, ampute) != EXIT_OK and not os.path.exists(ampute),
            "COUNTER-CASE: nothing is written to disk")
    app.arret.set()
    link.close()

    # 3. COUNTER-CASE - the application does not answer: a guard delay, no hang.
    link = Link(0)
    link.open()
    app = StubApp(link.port, mute=True)
    app.start()
    link.accept(5.0)
    debut = time.monotonic()
    col = run_direct(link, "shot", 1.0)
    ecoule = time.monotonic() - debut
    check(col.status == "timeout", "COUNTER-CASE: silence -> timeout, no hang")
    check(ecoule < 3.0, "the guard delay holds (%.1f s)" % ecoule)
    app.arret.set()
    link.close()

    # 4. CONTRE-CAS — l'application meurt en pleine command.
    link = Link(0)
    link.open()
    app = StubApp(link.port, mute=True)
    app.start()
    link.accept(5.0)
    app.arret.set()
    col = run_direct(link, "shot", 3.0)
    check(col.status in ("err", "timeout"), "COUNTER-CASE: deconnexion en vol -> failure net")
    link.close()

    # 5. COUNTER-CASE - the port is already taken.
    a = Link(0)
    a.open()
    b = Link(a.port)
    pris = False
    try:
        b.open()
    except PortBusy:
        pris = True
    check(pris, "COUNTER-CASE: a second taker of the port -> PortBusy (a clear message)")
    a.close()
    b.close()


def autotest_relay():
    if not hasattr(socket, "AF_UNIX"):
        print("-- relay: skipped (no AF_UNIX) --", flush=True)
        return
    print("-- relay: one instance holds the port --", flush=True)
    dossier = tempfile.mkdtemp(prefix="devlink-relay-")
    path = os.path.join(dossier, "relay.sock")
    poste = Host(0, path, silencieux=True)
    poste.open()
    arret = threading.Event()
    fil = threading.Thread(target=poste.sert, args=(arret,), daemon=True)
    fil.start()
    try:
        check(relay_running(path), "the relay answers")

        # COUNTER-CASE - A COMMAND SENT BEFORE THE APPLICATION IS LAUNCHED.
        # It must WAIT, not fail: on console one starts devlink then the
        # application, and the reverse order is the common case.
        resultat = {}

        def tot():
            resultat["col"] = run_via_relay(path, "btn a", 6.0)

        client = threading.Thread(target=tot, daemon=True)
        client.start()
        time.sleep(0.7)                      # nothing is connected during this time
        app = StubApp(poste.link.port)
        app.start()
        client.join(timeout=10)
        col = resultat.get("col")
        check(col is not None and col.succeeded(),
                "COUNTER-CASE: a command sent BEFORE the connection -> waited for, then played")

        col = run_via_relay(path, "shot", 10.0)
        check(col.succeeded() and col.png is not None,
                "capture received through the relay (%s)" % col.summary())
        col = run_via_relay(path, "state", 5.0)
        check(col.succeeded() and "focus=1" in (col.state or ""), "the state read through the relay")

        # COUNTER-CASE - a malformed request must play NOTHING.
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(3)
        s.connect(path)
        s.sendall(b"PILOTE 5 btn a\n")
        reply = s.recv(256).decode("utf-8", "replace")
        s.close()
        check("END malformee" in reply, "COUNTER-CASE: demande hors format rejetee")

        # COUNTER-CASE - a second listener on the same relay.
        double = Host(0, path, silencieux=True)
        refuse = False
        try:
            double.open()
        except PortBusy:
            refuse = True
        check(refuse, "COUNTER-CASE: a second listener refused, with a remedy to offer")
        double.close()

        # COUNTER-CASE - a command whose timeout expired on the relay side.
        app.arret.set()
        time.sleep(0.3)
        col = run_via_relay(path, "btn a", 1.0)
        check(col.status in ("timeout", "err"),
                "COUNTER-CASE: nobody left at the other end -> a clean verdict, no endless wait")
    finally:
        arret.set()
        fil.join(timeout=3)
        poste.close()


def autotest():
    debut = time.monotonic()
    print("devlink --autotest: offline checks, with no application and no console.", flush=True)
    autotest_parsing()
    autotest_end_to_end()
    autotest_relay()
    print("\n%d checks, %d failure(s), %.1f s"
          % (_verifs, _echecs, time.monotonic() - debut))
    return EXIT_OK if _echecs == 0 else EXIT_FAILURE


# ── Entree ──────────────────────────────────────────────────────────────────

def build_parser():
    p = argparse.ArgumentParser(
        prog="devlink.py",
        description="Drives the Shadow client's interface from this machine "
                    "(it is the application that connects here).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Arm the channel once: tools/switch-logsink.sh setup "
               "(drops logsink.txt on the SD card). See tools/client/DEVLINK.md.")
    p.add_argument("--port", type=int, default=PORT_DEFAULT,
                   help="listening port (default %d, or LOGSINK_PORT)" % PORT_DEFAULT)
    p.add_argument("--socket", default=RELAY_DEFAULT,
                   help="the relay's entry point (default %s)" % RELAY_DEFAULT)
    p.add_argument("--wait", type=float, default=WAIT_DEFAULT,
                   help="seconds to wait for the application to connect")
    p.add_argument("--timeout", type=float, default=0.0,
                   help="seconds to wait for a reply (0 = per-command default)")
    p.add_argument("--pace", type=float, default=0.35,
                   help="pause between two script commands (default 0.35 s)")
    p.add_argument("--verbose", action="store_true",
                   help="also print the log received during the command")
    s = p.add_subparsers(dest="what")

    e = s.add_parser("listen", help="listen to the log and act as relay")
    e.add_argument("path_out", nargs="?", help="log file to write in addition to the display")

    c = s.add_parser("shot", help="capture the screen")
    c.add_argument("path_out", nargs="?", help="PNG to write (default: devlink-<date>.png)")

    s.add_parser("state", help="describe the current screen")

    b = s.add_parser("btn", help="press and release a button")
    b.add_argument("nom", help=" | ".join(BUTTONS))

    n = s.add_parser("nav", help="navigate")
    n.add_argument("sens", help=" | ".join(DIRECTIONS))

    t = s.add_parser("tap", help="touch at the given screen position")
    t.add_argument("x", type=int)
    t.add_argument("y", type=int)

    r = s.add_parser("cmd", help="send a raw line (quit, ping...)")
    r.add_argument("text", nargs="+")

    h = s.add_parser("hold", help="hold a button (the hold-to-leave pages)")
    h.add_argument("nom", help=" | ".join(BUTTONS))
    h.add_argument("ms", type=int, help="duration in milliseconds")

    s.add_parser("release", help="let go of everything at once")

    st = s.add_parser("stick", help="push a stick (-100..100)")
    st.add_argument("cote", choices=STICK_SIDES)
    st.add_argument("x", type=int)
    st.add_argument("y", type=int)
    st.add_argument("ms", nargs="?", type=int, default=0)

    sw = s.add_parser("swipe", help="drag a finger from one point to another")
    sw.add_argument("x1", type=int); sw.add_argument("y1", type=int)
    sw.add_argument("x2", type=int); sw.add_argument("y2", type=int)
    sw.add_argument("ms", nargs="?", type=int, default=0)

    s.add_parser("version", help="say which build is running, and from which .nro")

    ev = s.add_parser("env", help="read or change a SHADOW_* toggle (next launch)")
    ev.add_argument("assignment", nargs="?",
                    help="SHADOW_X=1 to set, SHADOW_X= to remove; nothing = list")

    s.add_parser("quit", help="close the application (the .nro can then be pushed)")

    rl = s.add_parser("relaunch",
                      help="close the application AND load a .nro instead")
    rl.add_argument("path", nargs="?",
                    help="/switch/....nro; with no argument, reloads the running .nro")

    j = s.add_parser("script", help="play a list of commands")
    j.add_argument("path_out")
    return p


def main(argv):
    # Quiet mode: the tool's own check, before any argparse.
    if "--autotest" in argv:
        return autotest()
    p = build_parser()
    args = p.parse_args(argv)
    if not args.what:
        p.print_help()
        return EXIT_USAGE
    try:
        if args.what == "listen":
            return cmd_listen(args)
        if args.what == "shot":
            return cmd_shot(args)
        if args.what == "state":
            return cmd_state(args)
        if args.what == "btn":
            return cmd_simple(args, ["btn", args.nom])
        if args.what == "nav":
            return cmd_simple(args, ["nav", args.sens])
        if args.what == "tap":
            return cmd_simple(args, ["tap", str(args.x), str(args.y)])
        if args.what == "cmd":
            return cmd_simple(args, list(args.text))
        if args.what == "hold":
            return cmd_simple(args, ["hold", args.nom, str(args.ms)])
        if args.what == "release":
            return cmd_simple(args, ["release"])
        if args.what == "stick":
            m = ["stick", args.cote, str(args.x), str(args.y)]
            if args.ms:
                m.append(str(args.ms))
            return cmd_simple(args, m)
        if args.what == "swipe":
            m = ["swipe", str(args.x1), str(args.y1), str(args.x2), str(args.y2)]
            if args.ms:
                m.append(str(args.ms))
            return cmd_simple(args, m)
        if args.what == "version":
            return cmd_text(args, ["version"])
        if args.what == "env":
            # With no argument this is a READ (a text reply); with one, a write
            # (a plain acknowledgement).
            if args.assignment:
                return cmd_simple(args, ["env", args.assignment])
            return cmd_text(args, ["env"])
        if args.what == "quit":
            return cmd_quit(args)
        if args.what == "relaunch":
            return cmd_relaunch(args)
        if args.what == "script":
            return cmd_script(args)
    except KeyboardInterrupt:
        trace("interrupted.", True)
        return EXIT_FAILURE
    p.print_help()
    return EXIT_USAGE


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
