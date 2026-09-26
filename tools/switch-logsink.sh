#!/usr/bin/env bash
# switch-logsink — receives the Switch application's log LIVE.
#
# Removes the manual round trip that paced every run: relaunch the app, quit to
# hbmenu, start ftpd, pull the files. Here the console pushes its lines to this
# machine as they happen, and a run costs only ONE
# geste humain : lancer l'application.
#
#   tools/switch-logsink.sh setup     # drops logsink.txt on the SD card (via ftpd)
#   tools/switch-logsink.sh listen    # waits and prints the log
#   tools/switch-logsink.sh listen out.log
#
# `setup` needs ftpd once; after that everything goes over the network.
set -euo pipefail
PORT="${LOGSINK_PORT:-9999}"
SWITCH_IP="${SWITCH_IP:-192.168.1.17}"
# FTP port and credentials: 5000 was hardcoded, which made the script unusable
# as soon as ftpd ran elsewhere (sys-ftpd already holds 5000).
SWITCH_PORT="${SWITCH_PORT:-5000}"
CRED_FILE="$(dirname "$0")/switch-ftp.env"
[ -f "$CRED_FILE" ] && . "$CRED_FILE"
CURL_AUTH=()
[ -n "${SWITCH_FTP_USER:-}" ] && CURL_AUTH=(-u "${SWITCH_FTP_USER}:${SWITCH_FTP_PASS:-}")
CMD_FIFO="${TMPDIR:-/tmp}/shadow-logsink-cmd"

host_ip() {
    # the address of THIS machine as the console sees it
    ip -4 route get "$SWITCH_IP" 2>/dev/null | grep -oP 'src \K[0-9.]+' | head -1
}

case "${1:-listen}" in
  setup)
    ip=$(host_ip)
    [ -z "$ip" ] && { echo "Adresse locale introuvable vers $SWITCH_IP"; exit 1; }
    echo "$ip:$PORT" > /tmp/logsink.txt
    echo "Destination : $ip:$PORT"
    curl -sS "${CURL_AUTH[@]}" -T /tmp/logsink.txt "ftp://$SWITCH_IP:$SWITCH_PORT/switch/halyard/logsink.txt" \
      && echo "Dropped on the SD card. ftpd is not needed afterwards." \
      || { echo "FAIL - is ftpd.nro running?"; exit 1; }
    ;;
  listen)
    out="${2:-}"
    echo "Listening on port $PORT - launch the application on the console."
    # `nc -l PORT` (OpenBSD) or `nc -l -p PORT` (GNU), depending on the variant
    if nc -h 2>&1 | grep -q "\-p port"; then LISTEN=(nc -l -p "$PORT"); else LISTEN=(nc -l "$PORT"); fi
    # LOOP: `nc -l` returns as soon as the console disconnects (the application
    # closing). Without this loop the listener had to be restarted on every
    # startup, and a launch made during the gap lost its ENTIRE log -
    # the network mirror tries ONCE at startup and then disables itself.
    # Ctrl-C sort proprement.
    # The command channel: a named pipe feeds `nc`'s input, so we can
    # drive the RUNNING application without it reconnecting -
    # `tools/switch-logsink.sh cmd quit` is enough to close it so a binary can be
    # pushed (the .nro is locked while it runs).
    # A `sleep` keeps the pipe open: without it, `cat` would return as soon as
    # first writer closes and `nc` would cut the connection.
    [ -p "$CMD_FIFO" ] || { rm -f "$CMD_FIFO"; mkfifo "$CMD_FIFO"; }
    sleep infinity > "$CMD_FIFO" &
    HOLD=$!
    trap 'kill $HOLD 2>/dev/null; echo; echo "ecoute arretee."; exit 0' INT TERM
    echo "commandes : tools/switch-logsink.sh cmd quit"
    while true; do
        # Drain the pipe BEFORE accepting: a command written while nobody was
        # listening stayed queued there and left the moment the connection
        # next one. Seen for real — a late `quit` closed a session
        # opened, hitting an application that had just started - exactly the
        # kind of surprise a diagnostic tool must not produce.
        while IFS= read -r -t 0.1 _residu < "$CMD_FIFO"; do
            echo "--- commande en attente ignoree : $_residu ---"
        done 2>/dev/null
        if [ -n "$out" ]; then "${LISTEN[@]}" < "$CMD_FIFO" | tee -a "$out";
        else "${LISTEN[@]}" < "$CMD_FIFO"; fi
        echo "--- console deconnectee, en attente d'un nouveau lancement ---"
    done
    ;;
  cmd)
    # Sends a command to the RUNNING application, through the listener already established.
    #   cmd quit              -> ferme proprement l'application (pour pouvoir pousser)
    #   cmd ping              -> checks that the channel answers
    #   cmd "autotest 8 25 5" -> arme une serie a chaud
    if [ ! -p "$CMD_FIFO" ]; then
        echo "Aucune ecoute active (tube $CMD_FIFO absent)."
        echo "Lancer d'abord : tools/switch-logsink.sh listen [fichier]"
        exit 1
    fi
    shift
    printf '%s\n' "$*" > "$CMD_FIFO"
    echo "commande envoyee : $*"
    ;;
  drive)
    # Canal BIDIRECTIONNEL : pilote l'application EN MARCHE, sans ftpd ni SD.
    #   tools/switch-logsink.sh drive 20 25 5   -> arme 20 sessions de 25 s
    #   tools/switch-logsink.sh drive ping      -> checks that the channel answers
    # Listens to the log AND sends the command on the same socket.
    if [ "${2:-}" = "ping" ]; then CMD="ping"; else
        CMD="autotest ${2:-20} ${3:-25} ${4:-5}"; fi
    echo "Ecoute sur $PORT ; commande a l'ouverture : $CMD"
    if nc -h 2>&1 | grep -q "\-p port"; then LISTEN=(nc -l -p "$PORT"); else LISTEN=(nc -l "$PORT"); fi
    # the command leaves as soon as the console connects
    { sleep 2; printf '%s\n' "$CMD"; cat >/dev/null; } | "${LISTEN[@]}" | tee "${5:-/dev/stdout}"
    ;;
  autotest)
    # tools/switch-logsink.sh autotest 10 30 5   -> 10 sessions of 30 s, 5 s of rest
    runs="${2:-10}"; dur="${3:-30}"; pause="${4:-5}"
    printf 'runs=%s duration=%s pause=%s\n' "$runs" "$dur" "$pause" > /tmp/autotest.txt
    curl -sS "${CURL_AUTH[@]}" -T /tmp/autotest.txt "ftp://$SWITCH_IP:$SWITCH_PORT/switch/halyard/autotest.txt" \
      && echo "Plan filed: $runs sessions of ${dur}s, ${pause}s of rest." \
      || { echo "FAIL - is ftpd.nro running?"; exit 1; }
    echo "Launch the application: it will run the trials on its own."
    ;;
  autotest-off)
    printf 'runs=1 duration=0 pause=0\n' > /tmp/autotest.txt
    curl -sS "${CURL_AUTH[@]}" -T /tmp/autotest.txt "ftp://$SWITCH_IP:$SWITCH_PORT/switch/halyard/autotest.txt" \
      && echo "Boucle desactivee (une seule session, comportement normal)."
    ;;
  *) echo "usage: $0 {setup|listen [fichier]|autotest [N] [duree] [repos]|autotest-off}"; exit 2 ;;
esac
