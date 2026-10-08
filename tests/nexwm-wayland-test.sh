#!/bin/sh
# Run the real NexWM Wayland compositor on wlroots' headless backend and check its public protocol globals.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
# What this test says is written where tests/ci-annotate.py reads it (the same file tests/nexwm-test.sh fills in), so a
# failure in CI is readable through the API: the log of a job is not always reachable, and this test runs after the
# X11 one in the same step.
OUT=${HDE_TEST_OUT:-/tmp/hde-nexwm}
say() {
    printf '%s\n' "$*"
    [ -d "$OUT" ] && printf '%s\n' "$*" >> "$OUT/results.txt"
    return 0
}
BUILD=${BUILD:-build}
B=$(cd "$BUILD" 2>/dev/null && pwd) || { echo "build dir '$BUILD' not found — run make first" >&2; exit 2; }
NEXWM="$B/nexwm"
PROBE="$B/nexwm-wayland-probe"

if [ ! -x "$NEXWM" ]; then echo "nexwm is not built (make build/nexwm)" >&2; exit 2; fi
if [ ! -x "$PROBE" ]; then echo "Wayland probe is not built (make build/nexwm-wayland-probe)" >&2; exit 2; fi
if ! command -v timeout >/dev/null 2>&1; then
    say "SKIP: nexwm-wayland: coreutils timeout is not installed"
    exit 0
fi
if ! "$NEXWM" --version 2>&1 | grep -q 'Wayland compositor (wlroots): yes'; then
    say "SKIP: nexwm-wayland: this NexWM was built without wlroots"
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
export WLR_RENDERER_ALLOW_SOFTWARE=1        # a machine with no graphics card: software rendering is the only kind
export LIBGL_ALWAYS_SOFTWARE=1
export NEXWM_TEST_MARKER="$TMP/session-started"
unset DISPLAY WAYLAND_DISPLAY

# The compositor's --session path is the same path hde-session uses at login; a marker proves it is started after
# WAYLAND_DISPLAY has been assigned, not merely that the server itself opened a socket.
SESSION="touch $NEXWM_TEST_MARKER; exec sleep 60"

# Start the compositor and wait for its socket. wlroots can be built with the software (pixman) renderer and/or the
# GLES2 one, and a distribution's package decides which: the software renderer is the one that needs nothing but the
# CPU, so it is tried first, and a build without it gets the GLES2 renderer with software rendering allowed above.
# Every attempt keeps its own log, and the logs are printed when none of them opens a socket: a compositor that does
# not start has to say why.
COMPOSITOR_LOG=
start_compositor() {    # renderer
    rm -f "$NEXWM_TEST_MARKER" "$RUNTIME/wayland-0"
    COMPOSITOR_LOG="$TMP/compositor-$1.log"
    WLR_RENDERER="$1" "$NEXWM" --wayland --config /dev/null --session "$SESSION" >"$COMPOSITOR_LOG" 2>&1 &
    PID=$!
    socket="$RUNTIME/wayland-0"
    i=0
    while [ ! -S "$socket" ] && [ "$i" -lt 100 ]; do
        kill -0 "$PID" 2>/dev/null || return 1
        sleep 0.1
        i=$((i + 1))
    done
    [ -S "$socket" ] && return 0
    # it is still there but has not opened its socket in ten seconds: it is put away before the next renderer is tried
    kill -TERM "$PID" 2>/dev/null || :
    wait "$PID" 2>/dev/null || :
    return 1
}
if ! start_compositor pixman && ! start_compositor gles2; then
    say "FAIL: nexwm-wayland: the compositor did not open a Wayland socket (tried the pixman and the GLES2 renderer): $(tail -n 6 "$COMPOSITOR_LOG" | tr '\n' '|')"
    cat "$COMPOSITOR_LOG"
    exit 1
fi
export WAYLAND_DISPLAY=wayland-0

if ! output=$(timeout 10 "$PROBE" 2>&1); then
    say "FAIL: nexwm-wayland: protocol probe did not find all required globals"
    printf '%s\n' "$output"
    cat "$COMPOSITOR_LOG"
    exit 1
fi
say "PASS: nexwm-wayland: headless compositor publishes $output"

i=0
while [ ! -f "$NEXWM_TEST_MARKER" ] && [ "$i" -lt 30 ]; do sleep 0.1; i=$((i + 1)); done
if [ -f "$NEXWM_TEST_MARKER" ]; then
    say "PASS: nexwm-wayland: --session starts inside the Wayland socket"
else
    say "FAIL: nexwm-wayland: --session did not start"
    cat "$COMPOSITOR_LOG"
    exit 1
fi

kill -TERM "$PID"
i=0
while kill -0 "$PID" 2>/dev/null && [ "$i" -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
if kill -0 "$PID" 2>/dev/null; then
    say "FAIL: nexwm-wayland: compositor did not stop on SIGTERM"
    cat "$COMPOSITOR_LOG"
    exit 1
fi
if wait "$PID"; then status=0; else status=$?; fi
PID=
if [ "$status" -eq 0 ]; then
    say "PASS: nexwm-wayland: compositor shuts down cleanly"
else
    say "FAIL: nexwm-wayland: compositor exited with status $status (its last words: $(tail -n 6 "$COMPOSITOR_LOG" | tr '\n' '|'))"
    cat "$COMPOSITOR_LOG"
    exit 1
fi
