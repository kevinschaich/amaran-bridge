#!/usr/bin/env bash
#
# devloop -- a command bus over the shared repo folder.
#
# Run this once, in a terminal on your Mac, and leave it running:
#
#     ./devloop.sh
#
# It does three things forever:
#   1. tails the board's serial output into .devloop/serial.log
#   2. watches .devloop/in/ for command files and runs them
#   3. writes each command's output to .devloop/out/
#
# Claude can read and write those files through the shared folder, so it can
# build, flash, send console commands and read logs without you relaying
# anything. You can watch it all happen in this terminal.
#
# Ctrl+C stops it. Nothing runs when it is not running.
#
# SECURITY NOTE: the `sh` verb runs arbitrary shell commands as you. That is
# the point -- it is what makes the loop useful -- but it means anything that
# can write into .devloop/in/ can run commands on your machine. Stop this
# script when you are done.

set -eo pipefail
cd "$(dirname "$0")"

PORT=""
BAUD=115200
DIR=".devloop"
MAX_LOG_BYTES=4000000

while [ $# -gt 0 ]; do
    case "$1" in
        -p|--port) PORT="$2"; shift 2 ;;
        -h|--help) sed -n '2,26p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 1 ;;
    esac
done

list_ports() {
    ls /dev/cu.usbmodem* /dev/cu.usbserial* /dev/cu.SLAB_USBtoUART* \
       /dev/cu.wchusbserial* /dev/ttyUSB* /dev/ttyACM* 2>/dev/null || true
}

if [ -z "$PORT" ]; then
    # shellcheck disable=SC2207
    FOUND=( $(list_ports) )
    case ${#FOUND[@]} in
        0) echo "No serial port found. Plug the board in, or pass -p /dev/cu.xxx" >&2; exit 1 ;;
        1) PORT="${FOUND[0]}" ;;
        *) echo "Multiple ports found -- pick one with -p:" >&2
           printf '  %s\n' "${FOUND[@]}" >&2; exit 1 ;;
    esac
fi

if ! command -v idf.py >/dev/null 2>&1; then
    if [ -f "$HOME/esp/esp-idf/export.sh" ]; then
        echo "sourcing ESP-IDF..."
        # shellcheck disable=SC1091
        . "$HOME/esp/esp-idf/export.sh" >/dev/null
    else
        echo "ESP-IDF not found; build and flash verbs will fail." >&2
    fi
fi

mkdir -p "$DIR/in" "$DIR/out"
: > "$DIR/serial.log"

# ------------------------------------------------------------ serial tail ---

TAIL_PID=""

start_tail() {
    [ -n "$TAIL_PID" ] && return 0
    stty -f "$PORT" "$BAUD" raw -echo 2>/dev/null || \
    stty -F "$PORT" "$BAUD" raw -echo 2>/dev/null || true
    cat "$PORT" >> "$DIR/serial.log" 2>/dev/null &
    TAIL_PID=$!
}

stop_tail() {
    [ -z "$TAIL_PID" ] && return 0
    kill "$TAIL_PID" 2>/dev/null || true
    wait "$TAIL_PID" 2>/dev/null || true
    TAIL_PID=""
}

rotate_log() {
    local size
    size=$(wc -c < "$DIR/serial.log" 2>/dev/null || echo 0)
    if [ "$size" -gt "$MAX_LOG_BYTES" ]; then
        tail -n 3000 "$DIR/serial.log" > "$DIR/serial.log.tmp"
        mv "$DIR/serial.log.tmp" "$DIR/serial.log"
        stop_tail; start_tail
    fi
}

cleanup() {
    stop_tail
    echo "devloop stopped."
    exit 0
}
trap cleanup INT TERM

# ---------------------------------------------------------------- commands ---

# A command file's first line is the verb; the rest is its argument body.
run_command() {
    local file="$1" name verb body out
    name=$(basename "$file" .cmd)
    out="$DIR/out/$name.out"
    verb=$(head -n 1 "$file")
    body=$(tail -n +2 "$file")

    echo "[devloop] $name: $verb" | tee -a "$DIR/serial.log"
    : > "$out"

    local rc=0
    case "$verb" in
        build)
            idf.py build >> "$out" 2>&1 || rc=$?
            ;;
        flash)
            stop_tail
            { idf.py build && idf.py -p "$PORT" flash; } >> "$out" 2>&1 || rc=$?
            sleep 1
            start_tail
            ;;
        reset)
            stop_tail
            python -m esptool --chip esp32c5 -p "$PORT" run >> "$out" 2>&1 || rc=$?
            sleep 1
            start_tail
            ;;
        send)
            # Each line of the body is one console command.
            while IFS= read -r line; do
                [ -z "$line" ] && continue
                printf '%s\r\n' "$line" > "$PORT" || rc=$?
                sleep 0.4
            done <<< "$body"
            echo "sent" >> "$out"
            ;;
        logclear)
            stop_tail; : > "$DIR/serial.log"; start_tail
            echo "cleared" >> "$out"
            ;;
        sh)
            # Arbitrary shell. See the security note at the top of this file.
            bash -c "$body" >> "$out" 2>&1 || rc=$?
            ;;
        *)
            echo "unknown verb: $verb" >> "$out"; rc=64
            ;;
    esac

    echo "$rc" > "$DIR/out/$name.done"
    rm -f "$file"
    echo "[devloop] $name: exit $rc"
}

# -------------------------------------------------------------------- main ---

echo "devloop running"
echo "  port : $PORT"
echo "  bus  : $(pwd)/$DIR"
echo "  log  : $DIR/serial.log"
echo "Ctrl+C to stop."

start_tail

while true; do
    printf 'alive %s port=%s tail=%s\n' "$(date +%s)" "$PORT" "${TAIL_PID:-none}" \
        > "$DIR/status"

    # Keep the tail alive. It dies on unplug, reflash and panic reboot, and the
    # USB port re-enumerates on reset, which leaves the old descriptor stale --
    # so re-open unconditionally whenever we do not have a live reader.
    if [ -n "$TAIL_PID" ] && ! kill -0 "$TAIL_PID" 2>/dev/null; then
        TAIL_PID=""
    fi
    if [ -z "$TAIL_PID" ] && [ -e "$PORT" ]; then
        start_tail
    fi

    for f in "$DIR"/in/*.cmd; do
        [ -e "$f" ] || continue
        run_command "$f"
    done

    rotate_log
    sleep 1
done
