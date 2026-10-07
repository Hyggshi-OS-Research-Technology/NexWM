#!/bin/sh
# tests/nexwm-test.sh — NexWM, the window manager of HDE, driven in a real X server.
#
# What is checked, in this order:
#
#   1. without a display at all: the help and the version, a mistake in the configuration file (exit status 2), no
#      display (exit status 3) — what a session shows the user on a login screen, or finds in its log;
#   2. in Xvfb: NexWM takes the screen over (_NET_SUPPORTING_WM_CHECK and the EWMH properties a session needs), puts a
#      frame around a window with the border from the configuration, gives it the focus, and tells the programs how
#      thick that frame is (_NET_FRAME_EXTENTS);
#   3. the key bindings of ~/.config/hde/nexwm.conf (HDE_NEXWM_CONF here, so the file of the user is never touched):
#      workspaces (counted from 1 in the file, from 0 in the protocol), moving a window to another workspace, snap
#      left/right, maximize and unmaximize, full screen, spawn, and close (WM_DELETE_WINDOW, not a kill);
#   4. what HDE's panel asks of it: the struts of a dock window shrink the work area, and a maximized window stays clear
#      of the panel (tests/nexwm-client.c --dock is that panel);
#   5. the way out: `quit` gives every window back (no frame, no _NET_FRAME_EXTENTS, back to the root) and leaves the
#      properties of the root clean;
#   6. --replace: with Metacity running, `nexwm --x11` refuses to start (exit status 3) and `nexwm --replace` takes the
#      screen over from it. Metacity is what HDE runs by default, so this is how a user meets NexWM mid-session — and the
#      refusal must leave it running: taking the WM_S0 selection away from a window manager is how you ask it to quit,
#      which is what --replace does on purpose and a plain start must not do.
#
#   sh tests/nexwm-test.sh                       everything this machine can do
#   HDE_TEST_OUT=/tmp/where sh tests/nexwm-test.sh
#
# The test puts everything it starts away, and a watchdog (tests/nexwm-watchdog.sh, HDE_TEST_WATCHDOG seconds, 300 by
# default) ends it with a FAIL line naming the last check if it ever hangs: a stuck CI job cannot be read, a failed one
# can. Every question asked of the X server is bounded too — a tool that waits for ever waits in the middle of a check,
# and that is how a two-minute test becomes a step nobody can read.
#
# Environment: HDE_NEXWM_DISPLAY (the X server to use; default :81, started with Xvfb here if nothing answers),
# HDE_TEST_OUT (/tmp/hde-nexwm). Needs: Xvfb xdotool x11-utils (xprop, xwininfo) and the two programs the Makefile
# builds (build/nexwm, build/nexwm-client); metacity is optional (step 6 is skipped without it) and so are python3 +
# libX11 (tests/xtool.py owns that one check: nothing can read a selection through xprop).
# Exit status = failures; results.txt has the PASS/FAIL lines (tests/ci-annotate.py reads it).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
CASE=$(cd "$HERE/.." && pwd)
BUILD=${BUILD:-build}
B=$(cd "$BUILD" 2>/dev/null && pwd) || { echo "build dir '$BUILD' not found — run make first" >&2; exit 2; }
OUT=${HDE_TEST_OUT:-/tmp/hde-nexwm}
DISP=${HDE_NEXWM_DISPLAY:-:81}
NEXWM="$B/nexwm"
CLIENT="$B/nexwm-client"
CONF="$OUT/nexwm.conf"
LOG="$OUT/nexwm.log"

if [ ! -x "$NEXWM" ]; then echo "tests/nexwm-test.sh: $NEXWM is not built (make build/nexwm)" >&2; exit 2; fi

rm -rf "$OUT"
mkdir -p "$OUT/bin"
: > "$OUT/results.txt"

# A watchdog for the whole test. A CI job that hangs tells nobody anything: if this is still running after
# HDE_TEST_WATCHDOG seconds (300 by default), the last line of results.txt is the evidence of where it got stuck, and
# the test leaves with a failure the annotator can publish (tests/ci-annotate.py reads that file).
MAIN_PID=$$
# Every process this test starts is named here from the beginning, so the cleanup below can put all of them away even
# when the test ends in the middle of a section.
XVFB_PID=""; WM_PID=""; WM2_PID=""; ALPHA_PID=""; BETA_PID=""; DOCK_PID=""; METACITY_PID=""

# The watchdog is tests/nexwm-watchdog.sh, a separate program with its own reasons written down at the top of it: it
# says where a hanging test got stuck, asks it to leave, and kills what is left of it when the signal cannot be acted
# on. It is started here with the pid of this shell, and nothing of it goes to the stdout of the step (see below).
sh "$HERE/nexwm-watchdog.sh" "$MAIN_PID" "$OUT" "${HDE_TEST_WATCHDOG:-300}" >/dev/null 2>&1 &
WATCHDOG_PID=$!

# Every process this test starts is named here from the beginning, so the cleanup below can put all of them away even
# when the test ends in the middle of a section.
XVFB_PID=""; WM_PID=""; WM2_PID=""; ALPHA_PID=""; BETA_PID=""; DOCK_PID=""; METACITY_PID=""

# What the test started is put away by the test, whatever way it ends: the X server, the window manager(s), the windows
# and the tools that may be waiting for the X server. A test that leaves a process behind leaves a CI step that never
# finishes (the step is over only when the last holder of its stdout is gone), which is a hang nobody can read.
cleanup() {
    kill -TERM "$WATCHDOG_PID" 2>/dev/null
    for p in ${WM_PID:-} ${WM2_PID:-} ${METACITY_PID:-} ${ALPHA_PID:-} ${BETA_PID:-} ${DOCK_PID:-} ${XVFB_PID:-}; do
        [ -n "$p" ] && kill -TERM "$p" 2>/dev/null
    done
}
trap 'cleanup' 0                                   # however this test ends, what it started is put away
trap 'exit 143' 1 2 15                             # ... and a signal ends it: a trap that only runs cleanup would
                                                   #     catch the signal and carry on, which is how a CI step of 25
                                                   #     minutes happens (the watchdog would have said its line and the
                                                   #     test would have kept going)
FAILS=0
pass() { echo "PASS: nexwm: $*" | tee -a "$OUT/results.txt"; }
fail() { echo "FAIL: nexwm: $*" | tee -a "$OUT/results.txt"; FAILS=$((FAILS + 1)); }
info() { echo "INFO: nexwm: $*" | tee -a "$OUT/results.txt"; }
have() { command -v "$1" >/dev/null 2>&1; }
# A build made without libxcb has a nexwm that can do nothing on X11 but say what to install (the Makefile finds
# libxcb with pkg-config, and `nexwm --version` reports what went into the program): nothing to drive then — a SKIP
# line, not a failure, so a machine without the -dev package is not a failing machine. (make check-nexwm says the
# same thing before it gets here; this is for a direct run.)
BUILD_HAS_X11="$("$NEXWM" --version 2>/dev/null | grep -c "X11 window manager (XCB): yes")"
if [ "$BUILD_HAS_X11" -eq 0 ]; then
    info "this nexwm was built without libxcb, so it has no X11 window manager — the window manager test is skipped"
    echo "SKIP: nexwm: no X11 window manager in this build (libxcb1-dev/libxcb-devel was not there at build time)" \
        | tee -a "$OUT/results.txt"
    exit 0
fi
if [ ! -x "$CLIENT" ]; then
    info "$CLIENT is not built: nothing to put on the screen — the window manager test is skipped"
    echo "SKIP: nexwm: $CLIENT is not built (make build/nexwm-client)" | tee -a "$OUT/results.txt"
    exit 0
fi

# every check carries both values in its message: a failure in CI has to be readable without the machine
eq() { if [ "$2" = "$3" ]; then pass "$1: $3"; else fail "$1: expected '$2', got '$3'"; fi; }
has() { case "$3" in *"$2"*) pass "$1" ;; *) fail "$1 ('$2' is not in '$(printf '%s' "$3" | tr '\n' '|')')" ;; esac; }
hasnt() { case "$3" in *"$2"*) fail "$1 ('$2' is in '$(printf '%s' "$3" | tr '\n' '|')')" ;; *) pass "$1" ;; esac; }

# ---- reading properties: xprop for properties, xwininfo for places, sizes and map states -----------------
# Every question asked of the X server is bounded. xprop, xwininfo and xdotool wait for the server to answer, and a tool
# that waits for ever is a test that hangs — and a CI job that hangs tells nobody anything. 15 s is many times what any
# of these calls needs; reaching it is a failure with a readable message instead of a job nobody can read.
if have timeout; then TMO="timeout 15"; else TMO=""; fi
rprop() { $TMO xprop -root "$1" 2>/dev/null; }                         # the line of a root property
# A window id that is empty is not a question xwininfo will answer: `xwininfo -id ""` reads it as "no window named" and
# waits for a window to be clicked on — the test would hang until something else ended it. No id means no answer here.
wprop() { [ -n "$1" ] || return 0; $TMO xprop -id "$1" "$2" 2>/dev/null; }  # ... of a window property
# only its value. Most properties come as "NAME(TYPE) = value"; a property of type WINDOW comes as
# "NAME(WINDOW): window id # 0x…" (no "=" at all — this is what _NET_SUPPORTING_WM_CHECK, _NET_CLIENT_LIST and
# _NET_ACTIVE_WINDOW look like), and a property that is not there prints nothing once both forms are tried.
xvalue() { sed -n -e 's/^[^=]*= *//p' -e 's/^.*window id # *//p'; }
rval() { rprop "$1" | xvalue | head -n1; }
wval() { wprop "$1" "$2" | xvalue | head -n1; }
# a window's absolute place and size as "X Y W H" (empty when the window is not there any more, or when there is no id)
xywh() {
    [ -n "$1" ] || return 0
    $TMO xwininfo -id "$1" 2>/dev/null | awk '
        /Absolute upper-left X:/ { x = $NF }
        /Absolute upper-left Y:/ { y = $NF }
        /^  Width:/  { w = $NF }
        /^  Height:/ { h = $NF }
        END { if (w != "" && x != "") printf "%s %s %s %s\n", x, y, w, h }'
}
mapstate() { [ -n "$1" ] || return 0; $TMO xwininfo -id "$1" 2>/dev/null | sed -n 's/^  Map State: //p'; }
# the window a window is inside of. xwininfo prints how it is framed on the screen only when it is asked for the
# children as well: plain `xwininfo -id X` has no "Parent window id" line at all, and asking it for the parent of
# nothing is how a test waits for a mouse click that never comes.
parentof() { [ -n "$1" ] || return 0; $TMO xwininfo -children -id "$1" 2>/dev/null \
    | sed -n 's/^ *Parent window id: \(0x[0-9a-f]*\).*/\1/p'; }
rootid()   { $TMO xwininfo -root 2>/dev/null | sed -n 's/^xwininfo: Window id: \(0x[0-9a-f]*\).*/\1/p'; }
id_of()    { sed -n 's/.*\(0x[0-9a-f]*\).*/\1/p' | head -n1; }         # the window id inside a property's value
# the ICCCM name of the window manager role (WM_S0) is a *selection*, which xprop cannot read: tests/xtool.py can (it
# talks to libX11 through ctypes, so python3 and libX11 both have to be there — when they are not, the check says so
# instead of failing, the way a machine without xdotool or xprop skips the sections that need them)
XT="python3 $HERE/xtool.py"
have_xt() { [ -f "$HERE/xtool.py" ] && $XT selection-owner WM_S0 >/dev/null 2>&1; }
sel_owner() { [ -n "$1" ] && $XT selection-owner "$1" 2>/dev/null; }
wm_name() {                                                            # the name the window manager published
    id=$(rval _NET_SUPPORTING_WM_CHECK | id_of)
    [ -n "$id" ] && wval "$id" _NET_WM_NAME | tr -d '"'
}

# ---- waiting (a window manager is as fast as X is; the test never sleeps longer than it has to) -----------
wait_root() {   # property, what its value must contain, tries
    i=0; n=${3:-25}
    while [ "$i" -lt "$n" ]; do
        v=$(rval "$1")
        case "$v" in *"$2"*) return 0 ;; esac
        i=$((i + 1)); sleep 0.25
    done
    return 1
}
wait_log_in() { # the log file, what it must show, tries
    i=0; n=${3:-20}
    while [ "$i" -lt "$n" ]; do
        grep -q -- "$2" "$1" 2>/dev/null && return 0
        i=$((i + 1)); sleep 0.25
    done
    return 1
}
wait_log() { wait_log_in "$LOG" "$1" "${2:-20}"; }
wait_place() {  # a window must end up at this place and size: window, "X Y W H", tries
    i=0; n=${3:-25}
    while [ "$i" -lt "$n" ]; do
        [ "$(xywh "$1")" = "$2" ] && return 0
        i=$((i + 1)); sleep 0.25
    done
    return 1
}
wait_mapstate() {   # a window must end up in this map state: window, state, tries
    i=0; n=${3:-25}
    while [ "$i" -lt "$n" ]; do
        [ "$(mapstate "$1")" = "$2" ] && return 0
        i=$((i + 1)); sleep 0.25
    done
    return 1
}
# A process that was asked to leave must really leave. `wait` on its own would block for ever when it does not — a CI
# job that hangs tells nobody anything, so every wait here has a limit, and running out of it is a failure with the
# state of the program in the message. After this, `wait PID` can be used to read the exit status: it is gone already.
wait_gone() {   # pid, what it is (for the message), tries
    i=0; n=${3:-40}
    while [ "$i" -lt "$n" ]; do
        kill -0 "$1" 2>/dev/null || return 0
        # a child that has ended but was not waited for is a zombie: `kill -0` still says it is there, and the state
        # of the process is what tells them apart (Linux /proc; without it, only the exit of the process counts)
        if [ -r "/proc/$1/status" ] && grep -q "^State:.*Z" "/proc/$1/status" 2>/dev/null; then return 0; fi
        i=$((i + 1)); sleep 0.25
    done
    info "$2 (pid $1) is still running after $((n / 4)) s"
    return 1
}
# one key of the configuration: press it, then require the line it must leave in the log
pressed() {     # what it does, the key, the line the log must show
    $TMO xdotool key "$2" >/dev/null 2>&1
    if wait_log "$3" 20; then
        pass "$1 ($(grep -m1 -- "$3" "$LOG"))"
    else
        fail "$1: '$2' did not make the log say '$3'"
    fi
}
# where a window must be: the place and the size both (the message carries what was seen instead)
at() {          # what it is, the window, "X Y W H"
    if wait_place "$2" "$3" 8; then
        pass "$1: at $3"
    else
        fail "$1: expected '$3', got '$(xywh "$2")'"
    fi
}

# ---- what the test starts, and how it is cleaned up again -----------------------------------------------
# The processes this test starts are named at the top of the file, and the cleanup and the traps there put them away.
# (There used to be a second cleanup and a second trap here. A trap installed later replaces the one before it, so the
# watchdog's signal ran that cleanup and then the test carried on — with an X server that no longer existed.)

# ---- 1. what needs no display at all ---------------------------------------------------------------------
out=$(env -u DISPLAY -u WAYLAND_DISPLAY "$NEXWM" --version 2>&1)
has "the version says what this is" "NexWM" "$out"
has "and which of its two sides went into this build (X11 here, or this test would not be running)" \
    "X11 window manager (XCB): yes" "$out"
has "and the same for the Wayland side (yes or no, with what to install)" "Wayland compositor (wlroots):" "$out"

# the Wayland side of this build: either the compositor (still being written: it says where it is going, status 4) or
# nothing at all (status 4 and what to install) — never a crash and never a silent success
out=$(env -u DISPLAY -u WAYLAND_DISPLAY "$NEXWM" --wayland 2>&1); st=$?
if [ "$st" = 4 ]; then
    has "the Wayland side answers for itself (status 4: the compositor is the next step)" "compositor" "$out"
else
    fail "--wayland: expected status 4, got $st ('$out')"
fi

# the compositor gets the session command from the session (hde-session --wayland --wm nexwm): it must be taken, not
# refused as an unknown option — the compositor itself says what it does with it
out=$(env -u DISPLAY -u WAYLAND_DISPLAY "$NEXWM" --wayland --session "/usr/bin/hde-session --wayland-inner" 2>&1); st=$?
if [ "$st" = 4 ]; then
    pass "the Wayland side takes the session command of a session (status 4: the compositor is the next step)"
else
    fail "--wayland --session: expected status 4, got $st ('$out')"
fi
hasnt "and it is not refused as an unknown option" "unknown option" "$out"
out=$(env -u DISPLAY -u WAYLAND_DISPLAY "$NEXWM" --wayland --session 2>&1); st=$?
if [ "$st" != 0 ]; then
    pass "a --session without a command is a mistake, not a silent no-op (status $st)"
else
    fail "--wayland --session (no command): expected a failure, got status 0"
fi

out=$("$NEXWM" --help 2>&1)
has "the help names the X11 window manager" "--x11" "$out"
has "the help names the Wayland side" "--wayland" "$out"
has "and the session command the Wayland session starts it with" "--session CMD" "$out"
has "the help shows how a key is written" "key Super+Return spawn" "$out"

out=$(env -u DISPLAY -u WAYLAND_DISPLAY "$NEXWM" --x11 2>&1); st=$?
if [ "$st" = 3 ]; then
    has "with no display it says so (status 3)" "cannot open the display" "$out"
else
    fail "with no display: expected status 3, got $st ('$out')"
fi

out=$("$NEXWM" --config "$OUT/this-file-was-never-written.conf" --x11 2>&1); st=$?
if [ "$st" = 2 ]; then
    has "a configuration file that is not there is refused (status 2)" "cannot read the configuration" "$out"
else
    fail "--config with a missing file: expected status 2, got $st ('$out')"
fi

printf '# a mistake on the third line\nborder 6\nborder banana\n' > "$OUT/wrong.conf"
out=$("$NEXWM" --config "$OUT/wrong.conf" --x11 2>&1); st=$?
if [ "$st" = 2 ]; then
    has "a line that makes no sense is refused with its line number (status 2)" "line 3: border takes" "$out"
else
    fail "a wrong line: expected status 2 and the line number, got $st ('$out')"
fi

printf 'key Super+Nonsense spawn xterm\n' > "$OUT/wrong2.conf"
out=$("$NEXWM" --config "$OUT/wrong2.conf" --x11 2>&1); st=$?
if [ "$st" = 2 ]; then
    has "a key nobody knows is refused (status 2)" "is not a key this knows" "$out"
else
    fail "an unknown key: expected status 2, got $st ('$out')"
fi

out=$(env -u DISPLAY -u WAYLAND_DISPLAY HDE_NEXWM_CONF="$OUT/not-there.conf" "$NEXWM" --x11 2>&1); st=$?
if [ "$st" = 3 ]; then
    pass "a configuration file that is not there is not an error (the defaults are used)"
else
    fail "a missing HDE_NEXWM_CONF: expected status 3 (no display), got $st ('$out')"
fi

# ---- 2. the window manager in a real X server ------------------------------------------------------------
if ! have Xvfb || ! have xdotool || ! have xprop || ! have xwininfo; then
    fail "Xvfb, xdotool, xprop and xwininfo are needed for the rest (sudo apt install xvfb xdotool x11-utils)"
else
    export DISPLAY="$DISP"
    if $TMO xdotool getdisplaygeometry >/dev/null 2>&1; then
        info "using the X server already on $DISP"
    else
        Xvfb "$DISP" -screen 0 1024x768x24 -nolisten tcp > "$OUT/xvfb.log" 2>&1 &
        XVFB_PID=$!
        i=0
        while [ "$i" -lt 40 ]; do
            $TMO xdotool getdisplaygeometry >/dev/null 2>&1 && break
            i=$((i + 1)); sleep 0.25
        done
    fi
    geo=$($TMO xdotool getdisplaygeometry 2>/dev/null)
    if [ "$geo" != "1024 768" ]; then
        fail "no 1024x768 X server on $DISP (Xvfb said: $(tail -n 2 "$OUT/xvfb.log" 2>/dev/null | tr '\n' '|'))"
    else
        pass "an X server is there to manage ($DISP: $geo)"

        # the configuration this window manager runs with: a border of 6, three workspaces, and two keys of our own
        # (everything else is the default set: Super+Q close, Super+Shift+Q quit, Super+Left/Right/Up/Down, Super+F
        #  full screen, Super+1..9 workspaces, Super+Shift+1..9 move-to, Super+Tab next)
        cat > "$CONF" <<'EOF'
# nexwm-test.sh: the configuration the window manager of this test runs with
border 6
desktops 3
colors 0x101010 0xff8800
key Super+Return spawn nexwm-touch
key Super+T spawn /not/there/nothing
key Super+9 workspace 3
EOF
        # the program Super+Return must start: it is found through PATH, and it leaves a file behind
        printf '#!/bin/sh\ntouch "%s/touched"\n' "$OUT" > "$OUT/bin/nexwm-touch"
        chmod 755 "$OUT/bin/nexwm-touch"
        PATH="$OUT/bin:$PATH"; export PATH

        "$NEXWM" --x11 --config "$CONF" > "$LOG" 2>&1 &
        WM_PID=$!
        if ! wait_root _NET_SUPPORTING_WM_CHECK "0x" 40; then
            alive=alive; kill -0 "$WM_PID" 2>/dev/null || alive=gone
            fail "the window manager did not come up ($alive; log: $(tail -n 3 "$LOG" | tr '\n' '|'); xprop: $(rprop _NET_SUPPORTING_WM_CHECK | tr '\n' '|'))"
        else
            has "it announces itself as the window manager of this screen" "NexWM" "$(wm_name)"
            if have_xt; then
                owner=$(sel_owner WM_S0)
                if [ -n "$owner" ] && [ "$owner" != 0 ]; then
                    pass "and it holds the WM_S0 selection, which is what being the window manager means (ICCCM)"
                else
                    fail "it does not hold the WM_S0 selection (owner '$owner')"
                fi
            else
                info "tests/xtool.py cannot read a selection here: the WM_S0 check is skipped"
            fi
            has "it says what it is doing in the log" "nexwm: NexWM" "$(cat "$LOG")"
            has "with the border of the configuration file" "frame 6 px" "$(cat "$LOG")"
            has "and the workspaces of the configuration file" "3 workspaces" "$(cat "$LOG")"
            eq "_NET_NUMBER_OF_DESKTOPS is what the file says" "3" "$(rval _NET_NUMBER_OF_DESKTOPS)"
            eq "_NET_CURRENT_DESKTOP starts at the first workspace" "0" "$(rval _NET_CURRENT_DESKTOP)"
            has "_NET_WORKAREA covers the whole screen while nothing reserves room" "0, 0, 1024, 768" \
                "$(rval _NET_WORKAREA)"
            has "_NET_SUPPORTED offers the frame extents HDE's programs ask for" "_NET_FRAME_EXTENTS" \
                "$(rval _NET_SUPPORTED)"
            has "_NET_SUPPORTED offers the struts HDE's panel reserves room with" "_NET_WM_STRUT_PARTIAL" \
                "$(rval _NET_SUPPORTED)"
            has "the key bindings are published for Settings (and for xprop)" "Super+Q close" "$(rval _NEXWM_KEYS)"
            has "the key the configuration file added is published too, with its workspace" "Super+9 workspace 3" \
                "$(rval _NEXWM_KEYS)"

            # a window, and what the window manager does with it
            "$CLIENT" --title nexwm-alpha --class nexwm-alpha --size 600x400 --pos 60,60 > "$OUT/alpha.out" 2>&1 &
            ALPHA_PID=$!
            ALPHA=""
            i=0
            while [ "$i" -lt 30 ]; do
                ALPHA=$(sed -n 's/^id=//p' "$OUT/alpha.out" | head -n1)
                [ -n "$ALPHA" ] && break
                i=$((i + 1)); sleep 0.2
            done
            if [ -z "$ALPHA" ]; then
                fail "the test window did not start (see $OUT/alpha.out)"
            else
                info "the test window is $ALPHA, asked for 600x400 at 60,60"
                if wait_root _NET_CLIENT_LIST "$ALPHA" 40; then
                    pass "it is in _NET_CLIENT_LIST"
                else
                    fail "it is not in _NET_CLIENT_LIST ($(rval _NET_CLIENT_LIST))"
                fi
                eq "_NET_FRAME_EXTENTS is the border of the configuration file" "6, 6, 6, 6" \
                   "$(wval "$ALPHA" _NET_FRAME_EXTENTS)"
                eq "the window kept the place it asked for" "60 60 600 400" "$(xywh "$ALPHA")"
                FRAME=$(parentof "$ALPHA")
                info "the window the test window sits in is '$FRAME' (xwininfo -children)"
                eq "the frame around it is the window plus two borders" "54 54 612 412" "$(xywh "$FRAME")"
                eq "and the window has the focus" "$ALPHA" "$(rval _NET_ACTIVE_WINDOW | id_of)"
                has "the panel is told what it may ask the window to do" "_NET_WM_ACTION_CLOSE" \
                    "$(wval "$ALPHA" _NET_WM_ALLOWED_ACTIONS)"
                has "it can be maximized, so the panel offers that too" "_NET_WM_ACTION_MAXIMIZE_VERT" \
                    "$(wval "$ALPHA" _NET_WM_ALLOWED_ACTIONS)"
                has "the log says it took the window over" "managing 0x${ALPHA#0x} 'nexwm-alpha'" "$(cat "$LOG")"
                has "and that the window has the focus" "focus 0x${ALPHA#0x} 'nexwm-alpha'" "$(cat "$LOG")"

                # ---- 3. the key bindings -------------------------------------------------------------------
                # Super+9 is "workspace 3" in the file: the file counts from 1, the protocol from 0
                pressed "Super+9 goes to the third workspace of the file" "super+9" "workspace 3/3"
                eq "and _NET_CURRENT_DESKTOP is the index of it (0-based)" "2" "$(rval _NET_CURRENT_DESKTOP)"
                wait_mapstate "$ALPHA" IsUnMapped 20
                eq "a window of another workspace is unmapped, not lost" "IsUnMapped" "$(mapstate "$ALPHA")"
                has "it is still the session's window (the panel lists it)" "$ALPHA" "$(rval _NET_CLIENT_LIST)"

                pressed "Super+1 comes back to the first workspace" "super+1" "workspace 1/3"
                eq "and the first workspace is index 0" "0" "$(rval _NET_CURRENT_DESKTOP)"
                wait_mapstate "$ALPHA" IsViewable 20
                eq "the window is on the screen again" "IsViewable" "$(mapstate "$ALPHA")"
                eq "and it has the focus back" "$ALPHA" "$(rval _NET_ACTIVE_WINDOW | id_of)"

                # snap: half of the work area (1024x768), the frame following the window
                pressed "Super+Right snaps the window to the right half" "super+Right" "snapped right"
                at "the right half of the screen: 512 wide, full height" "$ALPHA" "512 0 512 768"
                pressed "Super+Left snaps it to the left half" "super+Left" "snapped left"
                at "the left half of the screen" "$ALPHA" "0 0 512 768"

                pressed "Super+Up maximizes it" "super+Up" "maximized 0x${ALPHA#0x}"
                at "the whole screen while nothing reserves room" "$ALPHA" "0 0 1024 768"
                has "and _NET_WM_STATE says so (the panel draws its button that way)" "_NET_WM_STATE_MAXIMIZED_VERT" \
                    "$(wval "$ALPHA" _NET_WM_STATE)"
                has "with the horizontal half too" "_NET_WM_STATE_MAXIMIZED_HORZ" "$(wval "$ALPHA" _NET_WM_STATE)"
                pressed "Super+Down gives the old place back" "super+Down" "unmaximized 0x${ALPHA#0x}"
                at "the place before the maximize (the snapped one)" "$ALPHA" "0 0 512 768"
                hasnt "and the maximized state is gone" "_NET_WM_STATE_MAXIMIZED_VERT" "$(wval "$ALPHA" _NET_WM_STATE)"

                pressed "Super+F makes it full screen" "super+F" "full screen 0x${ALPHA#0x}"
                at "the whole screen" "$ALPHA" "0 0 1024 768"
                has "and _NET_WM_STATE says full screen" "_NET_WM_STATE_FULLSCREEN" "$(wval "$ALPHA" _NET_WM_STATE)"
                pressed "Super+F again takes it back" "super+F" "back from full screen"
                at "the place it had before" "$ALPHA" "0 0 512 768"
                hasnt "and the full screen state is gone" "_NET_WM_STATE_FULLSCREEN" "$(wval "$ALPHA" _NET_WM_STATE)"

                # ---- 4. HDE's panel: a dock window reserves room on the screen ------------------------------
                "$CLIENT" --title nexwm-dock --class nexwm-dock --dock 24 > "$OUT/dock.out" 2>&1 &
                DOCK_PID=$!
                DOCK=""
                i=0
                while [ "$i" -lt 30 ]; do
                    DOCK=$(sed -n 's/^id=//p' "$OUT/dock.out" | head -n1)
                    [ -n "$DOCK" ] && break
                    i=$((i + 1)); sleep 0.2
                done
                if [ -z "$DOCK" ]; then
                    fail "the panel window of the test did not start (see $OUT/dock.out)"
                else
                    if wait_root _NET_WORKAREA "0, 24, 1024, 744" 40; then
                        pass "the strut of the panel shrinks the work area (0, 24, 1024, 744)"
                    else
                        fail "the work area did not shrink for the panel (got '$(rval _NET_WORKAREA)')"
                    fi
                    has "the log explains the work area" "workarea 1024x744 at 0,24" "$(cat "$LOG")"
                    eq "a dock window is not framed (no border around the panel)" "0, 0, 0, 0" \
                       "$(wval "$DOCK" _NET_FRAME_EXTENTS)"
                    has "and the panel is marked to stay out of the window list" "_NET_WM_STATE_SKIP_TASKBAR" \
                        "$(wval "$DOCK" _NET_WM_STATE)"
                    eq "the dock keeps the place and the size it asked for" "0 0 1024 24" "$(xywh "$DOCK")"

                    pressed "Super+Up maximizes the focused window" "super+Up" "maximized 0x${ALPHA#0x}"
                    at "a maximized window stays clear of the panel" "$ALPHA" "0 24 1024 744"
                    pressed "Super+Down gives the place back again" "super+Down" "unmaximized 0x${ALPHA#0x}"
                    at "the place before (the snapped one, under the panel)" "$ALPHA" "0 0 512 768"
                fi

                # ---- a second window, and moving it to another workspace -----------------------------------
                "$CLIENT" --title nexwm-beta --class nexwm-beta --size 700x500 --pos 200,150 > "$OUT/beta.out" 2>&1 &
                BETA_PID=$!
                BETA=""
                i=0
                while [ "$i" -lt 30 ]; do
                    BETA=$(sed -n 's/^id=//p' "$OUT/beta.out" | head -n1)
                    [ -n "$BETA" ] && break
                    i=$((i + 1)); sleep 0.2
                done
                if [ -z "$BETA" ]; then
                    fail "the second test window did not start (see $OUT/beta.out)"
                else
                    if wait_root _NET_ACTIVE_WINDOW "$BETA" 40; then
                        pass "a new window gets the focus by itself"
                    else
                        fail "a new window did not get the focus (_NET_ACTIVE_WINDOW is $(rval _NET_ACTIVE_WINDOW))"
                    fi
                    has "the new window is in the list as well" "$BETA" "$(rval _NET_CLIENT_LIST)"
                    pressed "Super+Shift+2 sends the focused window to workspace 2" "super+shift+2" "to workspace 2"
                    eq "and _NET_WM_DESKTOP says where it went" "1" "$(wval "$BETA" _NET_WM_DESKTOP)"
                    wait_mapstate "$BETA" IsUnMapped 20
                    eq "it is unmapped while its workspace is not the one on the screen" "IsUnMapped" \
                       "$(mapstate "$BETA")"
                    eq "the window of the first workspace is still there" "IsViewable" "$(mapstate "$ALPHA")"

                    pressed "Super+2 goes to the second workspace" "super+2" "workspace 2/3"
                    eq "and the desk is the second one" "1" "$(rval _NET_CURRENT_DESKTOP)"
                    wait_mapstate "$BETA" IsViewable 20
                    eq "the window that moved there is on the screen" "IsViewable" "$(mapstate "$BETA")"
                    eq "and has the focus" "$BETA" "$(rval _NET_ACTIVE_WINDOW | id_of)"
                    eq "while the window it left is unmapped" "IsUnMapped" "$(mapstate "$ALPHA")"
                    pressed "Super+1 comes back" "super+1" "workspace 1/3"
                    wait_mapstate "$ALPHA" IsViewable 20
                    eq "and the first window is on the screen again" "IsViewable" "$(mapstate "$ALPHA")"
                fi

                # ---- spawn ---------------------------------------------------------------------------------
                rm -f "$OUT/touched"
                pressed "Super+Return starts the program of the configuration file" "super+Return" "spawn: nexwm-touch"
                i=0
                while [ "$i" -lt 20 ]; do
                    [ -f "$OUT/touched" ] && break
                    i=$((i + 1)); sleep 0.25
                done
                if [ -f "$OUT/touched" ]; then
                    pass "and the program really ran (the file it writes is there)"
                else
                    fail "the program of Super+Return did not run (no $OUT/touched)"
                fi
                pressed "a key bound to a program that is not there says which one" "super+t" \
                        "cannot run '/not/there/nothing': not there"

                # ---- close: WM_DELETE_WINDOW, not a kill ---------------------------------------------------
                pressed "Super+Q asks the focused window to close" "super+Q" "asked 0x${ALPHA#0x} 'nexwm-alpha' to close"
                if wait_log_in "$OUT/alpha.out" "the window manager asked me to close" 20; then
                    pass "the window itself was asked to close (it says so), it was not killed"
                else
                    fail "the window was not asked to close (see $OUT/alpha.out)"
                fi
                if wait_gone "$ALPHA_PID" "the window that was asked to close" 40; then
                    wait "$ALPHA_PID" 2>/dev/null
                    pass "the window left by itself (a close is not a kill)"
                else
                    fail "the window is still there after being asked to close — it was not killed, so it must have been asked"
                    kill "$ALPHA_PID" 2>/dev/null
                fi
                ALPHA_PID=""
                if wait_root _NET_CLIENT_LIST "$BETA" 20; then
                    hasnt "the window is out of _NET_CLIENT_LIST" "$ALPHA" "$(rval _NET_CLIENT_LIST)"
                else
                    fail "_NET_CLIENT_LIST lost the window that is still there"
                fi
                has "the log says it let the window go" "unmanaging 0x${ALPHA#0x}" "$(cat "$LOG")"

                # ---- 5. the way out: quit gives everything back ---------------------------------------------
                $TMO xdotool key super+2 >/dev/null 2>&1            # to the workspace of the window that is left
                wait_mapstate "$BETA" IsViewable 20
                $TMO xdotool key super+shift+q >/dev/null 2>&1
                if wait_log "window(s) given back" 40; then
                    if wait_gone "$WM_PID" "the window manager that quit" 40; then
                        wait "$WM_PID" 2>/dev/null; st=$?
                        if [ "$st" = 0 ]; then pass "quit leaves the window manager with status 0"
                        else fail "quit left the window manager with status $st (0 expected)"; fi
                    else
                        fail "quit said it gave the windows back, but the window manager is still running"
                        kill "$WM_PID" 2>/dev/null
                    fi
                else
                    fail "quit did not make the window manager leave (log: $(tail -n 2 "$LOG" | tr '\n' '|'))"
                    kill "$WM_PID" 2>/dev/null
                fi
                WM_PID=""
                has "it said what it gave back" "leaving:" "$(cat "$LOG")"
                if have_xt; then
                    eq "and the WM_S0 selection went back with it (the connection closed)" "0" "$(sel_owner WM_S0)"
                fi
                hasnt "_NET_SUPPORTING_WM_CHECK is gone from the root" "window id" "$(rprop _NET_SUPPORTING_WM_CHECK)"
                hasnt "and the keys it published are gone too" "Super" "$(rprop _NEXWM_KEYS)"
                hasnt "the frame extents of the window it managed are gone" "6, 6" "$(wprop "$BETA" _NET_FRAME_EXTENTS)"
                eq "the window was given back to the root without its frame" "$(rootid)" "$(parentof "$BETA")"
                eq "where it was asked to be, at the size it asked for" "200 150 700 500" "$(xywh "$BETA")"
                eq "and it is on the screen" "IsViewable" "$(mapstate "$BETA")"

                # ---- 6. --replace: what happens when Metacity (HDE's default) is already running ------------
                kill "$BETA_PID" "$DOCK_PID" 2>/dev/null
                BETA_PID=""; DOCK_PID=""
                sleep 0.4
                if ! have metacity; then
                    info "metacity is not installed: the --replace checks are skipped"
                else
                    metacity > "$OUT/metacity.log" 2>&1 &
                    METACITY_PID=$!
                    i=0
                    while [ "$i" -lt 40 ]; do
                        [ "$(wm_name)" = "Metacity" ] && break
                        i=$((i + 1)); sleep 0.25
                    done
                    eq "Metacity is the window manager to start with" "Metacity" "$(wm_name)"

                    out=$("$NEXWM" --x11 --config "$CONF" 2>&1); st=$?
                    if [ "$st" = 3 ]; then
                        has "without --replace NexWM refuses to fight for the screen" "another window manager is running" \
                            "$out"
                    else
                        fail "without --replace: expected status 3, got $st ('$out')"
                    fi
                    # ... and it must not have touched it on the way out: taking the WM_S0 selection away from a window
                    # manager that is running is how you ask it to leave, and a start that refuses has no business doing
                    # that (Metacity would have quit, taking the session's window manager with it)
                    eq "Metacity is still the window manager after that" "Metacity" "$(wm_name)"

                    "$NEXWM" --x11 --replace --config "$CONF" > "$OUT/nexwm2.log" 2>&1 &
                    WM2_PID=$!
                    i=0
                    while [ "$i" -lt 60 ]; do
                        [ "$(wm_name)" = "NexWM" ] && break
                        i=$((i + 1)); sleep 0.25
                    done
                    eq "with --replace it takes the screen over from Metacity" "NexWM" "$(wm_name)"
                    has "and the log says how" "asking it to hand over (--replace)" "$(cat "$OUT/nexwm2.log")"
                    i=0
                    while [ "$i" -lt 20 ]; do
                        kill -0 "$METACITY_PID" 2>/dev/null || break
                        i=$((i + 1)); sleep 0.25
                    done
                    if kill -0 "$METACITY_PID" 2>/dev/null; then
                        info "Metacity is still there (as a zombie or still running): $(tail -n 1 "$OUT/metacity.log")"
                    else
                        pass "Metacity left the screen to it"
                    fi
                    METACITY_PID=""
                    $TMO xdotool key super+shift+q >/dev/null 2>&1
                    if wait_log_in "$OUT/nexwm2.log" "window(s) given back" 40; then
                        if wait_gone "$WM2_PID" "the window manager that replaced Metacity" 40; then
                            wait "$WM2_PID" 2>/dev/null; st=$?
                            if [ "$st" = 0 ]; then pass "and the second window manager leaves cleanly as well"
                            else fail "the second window manager left with status $st (0 expected)"; fi
                        else
                            fail "the second window manager said it gave the windows back, but it is still running"
                            kill "$WM2_PID" 2>/dev/null
                        fi
                    else
                        fail "the window manager that replaced Metacity did not leave"
                        kill "$WM2_PID" 2>/dev/null
                    fi
                    WM2_PID=""
                fi
            fi
        fi
    fi
fi

# What the window manager and the windows of the test said, at the end: when a check fails in CI, the log of the run is
# not always reachable, and this is what is needed to see what happened (tests/ci-annotate.py publishes these lines).
info "the window manager said: $(tail -n 4 "$LOG" 2>/dev/null | tr '\n' '|')"
info "the test windows said: $(tail -n 2 "$OUT/alpha.out" 2>/dev/null | tr '\n' '|') $(tail -n 2 "$OUT/beta.out" 2>/dev/null | tr '\n' '|')"

echo ""
if [ "$FAILS" = 0 ]; then
    echo "== nexwm-test: all checks passed (0 failed)" | tee -a "$OUT/results.txt"
else
    echo "== nexwm-test: $FAILS check(s) failed" | tee -a "$OUT/results.txt"
fi
exit "$FAILS"
