#!/usr/bin/env bash
# Launch a ROM on Android, inject a debug harness input sequence, and pull a screenshot.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

ADB="${ADB:-adb}"
PACKAGE="${MELONDS_PACKAGE:-me.magnum.melonds.nightly.dev}"
ACTIVITY="${MELONDS_ACTIVITY:-me.magnum.melonds.ui.emulator.EmulatorActivity}"
RECEIVER_CLASS="${MELONDS_DEBUG_RECEIVER:-me.magnum.melonds.debug.EmulatorDebugReceiver}"

URI=""
SEQUENCE=""
LOAD_STATE_URI=""
PRESS_BUTTON=""
PRESS_COUNT=0
PRESS_MS=400
GAP_MS=400
LAUNCH_WAIT=20
POST_WAIT=8
FAST_FORWARD=""
SCREENSHOT_OUT=""
REMOTE_SCREENSHOT=""

usage() {
    cat <<EOF
Usage: $0 --uri ROM_CONTENT_URI [sequence options] [options]

Sequence options:
  --sequence CSV       Direct harness sequence, e.g. 'A,A,DOWN,A,SLEEP:2000'
  --press-a COUNT      Convenience shorthand for COUNT presses of A
  --press BUTTON COUNT Convenience shorthand for COUNT presses of BUTTON

Other options:
  --load-state-uri URI Savestate content:// URI to load through the harness before inputs
  --press-ms MS        Button hold duration per command. Default: $PRESS_MS
  --gap-ms MS          Delay between commands. Default: $GAP_MS
  --launch-wait SEC    Seconds to wait after ROM launch. Default: $LAUNCH_WAIT
  --post-wait SEC      Seconds to wait after harness injection. Default: $POST_WAIT
  --fast-forward BOOL  true/false. Toggle fast-forward before the input sequence
  --screenshot PATH    Local screenshot output path. Default: /tmp/<timestamp>-harness.png
  --package NAME       App package. Default: $PACKAGE
  --activity NAME      Activity class. Default: $ACTIVITY
  -h, --help           Show this help

Environment:
  ADB                  adb binary. Default: adb
  MELONDS_PACKAGE      Overrides default package
  MELONDS_ACTIVITY     Overrides default activity
  MELONDS_DEBUG_RECEIVER Overrides default receiver class

Examples:
  $0 --uri 'content://...' --press-a 30
  $0 --uri 'content://...' --sequence 'A,A,DOWN,A,SLEEP:3000,A'
EOF
    exit 2
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --uri) URI="$2"; shift 2 ;;
        --sequence) SEQUENCE="$2"; shift 2 ;;
        --press-a) PRESS_BUTTON="A"; PRESS_COUNT="$2"; shift 2 ;;
        --press) PRESS_BUTTON="$2"; PRESS_COUNT="$3"; shift 3 ;;
        --load-state-uri) LOAD_STATE_URI="$2"; shift 2 ;;
        --press-ms) PRESS_MS="$2"; shift 2 ;;
        --gap-ms) GAP_MS="$2"; shift 2 ;;
        --launch-wait) LAUNCH_WAIT="$2"; shift 2 ;;
        --post-wait) POST_WAIT="$2"; shift 2 ;;
        --fast-forward) FAST_FORWARD="$2"; shift 2 ;;
        --screenshot) SCREENSHOT_OUT="$2"; shift 2 ;;
        --package) PACKAGE="$2"; shift 2 ;;
        --activity) ACTIVITY="$2"; shift 2 ;;
        -h|--help) usage ;;
        *) echo "Unknown argument: $1" >&2; usage ;;
    esac
done

if [[ -z "$URI" ]]; then
    echo "Error: --uri is required." >&2
    usage
fi

if [[ -n "$PRESS_BUTTON" ]]; then
    if ! [[ "$PRESS_COUNT" =~ ^[0-9]+$ ]] || [[ "$PRESS_COUNT" -le 0 ]]; then
        echo "Error: --press count must be a positive integer." >&2
        exit 1
    fi

    generated=""
    for ((i = 0; i < PRESS_COUNT; i++)); do
        if [[ -n "$generated" ]]; then
            generated+=","
        fi
        generated+="$PRESS_BUTTON"
    done
    SEQUENCE="$generated"
fi

if [[ -z "$SEQUENCE" && -z "$LOAD_STATE_URI" && -z "$FAST_FORWARD" ]]; then
    echo "Error: specify at least one of --sequence, --press-a/--press, --load-state-uri, or --fast-forward." >&2
    exit 1
fi

if [[ -z "$SCREENSHOT_OUT" ]]; then
    SCREENSHOT_OUT="/tmp/$(date -u +%Y%m%dT%H%M%SZ)-harness.png"
fi

REMOTE_SCREENSHOT="/sdcard/Download/$(basename "$SCREENSHOT_OUT")"

if ! "$ADB" get-state >/dev/null 2>&1; then
    echo "Error: no Android device is connected or authorized for adb." >&2
    "$ADB" devices -l >&2 || true
    exit 1
fi

echo "Launching ROM..."
"$ADB" shell am start -S \
    -n "${PACKAGE}/${ACTIVITY}" \
    --es uri "$URI" >/dev/null

sleep "$LAUNCH_WAIT"

BROADCAST_ARGS=(
    shell am broadcast
    -a me.magnum.melonds.DEBUG_EMULATOR
    -n "${PACKAGE}/${RECEIVER_CLASS}"
)

if [[ -n "$LOAD_STATE_URI" ]]; then
    BROADCAST_ARGS+=(--es load_state_uri "$LOAD_STATE_URI")
fi

if [[ -n "$FAST_FORWARD" ]]; then
    BROADCAST_ARGS+=(--ez fast_forward "$FAST_FORWARD")
fi

if [[ -n "$SEQUENCE" ]]; then
    BROADCAST_ARGS+=(--es sequence "$SEQUENCE" --el press_ms "$PRESS_MS" --el gap_ms "$GAP_MS")
fi

echo "Injecting harness sequence..."
"$ADB" "${BROADCAST_ARGS[@]}" >/dev/null

sleep "$POST_WAIT"

echo "Capturing screenshot..."
"$ADB" shell screencap -p "$REMOTE_SCREENSHOT" >/dev/null 2>&1
mkdir -p "$(dirname "$SCREENSHOT_OUT")"
"$ADB" pull "$REMOTE_SCREENSHOT" "$SCREENSHOT_OUT" >/dev/null

echo "$SCREENSHOT_OUT"
