#!/usr/bin/env bash
# Reach a harness-validated Android scene, then run simpleperf and save stat/report artifacts.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

PACKAGE="${MELONDS_PACKAGE:-}"
ACTIVITY="${MELONDS_ACTIVITY:-me.magnum.melonds.ui.emulator.EmulatorActivity}"
URI=""
DURATION=10
FREQ=1000
OUT_DIR=""
EVENTS="${SIMPLEPERF_EVENTS:-task-clock,cpu-cycles,instructions}"
ADB="${ADB:-adb}"
RUN_RECORD=1
RUN_LABEL=""
REQUIRE_PROFILE_BUILD="${HARNESS_EXPECT_PROFILE:-any}"
LAUNCH_ONLY=0
declare -a HARNESS_ARGS=()

usage() {
    cat <<EOF
Usage: $0 --uri ROM_CONTENT_URI [options]

Options:
  --uri URI          Android content:// URI passed to the harness launcher.
  --package NAME    App package. Default: auto-detect installed melonDS variant
  --activity NAME   Activity class. Default: $ACTIVITY
  --duration SEC    simpleperf duration. Default: $DURATION
  --freq HZ         Sampling frequency for simpleperf record. Default: $FREQ
  --events LIST     Events for simpleperf stat. Default: $EVENTS
  --out-dir DIR     Output directory. Default: docs/baselines/android-simpleperf/<timestamp>
  --stat-only       Skip record/report and collect stat output only.
  --label TEXT      Label passed through to the harness metadata.
  --require-profile-build on|off|any
                    Require the harness to detect a matching LITEV_PROFILE build. Default: $REQUIRE_PROFILE_BUILD
  --launch-only     Use the harness launch-only flow instead of the default gameplay-driving benchmark flow.
  --harness-arg ARG Extra argument forwarded to run_android_harness.sh. Repeat as needed.

Environment:
  ADB               adb binary. Default: adb
  MELONDS_PACKAGE   Overrides default package.
  SIMPLEPERF_EVENTS Overrides default stat events.
EOF
    exit 2
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --uri) URI="$2"; shift 2 ;;
        --package) PACKAGE="$2"; shift 2 ;;
        --activity) ACTIVITY="$2"; shift 2 ;;
        --duration) DURATION="$2"; shift 2 ;;
        --freq) FREQ="$2"; shift 2 ;;
        --events) EVENTS="$2"; shift 2 ;;
        --out-dir) OUT_DIR="$2"; shift 2 ;;
        --stat-only) RUN_RECORD=0; shift ;;
        --label) RUN_LABEL="$2"; shift 2 ;;
        --require-profile-build) REQUIRE_PROFILE_BUILD="$2"; shift 2 ;;
        --launch-only) LAUNCH_ONLY=1; shift ;;
        --harness-arg) HARNESS_ARGS+=("$2"); shift 2 ;;
        -h|--help) usage ;;
        *) echo "Unknown argument: $1" >&2; usage ;;
    esac
done

if [[ -z "$URI" ]]; then
    echo "Error: --uri is required." >&2
    usage
fi

if [[ -z "$OUT_DIR" ]]; then
    OUT_DIR="$REPO_ROOT/docs/baselines/android-simpleperf/$(date -u +%Y%m%dT%H%M%SZ)"
fi

mkdir -p "$OUT_DIR"

HARNESS_SCRIPT="$REPO_ROOT/tools/bench/run_android_harness.sh"
HARNESS_LOG="$OUT_DIR/harness.txt"
SCREENSHOT_OUT="$OUT_DIR/top.png"

if ! "$ADB" get-state >/dev/null 2>&1; then
    echo "Error: no Android device is connected or authorized for adb." >&2
    "$ADB" devices -l >&2 || true
    exit 1
fi

if ! "$ADB" shell command -v simpleperf >/dev/null 2>&1; then
    echo "Error: simpleperf was not found on the target device." >&2
    exit 1
fi

if [[ ! -x "$HARNESS_SCRIPT" ]]; then
    echo "Error: harness script '$HARNESS_SCRIPT' is not executable." >&2
    exit 1
fi

case "$REQUIRE_PROFILE_BUILD" in
    on|off|any) ;;
    *)
        echo "Error: --require-profile-build must be one of: on, off, any." >&2
        exit 1
        ;;
esac

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
        if grep -q "^package:${candidate}$" <<<"$installed"; then
            echo "$candidate"
            return 0
        fi
    done

    return 1
}

if [[ -z "$PACKAGE" ]]; then
    if ! PACKAGE="$(detect_package)"; then
        echo "Error: no supported melonDS Android package is installed." >&2
        "$ADB" shell pm list packages | grep 'me.magnum.melonds' >&2 || true
        exit 1
    fi
fi

DEVICE_ID="$("$ADB" shell getprop ro.serialno | tr -d '\r')"
MODEL="$("$ADB" shell getprop ro.product.model | tr -d '\r')"
SDK="$("$ADB" shell getprop ro.build.version.sdk | tr -d '\r')"
DEVICE_DATA="/data/local/tmp/${PACKAGE}.perf.data"

HARNESS_CMD=(
    "$HARNESS_SCRIPT"
    --uri "$URI"
    --skip-metrics
    --screenshot "$SCREENSHOT_OUT"
    --require-profile-build "$REQUIRE_PROFILE_BUILD"
    --package "$PACKAGE"
    --activity "$ACTIVITY"
)

if [[ -n "$RUN_LABEL" ]]; then
    HARNESS_CMD+=(--label "$RUN_LABEL")
fi

if [[ "$LAUNCH_ONLY" -eq 1 ]]; then
    HARNESS_CMD+=(--launch-only)
fi

if [[ "${#HARNESS_ARGS[@]}" -gt 0 ]]; then
    HARNESS_CMD+=("${HARNESS_ARGS[@]}")
fi

{
    printf 'Harness command:'
    printf ' %q' "${HARNESS_CMD[@]}"
    printf '\n'
} > "$HARNESS_LOG"

"${HARNESS_CMD[@]}" >> "$HARNESS_LOG" 2>&1

PID="$("$ADB" shell pidof "$PACKAGE" | tr -d '\r' || true)"
if [[ -z "$PID" ]]; then
    "$ADB" logcat -d -v time > "$OUT_DIR/logcat-failed.txt"
    echo "Error: $PACKAGE is not running after launch. See $OUT_DIR/logcat-failed.txt" >&2
    exit 1
fi

cat > "$OUT_DIR/metadata.json" <<EOF
{
  "timestamp": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "device_id": "$DEVICE_ID",
  "model": "$MODEL",
  "sdk": "$SDK",
  "package": "$PACKAGE",
  "activity": "$ACTIVITY",
  "pid": "$PID",
  "duration_sec": $DURATION,
  "events": "$EVENTS",
  "uri": "$URI",
  "launch_only": $LAUNCH_ONLY,
  "require_profile_build": "$REQUIRE_PROFILE_BUILD",
  "label": "${RUN_LABEL}",
  "harness_log": "$HARNESS_LOG",
  "screenshot": "$SCREENSHOT_OUT"
}
EOF

"$ADB" shell simpleperf stat \
    --app "$PACKAGE" \
    --duration "$DURATION" \
    --per-thread \
    -e "$EVENTS" \
    > "$OUT_DIR/stat.txt" 2>&1

if [[ "$RUN_RECORD" -eq 1 ]]; then
    "$ADB" shell rm -f "$DEVICE_DATA"
    "$ADB" shell simpleperf record \
        --app "$PACKAGE" \
        --duration "$DURATION" \
        -f "$FREQ" \
        -o "$DEVICE_DATA" \
        > "$OUT_DIR/record.txt" 2>&1
    "$ADB" shell simpleperf report \
        -i "$DEVICE_DATA" \
        --sort comm,dso,symbol \
        --percent-limit 0.5 \
        > "$OUT_DIR/report.txt" 2>&1
    "$ADB" pull "$DEVICE_DATA" "$OUT_DIR/perf.data" >/dev/null
fi

"$ADB" logcat -d -v time > "$OUT_DIR/logcat.txt"

echo "Android simpleperf artifacts written to: $OUT_DIR"
