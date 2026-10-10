#!/usr/bin/env bash
# One command from the device's bridge logs to bridge_field_map.md.
#
# Modes:
#   run_pipeline.sh             decode the logs in LOG_DIR into this folder's outputs
#   run_pipeline.sh --pull P    pull the device log for phase P into LOG_DIR, then decode
#   run_pipeline.sh --capture   build the hook and probes, run every phase on the device,
#                               decode the live logs in a temporary copy, and check their
#                               calls against the committed CSVs (committed files are not written)
#
# Phases: bringup (vksubmit), fill (vksubmit fill), dispatch (dispatch).
# Inputs:
#   PVR_VENDOR   srv_um.dis and syms.txt from the device's libsrv_um.so (not in the repo).
#   LOG_DIR      folder with bridge_<phase>.log files for the default and --pull modes.
# The three existing layout CSVs beside this script are the reference call/size structure.
# Every mode checks each phase against them before publishing regenerated outputs.
#   ADB_SERIAL   device serial; needed when several devices are attached.
#   NDK_CLANG    NDK aarch64-linux-android33-clang; needed for --capture.
set -o pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
LOG_DIR="${LOG_DIR:-$HERE}"
export PVR_VENDOR="${PVR_VENDOR:-$HERE/vendor}"
PY="${PYTHON:-python}"
PHASES="bringup fill dispatch"
DEV="/data/local/tmp"

fail() {
    echo "run_pipeline: $*" >&2
    exit 1
}

# Git Bash needs native paths for the Windows adb, clang and python binaries.
native() {
    if command -v cygpath >/dev/null 2>&1; then cygpath -w "$1"; else printf '%s' "$1"; fi
}

adb_() {
    MSYS_NO_PATHCONV=1 adb ${ADB_SERIAL:+-s "$ADB_SERIAL"} "$@"
}

check_vendor() {
    if [ ! -f "$PVR_VENDOR/srv_um.dis" ] || [ ! -f "$PVR_VENDOR/syms.txt" ]; then
        fail "PVR_VENDOR=$PVR_VENDOR has no srv_um.dis / syms.txt"
    fi
}

pull_phase() {
    case " $PHASES " in
        *" $1 "*) ;;
        *) fail "--pull needs one of: $PHASES" ;;
    esac
    command -v adb >/dev/null 2>&1 || fail "adb is not on PATH"
    mkdir -p "$LOG_DIR" || fail "cannot create LOG_DIR=$LOG_DIR"
    adb_ pull "$DEV/bridge.log" "$(native "$LOG_DIR/bridge_$1.log")" >/dev/null \
        || fail "adb pull of $DEV/bridge.log failed: check the device is connected and authorized, that the probe ran with the hook, and set ADB_SERIAL if several devices are attached"
    echo "pulled $LOG_DIR/bridge_$1.log"
}

# decode_into LOG_DIR DIR: stage all six outputs and publish only after every check passes.
# The subshell owns its temporary directory and cleanup; pipefail preserves the decoder's status.
decode_into() (
    local logs="$1" dir="$2" phase work staged csv
    for phase in $PHASES; do
        [ -f "$logs/bridge_$phase.log" ] || fail "missing $logs/bridge_$phase.log (capture it with --pull $phase or --capture)"
    done
    work="$(mktemp -d)" || fail "cannot create decode staging directory"
    trap 'rm -rf "$work"' EXIT
    mkdir "$work/hooks" || fail "cannot create staging hooks directory"
    cp "$dir/extract_bridge.py" "$dir/decode_layouts.py" "$work/" \
        && cp "$dir/hooks/bridge_abi.h" "$work/hooks/" || fail "cannot stage decoder and shared ABI"
    staged="$(native "$work")"
    logs="$(native "$logs")"
    "$PY" "$staged/extract_bridge.py" || return 1
    "$PY" "$staged/decode_layouts.py" --from-log "$logs/bridge_bringup.log" "$staged/bringup_layouts.csv" trace || return 1
    "$PY" "$staged/decode_layouts.py" --from-log "$logs/bridge_fill.log" "$staged/submit_layouts.csv" sorted || return 1
    "$PY" "$staged/decode_layouts.py" --from-log "$logs/bridge_dispatch.log" "$staged/dispatch_layouts.csv" sorted || return 1
    for phase in $PHASES; do
        case $phase in
            bringup) csv=bringup_layouts.csv ;;
            fill) csv=submit_layouts.csv ;;
            dispatch) csv=dispatch_layouts.csv ;;
        esac
        [ -s "$dir/$csv" ] || fail "$phase: missing reference $dir/$csv"
        layout_key "$dir/$csv" > "$work/reference.keys" || fail "$phase: cannot read reference $csv"
        [ -s "$work/reference.keys" ] || fail "$phase: empty reference structure in $csv"
        layout_key "$work/$csv" > "$work/captured.keys" || fail "$phase: cannot read captured $csv"
        if ! diff -u "$work/reference.keys" "$work/captured.keys" > "$work/structure.diff"; then
            echo "run_pipeline: $phase structure differs from reference $csv (bridge, func, symbol, in size, out size):" >&2
            cat "$work/structure.diff" >&2
            return 1
        fi
    done
    "$PY" "$staged/decode_layouts.py" | tee "$work/decode_out.txt" || return 1
    for csv in bridge_table.csv bringup_layouts.csv submit_layouts.csv dispatch_layouts.csv decode_out.txt bridge_field_map.md; do
        cp "$work/$csv" "$dir/$csv" || fail "cannot publish $csv"
    done
)

# Row keys for the structural check: bridge, func, symbol, in_size, out_size (no hex, which holds run-specific pointers).
layout_key() {
    tr -d '\r' < "$1" | awk -F, 'NR > 1 { print $1 "," $2 "," $3 "," $6 "," $8 }' | sort
}

capture() {
    [ -n "${NDK_CLANG:-}" ] || fail "--capture needs NDK_CLANG set to the NDK aarch64-linux-android33-clang"
    command -v adb >/dev/null 2>&1 || fail "adb is not on PATH"
    local build live work phase probe args out st rc loss_marker loss_status
    loss_marker="$(sed -n 's/^#define BRIDGE_CAPTURE_LOSS_MARKER "\([^"]*\)".*/\1/p' "$HERE/hooks/bridge_abi.h")"
    [ -n "$loss_marker" ] || fail "shared ABI has no capture-loss marker"
    build="$(mktemp -d)"
    live="$(mktemp -d)"
    work="$(mktemp -d)"

    "$NDK_CLANG" -shared -fPIC -Wall -o "$(native "$build/ioctl_buf.so")" "$(native "$HERE/hooks/ioctl_buf.c")" -ldl \
        || fail "hook build failed"
    "$NDK_CLANG" -fPIE -pie -Wall -o "$(native "$build/vksubmit")" "$(native "$HERE/probes/vksubmit.c")" -lvulkan \
        || fail "vksubmit build failed"
    "$NDK_CLANG" -fPIE -pie -Wall -o "$(native "$build/dispatch")" "$(native "$HERE/probes/dispatch.c")" -lvulkan \
        || fail "dispatch build failed"
    adb_ push "$(native "$build/ioctl_buf.so")" "$(native "$build/vksubmit")" "$(native "$build/dispatch")" "$DEV/" >/dev/null \
        || fail "adb push of the hook and probes failed: check the device is connected and authorized"
    adb_ shell "chmod 755 $DEV/vksubmit $DEV/dispatch" || fail "chmod of the probes on the device failed"

    for phase in $PHASES; do
        case $phase in
            bringup) probe=vksubmit; args="" ;;
            fill) probe=vksubmit; args="fill" ;;
            dispatch) probe=dispatch; args="" ;;
        esac
        out="$(adb_ shell "rm -f $DEV/bridge.log; cd $DEV && LD_PRELOAD=$DEV/ioctl_buf.so timeout 60 ./$probe $args; echo rc=\$?" 2>&1 | tr -d '\r')"
        st=$?
        rc="$(printf '%s\n' "$out" | sed -n 's/^rc=//p' | tail -1)"
        # Stage the probe output so the loss check reads a file, never a pipeline whose status
        # could come from another member. grep status 0 means the hook reported loss, 1 that it
        # did not, and anything else that the check itself failed: an untrusted check must fail
        # the phase, never pass it as a clean run.
        printf '%s\n' "$out" > "$work/probe_$phase.out" \
            || fail "$phase cannot stage the probe output for the capture-loss check"
        grep -F -- "$loss_marker" "$work/probe_$phase.out" > "$work/loss_$phase.txt"
        loss_status=$?
        [ "$loss_status" -ne 0 ] \
            || fail "$phase hook reported capture loss: $(tr '\n' ' ' < "$work/loss_$phase.txt")"
        [ "$loss_status" -eq 1 ] \
            || fail "$phase cannot check the probe output for capture loss (grep status $loss_status): refusing to report an unchecked run as clean"
        [ "$st" -eq 0 ] && [ "$rc" = 0 ] \
            || fail "$phase probe failed on the device (adb status $st, probe rc ${rc:-missing}): $(printf '%s\n' "$out" | tail -3 | tr '\n' ' ')"
        adb_ pull "$DEV/bridge.log" "$(native "$live/bridge_$phase.log")" >/dev/null \
            || fail "adb pull of $DEV/bridge.log failed after the $phase probe"
        [ -s "$live/bridge_$phase.log" ] || fail "$phase log is empty: the hook did not load"
        echo "$phase: probe rc=0, log $(wc -l < "$live/bridge_$phase.log") lines"
    done

    cp -r "$HERE/." "$work/" || fail "cannot copy the pipeline into $work"
    decode_into "$live" "$work" || fail "decode of the live logs failed (work copy: $work)"

    echo "live logs: $live"
    echo "live decode: $work"
    rm -rf "$build"
    return 0
}

case "${1:-}" in
    "") [ "$#" -eq 0 ] || fail "usage: run_pipeline.sh [--pull PHASE | --capture]" ;;
    --pull) [ "$#" -eq 2 ] || fail "usage: run_pipeline.sh --pull PHASE" ;;
    --capture) [ "$#" -eq 1 ] || fail "usage: run_pipeline.sh --capture" ;;
    *) fail "usage: run_pipeline.sh [--pull PHASE | --capture]" ;;
esac

if [ "${1:-}" = "--capture" ]; then
    check_vendor
    capture
    exit $?
fi

if [ "${1:-}" = "--pull" ]; then
    pull_phase "${2:-}"
fi

check_vendor
decode_into "$LOG_DIR" "$HERE"
