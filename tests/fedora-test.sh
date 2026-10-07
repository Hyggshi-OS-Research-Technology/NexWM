#!/bin/sh
# tests/fedora-test.sh — HDE on Fedora, and on a *base* Fedora (a minimal install without any desktop package).
#
# The two are different tests on purpose:
#
#   sh tests/fedora-test.sh --full   a Fedora that has the packages of packaging/fedora/deps.sh: the Fedora package
#                                    names in HDE's messages (dnf, not apt; libwnck3-devel, not libwnck-3-dev), Fedora's
#                                    os-release in About, a whole session in Xvfb with the GTK window manager of the
#                                    system, the Start menu, notifications, a screenshot, the SDDM login theme, and the
#                                    "HDE (Wayland)" session with labwc.
#
#   sh tests/fedora-test.sh --base   a base Fedora: only what compiles HDE (packaging/fedora/deps.sh list build) and
#                                    Xvfb. No window manager, no icon theme, no NetworkManager, no labwc. HDE has to
#                                    build, the unit tests have to pass, and the programs have to start and *degrade
#                                    gracefully*: say what is missing (with a dnf hint) and keep running.
#
# Like tests/smoke.sh: needs Xvfb, dbus-run-session, python3; use `sh tests/fedora-test.sh --full|--base`.
# Output: $HDE_TEST_OUT (default /tmp/hde-fedora): results.txt, session.log, shot-*.png. Exit status = failures.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
BUILD=${BUILD:-build}
B=$(cd "$BUILD" 2>/dev/null && pwd) || { echo "build dir '$BUILD' not found (run make first)"; exit 2; }
OUT=${HDE_TEST_OUT:-/tmp/hde-fedora}
DISP=${HDE_TEST_DISPLAY:-:78}
MODE=""
case "${1:-}" in
--full) MODE=full ;;
--base) MODE=base ;;
*) echo "usage: $0 --full|--base      (see the comments at the top of this file)" >&2; exit 2 ;;
esac
XT="python3 $HERE/xtool.py"

if [ -z "${HDE_FEDORA_INNER:-}" ]; then
    rm -rf "$OUT"
    # .cache/hde is where the session writes its log (hde-start and the session both do): without it the shell cannot
    # even open the log file and the session never starts
    mkdir -p "$OUT/home/.config/hde" "$OUT/home/.cache/hde" "$OUT/home/.local/share/applications" "$OUT/home/Desktop" "$OUT/run"
    chmod 700 "$OUT/run"
    : > "$OUT/results.txt"
    # a tool this test cannot do without: said in the results too, so it is on the job summary and not only in the log
    for t in Xvfb dbus-run-session python3 rpm; do
        command -v $t >/dev/null 2>&1 ||
            { echo "FAIL: fedora: missing $t (dnf install xorg-x11-server-Xvfb dbus-tools python3 rpm)" | tee -a "$OUT/results.txt"; exit 2; }
    done
    printf '[settings]\nwm=auto\n' > "$OUT/home/.config/hde/settings.ini"
    printf '[Desktop Entry]\nType=Application\nName=Fedora Test App\nComment=HDE on Fedora\nExec=touch %s/fedora-test-app\nIcon=applications-utilities\nCategories=Utility;\n' \
        "$OUT" > "$OUT/home/.local/share/applications/fedora-test-app.desktop"
    export HOME="$OUT/home" XDG_CONFIG_HOME="$OUT/home/.config" XDG_CACHE_HOME="$OUT/home/.cache"
    export XDG_DATA_HOME="$OUT/home/.local/share" XDG_RUNTIME_DIR="$OUT/run" LANG=C.UTF-8 NO_AT_BRIDGE=1
    export HDE_DEBUG=1 PATH="$B:$PATH"
    unset WAYLAND_DISPLAY HDE_SESSION_PID XDG_CURRENT_DESKTOP GDK_BACKEND

    Xvfb "$DISP" -screen 0 1280x800x24 -nolisten tcp > "$OUT/xvfb.log" 2>&1 &
    XVFB=$!
    export DISPLAY="$DISP"
    for _ in $(seq 1 50); do $XT popups >/dev/null 2>&1 && break; sleep 0.2; done
    HDE_FEDORA_INNER=1 dbus-run-session -- sh "$0" "--$MODE"
    rc=$?
    kill $XVFB 2>/dev/null
    echo "== results ($OUT/results.txt)"
    cat "$OUT/results.txt"
    exit $rc
fi

# ===================== inside dbus-run-session =====================
FAILS=0
pass() { echo "PASS: fedora: $*" | tee -a "$OUT/results.txt"; }
fail() { echo "FAIL: fedora: $*" | tee -a "$OUT/results.txt"; FAILS=$((FAILS + 1)); }
info() { echo "INFO: fedora: $*" | tee -a "$OUT/results.txt"; }
skip() { echo "SKIP: fedora: $*" | tee -a "$OUT/results.txt"; }
check() { desc=$1; shift; if "$@" >/dev/null 2>&1; then pass "$desc"; else fail "$desc"; fi; }
checknot() { desc=$1; shift; if "$@" >/dev/null 2>&1; then fail "$desc"; else pass "$desc"; fi; }
have() { command -v "$1" >/dev/null 2>&1; }
shot() { have import && import -display "$DISPLAY" -window root "$OUT/shot-$1.png" 2>/dev/null; }
nlog() { n=$(grep -c "$1" "$LOG" 2>/dev/null); echo "${n:-0}"; }
wait_log() { i=0; while [ "$i" -lt "${2:-50}" ]; do grep -q "$1" "$LOG" 2>/dev/null && return 0; sleep 0.2; i=$((i + 1)); done; return 1; }
LOG="$XDG_CACHE_HOME/hde/session.log"

info "mode: $MODE; run on $(sed -n 's/^PRETTY_NAME=//p' /etc/os-release 2>/dev/null | tr -d '"')"

# ---------- 1. this really is a Fedora (the test would prove nothing on another system) ----------
if grep -qE '^(ID=fedora|ID_LIKE=.*fedora)' /etc/os-release 2>/dev/null; then
    pass "Fedora: $(sed -n 's/^PRETTY_NAME=//p' /etc/os-release | tr -d '"')"
else
    fail "this is not Fedora: /etc/os-release has no ID=fedora / ID_LIKE=fedora"
fi
# (the container images of newer Fedoras ship no fedora-release package: what counts is that rpm and /etc/os-release
# agree with each other)
sysrel=$(rpm -q fedora-release 2>/dev/null | head -n1)
[ -n "$sysrel" ] || sysrel=$(rpm -q --whatprovides system-release 2>/dev/null | head -n1)
[ -n "$sysrel" ] || sysrel=$(rpm -q fedora-release-common 2>/dev/null | head -n1)
if [ -n "$sysrel" ]; then
    pass "rpm knows the system ($sysrel)"
else
    skip "rpm knows no fedora-release package (the container images have none)"
fi
info "dnf $(dnf --version 2>/dev/null | head -n1); gcc $(gcc -dumpversion 2>/dev/null)"

# ---------- 2. the Fedora dependency list of this repository ----------
DEPS="$HERE/../packaging/fedora/deps.sh"
if [ -x "$DEPS" ]; then
    if have dnf && [ "$MODE" = full ]; then
        miss=$(sh "$DEPS" missing build 2>/dev/null | tr '\n' ' ')
        if [ -z "$miss" ]; then pass "every build dependency of packaging/fedora/deps.sh is installed"
        else fail "build dependencies missing: $miss"; fi
    else
        info "the build list: $(sh "$DEPS" list build | tr '\n' ' ')"
    fi
    n=$(sh "$DEPS" list build | wc -l)
    check "packaging/fedora/deps.sh lists the build dependencies ($n entries)" test "$n" -ge 10
fi

# ---------- 3. HDE speaks Fedora: dnf, Fedora's package names, Fedora's advice ----------
"$B/hde-settings" --deps build > "$OUT/deps-build.txt" 2>&1
if grep -q "^  sudo dnf install " "$OUT/deps-build.txt" && grep -q "gtk3-devel" "$OUT/deps-build.txt" &&
   grep -q "libwnck3-devel" "$OUT/deps-build.txt" &&
   ! grep -qE "(^| )(libgtk-3-dev|libwnck-3-dev|libxrandr-dev|libxi-dev|libx11-dev|libgtk-layer-shell-dev|libxi6)( |$)" "$OUT/deps-build.txt"; then
    pass "hde-settings --deps build: $(sed -n 's/^  //p' "$OUT/deps-build.txt" | tr -d '\n' | cut -c1-60)…"
else
    fail "hde-settings --deps build prints a dnf command with Fedora's package names: $(tr '\n' '|' < "$OUT/deps-build.txt")"
fi
"$B/hde-settings" --deps runtime > "$OUT/deps-runtime.txt" 2>&1
if grep -q "^  sudo dnf install " "$OUT/deps-runtime.txt" && grep -q "NetworkManager" "$OUT/deps-runtime.txt"; then
    pass "hde-settings --deps runtime: Debian's network-manager is Fedora's NetworkManager"
else
    fail "hde-settings --deps runtime prints dnf with Fedora's names: $(tr '\n' '|' < "$OUT/deps-runtime.txt")"
fi
sed 's/^/INFO:   /' "$OUT/deps-build.txt" "$OUT/deps-runtime.txt" >> "$OUT/results.txt"

# ---------- 4. the unit tests (no X server, no desktop packages needed) ----------
for t in randr-plan-test svgpath-test power-test measure-test distro-test; do
    if [ -x "$B/$t" ]; then
        if "$B/$t" > "$OUT/$t.txt" 2>&1; then
            pass "unit test $t: $(grep -c '^PASS' "$OUT/$t.txt") checks passed"
        else
            fail "unit test $t: $(grep '^FAIL' "$OUT/$t.txt" | head -n 2 | tr '\n' ' ')…"
        fi
    else
        skip "unit test $t is not built (make check-unit)"
    fi
done
if [ -x "$B/distro-test" ]; then
    # the Fedora-specific half of that test: on Fedora, HDE must *not* point at apt
    if grep -q "Fedora: a missing network manager is reported as 'sudo dnf install NetworkManager'" "$OUT/distro-test.txt"; then
        pass "distro-test: on Fedora HDE points at dnf and Fedora's package names"
    else
        fail "distro-test: the Fedora part did not run (HDE_OS_RELEASE?)"
    fi
fi

# ---------- 5. the programs of a Fedora build are complete (no library of a Debian package missing) ----------
miss=""
for p in "$B"/hde-panel "$B"/hde-desktop "$B"/hde-settings "$B"/hde-session "$B"/hde-hotkeys "$B"/hde-xsettings \
         "$B"/hde-screenshot "$B"/hde-files; do
    [ -x "$p" ] || continue
    if ldd "$p" 2>/dev/null | grep -q "not found"; then miss="$miss $(basename "$p")"; fi
done
if [ -z "$miss" ]; then pass "ldd: every built program finds all of its libraries"
else fail "ldd: missing libraries for:$miss"; ldd "$B/hde-panel" 2>/dev/null | grep "not found" | sed 's/^/INFO:   /' >> "$OUT/results.txt"; fi

# ---------- 6. a session, with what this Fedora has ----------
"$B/hde-session" > "$LOG" 2>&1 &
SESSION=$!
export HDE_SESSION_PID=$SESSION
sleep 7
shot "01-session"
check "hde-panel is running" pgrep -x hde-panel
check "hde-desktop is running" pgrep -x hde-desktop
check "hde-hotkeys is running" pgrep -x hde-hotkeys
check "the session is still alive (it did not give up on anything missing)" sh -c "kill -0 $SESSION"

# a session that did not come up: the log is the only witness, so put it in the results (they are the CI annotation)
if ! pgrep -x hde-panel >/dev/null 2>&1 || ! kill -0 "$SESSION" 2>/dev/null; then
    if [ -f "$LOG" ]; then
        info "the session log so far ($LOG, $(wc -l < "$LOG") lines):"
        tail -n 25 "$LOG" | sed 's/^/INFO:   /' >> "$OUT/results.txt"
    else
        info "there is no session log at $LOG (the session never started)"
    fi
    info "processes: $(pgrep -a hde 2>/dev/null | tr '\n' '|')"
fi

if [ "$MODE" = full ]; then
    # a window manager from Fedora, chosen by HDE's auto mode (GTK ones first)
    WM=$(sed -n 's/^hde-session: starting window manager \([A-Za-z0-9]*\).*/\1/p' "$LOG" | head -n1)
    if [ -n "$WM" ]; then pass "a Fedora window manager is running: $WM"
    else fail "no window manager started: $(grep -i 'window manager' "$LOG" | tail -n 2 | tr '\n' ' ')"; fi
    check "the window manager is the EWMH one (a supporting WM check window exists)" \
        sh -c "[ \"\$($XT root-window _NET_SUPPORTING_WM_CHECK)\" != 0 ]"
    export HDE_WM_RUNNING=1

    check "the panel publishes its window (_HDE_PANEL_WINDOW)" \
        sh -c "[ \"\$($XT root-window _HDE_PANEL_WINDOW)\" != 0 ]"
    l=$(grep "^hde-panel: measured: " "$LOG" | tail -n 1)
    case "$l" in
    *"fits") pass "the panel measured itself on this screen: ${l#hde-panel: measured: }" ;;
    *)       fail "the panel did not measure itself (${l:-nothing logged})" ;;
    esac
    "$B/hde-panel" --measure > "$OUT/measure.txt" 2>&1 &&
        pass "hde-panel --measure: $(sed -n 's/^Windows get: *//p' "$OUT/measure.txt")" ||
        fail "hde-panel --measure failed: $(tr '\n' '|' < "$OUT/measure.txt")"

    # the Start menu opens with the Super key and finds an application of this user
    n0=$($XT popups); xdotool key super; sleep 1.5; n1=$($XT popups)
    if [ "$n1" -gt "$n0" ]; then pass "Super opens the Start menu ($n0 -> $n1 pop-up windows)"
    else fail "Super opens the Start menu ($n0 -> $n1 pop-up windows)"; fi
    xdotool type --delay 40 "Fedora Test"; sleep 1.5
    if grep -q "hde-panel: start menu: search 'Fedora Test': [1-9][0-9]* result(s), best: Fedora Test App" "$LOG"; then
        pass "typing searches the Start menu and finds the application"
    else
        fail "the Start menu search ($(grep 'start menu: search' "$LOG" | tail -n 1))"
    fi
    xdotool key Return; sleep 2
    check "the application the Start menu started ran (it touched its file)" test -f "$OUT/fedora-test-app"
    shot "02-start-menu"
    xdotool key Escape; sleep 0.5

    # a real window: the terminal key (Ctrl+Alt+T). On Fedora HDE finds what is installed (xterm in the test image,
    # hde-cmd once this repository ships it)
    before=$(xdotool search --onlyvisible --class "" 2>/dev/null | wc -l)
    xdotool key ctrl+alt+t; sleep 3
    after=$(xdotool search --onlyvisible --class "" 2>/dev/null | wc -l)
    if [ "$after" -gt "$before" ]; then pass "Ctrl+Alt+T opened a terminal window ($before -> $after windows)"
    else fail "Ctrl+Alt+T opened a terminal window ($before -> $after windows; $(grep -i terminal "$LOG" | tail -n 1))"; fi
    W=$(xdotool search --onlyvisible --name "." 2>/dev/null | tail -n 1)
    if [ -n "$W" ]; then
        fr=$($XT frame "$W")
        st=$($XT wm-state "$W")
        if [ -n "$fr" ] && [ "$fr" != "0 0 0 0" ]; then pass "the window manager framed the window (${fr})"
        else fail "the window manager did not frame the window ($(xprop -id "$W" WM_NAME 2>/dev/null | head -n1))"; fi
        info "window $W: frame '$fr' state '$st'"
        $XT maximize "$W"; sleep 1.5
        if $XT wm-state "$W" | grep -q MAXIMIZED; then pass "the window manager maximized the window (EWMH works)"
        else fail "EWMH maximize did not work ($($XT wm-state "$W"))"; fi
        xdotool windowkill "$W" 2>/dev/null
    fi
    shot "03-terminal"

    # notifications (HDE's own daemon in the panel; Fedora has notify-send in libnotify)
    if have notify-send; then
        c0=$(nlog "hde-notify: notification")
        notify-send -i dialog-information "Fedora" "HDE on Fedora" 2>/dev/null
        sleep 2
        if [ "$(nlog "hde-notify: notification")" -gt "$c0" ]; then pass "notify-send reached HDE's notification daemon"
        else fail "notify-send did not reach HDE's notification daemon"; fi
    else
        skip "notify-send is not installed (dnf install libnotify)"
    fi

    # a screenshot with PrtSc (HDE's own screenshot tool, no ImageMagick needed). The names of the tool are
    # Screenshot_<date>_<time>.png; the shot-*.png files are the pictures this test itself takes with ImageMagick
    SHOTS="$HOME/Pictures/Screenshots"
    nshots() { ls "$SHOTS"/*.png 2>/dev/null | wc -l; }
    n0=$(nshots); xdotool key Print; sleep 3; n1=$(nshots)
    if [ "$n1" -gt "$n0" ]; then
        pass "PrtSc wrote a screenshot: $(basename "$(ls -t "$SHOTS"/*.png | head -n1)")"
    else
        fail "PrtSc did not write a screenshot ($n0 -> $n1 in $SHOTS; $(grep -i screenshot "$LOG" | tail -n 1))"
    fi

    # Fedora's GTK theme reaches HDE's XSETTINGS (the panel and the apps follow it). The key names are the ones of the
    # XSETTINGS protocol (Net/ThemeName), as HDE's manager publishes them
    if $XT xsettings 2>/dev/null | grep -q "Net/ThemeName"; then
        pass "hde-xsettings publishes the theme of this Fedora: $($XT xsettings | grep -m1 Net/ThemeName)"
    else
        fail "no XSETTINGS published ($($XT xsettings 2>&1 | head -n 2 | tr '\n' ' '))"
    fi

    # the Fedora logo of About (the icon theme of the system; Fedora installs fedora-logo-icon itself)
    if have gtk3-icon-browser || rpm -q fedora-logos >/dev/null 2>&1 || [ -f /usr/share/icons/hicolor/*/apps/fedora-logo-icon.* ]; then
        "$B/hde-settings" --about > "$OUT/about.txt" 2>&1
        if grep -q "^Operating system: Fedora" "$OUT/about.txt"; then
            pass "About: $(sed -n 's/^Operating system: //p' "$OUT/about.txt")"
        else
            fail "About does not show Fedora: $(grep -i 'operating system' "$OUT/about.txt" | head -n1)"
        fi
        if grep -qE "^Based on: " "$OUT/about.txt"; then
            info "About shows a base system: $(sed -n 's/^Based on: //p' "$OUT/about.txt" | head -n1) (a Fedora derivative)"
        else
            pass "About shows no 'Based on' line for Fedora itself (ID_LIKE is empty on Fedora)"
        fi
        if grep -qi "Fedora asks for" "$OUT/about.txt" || grep -qi "HDE itself needs" "$OUT/about.txt"; then
            pass "the RAM advice is written for this system, not for Debian"
        else
            info "About prints no RAM advice in --about (that text is in the window)"
        fi
    else
        skip "fedora-logos (the fedora-logo-icon icon theme) is not installed"
    fi

    # the SDDM login screen theme (its own folder, login/sddm/): only when this Fedora has SDDM
    if [ -x "$HERE/sddm-test.sh" ] && { have sddm-greeter || have sddm-greeter-qt6; }; then
        out=$(HDE_SDDM_DISPLAY="$DISPLAY" HDE_TEST_OUT="$OUT/sddm" sh "$HERE/sddm-test.sh" --theme "$HERE/../login/sddm/hde" 2>&1)
        case "$out" in
        *"0 failed"*) pass "the SDDM theme renders in Fedora's greeter ($(echo "$out" | tail -n 1))" ;;
        *)            fail "the SDDM theme: $(echo "$out" | grep '^FAIL' | head -n 2 | tr '\n' ' ')" ;;
        esac
    else
        skip "sddm-greeter is not installed (the Fedora package is sddm)"
    fi

    # the Wayland session with labwc (Fedora has labwc): a quick check that HDE comes up on Wayland here too.
    # The deep test is tests/wayland-test.sh (run by the same CI job).
    if have labwc; then
        # env -u HDE_SESSION_PID: the Wayland test logs out of its own session, and `hde-session logout` would otherwise
        # find *this* session (HDE_SESSION_PID, or the /proc fallback) and end it here as well
        out=$(env -u HDE_SESSION_PID WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 \
              WLR_LIBINPUT_NO_DEVICES=1 sh "$HERE/wayland-test.sh" 2>&1)
        case "$out" in
        *"all checks passed"*) pass "the Wayland session starts on Fedora ($(printf '%s\n' "$out" | grep -c '^PASS') checks)" ;;
        *FAIL*)                fail "the Wayland session on Fedora: $(printf '%s\n' "$out" | grep FAIL | head -n 1)" ;;
        *)                     info "the Wayland test said: $(printf '%s\n' "$out" | tail -n 2 | tr '\n' ' ')" ;;
        esac
    else
        skip "labwc is not installed (dnf install labwc)"
    fi
else
    # ---------------- base Fedora: the desktop packages really are absent ----------------
    checknot "no window manager is installed (that is the point of a base Fedora)" sh -c \
        "command -v metacity || command -v openbox || command -v xfwm4 || command -v marco"
    checknot "no NetworkManager (nmcli) is installed" sh -c "command -v nmcli"
    checknot "no labwc is installed" sh -c "command -v labwc"
    if wait_log "hde-session: no window manager could be started" 20; then
        pass "the session says it found no window manager: $(grep -m1 'no window manager' "$LOG")"
    else
        fail "the session did not report the missing window manager: $(grep -i 'window manager' "$LOG" | tail -n 2 | tr '\n' ' ')"
    fi
    if grep -q "Install one, e.g.: sudo dnf install metacity" "$LOG"; then
        pass "and it says how to install one, with dnf"
    else
        fail "the missing window manager hint is not Fedora's: $(grep -i 'Install one' "$LOG" | tail -n 1)"
    fi
    check "the panel runs without a window manager" pgrep -x hde-panel
    check "the desktop runs without a window manager" pgrep -x hde-desktop
    check "no program of the session crashed on the way" sh -c \
        "! grep -qE 'hde-(panel|desktop|hotkeys): (segmentation|GLib-ERROR|ERROR:)' '$LOG'"
    check "the panel still measures itself" sh -c "[ \"\$($XT root-window _HDE_PANEL_WINDOW)\" != 0 ]"
    check "hde-panel --measure works without a window manager" sh "$B/hde-panel --measure"
    check "hde-settings --apply (login-time settings) works without setxkbmap/xset" sh "$B/hde-settings --apply"
    check "hde-settings --about works on a base Fedora" sh "$B/hde-settings --about"
    check "hde-settings --displays works without NetworkManager/BlueZ" sh "$B/hde-settings --displays"
    check "hde-settings --power works without upower/power-profiles-daemon" sh "$B/hde-settings --power"
    check "hde-files --version works" sh "$B/hde-files" --version
    check "hde-hotkeys --help works" sh "$B/hde-hotkeys" --help
    if grep -q "sudo dnf install" "$LOG"; then
        pass "the session log tells the user what to install with dnf: $(grep -m1 'sudo dnf install' "$LOG" | sed 's/^.*Install one: //')"
    else
        info "the session log has no dnf hint (nothing was missing enough to mention)"
    fi
    shot "01-base-session"
fi

# ---------- 7. leaving no process behind ----------
if kill -0 "$SESSION" 2>/dev/null; then
    kill -TERM "$SESSION" 2>/dev/null
    sleep 2
    if kill -0 "$SESSION" 2>/dev/null; then fail "the session did not stop on SIGTERM"; kill -KILL "$SESSION" 2>/dev/null
    else pass "the session stops on SIGTERM"; fi
else
    fail "the session exited before it was asked to"
fi

if [ "$FAILS" = 0 ]; then
    echo "== fedora-test ($MODE): all checks passed" | tee -a "$OUT/results.txt"
else
    echo "== fedora-test ($MODE): $FAILS check(s) FAILED" | tee -a "$OUT/results.txt"
fi
exit "$FAILS"
