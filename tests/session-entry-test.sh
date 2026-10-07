#!/bin/sh
# tests/session-entry-test.sh — the login entries of HDE, and what happens when the compositor one of them names is not
# there yet.
#
#   "NexWM" (data/nexwm.desktop → /usr/share/xsessions) is the HDE session on X11 with NexWM as its window manager;
#   "NexWM (Wayland)" (data/nexwm-wayland.desktop → /usr/share/wayland-sessions) is the same on HDE's own compositor.
#   "HDE (Wayland)" stays the labwc one — it is not touched by any of this.
#
#   A machine where nexwm was built without wlroots (or while the compositor is still being written) must not end at a
#   black screen when that entry is picked: hde-session starts the compositor, sees it leave at once, says so in its log
#   and starts labwc instead. This test drives exactly that, with stand-ins for nexwm and labwc — the stand-ins are the
#   only compositors here, and the real labwc of the machine is never started.
#
#   sh tests/session-entry-test.sh
# Needs: nothing but sh; the hde-session the Makefile builds is used when it is there (make build/hde-session), and the
# part that runs it is skipped (with a SKIP line) when it is not.
# Output: $HDE_TEST_OUT (default /tmp/hde-session-entry): results.txt, and the runs of hde-session in *.log
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
CASE=$(cd "$HERE/.." && pwd)
BUILD=${BUILD:-build}
B=$(cd "$BUILD" 2>/dev/null && pwd) || { echo "build dir '$BUILD' not found — run make first" >&2; exit 2; }
OUT=${HDE_TEST_OUT:-/tmp/hde-session-entry}

rm -rf "$OUT"
mkdir -p "$OUT/bin" "$OUT/home/.config" "$OUT/run"
chmod 700 "$OUT/run"
: > "$OUT/results.txt"

pass() { echo "PASS: session-entry: $*" | tee -a "$OUT/results.txt"; }
fail() { echo "FAIL: session-entry: $*" | tee -a "$OUT/results.txt"; FAILS=$((FAILS + 1)); }
info() { echo "INFO: session-entry: $*" | tee -a "$OUT/results.txt"; }
skip() { echo "SKIP: session-entry: $*" | tee -a "$OUT/results.txt"; }
eq()   { if [ "$2" = "$3" ]; then pass "$1: $3"; else fail "$1: expected '$2', got '$3'"; fi; }
has()  { case "$3" in *"$2"*) pass "$1" ;; *) fail "$1 ('$2' is not in '$(printf '%s' "$3" | tr '\n' '|')')" ;; esac; }
hasnt(){ case "$3" in *"$2"*) fail "$1 ('$2' is in '$(printf '%s' "$3" | tr '\n' '|')')" ;; *) pass "$1" ;; esac; }
FAILS=0

# ---- 1. the files the login screen reads -----------------------------------------------------------------------
entry() { sed -n "s/^$2=//p" "$1" | head -n1; }               # one key of a desktop entry

for f in data/nexwm.desktop data/nexwm-wayland.desktop data/hde-wayland.desktop; do
    if [ ! -f "$CASE/$f" ]; then fail "$f is there"; continue; fi
    eq "$f: it is an application (the login screen shows it)" "Application" "$(entry "$CASE/$f" Type)"
done

x11=$(cat "$CASE/data/nexwm.desktop" 2>/dev/null)
has "NexWM (X11) starts the session with the window manager it is named after" "--wm nexwm" "$(entry "$CASE/data/nexwm.desktop" Exec)"
hasnt "and it is not the Wayland session (that one has its own entry)" "--wayland" "$(entry "$CASE/data/nexwm.desktop" Exec)"
has "NexWM (X11) is only offered when the session program is installed" "hde-session" \
    "$(entry "$CASE/data/nexwm.desktop" TryExec)"

wl=$(cat "$CASE/data/nexwm-wayland.desktop" 2>/dev/null)
has "NexWM (Wayland) asks for NexWM as the compositor" "--wm nexwm" "$(entry "$CASE/data/nexwm-wayland.desktop" Exec)"
has "and it is the Wayland session of that entry" "--wayland" "$(entry "$CASE/data/nexwm-wayland.desktop" Exec)"
has "and it is only offered when NexWM is installed" "nexwm" "$(entry "$CASE/data/nexwm-wayland.desktop" TryExec)"
has "it says what it is on the login screen" "NexWM (Wayland)" "$wl"

labwc=$(entry "$CASE/data/hde-wayland.desktop" Exec)
has "HDE (Wayland) is still the labwc session (labwc is not removed)" "--wayland" "$labwc"
hasnt "and it does not ask for another compositor" "--wm" "$labwc"
has "and the login screen only offers it with labwc installed" "labwc" "$(entry "$CASE/data/hde-wayland.desktop" TryExec)"

mk_install=$(sed -n '/^install:/,/^uninstall:/p' "$CASE/Makefile" 2>/dev/null)
mk_uninstall=$(sed -n '/^uninstall:/,$p' "$CASE/Makefile" 2>/dev/null)
has "make install puts NexWM on the login screen (xsessions)" '$(XSESSIONS)/nexwm.desktop' "$mk_install"
has "and NexWM (Wayland) too (wayland-sessions)" '$(WLSESSIONS)/nexwm-wayland.desktop' "$mk_install"
has "and HDE (Wayland) stays there next to it" '$(WLSESSIONS)/hde-wayland.desktop' "$mk_install"
has "and it all goes away again with make uninstall" '$(WLSESSIONS)/nexwm-wayland.desktop' "$mk_uninstall"

# ---- 2. what hde-session does with it --------------------------------------------------------------------------
if [ ! -x "$B/hde-session" ]; then
    skip "hde-session is not built (make build/hde-session) — the part that runs a session is skipped"
    echo "== session-entry-test: $FAILS check(s) failed (the entries themselves were checked above)" \
        | tee -a "$OUT/results.txt"
    exit "$FAILS"
fi

# hde-session finds the compositor next to itself (it reads /proc/self/exe), so the test runs a copy of it from a
# directory of its own: that is what makes the nexwm and labwc of the machine — and of this test — unambiguous.
cp "$B/hde-session" "$OUT/bin/hde-session"

# The stand-ins: a nexwm that leaves at once (the build without the compositor), a labwc that says it started, and a
# nexwm that stays (a compositor that is really there).
cat > "$OUT/bin/labwc" <<'EOF'
#!/bin/sh
echo "labwc: stand-in started with: $*"
exit 0
EOF
cat > "$OUT/bin/nexwm-leaves" <<'EOF'
#!/bin/sh
echo "nexwm: the Wayland compositor of NexWM is being written; this build has wlroots, so it is the next step" >&2
exit 4
EOF
cat > "$OUT/bin/nexwm-stays" <<'EOF'
#!/bin/sh
echo "nexwm: stand-in compositor: $*"
sleep 4
exit 7
EOF
chmod 755 "$OUT/bin/labwc" "$OUT/bin/nexwm-leaves" "$OUT/bin/nexwm-stays"
cp "$OUT/bin/nexwm-leaves" "$OUT/bin/nexwm"

run_session() {   # what to run (the rest of it is the environment of a session that has not started yet)
    name=$1; shift
    env -u DISPLAY -u WAYLAND_DISPLAY -u GDK_BACKEND \
        HOME="$OUT/home" XDG_CONFIG_HOME="$OUT/home/.config" XDG_CACHE_HOME="$OUT/home/.cache" \
        XDG_RUNTIME_DIR="$OUT/run" LANG=C.UTF-8 \
        DBUS_SESSION_BUS_ADDRESS="unix:path=$OUT/run/not-really-a-bus" \
        PATH="$OUT/bin:$B:$PATH" \
        "$@" > "$OUT/$name.log" 2>&1
    echo "== session-entry: $name (status $?, log $OUT/$name.log)" >> "$OUT/results.txt"
}
run_session nexwm-leaves "$OUT/bin/hde-session" --wayland --wm nexwm
eq "the session is over with status 0 when it handed the screen to labwc" "0" \
   "$(sed -n 's/^== session-entry: nexwm-leaves (status \([0-9]*\).*/\1/p' "$OUT/results.txt" | tail -n1)"
out=$(cat "$OUT/nexwm-leaves.log")
has "when NexWM's compositor leaves at once, the session says so" "left at once" "$out"
has "and names the status it left with (4: not in this build)" "status 4" "$out"
has "and it starts labwc instead, so the entry is never a black screen" "labwc: stand-in started" "$out"
has "and the labwc session is the one that runs inside it" "--wayland-inner" "$out"

run_session nexwm-stays "$OUT/bin/hde-session" --wayland --wm nexwm
out=$(cat "$OUT/nexwm-stays.log")
has "a compositor that is there gets the session command to run inside itself" "--session" "$out"
hasnt "and labwc is not started then (the compositor that is there keeps the screen)" "labwc: stand-in started" "$out"
eq "the status of the compositor is the status of the session (7)" "7" \
   "$(sed -n 's/^== session-entry: nexwm-stays (status \([0-9]*\).*/\1/p' "$OUT/results.txt" | tail -n1)"

# the login entry itself: the Exec line of "NexWM (Wayland)", run the way the login screen runs it
cp "$OUT/bin/nexwm-leaves" "$OUT/bin/nexwm"
run_session hde-start sh "$CASE/data/hde-start" --wm nexwm --wayland      # the script as the login screen runs it
# hde-start keeps its own log in the cache directory (it redirects itself there): that is where its words are
cp "$OUT/home/.cache/hde/session.log" "$OUT/hde-start.log" 2>/dev/null
out=$(cat "$OUT/hde-start.log" 2>/dev/null)
has "the entry of NexWM (Wayland) says which session it runs" "--wm nexwm" "$out"
has "and that session comes up on labwc where NexWM's compositor is not there" "labwc: stand-in started" "$out"

echo "== session-entry-test: $FAILS check(s) failed" | tee -a "$OUT/results.txt"
exit "$FAILS"
