#!/bin/bash
# switch-sync: a fast push/pull between the PC and the Switch through ftpd.nro (port 5000).
#
# Switch prerequisites:
#   - ftpd.nro installed on the SD card (already present in /switch/)
#   - Launch ftpd.nro from hbmenu (Atmosphere) BEFORE push/pull
#   - Note: a standalone ftpd.nro exits when the user quits back to hbmenu.
#     For FTP in the background while another homebrew runs, use
#     sys-ftpd-light (sysmodule).
#
# Usage :
#   tools/switch-sync.sh push                  # push le NRO build (default)
#   tools/switch-sync.sh push <local-file> <remote-path>
#   tools/switch-sync.sh logs                  # pull all the usual logs
#   tools/switch-sync.sh pull <remote-path> <local-path>
#   tools/switch-sync.sh ping                  # check ftpd alive

set -e

SWITCH_IP="${SWITCH_IP:-192.168.1.17}"
SWITCH_PORT="${SWITCH_PORT:-5000}"
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NRO_LOCAL="$PROJECT_ROOT/build_switch/halyard.nro"
NRO_REMOTE="/switch/halyard.nro"
LOGS_REMOTE_DIR="/switch/halyard"

FTP_BASE="ftp://${SWITCH_IP}:${SWITCH_PORT}"

# FTP authentication - empty by default (ftpd.nro with no credentials).
#
# It became necessary on 2026-08-25: sys-ftpd refuses everything until
# /config/sys-ftpd/config.ini has no user, and ftpd.nro may not either
# to have one. With no support here, a `push` failed on a "430" without saying
# what to do. Provide them through environment variables, or in the file
# tools/switch-ftp.env (ignore par git) :
#
#   SWITCH_FTP_USER=ftpd SWITCH_FTP_PASS=ftpd tools/switch-sync.sh push
#
CRED_FILE="$(dirname "$0")/switch-ftp.env"
[ -f "$CRED_FILE" ] && . "$CRED_FILE"
SWITCH_FTP_USER="${SWITCH_FTP_USER:-}"
SWITCH_FTP_PASS="${SWITCH_FTP_PASS:-}"
CURL_AUTH=()
[ -n "$SWITCH_FTP_USER" ] && CURL_AUTH=(-u "${SWITCH_FTP_USER}:${SWITCH_FTP_PASS}")

cmd_ping() {
    echo "Pinging ftpd at $SWITCH_IP:$SWITCH_PORT ..."
    if curl -s "${CURL_AUTH[@]}" --connect-timeout 3 -o /dev/null "${FTP_BASE}/" 2>/dev/null; then
        echo "OK — ftpd reachable"
        return 0
    fi
    echo "FAIL — ftpd injoignable ou refuse l'authentification."
    echo "  ftpd.nro lance ? bon port (SWITCH_PORT=$SWITCH_PORT) ?"
    echo "  identifiants : SWITCH_FTP_USER / SWITCH_FTP_PASS, ou tools/switch-ftp.env"
    return 1
}

cmd_push() {
    local local_file="${1:-$NRO_LOCAL}"
    local remote_path="${2:-$NRO_REMOTE}"
    if [ ! -f "$local_file" ]; then
        echo "ERROR: $local_file not found"
        exit 1
    fi
    echo "Push: $local_file → $SWITCH_IP$remote_path"
    local size=$(stat -c%s "$local_file")
    local started=$(date +%s)
    curl --silent --show-error --fail "${CURL_AUTH[@]}" \
        -T "$local_file" \
        "${FTP_BASE}${remote_path}"
    local elapsed=$(($(date +%s) - started))
    elapsed=${elapsed:-1}
    local mbps=$((size / 1024 / 1024 / elapsed))
    echo "OK — $size bytes in ${elapsed}s (~${mbps} MB/s)"
}

# ---------------------------------------------------------------- relance ---
#
# RELOAD-1 (2026-09-12): an iteration no longer costs a human gesture.
#
# The problem: a .nro that IS RUNNING is locked, so the new build cannot land
# on the path in use (FTP 450 - which is also why
# (`quit` has existed since S45). But since RELOAD-1 the application can name
# le .nro a charger a sa sortie (`envSetNextLoad`).
#
# Hence two alternating names: we push to the one that is NOT running, then
# it asks the running application to hand over to that one. A launch
# human gesture per SESSION instead of one per iteration.
#
# HOW WE KNOW WHICH ONE IS FREE: we try, and the LOCK answers. A counter kept
# here would desynchronise the first time someone launched by hand on the
# console, and the error would then read "it is pushing to the locked file", a
# long way from its cause. The lock never lies: it is the real state.
NRO_REMOTE_A="/switch/halyard.nro"
NRO_REMOTE_B="/switch/halyard.b.nro"
DEVLINK="$PROJECT_ROOT/tools/client/devlink.py"

# Pushes without leaving the script on failure (cmd_push does exit).
try_push() {
    curl --silent --show-error --fail "${CURL_AUTH[@]}" \
        -T "$1" "${FTP_BASE}${2}" 2>/dev/null
}

cmd_relance() {
    local local_file="${1:-$NRO_LOCAL}"
    if [ ! -f "$local_file" ]; then
        echo "ERREUR : $local_file introuvable — construire d'abord le .nro"
        exit 1
    fi
    local size; size=$(stat -c%s "$local_file")
    local target="" lock_seen=0

    # A first: it is the name hbmenu shows, hence the one a manual launch
    # takes, hence most often the locked one - and the attempt that fails is
    # also the one that tells us what is running.
    echo -n "Essai sur $NRO_REMOTE_A … "
    if try_push "$local_file" "$NRO_REMOTE_A"; then
        echo "libre, pousse ($size octets)"; target="$NRO_REMOTE_A"
    else
        echo "locked (that is the one running)"
        lock_seen=1
        echo -n "Essai sur $NRO_REMOTE_B … "
        if try_push "$local_file" "$NRO_REMOTE_B"; then
            echo "libre, pousse ($size octets)"; target="$NRO_REMOTE_B"
        else
            echo "echec aussi."
            # TWO FAILURES DO NOT MEAN TWO LOCKS. An upload can fail because the
            # file is running, OR because nothing answers any more - the console
            # went to sleep, the Wi-Fi dropped. Seen on 2026-09-12: the message
            # spoke of "two instances" when the console had simply switched off.
            # So we ask BEFORE concluding.
            if curl -s "${CURL_AUTH[@]}" --connect-timeout 3 -o /dev/null "${FTP_BASE}/" 2>/dev/null; then
                echo "ERROR: the console answers, but both names are locked."
                echo "  Deux instances lancees ? Fermer l'application et reessayer."
            else
                echo "ERROR: the console no longer answers ($SWITCH_IP:$SWITCH_PORT)."
                echo "  Asleep, switched off, or moved to the other interface -"
                echo "  the IP changes between Wi-Fi and Ethernet. Check: $0 ping"
            fi
            exit 1
        fi
    fi

    # NO LOCK = NOTHING IS RUNNING, and that must be said BEFORE sending a
    # command to nobody. Without this test the failure that follows was blamed
    # on the build ("it predates RELOAD-1"), a plausible hypothesis and, that
    # day, a false one: the console simply no longer had the application open. A
    # message that names the wrong culprit costs more
    # more expensive than a missing message — and the lock had already said so.
    if [ "$lock_seen" -eq 0 ]; then
        echo "Both names were free: NO application is running on the console."
        echo "  $NRO_REMOTE_A is up to date. There is nothing to hand over to -"
        echo "  launch it from hbmenu, and the later relaunches happen on their own."
        exit 1
    fi

    echo "Handing over to $target …"
    if "$DEVLINK" --delai 10 relaunch "$target"; then
        echo "OK — l'application redemarre sur $target."
        echo "     Keep the devlink listener open: it accepts the new"
        echo "     connexion toute seule."
    else
        echo "FAILED to hand over. $target is up to date and the application"
        echo "     is still running: relaunching it by hand starts from there."
        exit 1
    fi
}

cmd_pull() {
    local remote_path="$1"
    local local_path="$2"
    if [ -z "$remote_path" ] || [ -z "$local_path" ]; then
        echo "Usage: $0 pull <remote-path> <local-path>"
        exit 1
    fi
    echo "Pull: $SWITCH_IP$remote_path → $local_path"
    curl --silent --show-error --fail "${CURL_AUTH[@]}" \
        -o "$local_path" \
        "${FTP_BASE}${remote_path}"
    echo "OK — $(stat -c%s "$local_path") bytes"
}

cmd_logs() {
    local out_dir="${1:-/tmp/switch-logs-$(date +%Y%m%d_%H%M%S)}"
    mkdir -p "$out_dir"
    echo "Pulling logs to $out_dir/"
    # `webrtc.log` / `webrtc_prev.log` were S81's predecessors and are gone: the
    # rename moved the data directory, so nothing under it can carry them.
    for f in halyard.log halyard-precedent.log borealis.log \
             last_clients_launcher.json last_clients_main.json last_clients_usb.json \
             last_vm_ip_response.txt last_vms_response.json last_vm_start_response.txt \
             last_stream.txt curl_trace.log proximus_trace.log quic_trace.log quic_handshake.log \
             device.uuid settings.txt; do
        echo -n "  $f ... "
        if curl --silent --fail "${CURL_AUTH[@]}" \
            -o "$out_dir/$f" \
            "${FTP_BASE}${LOGS_REMOTE_DIR}/$f" 2>/dev/null; then
            echo "$(stat -c%s "$out_dir/$f") bytes"
        else
            rm -f "$out_dir/$f"
            echo "(absent)"
        fi
    done
    echo ""
    echo "Logs in: $out_dir"
    echo ""
    echo "Quick refs:"
    echo "  ls -la $out_dir/"
    echo "  tail -50 $out_dir/halyard.log"
}

cmd_help() {
    cat <<EOF
switch-sync — push/pull via ftpd (Switch port 5000)

Commands:
  push [<local> <remote>]   Push file (default: build NRO → /switch/halyard.nro)
  pull <remote> <local>     Pull file
  logs [<out-dir>]          Pull all standard logs to a fresh dir under /tmp
  ping                      Check ftpd reachable
  help                      This message

Env:
  SWITCH_IP                 Switch IP (default: $SWITCH_IP)
  SWITCH_PORT               ftpd port (default: $SWITCH_PORT)

Iteration workflow (RELOAD-1, 2026-09-12) - one human gesture per SESSION:

  0. Once: tools/switch-logsink.sh setup   (drops logsink.txt on the SD card)
  1. Open the listener FIRST:  tools/client/devlink.py listen
  2. Launch halyard.nro on the console - THE ONLY HUMAN GESTURE
  3. Then, as many times as you like:
       (cd build_switch && make halyard.nro)
       tools/switch-sync.sh relance
     It pushes to the name that is not running, then asks the application to
     hand over. The listener from step 1 accepts the new connection on its own.
  4. Piloter et regarder : devlink.py state | shot | btn | nav
  5. Journaux : tools/switch-sync.sh logs

  If the application CRASHES the chain breaks: it must be relaunched by hand.
  This is a mechanism for iterating, not for recovering.

The old workflow, still valid (manual ftpd):
  push -> quit the app on the console -> relaunch by hand -> logs
EOF
}

case "${1:-help}" in
    push)  shift; cmd_push "$@" ;;
    relance|relaunch) shift; cmd_relance "$@" ;;
    pull)  shift; cmd_pull "$@" ;;
    logs)  shift; cmd_logs "$@" ;;
    ping)  cmd_ping ;;
    help|--help|-h) cmd_help ;;
    *) cmd_help; exit 1 ;;
esac
