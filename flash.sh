#!/usr/bin/env bash
#
# Build and flash the bare-bones amaran bridge.
#
#   ./flash.sh                 build, flash, then tail the log (Ctrl+C to quit)
#   ./flash.sh --repl          same, but via idf.py monitor, which decodes
#                              panic backtraces (quit with Ctrl+])
#   ./flash.sh --logs          just tail the log, no build or flash
#   ./flash.sh -p /dev/cu.xxx  pick the serial port explicitly
#   ./flash.sh --build-only    compile and stop
#   ./flash.sh --no-monitor    build and flash, then exit
#   ./flash.sh --erase         erase flash first (see the seq warning below)
#   ./flash.sh --ports         just list candidate serial ports and exit
#
# Expects ESP-IDF v5.5.2+ installed at ~/esp/esp-idf (or already sourced).

# Deliberately no `set -u`: macOS ships bash 3.2, where an empty array
# expansion under `set -u` is an "unbound variable" error.
set -eo pipefail
cd "$(dirname "$0")"

PORT=""
ERASE=0
BUILD_ONLY=0
MONITOR=1
FLASH=1
# Default to a plain read-only tail, because idf.py monitor takes over the
# terminal and forwards Ctrl+C to the board -- its quit key is Ctrl+], which is
# unhelpful if your keyboard makes that awkward to type. --repl opts into the
# real thing when you need to type console commands or decode a panic.
REPL=0

list_ports() {
    ls /dev/cu.usbmodem* /dev/cu.usbserial* /dev/cu.SLAB_USBtoUART* \
       /dev/cu.wchusbserial* /dev/ttyUSB* /dev/ttyACM* 2>/dev/null || true
}

while [ $# -gt 0 ]; do
    case "$1" in
        -p|--port)    PORT="$2"; shift 2 ;;
        --erase)      ERASE=1; shift ;;
        --build-only) BUILD_ONLY=1; MONITOR=0; shift ;;
        --no-monitor) MONITOR=0; shift ;;
        --repl)       REPL=1; shift ;;
        --logs|--monitor) FLASH=0; ERASE=0; shift ;;
        --ports)      list_ports; exit 0 ;;
        -h|--help)    sed -n '2,16p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 1 ;;
    esac
done

# ---------------------------------------------------------------- config ---

for f in $([ "$FLASH" = 1 ] && echo main/config.h main/mesh_config.h); do
    if [ ! -f "$f" ]; then
        echo "missing $f" >&2
        if [ "$f" = "main/config.h" ]; then
            echo "  cp main/config.h.example main/config.h   # then edit it" >&2
        else
            echo "  cp ../amaran-BLE-control/esp32-firmware/main/mesh_config.h main/" >&2
        fi
        exit 1
    fi
done

# ---------------------------------------------------------------- esp-idf ---

# Tailing logs needs nothing but a serial port; only the build, flash and
# --repl paths need the toolchain.
if [ "$FLASH" = 0 ] && [ "$REPL" = 0 ]; then
    NEED_IDF=0
else
    NEED_IDF=1
fi

if [ "$NEED_IDF" = 1 ] && ! command -v idf.py >/dev/null 2>&1; then
    if [ -f "$HOME/esp/esp-idf/export.sh" ]; then
        echo "sourcing ESP-IDF from ~/esp/esp-idf"
        # shellcheck disable=SC1091
        . "$HOME/esp/esp-idf/export.sh" >/dev/null
    else
        echo "ESP-IDF not found. Install v5.5.2+ and source its export.sh." >&2
        exit 1
    fi
fi

# ------------------------------------------------------------------- port ---

if [ -z "$PORT" ] && [ "$BUILD_ONLY" = 0 ]; then
    # shellcheck disable=SC2207
    FOUND=( $(list_ports) )
    case ${#FOUND[@]} in
        0) echo "No serial port found. Plug the board in, or pass -p /dev/cu.xxx" >&2
           echo "(on the C5 devkit, use the port labelled UART, not the native USB one)" >&2
           exit 1 ;;
        1) PORT="${FOUND[0]}"
           echo "using serial port $PORT" ;;
        *) echo "Multiple serial ports found -- pick one with -p:" >&2
           printf '  %s\n' "${FOUND[@]}" >&2
           exit 1 ;;
    esac
fi

# Built as a string, not an array: bash 3.2 plus `set -u` makes empty arrays
# a minefield, and serial device paths never contain spaces.
PORT_ARGS=""
[ -n "$PORT" ] && PORT_ARGS="-p $PORT"

# ------------------------------------------------------------------ build ---

if [ "$FLASH" = 1 ]; then
    idf.py set-target esp32c5
fi

if [ "$ERASE" = 1 ]; then
    cat <<'EOF'

  WARNING: erase-flash resets the mesh sequence number to zero.
  Fixtures remember the last sequence number they saw from us and silently
  discard anything lower, so lights can go dead-quiet afterwards.
  AMARAN_SEQ_START in main/config.h exists to prevent that; if lights still
  ignore you, raise it or run `seqbump` from the serial console.

EOF
    # shellcheck disable=SC2086
    idf.py $PORT_ARGS erase-flash
fi

if [ "$FLASH" = 1 ]; then
    idf.py build

    if [ "$BUILD_ONLY" = 1 ]; then
        echo "build complete (--build-only, not flashing)"
        exit 0
    fi

    echo
    echo "flashing to $PORT ..."
    # shellcheck disable=SC2086
    idf.py $PORT_ARGS flash
fi

# ---------------------------------------------------------------- monitor ---

[ "$MONITOR" = 1 ] || exit 0

if [ "$REPL" = 1 ]; then
    echo
    echo "idf.py monitor -- log output with backtrace decoding."
    echo "QUIT WITH Ctrl+]  (Ctrl+C is sent to the board, not to this terminal)"
    echo
    # shellcheck disable=SC2086
    exec idf.py $PORT_ARGS monitor
fi

# Plain read-only tail. stty configures the line, cat prints it. Ctrl+C works
# normally because nothing puts the terminal into raw mode. The tradeoff is no
# typing and no backtrace symbolisation -- use --repl for those.
echo
echo "tailing $PORT -- Ctrl+C to quit.  (./flash.sh --repl to decode panics)"
echo

if stty -f "$PORT" 115200 raw -echo 2>/dev/null; then
    :                      # macOS / BSD
elif stty -F "$PORT" 115200 raw -echo 2>/dev/null; then
    :                      # Linux
else
    echo "could not configure $PORT; falling back to idf.py monitor (quit: Ctrl+])" >&2
    # shellcheck disable=SC2086
    exec idf.py $PORT_ARGS monitor
fi

exec cat "$PORT"
