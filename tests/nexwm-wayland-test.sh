#!/bin/sh
# Run the real NexWM Wayland compositor on wlroots' headless backend and check its public protocol globals.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
BUILD=${BUILD:-build}
B=$(cd "$BUILD" 2>/dev/null && pwd) || { echo "build dir '$BUILD' not found — run make first" >&2; exit 2; }
NEXWM="$B/nexwm"
PROBE="$B/nexwm-wayland-probe"

if [ ! -x "$NEXWM" ]; then echo "nexwm is not built (make build/nexwm)" >&2; exit 2; fi
if [ ! -x "$PROBE" ]; then echo "Wayland probe is not built (make build/nexwm-wayland-probe)" >&2; exit 2; fi
if ! command -v timeout >/dev/null 2>&1; then
    echo "SKIP: nexwm-wayland: coreutils timeout is not installed"
    exit 0
fi
if ! "$NEXWM" --version 2>&1 | grep -q 'Wayland compositor (wlroots): yes'; then
    echo "SKIP: nexwm-wayland: this NexWM was built without wlroots"
    exit 0
fi

TMP=$(mktemp -d /tmp/hde-nexwm-wayland.XXXXXX)
RUNTIME="$TMP/runtime"
mkdir -m 700 "$RUNTIME"
PID=
cleanup() {
    if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
        kill -TERM "$PID" 2>/dev/null || :
        i=0
        while kill -0 "$PID" 2>/dev/null && [ "$i" -lt 30 ]; do sleep 0.1; i=$((i + 1)); done
        kill -KILL "$PID" 2>/dev/null || :
        wait "$PID" 2>/dev/null || :
    fi
    rm -rf "$TMP"
}
trap cleanup 0
trap 'exit 143' 1 2 15

export XDG_RUNTIME_DIR="$RUNTIME"
export WLR_BACKENDS=headless
export WLR_HEADLESS_OUTPUTS=1
export WLR_RENDERER=pixman
export NEXWM_TEST_MARKER="$TMP/session-started"
unset DISPLAY WAYLAND_DISPLAY

# The compositor's --session path is the same path hde-session uses at login; a marker proves it is started after
# WAYLAND_DISPLAY has been assigned, not merely that the server itself opened a socket.
SESSION="touch $NEXWM_TEST_MARKER; exec sleep 60"
"$NEXWM" --wayland --config /dev/null --session "$SESSION" >"$TMP/compositor.log" 2>&1 &
PID=$!

socket="$RUNTIME/wayland-0"
i=0
while [ ! -S "$socket" ] && [ "$i" -lt 100 ]; do
    if ! kill -0 "$PID" 2>/dev/null; then
        echo "FAIL: nexwm-wayland: compositor exited before creating its socket"
        cat "$TMP/compositor.log"
        exit 1
    fi
    sleep 0.1
    i=$((i + 1))
done
if [ ! -S "$socket" ]; then
    echo "FAIL: nexwm-wayland: no Wayland socket appeared in 10 seconds"
    cat "$TMP/compositor.log"
    exit 1
fi
export WAYLAND_DISPLAY=wayland-0

if ! output=$(timeout 10 "$PROBE" 2>&1); then
    echo "FAIL: nexwm-wayland: protocol probe did not find all required globals"
    printf '%s\n' "$output"
    cat "$TMP/compositor.log"
    exit 1
fi
printf 'PASS: nexwm-wayland: headless compositor publishes %s\n' "$output"

i=0
while [ ! -f "$NEXWM_TEST_MARKER" ] && [ "$i" -lt 30 ]; do sleep 0.1; i=$((i + 1)); done
if [ -f "$NEXWM_TEST_MARKER" ]; then
    echo "PASS: nexwm-wayland: --session starts inside the Wayland socket"
else
    echo "FAIL: nexwm-wayland: --session did not start"
    cat "$TMP/compositor.log"
    exit 1
fi

kill -TERM "$PID"
i=0
while kill -0 "$PID" 2>/dev/null && [ "$i" -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
if kill -0 "$PID" 2>/dev/null; then
    echo "FAIL: nexwm-wayland: compositor did not stop on SIGTERM"
    cat "$TMP/compositor.log"
    exit 1
fi
if wait "$PID"; then status=0; else status=$?; fi
PID=
if [ "$status" -eq 0 ]; then
    echo "PASS: nexwm-wayland: compositor shuts down cleanly"
else
    echo "FAIL: nexwm-wayland: compositor exited with status $status"
    cat "$TMP/compositor.log"
    exit 1
fi
