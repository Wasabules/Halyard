#!/usr/bin/env bash
# vita-push.sh - the iteration loop over Wi-Fi, no cable and no gesture.
#
# Backed by vitacompanion (devnoname120), a taiHEN plugin that keeps an FTP
# server on 1337 and a command channel on 1338 running in the BACKGROUND -
# which is the whole difference with VitaShell's FTP: nothing has to be open on
# the console, so a round trip costs one command here and zero there.
#
# This is the Vita's answer to what RELOAD-1 gave the Switch. The mechanism is
# not the same and the difference matters: on Switch the running .nro names its
# successor through `envSetNextLoad`, so the application hands over to itself;
# here an EXTERNAL service kills the title and launches it again. Ours therefore
# survives a crash - the console is not relying on our process to still be alive
# - where `switch-sync.sh relance` needs the app to exit cleanly.
#
# `tools/vita-sync.sh companion` installs the plugin (once, over USB).
set -euo pipefail

VITA_IP="${VITA_IP:-}"
IP_FILE="$(cd "$(dirname "$0")/.." && pwd)/.vita-ip"
if [ -z "$VITA_IP" ] && [ -f "$IP_FILE" ]; then VITA_IP="$(cat "$IP_FILE")"; fi

TITLE_ID="${VITA_TITLE_ID:-HLYD00001}"
FTP_PORT="${VITA_FTP_PORT:-1337}"
CMD_PORT="${VITA_CMD_PORT:-1338}"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${VITA_BUILD_DIR:-$(cd "$(dirname "$0")/.." && pwd)/build_psv}"

usage() {
    cat <<USAGE
usage: $0 <command> [args]

  ip <A.B.C.D>     remember the console's address (written to .vita-ip)
  ping             is the companion answering?
  push             send build_psv/halyard.self as eboot.bin
  resources [sub]  send resources/ over FTP (sub = a subtree, e.g. i18n)
  relance          kill the title and launch it again
  run              push + relance - the whole round trip, one command
  logs [dest]      pull the data dir's logs into dest (default: ./vita-logs)
  dumps [dest]     pull the crash dumps (.psp2dmp) into dest; warns about the
                   .tmp ones, which are truncated and unreadable
  cmd '<line>'     send a raw command (see the vitacompanion README)
  config <ip>      patch ur0:tai/config.txt through VitaShell's FTP, so the
                   companion loads at boot - one-time, VitaShell open, SELECT
  shot             not available - the companion injects input but reads no screen

Address: \$VITA_IP, else .vita-ip at the repo root.
USAGE
    exit 2
}

need_ip() {
    [ -n "$VITA_IP" ] || { echo "no address: run '$0 ip <A.B.C.D>' once" >&2; exit 3; }
}

# The command channel wants a trailing newline and closes on its own. `nc -q1`
# is not portable, so we let the console close the connection and bound the wait
# ourselves - a companion that never answers must not hang the script.
send_cmd() {
    need_ip
    printf '%s\n' "$1" | timeout 10 nc "$VITA_IP" "$CMD_PORT" 2>/dev/null || true
}

case "${1:-}" in
  ip)
    [ $# -ge 2 ] || usage
    printf '%s' "$2" > "$IP_FILE"
    echo "address kept: $2  ($IP_FILE)"
    ;;

  ping)
    need_ip
    out="$(send_cmd version)"
    if [ -n "$out" ]; then echo "companion answers at $VITA_IP:$CMD_PORT — $out"
    else echo "no answer at $VITA_IP:$CMD_PORT (plugin loaded? console awake?)" >&2; exit 1; fi
    ;;

  push)
    need_ip
    SRC="$BUILD/halyard.self"
    [ -f "$SRC" ] || { echo "missing $SRC" >&2; exit 4; }
    # === REFUSE A STALE .self ==========================================
    #
    # `vita-elf-create` can fail AFTER the ELF has linked - it prints its
    # complaint, segfaults, and leaves the previous `.self` in place. Its
    # message contains no "error:", so a build whose outcome is read through a
    # grep for that word looks clean. That is how three deployments in a row
    # sent the PREVIOUS build while every step here reported success, and the
    # console kept announcing a commit nobody could explain.
    #
    # The ELF is the thing that was just linked; the `.self` is derived from
    # it. If the derived file is older, the derivation failed.
    ELF="$BUILD/halyard"
    if [ -f "$ELF" ] && [ "$ELF" -nt "$SRC" ]; then
        echo "REFUSING: $SRC is older than the ELF it comes from." >&2
        echo "vita-elf-create failed - rebuild and read its output:" >&2
        echo "  cmake --build $BUILD --target halyard.self" >&2
        exit 6
    fi
    echo "eboot.bin <- $(du -h "$SRC" | cut -f1)  ->  $VITA_IP"
    # `--ftp-method nocwd` with the double-slash form: the companion accepts a
    # full Vita path directly, which avoids a CWD into a drive-prefixed path
    # that generic clients get wrong.
    if ! curl -sS --max-time 120 --ftp-method nocwd -T "$SRC" \
              "ftp://$VITA_IP:$FTP_PORT//ux0:/app/$TITLE_ID/eboot.bin"; then
        echo "upload REFUSED - the title is probably running and holding" >&2
        echo "eboot.bin open. Use '$0 run', which kills it first." >&2
        exit 5
    fi
    echo "sent"
    ;;

  resources)
    # === WHY THIS EXISTS: AN EBOOT ALONE IS A HALF-DEPLOY ==================
    #
    # The .vpk packs `resources/` from the SOURCE TREE, and the running title
    # reads it from `ux0:app/<id>/resources/`. So a change to an i18n
    # catalogue, an icon or a sound reaches the console through NO other path:
    # `push` sends the executable only. That is not hypothetical - a session on
    # 2026-09-13 shipped new code with its old catalogue and the screen showed
    # `shadow/quality/mode` as a raw key, because the strings the code asked
    # for existed only on the dev machine.
    #
    # USB (`vita-sync.sh full`) does the same job faster for the whole tree.
    # This is the Wi-Fi path, and it takes a SUBTREE precisely because sending
    # 1.7 MB of fonts and sounds to correct three lines of text is the reason
    # one skips the step.
    need_ip
    SUB="${2:-}"
    SRC_DIR="$REPO_ROOT/resources${SUB:+/$SUB}"
    [ -d "$SRC_DIR" ] || { echo "no such subtree: $SRC_DIR" >&2; exit 4; }
    n=0
    # `find -type f` and not a recursive curl: the companion's FTP creates no
    # directory, so each file is sent to a path that must already exist. Every
    # directory here ships in the .vpk, which is what makes that safe - a NEW
    # directory needs a reinstall, and the failure below says so out loud.
    while IFS= read -r f; do
        rel="${f#"$REPO_ROOT/resources/"}"
        if ! curl -sS --max-time 60 --ftp-method nocwd -T "$f" \
                  "ftp://$VITA_IP:$FTP_PORT//ux0:/app/$TITLE_ID/resources/$rel"; then
            echo "REFUSED: resources/$rel - if the directory is new, the card" >&2
            echo "needs the .vpk reinstalled; FTP will not create it." >&2
            exit 5
        fi
        n=$((n + 1))
    done < <(find "$SRC_DIR" -type f)
    echo "resources${SUB:+/$SUB}: $n file(s) sent"
    ;;

  relance)
    need_ip
    # Kill THEN launch, and the kill is not optional: launching a title that is
    # already running is a no-op, so without it the console would keep showing
    # the previous build while every log said the new one had been sent.
    send_cmd "kill $TITLE_ID" >/dev/null
    sleep 1
    send_cmd "launch $TITLE_ID"
    echo "relaunched $TITLE_ID"
    ;;

  run)
    # KILL FIRST, and the order is not a style choice: a running title holds
    # its own eboot.bin open, so the upload comes back `550 Failed FTP upload`
    # and the console keeps the previous build while everything here reports
    # success. Measured 2026-09-13.
    need_ip
    send_cmd "kill $TITLE_ID" >/dev/null
    # === LET THE CORE DUMP FINISH ======================================
    #
    # When the application CRASHES, the system writes a `psp2core-*.psp2dmp`
    # and leaves it suffixed `.tmp` until it is done. Relaunching at once cuts
    # that write short: on 2026-09-13 a 266 KB dump stayed `.tmp` and
    # `vita-parse-core` broke on its notes, which cost a whole diagnosis -- the
    # call stack was right there, unreadable.
    #
    # One second was enough for a clean `kill`; three let the dump complete.
    # Three seconds per round trip against a dump lost exactly when it matters
    # most: not a close call. `VITA_KILL_WAIT` sets it.
    sleep "${VITA_KILL_WAIT:-3}"
    "$0" push
    send_cmd "launch $TITLE_ID" >/dev/null
    echo "relaunched $TITLE_ID"
    ;;

  logs)
    need_ip
    DEST="${2:-./vita-logs}"
    mkdir -p "$DEST"
    for f in halyard.log halyard.1.log stderr.log; do
        curl -sS --max-time 60 --ftp-method nocwd \
             -o "$DEST/$f" "ftp://$VITA_IP:$FTP_PORT//ux0:/data/halyard/$f" \
            && echo "  $f" || true
    done
    echo "logs in $DEST"
    ;;

  config)
    # === THE ONE STEP USB CANNOT DO =====================================
    #
    # taiHEN reads `ur0:tai/config.txt`, and `ur0:` is the internal memory:
    # USB mass storage exposes the card only. VitaShell's own FTP does reach
    # it, so this fetches the real file, edits it, and puts it back.
    #
    # WHAT THIS DELIBERATELY DOES NOT DO: create `ux0:tai/config.txt`. taiHEN
    # prefers that file when it exists, so writing one would OVERRIDE the
    # internal config wholesale and silently unload every plugin already set
    # up - a two-line convenience that breaks the console's existing setup.
    [ $# -ge 2 ] || usage
    IP="$2"
    TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
    echo "reading ur0:tai/config.txt from $IP ..."
    curl -sS --max-time 30 --ftp-method nocwd \
         -o "$TMP/config.txt" "ftp://$IP:1337//ur0:/tai/config.txt" \
        || { echo "could not read it - is VitaShell open with FTP on (SELECT)?" >&2; exit 5; }
    cp "$TMP/config.txt" "./config.txt.vita.bak"
    echo "backup kept: ./config.txt.vita.bak ($(wc -l < "$TMP/config.txt") lines)"

    if grep -q vitacompanion "$TMP/config.txt"; then
        echo "vitacompanion already listed - nothing to change"
        exit 0
    fi
    # UNDER THE FIRST SECTION OF EACH KIND, ONCE. A real config.txt is not the
    # tidy file the format suggests: this console's had FOUR `*KERNEL` headers
    # and THREE `*main`, accumulated by successive plugin installers each
    # appending its own. Inserting on every match - which is what the obvious
    # one-liner does - wrote the module seven times. taiHEN would then be asked
    # to load the same plugin again and again, and the user's config comes back
    # noticeably worse than it went in.
    awk '
        { print }
        /^\*KERNEL[[:space:]]*$/ && !k++ { print "ux0:tai/vitacompanion_kernel.skprx" }
        /^\*main[[:space:]]*$/   && !m++ { print "ux0:tai/vitacompanion.suprx" }
    ' "$TMP/config.txt" > "$TMP/new.txt"

    # Exactly one of each, or we do not write. A count is the only thing that
    # distinguishes "inserted where I meant" from "inserted everywhere".
    nk=$(grep -c '^ux0:tai/vitacompanion_kernel.skprx$' "$TMP/new.txt" || true)
    nu=$(grep -c '^ux0:tai/vitacompanion.suprx$'        "$TMP/new.txt" || true)
    if [ "$nk" != 1 ] || [ "$nu" != 1 ]; then
        echo "refusing to write: $nk kernel line(s), $nu user line(s), expected 1 each" >&2
        exit 8
    fi

    # Refuse to write a file that gained nothing: if neither section header was
    # found the config has a shape we did not expect, and overwriting it with an
    # unchanged copy would report success while changing nothing.
    if ! grep -q vitacompanion "$TMP/new.txt"; then
        echo "neither *KERNEL nor *main found in config.txt - not writing." >&2
        echo "add these two lines by hand, under those sections:" >&2
        echo "  ux0:tai/vitacompanion_kernel.skprx   (under *KERNEL)" >&2
        echo "  ux0:tai/vitacompanion.suprx          (under *main)" >&2
        exit 6
    fi
    curl -sS --max-time 30 --ftp-method nocwd -T "$TMP/new.txt" \
         "ftp://$IP:1337//ur0:/tai/config.txt" \
        || { echo "write FAILED - the backup is ./config.txt.vita.bak" >&2; exit 7; }
    echo "config.txt updated. REBOOT the console, then:"
    echo "  tools/vita-push.sh ip $IP && tools/vita-push.sh ping"
    ;;

  dumps)
    need_ip
    DEST="${2:-./vita-dumps}"
    mkdir -p "$DEST"
    LIST="$(curl -sS --max-time 60 "ftp://$VITA_IP:$FTP_PORT//ux0:/data/" 2>/dev/null \
            | grep -oE 'psp2core-[^ ]*' || true)"
    [ -n "$LIST" ] || { echo "no dump on the console"; exit 0; }
    for f in $LIST; do
        case "$f" in
          *.tmp) echo "  $f  -- TRUNCATED (dump interrupted, unreadable); skipped"; continue ;;
        esac
        curl -sS --max-time 300 --ftp-method nocwd -o "$DEST/$f" \
             "ftp://$VITA_IP:$FTP_PORT//ux0:/data/$f" && echo "  $f"
    done
    echo "dumps in $DEST"
    ;;

  cmd)
    [ $# -ge 2 ] || usage
    send_cmd "$2"
    ;;

  *) usage ;;
esac
