#!/usr/bin/env bash
# Launch a ROM on Android, inject a debug harness input sequence, and pull a screenshot.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

ADB="${ADB:-adb}"
PACKAGE="${MELONDS_PACKAGE:-}"
ACTIVITY="${MELONDS_ACTIVITY:-me.magnum.melonds.ui.emulator.EmulatorActivity}"
RECEIVER_CLASS="${MELONDS_DEBUG_RECEIVER:-me.magnum.melonds.debug.EmulatorDebugReceiver}"
SCENE_ANALYZER="$SCRIPT_DIR/analyze_harness_scene.py"
TOP_DISPLAY_ID="${MELONDS_TOP_DISPLAY_ID:-1}"
BOTTOM_DISPLAY_ID="${MELONDS_BOTTOM_DISPLAY_ID:-0}"
DEFAULT_BENCHMARK_SCENE="rendering"
DEFAULT_LAUNCH_ONLY_SCENE="menu"
DEFAULT_PRE_SEQUENCE_SCENE="menu"
DEFAULT_BENCHMARK_SEQUENCE="A,A,A,A,A,A,A,A,A,A,A,A,A,A,A,A,A,A,A,A,A,A,A,A,A,A,A,A,A,A"
DEFAULT_SECOND_BENCHMARK_SEQUENCE="A,A,A,A,A,A,A,A,A,A,A,A,A,A,A,A,A,A,A,A"
DEFAULT_PRE_SECOND_SEQUENCE_SCENE="rendering"

URI=""
RUN_LABEL="${HARNESS_LABEL:-}"
SEQUENCE=""
SECOND_SEQUENCE=""
LOAD_STATE_URI=""
SAVE_STATE_URI=""
PRESS_BUTTON=""
PRESS_COUNT=0
SECOND_PRESS_BUTTON=""
SECOND_PRESS_COUNT=0
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
CONTINUOUS_LOG=""
CONTINUOUS_DURATION_SEC=0
CONTINUOUS_FPS_INTERVAL_MS=0
BOTTOM_SCREENSHOT_OUT=""
CAPTURE_ONLY=0
LAUNCH_ONLY=0
WAIT_FOR_SCENE=""
WAIT_BEFORE_SEQUENCE=""
WAIT_BEFORE_SECOND_SEQUENCE=""
WAIT_TIMEOUT_SEC=60
WAIT_INTERVAL_SEC=2
REQUIRE_PROFILE_BUILD="${HARNESS_EXPECT_PROFILE:-any}"
PROFILE_BUILD="unknown"

git_stamp() {
    local repo="$1"
    if git -C "$repo" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        git -C "$repo" describe --always --dirty 2>/dev/null
    else
        echo "unknown"
    fi
}

append_metric_stamp() {
    local file="$1"
    local stamp_time="$2"
    local sample_token="$3"
    local main_repo_stamp="$4"
    local core_repo_stamp="$5"

    {
        echo "# harness_label=${RUN_LABEL:-unlabeled}"
        echo "# harness_utc=${stamp_time}"
        echo "# sample_token=${sample_token}"
        echo "# repo_main=${main_repo_stamp}"
        echo "# repo_core=${core_repo_stamp}"
        echo "# package=${PACKAGE}"
        echo "# activity=${ACTIVITY}"
        echo "# expect_scene=${EXPECT_SCENE:-none}"
        echo "# wait_for_scene=${WAIT_FOR_SCENE:-none}"
        echo "# litev_profile_build=${PROFILE_BUILD}"
    } >> "$file"
}

usage() {
    cat <<EOF
Usage: $0 [--uri ROM_CONTENT_URI] [sequence options] [options]

Sequence options:
  --sequence CSV       Direct harness sequence, e.g. 'A,A,DOWN,A,SLEEP:2000'
  --second-sequence CSV
                      Second direct harness sequence, run after an optional second scene gate
  --press-a COUNT      Convenience shorthand for COUNT presses of A
  --press-a-second COUNT
                      Convenience shorthand for COUNT presses of A in the second phase
  --press BUTTON COUNT Convenience shorthand for COUNT presses of BUTTON
  --press-second BUTTON COUNT
                      Convenience shorthand for COUNT presses of BUTTON in the second phase

Other options:
  --load-state-uri URI Savestate content:// URI to load through the harness before inputs
  --save-state-uri URI Savestate URI or path to save after the final scene gate
  --label TEXT        Stamp local metric logs with a human label for this run
  --press-ms MS        Button hold duration per command. Default: $PRESS_MS
  --gap-ms MS          Delay between commands. Default: $GAP_MS
  --launch-wait SEC    Seconds to wait after ROM launch. Default: $LAUNCH_WAIT
  --post-wait SEC      Seconds to wait after harness injection. Default: $POST_WAIT
  --fast-forward BOOL  true/false. Toggle fast-forward before the input sequence
  --screenshot PATH    Local top-screen screenshot output path. Default: /tmp/<timestamp>-top.png
  --bottom-screenshot PATH
                      Optional local bottom-screen screenshot output path
  --expect-scene NAME  Expected final scene: menu | gameplay_loaded | rendering | blackscreen | whiteframe
  --skip-scene-check   Skip baseline scene analysis after the screenshot
  --skip-metrics       Skip FPS sampling and simpleperf instruction counting
  --fps-samples N      Number of FPS samples to average. Default: $FPS_SAMPLE_COUNT
  --fps-interval-ms MS Delay between FPS samples. Default: $FPS_INTERVAL_MS
  --perf-duration SEC  simpleperf duration in seconds. Default: ceil(samples * interval)
  --continuous-log PATH
                      Capture a timestamped time-series log containing HARNESS_FPS_SAMPLE and LITEV_PROFILE lines
  --continuous-duration SEC
                      Duration for --continuous-log sampling after the final scene gate. Default: 0 (disabled)
  --continuous-fps-interval-ms MS
                      FPS time-series sample interval for --continuous-log. Default: --fps-interval-ms
  --wait-for-scene NAME
                      Wait until the top screen matches: menu | gameplay_loaded | rendering | blackscreen | whiteframe
  --wait-before-sequence NAME
                      Wait for a scene before injecting the input sequence
  --wait-before-second-sequence NAME
                      Wait for a scene before injecting the second input sequence
  --wait-timeout SEC   Timeout for --wait-for-scene. Default: $WAIT_TIMEOUT_SEC
  --wait-interval SEC  Poll interval for --wait-for-scene. Default: $WAIT_INTERVAL_SEC
  --top-display-id ID  Physical display ID for the DS top screen. Default: $TOP_DISPLAY_ID
  --bottom-display-id ID
                      Physical display ID for the DS bottom screen. Default: $BOTTOM_DISPLAY_ID
  --capture-only       Do not launch or inject inputs. Capture/measure the current app state only
  --launch-only        Launch the ROM and stop there. No input injection
  --package NAME       App package. Default: auto-detect installed melonDS variant
  --activity NAME      Activity class. Default: $ACTIVITY
  --require-profile-build on|off|any
                      Require installed app to report a matching LITEV_PROFILE build mode
  -h, --help           Show this help

Environment:
  ADB                  adb binary. Default: adb
  MELONDS_PACKAGE      Overrides auto-detected package
  MELONDS_ACTIVITY     Overrides default activity
  MELONDS_DEBUG_RECEIVER Overrides default receiver class
  MELONDS_TOP_DISPLAY_ID Overrides default top display id
  MELONDS_BOTTOM_DISPLAY_ID Overrides default bottom display id
  HARNESS_LABEL        Default value for --label
  HARNESS_EXPECT_PROFILE Default value for --require-profile-build

Examples:
  $0 --uri 'content://...' --press-a 30
  $0 --uri 'content://...' --sequence 'A,A,DOWN,A,SLEEP:3000,A'
  $0 --uri 'content://...' --press-a 60 --expect-scene rendering
  $0 --capture-only --screenshot /tmp/current-top.png --bottom-screenshot /tmp/current-bottom.png
  $0 --uri 'content://...' --launch-only
  $0 --uri 'content://...' --launch-only --wait-for-scene menu

Benchmark defaults:
  - launched benchmark runs default to waiting for menu, pressing A 30 times, waiting for rendering, pressing A 20 times, then waiting for rendering again
  - launch-only metrics runs default to waiting for menu
  - when a wait scene is active, screenshot validation defaults to that same scene
EOF
    exit 2
}

detect_package() {
    local candidates=(
        "me.magnum.melonds.dev"
        "me.magnum.melonds.nightly.dev"
        "me.magnum.melonds.nightly"
        "me.magnum.melonds"
    )
    local installed

    installed="$("$ADB" shell pm list packages 2>/dev/null || true)"
    for candidate in "${candidates[@]}"; do
        if grep -q "^package:${candidate}\$" <<<"$installed"; then
            echo "$candidate"
            return 0
        fi
    done

    return 1
}

resolve_component() {
    local package_name="$1"
    local component_class="$2"
    local component_kind="$3"
    local dump

    dump="$("$ADB" shell dumpsys package "$package_name" 2>/dev/null || true)"
    if grep -Fq "$component_class" <<<"$dump"; then
        return 0
    fi

    echo "Error: ${component_kind} '${component_class}' was not found in installed package '${package_name}'." >&2
    exit 1
}

probe_receiver() {
    if ! "$ADB" shell am broadcast \
        -a me.magnum.melonds.DEBUG_EMULATOR \
        -n "${PACKAGE}/${RECEIVER_CLASS}" \
        --ez cancel_sequence true >/dev/null 2>&1; then
        echo "Error: receiver '${RECEIVER_CLASS}' did not accept an explicit harness broadcast in package '${PACKAGE}'." >&2
        exit 1
    fi
}

preflight_package() {
    if [[ -z "$PACKAGE" ]]; then
        if ! PACKAGE="$(detect_package)"; then
            echo "Error: no supported melonDS Android package is installed." >&2
            "$ADB" shell pm list packages | grep 'me.magnum.melonds' >&2 || true
            exit 1
        fi
        echo "Auto-detected package: $PACKAGE"
    fi

    if ! "$ADB" shell pm list packages | grep -q "^package:${PACKAGE}\$"; then
        echo "Error: package '$PACKAGE' is not installed on the device." >&2
        "$ADB" shell pm list packages | grep 'me.magnum.melonds' >&2 || true
        exit 1
    fi

    if ! "$ADB" shell cmd package resolve-activity --brief -c android.intent.category.LAUNCHER "$PACKAGE" >/dev/null 2>&1; then
        echo "Error: package '$PACKAGE' does not resolve a launcher activity." >&2
        exit 1
    fi

    resolve_component "$PACKAGE" "$ACTIVITY" "activity"
    probe_receiver
}

query_profile_build() {
    local logs
    local line

    "$ADB" logcat -c
    "$ADB" shell am broadcast \
        -a me.magnum.melonds.DEBUG_EMULATOR \
        -n "${PACKAGE}/${RECEIVER_CLASS}" \
        --ez query_profile_build true >/dev/null 2>&1 || true
    sleep 1

    logs="$("$ADB" logcat -d -s EmulatorDebugReceiver:I 2>/dev/null || true)"
    line="$(grep 'HARNESS_PROFILE_BUILD' <<<"$logs" | tail -n 1 || true)"

    if grep -q 'enabled=true' <<<"$line"; then
        PROFILE_BUILD="on"
    elif grep -q 'enabled=false' <<<"$line"; then
        PROFILE_BUILD="off"
    else
        PROFILE_BUILD="unknown"
    fi

    echo "Detected LITEV_PROFILE build: $PROFILE_BUILD"

    if [[ "$REQUIRE_PROFILE_BUILD" != "any" && "$PROFILE_BUILD" != "$REQUIRE_PROFILE_BUILD" ]]; then
        echo "Error: required LITEV_PROFILE build '$REQUIRE_PROFILE_BUILD' but detected '$PROFILE_BUILD'." >&2
        exit 1
    fi
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --uri) URI="$2"; shift 2 ;;
        --sequence) SEQUENCE="$2"; shift 2 ;;
        --second-sequence) SECOND_SEQUENCE="$2"; shift 2 ;;
        --press-a) PRESS_BUTTON="A"; PRESS_COUNT="$2"; shift 2 ;;
        --press-a-second) SECOND_PRESS_BUTTON="A"; SECOND_PRESS_COUNT="$2"; shift 2 ;;
        --press) PRESS_BUTTON="$2"; PRESS_COUNT="$3"; shift 3 ;;
        --press-second) SECOND_PRESS_BUTTON="$2"; SECOND_PRESS_COUNT="$3"; shift 3 ;;
        --load-state-uri) LOAD_STATE_URI="$2"; shift 2 ;;
        --save-state-uri) SAVE_STATE_URI="$2"; shift 2 ;;
        --label) RUN_LABEL="$2"; shift 2 ;;
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
        --continuous-log) CONTINUOUS_LOG="$2"; shift 2 ;;
        --continuous-duration) CONTINUOUS_DURATION_SEC="$2"; shift 2 ;;
        --continuous-fps-interval-ms) CONTINUOUS_FPS_INTERVAL_MS="$2"; shift 2 ;;
        --wait-for-scene) WAIT_FOR_SCENE="$2"; shift 2 ;;
        --wait-before-sequence) WAIT_BEFORE_SEQUENCE="$2"; shift 2 ;;
        --wait-before-second-sequence) WAIT_BEFORE_SECOND_SEQUENCE="$2"; shift 2 ;;
        --wait-timeout) WAIT_TIMEOUT_SEC="$2"; shift 2 ;;
        --wait-interval) WAIT_INTERVAL_SEC="$2"; shift 2 ;;
        --top-display-id) TOP_DISPLAY_ID="$2"; shift 2 ;;
        --bottom-display-id) BOTTOM_DISPLAY_ID="$2"; shift 2 ;;
        --capture-only) CAPTURE_ONLY=1; shift 1 ;;
        --launch-only) LAUNCH_ONLY=1; shift 1 ;;
        --package) PACKAGE="$2"; shift 2 ;;
        --activity) ACTIVITY="$2"; shift 2 ;;
        --require-profile-build) REQUIRE_PROFILE_BUILD="$2"; shift 2 ;;
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

if [[ -n "$SECOND_PRESS_BUTTON" ]]; then
    if ! [[ "$SECOND_PRESS_COUNT" =~ ^[0-9]+$ ]] || [[ "$SECOND_PRESS_COUNT" -le 0 ]]; then
        echo "Error: --press-second count must be a positive integer." >&2
        exit 1
    fi

    generated=""
    for ((i = 0; i < SECOND_PRESS_COUNT; i++)); do
        if [[ -n "$generated" ]]; then
            generated+=","
        fi
        generated+="$SECOND_PRESS_BUTTON"
    done
    SECOND_SEQUENCE="$generated"
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

if ! [[ "$CONTINUOUS_DURATION_SEC" =~ ^[0-9]+$ ]]; then
    echo "Error: --continuous-duration must be a non-negative integer." >&2
    exit 1
fi

if [[ "$CONTINUOUS_FPS_INTERVAL_MS" -eq 0 ]]; then
    CONTINUOUS_FPS_INTERVAL_MS="$FPS_INTERVAL_MS"
fi

if ! [[ "$CONTINUOUS_FPS_INTERVAL_MS" =~ ^[0-9]+$ ]] || [[ "$CONTINUOUS_FPS_INTERVAL_MS" -le 0 ]]; then
    echo "Error: --continuous-fps-interval-ms must be a positive integer." >&2
    exit 1
fi

if [[ -n "$CONTINUOUS_LOG" && "$CONTINUOUS_DURATION_SEC" -le 0 ]]; then
    echo "Error: --continuous-log requires --continuous-duration SEC." >&2
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

case "$REQUIRE_PROFILE_BUILD" in
    on|off|any) ;;
    *)
        echo "Error: --require-profile-build must be one of: on, off, any." >&2
        exit 1
        ;;
esac

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

if [[ "$CAPTURE_ONLY" -eq 0 && "$LAUNCH_ONLY" -eq 0 && -n "$SEQUENCE" && -z "$WAIT_BEFORE_SEQUENCE" ]]; then
    WAIT_BEFORE_SEQUENCE="$DEFAULT_PRE_SEQUENCE_SCENE"
fi

if [[ "$CAPTURE_ONLY" -eq 0 && "$LAUNCH_ONLY" -eq 0 && -z "$SECOND_SEQUENCE" ]]; then
    SECOND_SEQUENCE="$DEFAULT_SECOND_BENCHMARK_SEQUENCE"
fi

if [[ "$CAPTURE_ONLY" -eq 0 && "$LAUNCH_ONLY" -eq 0 && -n "$SECOND_SEQUENCE" && -z "$WAIT_BEFORE_SECOND_SEQUENCE" ]]; then
    WAIT_BEFORE_SECOND_SEQUENCE="$DEFAULT_PRE_SECOND_SEQUENCE_SCENE"
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

collect_continuous_log() {
    local local_out="$1"
    local duration_sec="$2"
    local interval_ms="$3"
    local sample_count
    local sample_token
    local stamp_time
    local main_repo_stamp
    local core_repo_stamp
    local fps_pid
    local logcat_pid

    sample_count="$(python3 - <<PY
import math
print(max(1, math.ceil(($duration_sec * 1000.0) / $interval_ms)))
PY
)"
    sample_token="continuous-$RANDOM-$(date -u +%Y%m%d%H%M%S)"
    stamp_time="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    main_repo_stamp="$(git_stamp "$REPO_ROOT")"
    core_repo_stamp="$(git_stamp "$REPO_ROOT/melonDS-android-lib")"

    mkdir -p "$(dirname "$local_out")"
    : > "$local_out"
    append_metric_stamp "$local_out" "$stamp_time" "$sample_token" "$main_repo_stamp" "$core_repo_stamp"
    {
        echo "# continuous_duration_sec=${duration_sec}"
        echo "# continuous_fps_interval_ms=${interval_ms}"
        echo "# continuous_fps_sample_count=${sample_count}"
        echo "# log_tags=EmulatorDebugReceiver:I,melonDS:I"
    } >> "$local_out"

    echo "Collecting continuous FPS/profiler log: $local_out"
    "$ADB" logcat -c
    "$ADB" logcat -v epoch EmulatorDebugReceiver:I melonDS:I '*:S' >> "$local_out" &
    logcat_pid=$!

    "$ADB" shell am broadcast \
        -a me.magnum.melonds.DEBUG_EMULATOR \
        -n "${PACKAGE}/${RECEIVER_CLASS}" \
        --ei fps_sample_count "$sample_count" \
        --el fps_interval_ms "$interval_ms" \
        --es sample_token "$sample_token" >/dev/null &
    fps_pid=$!

    wait "$fps_pid"
    sleep 1
    kill "$logcat_pid" >/dev/null 2>&1 || true
    wait "$logcat_pid" >/dev/null 2>&1 || true

    echo "$local_out"
}

if ! "$ADB" get-state >/dev/null 2>&1; then
    echo "Error: no Android device is connected or authorized for adb." >&2
    "$ADB" devices -l >&2 || true
    exit 1
fi

preflight_package
query_profile_build

if [[ "$CAPTURE_ONLY" -eq 0 ]]; then
    echo "Launching ROM..."
    "$ADB" shell am start -S \
        -n "${PACKAGE}/${ACTIVITY}" \
        --es uri "$URI" >/dev/null

    sleep "$LAUNCH_WAIT"

    if [[ "$LAUNCH_ONLY" -eq 0 ]]; then
        if [[ -n "$WAIT_BEFORE_SEQUENCE" ]]; then
            wait_for_scene "$WAIT_BEFORE_SEQUENCE" "$WAIT_TIMEOUT_SEC" "$WAIT_INTERVAL_SEC"
        fi

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

        if [[ -n "$SECOND_SEQUENCE" ]]; then
            if [[ -n "$WAIT_BEFORE_SECOND_SEQUENCE" ]]; then
                wait_for_scene "$WAIT_BEFORE_SECOND_SEQUENCE" "$WAIT_TIMEOUT_SEC" "$WAIT_INTERVAL_SEC"
            fi

            SECOND_BROADCAST_ARGS=(
                shell am broadcast
                -a me.magnum.melonds.DEBUG_EMULATOR
                -n "${PACKAGE}/${RECEIVER_CLASS}"
                --es sequence "$SECOND_SEQUENCE"
                --el press_ms "$PRESS_MS"
                --el gap_ms "$GAP_MS"
            )

            echo "Injecting second harness sequence..."
            "$ADB" "${SECOND_BROADCAST_ARGS[@]}" >/dev/null

            sleep "$POST_WAIT"
        fi
    else
        echo "Launch-only mode: ROM launched without input injection"
    fi
else
    echo "Capture-only mode: using current app state"
fi

if [[ -n "$WAIT_FOR_SCENE" ]]; then
    wait_for_scene "$WAIT_FOR_SCENE" "$WAIT_TIMEOUT_SEC" "$WAIT_INTERVAL_SEC"
fi

if [[ -n "$SAVE_STATE_URI" ]]; then
    echo "Saving state..."
    "$ADB" shell am broadcast \
        -a me.magnum.melonds.DEBUG_EMULATOR \
        -n "${PACKAGE}/${RECEIVER_CLASS}" \
        --es save_state_uri "$SAVE_STATE_URI" >/dev/null
    sleep 2
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

if [[ -n "$CONTINUOUS_LOG" ]]; then
    collect_continuous_log "$CONTINUOUS_LOG" "$CONTINUOUS_DURATION_SEC" "$CONTINUOUS_FPS_INTERVAL_MS"
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
    STAMP_TIME="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    MAIN_REPO_STAMP="$(git_stamp "$REPO_ROOT")"
    CORE_REPO_STAMP="$(git_stamp "$REPO_ROOT/melonDS-android-lib")"

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

    : > "$PERF_LOG"
    append_metric_stamp "$PERF_LOG" "$STAMP_TIME" "$SAMPLE_TOKEN" "$MAIN_REPO_STAMP" "$CORE_REPO_STAMP"
    "$ADB" shell simpleperf stat \
        --app "$PACKAGE" \
        --duration "$PERF_DURATION_SEC" \
        -e "$PERF_EVENT" >> "$PERF_LOG" 2>&1

    wait "$FPS_BROADCAST_PID"
    : > "$FPS_LOG"
    append_metric_stamp "$FPS_LOG" "$STAMP_TIME" "$SAMPLE_TOKEN" "$MAIN_REPO_STAMP" "$CORE_REPO_STAMP"
    "$ADB" logcat -d -s EmulatorDebugReceiver:I >> "$FPS_LOG"

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
