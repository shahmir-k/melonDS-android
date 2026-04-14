#!/usr/bin/env bash
# Launch a ROM on Android, inject a debug harness input sequence, and pull a screenshot.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

ADB="${ADB:-adb}"
PACKAGE="${MELONDS_PACKAGE:-me.magnum.melonds.nightly.dev}"
ACTIVITY="${MELONDS_ACTIVITY:-me.magnum.melonds.ui.emulator.EmulatorActivity}"
RECEIVER_CLASS="${MELONDS_DEBUG_RECEIVER:-me.magnum.melonds.debug.EmulatorDebugReceiver}"
SCENE_ANALYZER="$SCRIPT_DIR/analyze_harness_scene.py"
TOP_DISPLAY_ID="${MELONDS_TOP_DISPLAY_ID:-1}"
BOTTOM_DISPLAY_ID="${MELONDS_BOTTOM_DISPLAY_ID:-0}"
DEFAULT_BENCHMARK_SCENE="gameplay_loaded"
DEFAULT_LAUNCH_ONLY_SCENE="menu"
DEFAULT_BENCHMARK_SEQUENCE="A,A,A,A,A,A,A,A,A,A,A,A,A,A,A,A,A,A,A,A,A,A,A,A,A,A,A,A,A,A"

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
EXPECT_SCENE=""
SKIP_SCENE_CHECK=0
SKIP_METRICS=0
FPS_SAMPLE_COUNT=6
FPS_INTERVAL_MS=1000
PERF_DURATION_SEC=""
PERF_EVENT="instructions"
BOTTOM_SCREENSHOT_OUT=""
CAPTURE_ONLY=0
LAUNCH_ONLY=0
WAIT_FOR_SCENE=""
WAIT_TIMEOUT_SEC=60
WAIT_INTERVAL_SEC=2

usage() {
    cat <<EOF
Usage: $0 [--uri ROM_CONTENT_URI] [sequence options] [options]

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
  --screenshot PATH    Local top-screen screenshot output path. Default: /tmp/<timestamp>-top.png
  --bottom-screenshot PATH
                      Optional local bottom-screen screenshot output path
  --expect-scene NAME  Expected final scene: menu | gameplay_loaded | blackscreen | whiteframe
  --skip-scene-check   Skip baseline scene analysis after the screenshot
  --skip-metrics       Skip FPS sampling and simpleperf instruction counting
  --fps-samples N      Number of FPS samples to average. Default: $FPS_SAMPLE_COUNT
  --fps-interval-ms MS Delay between FPS samples. Default: $FPS_INTERVAL_MS
  --perf-duration SEC  simpleperf duration in seconds. Default: ceil(samples * interval)
  --wait-for-scene NAME
                      Wait until the top screen matches: menu | gameplay_loaded | blackscreen | whiteframe
  --wait-timeout SEC   Timeout for --wait-for-scene. Default: $WAIT_TIMEOUT_SEC
  --wait-interval SEC  Poll interval for --wait-for-scene. Default: $WAIT_INTERVAL_SEC
  --top-display-id ID  Physical display ID for the DS top screen. Default: $TOP_DISPLAY_ID
  --bottom-display-id ID
                      Physical display ID for the DS bottom screen. Default: $BOTTOM_DISPLAY_ID
  --capture-only       Do not launch or inject inputs. Capture/measure the current app state only
  --launch-only        Launch the ROM and stop there. No input injection
  --package NAME       App package. Default: $PACKAGE
  --activity NAME      Activity class. Default: $ACTIVITY
  -h, --help           Show this help

Environment:
  ADB                  adb binary. Default: adb
  MELONDS_PACKAGE      Overrides default package
  MELONDS_ACTIVITY     Overrides default activity
  MELONDS_DEBUG_RECEIVER Overrides default receiver class
  MELONDS_TOP_DISPLAY_ID Overrides default top display id
  MELONDS_BOTTOM_DISPLAY_ID Overrides default bottom display id

Examples:
  $0 --uri 'content://...' --press-a 30
  $0 --uri 'content://...' --sequence 'A,A,DOWN,A,SLEEP:3000,A'
  $0 --uri 'content://...' --press-a 60 --expect-scene gameplay_loaded
  $0 --capture-only --screenshot /tmp/current-top.png --bottom-screenshot /tmp/current-bottom.png
  $0 --uri 'content://...' --launch-only
  $0 --uri 'content://...' --launch-only --wait-for-scene menu

Benchmark defaults:
  - launched benchmark runs default to pressing A 30 times, then waiting for gameplay_loaded
  - launch-only metrics runs default to waiting for menu
  - when a wait scene is active, screenshot validation defaults to that same scene
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
        --bottom-screenshot) BOTTOM_SCREENSHOT_OUT="$2"; shift 2 ;;
        --expect-scene) EXPECT_SCENE="$2"; shift 2 ;;
        --skip-scene-check) SKIP_SCENE_CHECK=1; shift 1 ;;
        --skip-metrics) SKIP_METRICS=1; shift 1 ;;
        --fps-samples) FPS_SAMPLE_COUNT="$2"; shift 2 ;;
        --fps-interval-ms) FPS_INTERVAL_MS="$2"; shift 2 ;;
        --perf-duration) PERF_DURATION_SEC="$2"; shift 2 ;;
        --wait-for-scene) WAIT_FOR_SCENE="$2"; shift 2 ;;
        --wait-timeout) WAIT_TIMEOUT_SEC="$2"; shift 2 ;;
        --wait-interval) WAIT_INTERVAL_SEC="$2"; shift 2 ;;
        --top-display-id) TOP_DISPLAY_ID="$2"; shift 2 ;;
        --bottom-display-id) BOTTOM_DISPLAY_ID="$2"; shift 2 ;;
        --capture-only) CAPTURE_ONLY=1; shift 1 ;;
        --launch-only) LAUNCH_ONLY=1; shift 1 ;;
        --package) PACKAGE="$2"; shift 2 ;;
        --activity) ACTIVITY="$2"; shift 2 ;;
        -h|--help) usage ;;
        *) echo "Unknown argument: $1" >&2; usage ;;
    esac
done

if [[ "$CAPTURE_ONLY" -eq 0 && -z "$URI" ]]; then
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

if [[ -z "$SCREENSHOT_OUT" ]]; then
    SCREENSHOT_OUT="/tmp/$(date -u +%Y%m%dT%H%M%SZ)-top.png"
fi

if ! [[ "$FPS_SAMPLE_COUNT" =~ ^[0-9]+$ ]] || [[ "$FPS_SAMPLE_COUNT" -le 0 ]]; then
    echo "Error: --fps-samples must be a positive integer." >&2
    exit 1
fi

if ! [[ "$FPS_INTERVAL_MS" =~ ^[0-9]+$ ]] || [[ "$FPS_INTERVAL_MS" -le 0 ]]; then
    echo "Error: --fps-interval-ms must be a positive integer." >&2
    exit 1
fi

if ! [[ "$WAIT_TIMEOUT_SEC" =~ ^[0-9]+$ ]] || [[ "$WAIT_TIMEOUT_SEC" -le 0 ]]; then
    echo "Error: --wait-timeout must be a positive integer." >&2
    exit 1
fi

if ! [[ "$WAIT_INTERVAL_SEC" =~ ^[0-9]+$ ]] || [[ "$WAIT_INTERVAL_SEC" -le 0 ]]; then
    echo "Error: --wait-interval must be a positive integer." >&2
    exit 1
fi

if [[ "$SKIP_METRICS" -eq 0 && -z "$WAIT_FOR_SCENE" ]]; then
    if [[ "$LAUNCH_ONLY" -eq 1 ]]; then
        WAIT_FOR_SCENE="$DEFAULT_LAUNCH_ONLY_SCENE"
    else
        WAIT_FOR_SCENE="$DEFAULT_BENCHMARK_SCENE"
    fi
fi

if [[ "$CAPTURE_ONLY" -eq 0 && "$LAUNCH_ONLY" -eq 0 && -z "$SEQUENCE" && -z "$LOAD_STATE_URI" && -z "$FAST_FORWARD" ]]; then
    SEQUENCE="$DEFAULT_BENCHMARK_SEQUENCE"
fi

if [[ "$CAPTURE_ONLY" -eq 0 && "$LAUNCH_ONLY" -eq 0 && -z "$SEQUENCE" && -z "$LOAD_STATE_URI" && -z "$FAST_FORWARD" ]]; then
    echo "Error: specify at least one of --sequence, --press-a/--press, --load-state-uri, or --fast-forward." >&2
    exit 1
fi

if [[ "$SKIP_SCENE_CHECK" -eq 0 && -z "$EXPECT_SCENE" && -n "$WAIT_FOR_SCENE" ]]; then
    EXPECT_SCENE="$WAIT_FOR_SCENE"
fi

REMOTE_SCREENSHOT="/sdcard/Download/$(basename "$SCREENSHOT_OUT")"

capture_top_to_file() {
    local local_out="$1"
    local remote_out="/sdcard/Download/$(basename "$local_out")"
    "$ADB" shell screencap -d "$TOP_DISPLAY_ID" -p "$remote_out" >/dev/null 2>&1
    mkdir -p "$(dirname "$local_out")"
    "$ADB" pull "$remote_out" "$local_out" >/dev/null
}

wait_for_scene() {
    local expected_scene="$1"
    local timeout_sec="$2"
    local interval_sec="$3"
    local deadline=$((SECONDS + timeout_sec))
    local probe="/tmp/harness-wait-$$.png"

    if [[ ! -f "$SCENE_ANALYZER" ]]; then
        echo "Error: scene analyzer not found at $SCENE_ANALYZER" >&2
        exit 1
    fi

    echo "Waiting for scene: $expected_scene"
    while (( SECONDS < deadline )); do
        capture_top_to_file "$probe"
        if python3 "$SCENE_ANALYZER" "$probe" --expect-scene "$expected_scene" >/dev/null 2>&1; then
            echo "Reached scene: $expected_scene"
            rm -f "$probe"
            return 0
        fi
        sleep "$interval_sec"
    done

    echo "Error: timed out waiting for scene '$expected_scene'" >&2
    python3 "$SCENE_ANALYZER" "$probe" || true
    rm -f "$probe"
    exit 1
}

if ! "$ADB" get-state >/dev/null 2>&1; then
    echo "Error: no Android device is connected or authorized for adb." >&2
    "$ADB" devices -l >&2 || true
    exit 1
fi

if [[ "$CAPTURE_ONLY" -eq 0 ]]; then
    echo "Launching ROM..."
    "$ADB" shell am start -S \
        -n "${PACKAGE}/${ACTIVITY}" \
        --es uri "$URI" >/dev/null

    sleep "$LAUNCH_WAIT"

    if [[ "$LAUNCH_ONLY" -eq 0 ]]; then
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
    else
        echo "Launch-only mode: ROM launched without input injection"
    fi
else
    echo "Capture-only mode: using current app state"
fi

if [[ -n "$WAIT_FOR_SCENE" ]]; then
    wait_for_scene "$WAIT_FOR_SCENE" "$WAIT_TIMEOUT_SEC" "$WAIT_INTERVAL_SEC"
fi

echo "Capturing screenshot..."
capture_top_to_file "$SCREENSHOT_OUT"

echo "$SCREENSHOT_OUT"

if [[ -n "$BOTTOM_SCREENSHOT_OUT" ]]; then
    REMOTE_BOTTOM_SCREENSHOT="/sdcard/Download/$(basename "$BOTTOM_SCREENSHOT_OUT")"
    "$ADB" shell screencap -d "$BOTTOM_DISPLAY_ID" -p "$REMOTE_BOTTOM_SCREENSHOT" >/dev/null 2>&1
    mkdir -p "$(dirname "$BOTTOM_SCREENSHOT_OUT")"
    "$ADB" pull "$REMOTE_BOTTOM_SCREENSHOT" "$BOTTOM_SCREENSHOT_OUT" >/dev/null
    echo "$BOTTOM_SCREENSHOT_OUT"
fi

if [[ "$SKIP_SCENE_CHECK" -eq 0 && -n "$EXPECT_SCENE" && -f "$SCENE_ANALYZER" ]]; then
    ANALYZER_ARGS=("$SCENE_ANALYZER" "$SCREENSHOT_OUT")
    ANALYZER_ARGS+=(--expect-scene "$EXPECT_SCENE")

    echo "Analyzing screenshot scene..."
    python3 "${ANALYZER_ARGS[@]}"
fi

if [[ "$SKIP_METRICS" -eq 0 ]]; then
    if ! "$ADB" shell command -v simpleperf >/dev/null 2>&1; then
        echo "Error: simpleperf was not found on the target device." >&2
        exit 1
    fi

    if [[ -z "$PERF_DURATION_SEC" ]]; then
        PERF_DURATION_SEC="$(python3 - <<PY
import math
print(max(1, math.ceil(($FPS_SAMPLE_COUNT * $FPS_INTERVAL_MS) / 1000.0)))
PY
)"
    fi

    SAMPLE_TOKEN="harness-$RANDOM-$(date -u +%Y%m%d%H%M%S)"
    FPS_LOG="/tmp/${SAMPLE_TOKEN}-fps.log"
    PERF_LOG="/tmp/${SAMPLE_TOKEN}-simpleperf.txt"

    echo "Collecting FPS and instruction metrics..."
    "$ADB" logcat -c
    (
        "$ADB" shell am broadcast \
            -a me.magnum.melonds.DEBUG_EMULATOR \
            -n "${PACKAGE}/${RECEIVER_CLASS}" \
            --ei fps_sample_count "$FPS_SAMPLE_COUNT" \
            --el fps_interval_ms "$FPS_INTERVAL_MS" \
            --es sample_token "$SAMPLE_TOKEN" >/dev/null
    ) &
    FPS_BROADCAST_PID=$!

    "$ADB" shell simpleperf stat \
        --app "$PACKAGE" \
        --duration "$PERF_DURATION_SEC" \
        -e "$PERF_EVENT" > "$PERF_LOG" 2>&1

    wait "$FPS_BROADCAST_PID"
    "$ADB" logcat -d -s EmulatorDebugReceiver:I > "$FPS_LOG"

    python3 - <<PY
import pathlib
import re
import sys

fps_log = pathlib.Path("$FPS_LOG").read_text()
perf_log = pathlib.Path("$PERF_LOG").read_text()
token = "$SAMPLE_TOKEN"

fps_match = None
for line in fps_log.splitlines():
    if token in line and "HARNESS_FPS" in line:
        fps_match = line
        break

if fps_match is None:
    print("Error: failed to parse HARNESS_FPS log line", file=sys.stderr)
    sys.exit(1)

avg_match = re.search(r"avg=([0-9.]+)", fps_match)
samples_match = re.search(r"samples=(\\[[^\\]]*\\])", fps_match)
instr_match = re.search(r"([0-9,]+)\\s+instructions\\b", perf_log)

if avg_match is None or samples_match is None:
    print("Error: failed to parse FPS samples from log line", file=sys.stderr)
    sys.exit(1)

if instr_match is None:
    print("Error: failed to parse instruction count from simpleperf output", file=sys.stderr)
    sys.exit(1)

avg_fps = float(avg_match.group(1))
samples = samples_match.group(1)
instructions = int(instr_match.group(1).replace(",", ""))

print(f"Average FPS: {avg_fps:.3f}")
print(f"FPS samples: {samples}")
print(f"CPU instructions: {instructions}")
PY
fi
