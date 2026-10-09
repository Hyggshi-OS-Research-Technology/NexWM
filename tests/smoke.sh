#!/bin/sh
# tests/smoke.sh — HDE smoke test: runs a whole HDE session in Xvfb, then checks the main features
# (Super key, the Start menu in its three layouts — search, keyboard, favorites, pin to panel —, F1/F2/F3, F6/F7
#  brightness, F8 Project window, notifications, PrtSc screenshots and the Screenshot window, desktop icon frame + icon
#  menu, Wi-Fi list, About with the logo of the system / memory used, the About window for Hyggshi OS, Debian, Linux Mint,
#  live Dark mode, panel at the top / height / items / extensions, the Control Center (quick toggles, brightness,
#  volume, Wi-Fi password in the list, Bluetooth, output devices, notifications), the battery panel (a fake battery),
#  the panel's right-click menus, WM switch without logout, restart after a crash).
#  Several screens for real: tests/display-test.sh (Xorg). The Wayland session: tests/wayland-test.sh (labwc).
#
#   make check            (or: BUILD=build sh tests/smoke.sh)
# Needs: Xvfb xdotool dbus-run-session python3. Optional: metacity openbox pulseaudio notify-send import(ImageMagick)
# Output: $HDE_TEST_OUT (default /tmp/hde-smoke): results.txt, *.log, screenshots shot-*.png
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
BUILD=${BUILD:-build}
B=$(cd "$BUILD" 2>/dev/null && pwd) || { echo "build dir '$BUILD' not found"; exit 2; }
OUT=${HDE_TEST_OUT:-/tmp/hde-smoke}
DISP=${HDE_TEST_DISPLAY:-:77}
XT="python3 $HERE/xtool.py"

if [ -z "${HDE_SMOKE_INNER:-}" ]; then
    for t in Xvfb xdotool dbus-run-session python3; do
        command -v $t >/dev/null 2>&1 || { echo "missing $t (sudo apt install xvfb xdotool dbus-x11 python3)"; exit 2; }
    done
    rm -rf "$OUT"
    mkdir -p "$OUT/home/.config/hde" "$OUT/home/Desktop" "$OUT/home/.local/share/applications" "$OUT/run" "$OUT/fakebin"
    chmod 700 "$OUT/run"
    printf '[settings]\nwm=auto\n' > "$OUT/home/.config/hde/settings.ini"
    # HDE_SMOKE_FONT (e.g. "DejaVu Sans 10"): the font the desktop is measured with. The checks below are in pixels
    # (the panel's clock fits on one line, the Settings window is 1020x700, the Pair button is here), so a test run on
    # a system whose default font is another size measures another desktop. The CI sets it on Fedora and on Arch,
    # where the container's font is not the one these numbers were written for.
    if [ -n "${HDE_SMOKE_FONT:-}" ]; then
        printf 'font=%s\n' "$HDE_SMOKE_FONT" >> "$OUT/home/.config/hde/settings.ini"
        echo "INFO: the desktop is measured with the font '$HDE_SMOKE_FONT' (HDE_SMOKE_FONT)" >> "$OUT/results.txt"
    fi
    sed "s|@PREFIX@/bin/hde-settings|$B/hde-settings|" "$HERE/../data/hyggshi-settings.desktop" \
        > "$OUT/home/.local/share/applications/hyggshi-settings.desktop"
    printf '[Desktop Entry]\nType=Link\nName=HDE Website\nURL=https://github.com/Hyggshi-OS-Research-Technology/NexWM\nIcon=web-browser\n' \
        > "$OUT/home/Desktop/hde.desktop"
    # desktop icons for the icon menu tests (sorted by name: Home, aaa-folder, bbb-notes.txt, hde.desktop)
    mkdir -p "$OUT/home/Desktop/aaa-folder"
    echo "inside" > "$OUT/home/Desktop/aaa-folder/inside.txt"
    echo "notes" > "$OUT/home/Desktop/bbb-notes.txt"
    # places of the Start menu, an app it can start without side effects
    mkdir -p "$OUT/home/Documents" "$OUT/home/Downloads" "$OUT/home/Music" "$OUT/home/Pictures" "$OUT/home/Videos"
    printf '[Desktop Entry]\nType=Application\nName=HDE Test App\nComment=Writes a file for the smoke test\nExec=touch %s/test-app-launched\nIcon=applications-utilities\nCategories=Utility;\n' \
        "$OUT" > "$OUT/home/.local/share/applications/hde-test-app.desktop"
    # os-release of other systems for the About window (HDE_OS_RELEASE): the logo of the system and of its base
    cat > "$OUT/os-release-hyggshios" <<'EOF'
PRETTY_NAME="Hyggshi OS 1.0 \"Sen Vàng\" (dựa trên Debian 13)"
NAME="Hyggshi OS"
VERSION_ID="1.0"
VERSION="1.0 (Sen Vàng) (Debian 13)"
VERSION_CODENAME="Sen Vàng"
HYGGSHI_BASE_CODENAME=trixie
ID=hyggshios
ID_LIKE=debian
HOME_URL="https://github.com/Hyggshi-OS-Research-Technology"
SUPPORT_URL="https://github.com/Hyggshi-OS-Research-Technology/Hyggshi-OS/issues"
BUG_REPORT_URL="https://github.com/Hyggshi-OS-Research-Technology/Hyggshi-OS/issues"
LOGO=distributor-logo
EOF
    cat > "$OUT/os-release-debian" <<'EOF'
PRETTY_NAME="Debian GNU/Linux 13 (trixie)"
NAME="Debian GNU/Linux"
VERSION_ID="13"
VERSION="13 (trixie)"
VERSION_CODENAME=trixie
ID=debian
HOME_URL="https://www.debian.org/"
SUPPORT_URL="https://www.debian.org/support"
BUG_REPORT_URL="https://bugs.debian.org/"
EOF
    cat > "$OUT/os-release-mint" <<'EOF'
NAME="Linux Mint"
VERSION="22.1 (Xia)"
ID=linuxmint
ID_LIKE="ubuntu debian"
PRETTY_NAME="Linux Mint 22.1"
VERSION_ID="22.1"
HOME_URL="https://www.linuxmint.com/"
SUPPORT_URL="https://forums.linuxmint.com/"
VERSION_CODENAME=xia
UBUNTU_CODENAME=noble
EOF
    printf 'NAME="Foo OS"\nPRETTY_NAME="Foo OS 2"\nID=foo\nANSI_COLOR="0;35"\n' > "$OUT/os-release-foo"
    # Openbox config like many LXDE/Openbox systems, with Print bound to an external program: HDE must keep the key
    # (the user-visible bug was 'Failed to execute child process "scrot"' coming from such a binding).
    if [ -f /etc/xdg/openbox/rc.xml ]; then
        mkdir -p "$OUT/home/.config/openbox"
        sed "s|<keyboard>|<keyboard><keybind key=\"Print\"><action name=\"Execute\"><command>touch $OUT/wm-print-pressed</command></action></keybind>|" \
            /etc/xdg/openbox/rc.xml > "$OUT/home/.config/openbox/rc.xml"
    fi

    # fake nmcli: fixed Wi-Fi list, logs every command (to check that the password is NOT on the command line)
    cat > "$OUT/fakebin/nmcli" <<'EOF'
#!/bin/sh
echo "ARGS: $*" >> "${HDE_FAKE_NMCLI_LOG:-/tmp/fake-nmcli.log}"
case "$*" in
  *"radio wifi on"*|*"radio wifi off"*) exit 0 ;;
  *"radio wifi"*) echo enabled ;;
  *"device status"*) printf 'wlan0:wifi:connected:Home WiFi\neth0:ethernet:unavailable:\nlo:loopback:unmanaged:\n' ;;
  *"connection show"*) printf 'Home WiFi:802-11-wireless\nCafe Free:802-11-wireless\nWired connection 1:802-3-ethernet\n' ;;
  *"wifi list"*) printf '*:Home WiFi:82:WPA2:wlan0\n :Home WiFi:40:WPA2:wlan0\n :Cafe Free:55::wlan0\n :Neighbor 5G:70:WPA2 WPA3:wlan0\n :Office\\: Guest:35:WPA1 WPA2:wlan0\n :Corp Secure:61:WPA2 802.1X:wlan0\n ::20:WPA2:wlan0\n' ;;
  *"--ask"*"connect"*) read -r pw; echo "PW-STDIN-LEN: ${#pw}" >> "${HDE_FAKE_NMCLI_LOG:-/tmp/fake-nmcli.log}"; [ "$pw" = "correct-horse" ] ;;
  *"wifi connect"*) echo "Error: Connection activation failed: (7) Secrets were required, but not provided." >&2; exit 4 ;;
  *) exit 0 ;;
esac
EOF
    chmod +x "$OUT/fakebin/nmcli"

    # a laptop battery for the battery icon and the battery panel (HDE_POWER_SUPPLY_DIR replaces /sys/class/power_supply)
    PS="$OUT/power_supply"
    mkdir -p "$PS/BAT0" "$PS/AC"
    printf 'Mains\n' > "$PS/AC/type"; printf '0\n' > "$PS/AC/online"
    for kv in type=Battery status=Discharging present=1 capacity=82 energy_now=47600000 energy_full=58000000 \
              energy_full_design=63700000 power_now=5200000 voltage_now=12100000 cycle_count=123 technology=Li-ion \
              manufacturer=SANYO model_name=45N1001; do
        printf '%s\n' "${kv#*=}" > "$PS/BAT0/${kv%%=*}"
    done
    export HDE_POWER_SUPPLY_DIR="$PS"

    export HOME="$OUT/home" XDG_CONFIG_HOME="$OUT/home/.config" XDG_CACHE_HOME="$OUT/home/.cache"
    export XDG_DATA_HOME="$OUT/home/.local/share" XDG_RUNTIME_DIR="$OUT/run"
    export HDE_FAKE_NMCLI_LOG="$OUT/nmcli.log" PATH="$OUT/fakebin:$PATH" LANG=C.UTF-8 NO_AT_BRIDGE=1
    unset WAYLAND_DISPLAY HDE_SESSION_PID HDE_WM DBUS_SESSION_BUS_ADDRESS XDG_CURRENT_DESKTOP

    Xvfb "$DISP" -screen 0 1280x800x24 -nolisten tcp > "$OUT/xvfb.log" 2>&1 &
    XVFB=$!
    export DISPLAY="$DISP"
    for i in $(seq 1 50); do $XT popups >/dev/null 2>&1 && break; sleep 0.2; done
    HDE_SMOKE_INNER=1 dbus-run-session -- sh "$0"
    rc=$?
    kill $XVFB 2>/dev/null
    echo "== results ($OUT/results.txt)"
    cat "$OUT/results.txt"
    exit $rc
fi

# ===================== inside dbus-run-session =====================
FAILS=0
pass() { echo "PASS: $*" | tee -a "$OUT/results.txt"; }
fail() { echo "FAIL: $*" | tee -a "$OUT/results.txt"; FAILS=$((FAILS + 1)); }
skip() { echo "SKIP: $*" | tee -a "$OUT/results.txt"; }
check() { desc=$1; shift; if "$@" >/dev/null 2>&1; then pass "$desc"; else fail "$desc"; fi; }
shot() { command -v import >/dev/null 2>&1 && import -display "$DISPLAY" -window root "$OUT/shot-$1.png" 2>/dev/null; }
popups() { $XT popups 2>/dev/null || echo 0; }
# pgeo "X Y W H" WHAT: the panel's window as the X server has it (the log only tells what hde-panel asked for)
pgeo() {
    pgeo_g=$($XT geometry "$($XT root-window _HDE_PANEL_WINDOW)" 2>/dev/null)
    if [ "$pgeo_g" = "$1" ]; then pass "$2 (window $pgeo_g)"; else fail "$2 (window ${pgeo_g:-?}, expected $1)"; fi
}
# maxwin WHAT top|bottom LIMIT: the Settings window, maximized, keeps clear of the panel, all of its title bar visible
# (the bug: with the panel at the top the title bar of a maximized window was under the panel). The frame the window
# manager drew around it: _NET_FRAME_EXTENTS. top: the frame starts at y >= LIMIT; bottom: it ends at y <= LIMIT.
maxwin() {
    mw=$(xdotool search --onlyvisible --name "^Hyggshi Settings$" 2>/dev/null | head -n 1)
    if [ -z "$mw" ]; then fail "$1: a maximized window keeps clear of the panel (no Settings window)"; return; fi
    case "$($XT wm-state "$mw")" in *_NET_WM_STATE_MAXIMIZED_VERT*) ;; *) $XT maximize "$mw"; sleep 1.5 ;; esac
    mf=$($XT frame "$mw")
    if [ "$2" = top ]; then
        mv=$(echo "$mf" | awk '{ print $2 }')
        if [ -n "$mv" ] && [ "$mv" -ge "$3" ]; then pass "$1: a maximized window stays below the panel, its title bar from y=$mv (frame $mf)"
        else fail "$1: a maximized window stays below the panel (frame ${mf:-?}, must begin at y >= $3; $($XT wm-state "$mw"))"; fi
    else
        mv=$(echo "$mf" | awk '{ print $2 + $4 }')
        if [ -n "$mv" ] && [ "$mv" -le "$3" ]; then pass "$1: a maximized window stays above the panel, down to y=$mv (frame $mf)"
        else fail "$1: a maximized window stays above the panel (frame ${mf:-?}, must end at y <= $3; $($XT wm-state "$mw"))"; fi
    fi
}
# sidebar WHAT: no black frame around the sidebar of Settings (the bug: the 14 px border of the box of buttons was never
# painted, GtkViewport keeping an opaque cache for a child with a background of its own). 3 points of that border.
sidebar() {
    sw=$(xdotool search --onlyvisible --name "^Hyggshi Settings$" 2>/dev/null | head -n 1)
    sg=$($XT geometry "$sw" 2>/dev/null)
    if [ -z "$sw" ] || [ -z "$sg" ]; then fail "Settings: no black frame around the sidebar ($1): no Settings window"; return; fi
    # shellcheck disable=SC2086  # "X Y W H" -> four arguments
    set -- "$1" $sg
    nb=0; cols=""
    for pt in "$(($2 + 5)) $(($3 + 300))" "$(($2 + 120)) $(($3 + 5))" "$(($2 + 243)) $(($3 + 300))"; do
        # shellcheck disable=SC2086  # "X Y" -> two arguments
        c=$($XT pixel $pt)
        cols="$cols ${pt% *},${pt#* }: $c;"
        [ "$(echo "$c" | awk '{ print $1 + $2 + $3 }')" -gt 60 ] || nb=$((nb + 1))
    done
    if [ "$nb" = 0 ]; then pass "Settings: no black frame around the sidebar ($1;${cols%;})"
    else fail "Settings: no black frame around the sidebar ($1: $nb black point(s);${cols%;})"; fi
}
ncorr() { grep -c "^hde-panel: measured: the panel window is at" "$OUT/session.log"; }
running() { pgrep -x "$1" >/dev/null 2>&1; }       # by process name (never matches the shell doing the check)
SETTINGS_INI="$XDG_CONFIG_HOME/hde/settings.ini"

if command -v pulseaudio >/dev/null 2>&1; then
    pulseaudio -D --exit-idle-time=-1 --log-target=file:"$OUT/pulse.log" >/dev/null 2>&1
    sleep 1.5
fi

# Fake BlueZ (python3-dbusmock) on a private "system bus": checks the Bluetooth device list
BLUEZ_MOCK=""
PPD_MOCK=""
SYSBUS_PID=""
bz() { m=$1; shift; gdbus call --system --dest org.bluez --object-path / --method "org.bluez.Mock.$m" "$@" >> "$OUT/bluez-mock.log" 2>&1; }
if python3 -c "import dbusmock" >/dev/null 2>&1; then
    set -- $(dbus-daemon --session --fork --print-address=1 --print-pid=1 2>/dev/null)
    if [ -n "${1:-}" ]; then
        export DBUS_SYSTEM_BUS_ADDRESS="$1"
        SYSBUS_PID="${2:-}"
        python3 -m dbusmock --system --template bluez5 >> "$OUT/bluez-mock.log" 2>&1 &
        BLUEZ_MOCK=$!
        for i in $(seq 1 40); do gdbus introspect --system --dest org.bluez --object-path / >/dev/null 2>&1 && break; sleep 0.2; done
        bz AddAdapter "'hci0'" "'HDE test PC'"
        bz AddDevice "'hci0'" "'11:22:33:44:55:66'" "'Galaxy Buds2'"
        # dbusmock < 0.28 (Ubuntu 22.04): PairDevice(adapter, address, class) — with an extra Class argument
        bz PairDevice "'hci0'" "'11:22:33:44:55:66'" || bz PairDevice "'hci0'" "'11:22:33:44:55:66'" 2360344
        bz ConnectDevice "'hci0'" "'11:22:33:44:55:66'"
        bz AddDevice "'hci0'" "'AA:BB:CC:DD:EE:01'" "'MX Keys'"
        bz AddDevice "'hci0'" "'AA:BB:CC:DD:EE:02'" "'Pixel 8'"
        gdbus call --system --dest org.bluez --object-path / \
            --method org.freedesktop.DBus.ObjectManager.GetManagedObjects > "$OUT/bluez-objects.txt" 2>&1
        # power-profiles-daemon for the power mode of the battery panel and the Control Center
        python3 -m dbusmock --system --template power_profiles_daemon >> "$OUT/ppd-mock.log" 2>&1 &
        PPD_MOCK=$!
        echo "INFO: dbusmock $(python3 -c 'import dbusmock; print(getattr(dbusmock, "__version__", "?"))' 2>&1)" >> "$OUT/results.txt"
    fi
fi

export HDE_DEBUG=1
"$B/hde-session" > "$OUT/session.log" 2>&1 &
SESSION=$!
export HDE_SESSION_PID=$SESSION
sleep 7
shot 01-session

# ---------- 1. session startup ----------
for p in hde-panel hde-desktop hde-hotkeys hde-xsettings; do check "$p is running" running $p; done
WM=$(sed -n 's/^hde-session: starting window manager \([A-Za-z0-9]*\).*/\1/p' "$OUT/session.log" | head -n1)
if [ -n "$WM" ]; then pass "window manager started: $WM"; else fail "window manager started"; fi
if command -v metacity >/dev/null 2>&1; then
    if [ "$WM" = Metacity ]; then pass "auto mode prefers a GTK window manager (Metacity over Openbox/Xfwm4)"
    else fail "auto mode prefers a GTK window manager (got '$WM')"; fi
fi
check "panel publishes _HDE_PANEL_WINDOW" sh -c "[ \"\$($XT root-window _HDE_PANEL_WINDOW)\" != 0 ]"
pgeo "0 766 1280 34" "the panel: 34 px high, its bottom edge on the bottom of the screen"
l=$(grep "^hde-panel: measured: " "$OUT/session.log" | tail -n 1)
case "$l" in
    *"panel 0,766 1280x34, 34 px reserved at the bottom: fits") pass "the panel measures the screen and its own window: ${l#hde-panel: measured: }" ;;
    *) fail "the panel measures the screen and its own window (${l:-nothing logged})" ;;
esac
"$B/hde-panel" --measure > "$OUT/measure.txt" 2>&1; rc=$?
if [ "$rc" = 0 ] && grep -q "^Result: *fits" "$OUT/measure.txt" && grep -q "^  window: *1280x34 at 0,766$" "$OUT/measure.txt"; then
    pass "hde-panel --measure: the panel fits; windows get $(sed -n 's/^Windows get: *//p' "$OUT/measure.txt")"
else fail "hde-panel --measure (exit $rc): $(tr '\n' '|' < "$OUT/measure.txt")"; fi
sed 's/^/INFO:   /' "$OUT/measure.txt" >> "$OUT/results.txt"
# someone else (a window manager, xdotool) moves the panel or makes it higher: it measures that and puts itself right
PW=$($XT root-window _HDE_PANEL_WINDOW)
c0=$(ncorr); xdotool windowmove "$PW" 0 520 2>/dev/null; sleep 2.5; c1=$(ncorr)
pgeo "0 766 1280 34" "the panel moved away by someone else is back at the bottom edge ($([ "$c1" -gt "$c0" ] && echo "it measured that and moved back" || echo "the window manager did not move it"))"
xdotool windowsize "$PW" 1280 64 2>/dev/null; sleep 2.5; c2=$(ncorr)
pgeo "0 766 1280 34" "the panel made higher by someone else is 34 px again ($([ "$c2" -gt "$c1" ] && echo "it measured that and put itself right" || echo "the window manager did not resize it"))"
grep "^hde-panel: measured: the panel window is at" "$OUT/session.log" | tail -n 4 | sed 's/^/INFO:   /' >> "$OUT/results.txt"
check "hde-xsettings owns _XSETTINGS_S0" $XT xsettings
check "hde-xsettings runs HDE's input service (_HDE_INPUT_S0)" sh -c "[ \"\$($XT selection-owner _HDE_INPUT_S0)\" != 0 ]"
check "the session log starts with the HDE build (commit) that runs" grep -q "^hde-session: HDE build " "$OUT/session.log"
check "no touchpad at login: the Touchpad scrolling window is not opened" \
    grep -q "hde-settings: touchpad setup: not shown, no touchpad found" "$OUT/session.log"
BASE_POPUPS=$(popups)       # override-redirect windows right after login (no menu, OSD or notification open)

# the screen layouts of F8 (PC screen only / Duplicate / Extend / Second screen only) without an X server
if [ -x "$B/randr-plan-test" ]; then
    "$B/randr-plan-test" > "$OUT/randr-plan-test.txt" 2>&1
    while IFS= read -r l; do
        case "$l" in PASS:*) pass "${l#PASS: }" ;; FAIL:*) fail "${l#FAIL: }" ;; esac
    done < "$OUT/randr-plan-test.txt"
else
    skip "randr-plan-test not built (make build/randr-plan-test)"
fi
# the SVG path reader that draws the logos of the distributions (data/logos), without an X server
if [ -x "$B/svgpath-test" ]; then
    "$B/svgpath-test" --logos "$HERE/../data/logos" > "$OUT/svgpath-test.txt" 2>&1
    nok=$(grep -c "^PASS: logo " "$OUT/svgpath-test.txt"); nbad=$(grep -c "^FAIL" "$OUT/svgpath-test.txt")
    if [ "$nbad" = 0 ] && [ "${nok:-0}" -ge 20 ]; then pass "the SVG path reader draws all $nok bundled distribution logos (and passes its unit checks)"
    else fail "the SVG path reader: $nbad failure(s): $(grep '^FAIL' "$OUT/svgpath-test.txt" | head -n 3 | tr '\n' ' ')"; fi
else
    skip "svgpath-test not built (make build/svgpath-test)"
fi

# the batteries (src/hde-power.c) with fake /sys/class/power_supply trees, without an X server
if [ -x "$B/power-test" ]; then
    "$B/power-test" > "$OUT/power-test.txt" 2>&1
    while IFS= read -r l; do
        case "$l" in PASS:*) pass "${l#PASS: }" ;; FAIL:*) fail "${l#FAIL: }" ;; esac
    done < "$OUT/power-test.txt"
else
    skip "power-test not built (make build/power-test)"
fi

# measuring the screen and the panel (src/hde-measure.c): where it belongs, what is wrong when it is not there
if [ -x "$B/measure-test" ]; then
    "$B/measure-test" > "$OUT/measure-test.txt" 2>&1
    while IFS= read -r l; do
        case "$l" in PASS:*) pass "${l#PASS: }" ;; FAIL:*) fail "${l#FAIL: }" ;; esac
    done < "$OUT/measure-test.txt"
else
    skip "measure-test not built (make build/measure-test)"
fi

# ---------- 2. Super key -> Start menu (the modern layout, like Linux Mint: the default) ----------
nlog() { nlog_n=$(grep -c "$1" "$OUT/session.log" 2>/dev/null); echo "${nlog_n:-0}"; }
n0=$(popups); xdotool key super; sleep 1.2; n1=$(popups)
if [ "$n1" -gt "$n0" ]; then pass "Super key opens the Start menu ($n0 -> $n1 popups)"; else fail "Super key opens the Start menu ($n0 -> $n1 popups)"; fi
m=$(grep "hde-panel: start menu: shown" "$OUT/session.log" | tail -n 1)
case "$m" in
    *"shown (modern)"*"places: Home, Desktop, Documents, Downloads, Music, Pictures, Videos"*)
        pass "it is the modern menu (picture, places, favorites, categories, apps)" ;;
    *) fail "it is the modern menu with the places (${m:-no 'shown' line})" ;;
esac
echo "INFO: $m" >> "$OUT/results.txt"
shot 02-start-menu
xdotool type --delay 120 "sett"; sleep 1.5
if grep -q "hde-panel: start menu: search 'sett': [1-9][0-9]* result(s), best: Hyggshi Settings" "$OUT/session.log"; then
    pass "typing in the Start menu searches at once (sett -> Hyggshi Settings)"
else fail "typing in the Start menu searches at once ($(grep "start menu: search" "$OUT/session.log" | tail -n 1))"; fi
shot 03-app-search
xdotool key Escape; sleep 0.6; n2=$(popups)
if [ "$n2" -gt "$n0" ]; then pass "Escape first clears the search (the menu stays open)"; else fail "Escape first clears the search ($n2 popups)"; fi
xdotool key Escape; sleep 0.8; n3=$(popups)
if [ "$n3" -le "$n0" ]; then pass "Escape again closes the Start menu"; else fail "Escape again closes the Start menu ($n3 popups)"; fi
xdotool key super; sleep 1.2; n1=$(popups); xdotool key super; sleep 1.2; n2=$(popups)
if [ "$n2" -lt "$n1" ]; then pass "pressing Super again closes the Start menu ($n1 -> $n2)"; else fail "pressing Super again closes the Start menu ($n1 -> $n2)"; fi
n0=$(popups); t0=$(nlog "hde-panel: toggle menu"); xdotool key super+s; sleep 1.5; n1=$(popups); t1=$(nlog "hde-panel: toggle menu")
if [ "$n1" -gt "$n0" ] && grep -q "hde-panel: command 2 " "$OUT/session.log"; then pass "Super+S opens the Start menu with its search box"
else fail "Super+S opens the Start menu with its search box ($n0 -> $n1 popups)"; fi
if [ "$t1" = "$t0" ]; then pass "Super+<key> combination does not toggle the Start menu"; else fail "Super+<key> toggled the Start menu ($t0 -> $t1)"; fi
xdotool key Escape; sleep 0.8
# regression: xdotool releases Super before S -> that release never reaches hde-hotkeys (it holds the Super+S grab); Super must still work
n0=$(popups); xdotool key super; sleep 1.2; n1=$(popups)
if [ "$n1" -gt "$n0" ]; then pass "Super still works right after a Super+<key> shortcut"; else fail "Super still works right after a Super+<key> shortcut ($n0 -> $n1)"; fi
xdotool key Escape; sleep 0.5

# ---------- 3. F1 / F2 / F3 volume ----------
if pactl info >/dev/null 2>&1; then
    vol() { pactl get-sink-volume @DEFAULT_SINK@ 2>/dev/null | grep -o '[0-9]*%' | head -n1 | tr -d %; }
    mut() { pactl get-sink-mute @DEFAULT_SINK@ 2>/dev/null | awk '{print $2}'; }
    pactl set-sink-volume @DEFAULT_SINK@ 50%; pactl set-sink-mute @DEFAULT_SINK@ 0
    xdotool key F3; sleep 0.7; shot 04-osd-volume; sleep 0.6
    v=$(vol); if [ "$v" = 55 ]; then pass "F3 raises the volume (50% -> $v%)"; else fail "F3 raises the volume (50% -> $v%)"; fi
    xdotool key F2; sleep 1.3
    v=$(vol); if [ "$v" = 50 ]; then pass "F2 lowers the volume (55% -> $v%)"; else fail "F2 lowers the volume (55% -> $v%)"; fi
    xdotool key F1; sleep 1.3
    if [ "$(mut)" = yes ]; then pass "F1 mutes"; else fail "F1 mutes (mute=$(mut))"; fi
    xdotool key F1; sleep 1.3
    if [ "$(mut)" = no ]; then pass "F1 unmutes"; else fail "F1 unmutes (mute=$(mut))"; fi
    pactl set-sink-volume @DEFAULT_SINK@ 98%; xdotool key F3; sleep 1.3
    v=$(vol); if [ "$v" = 100 ]; then pass "F3 stops at 100% (98% -> $v%)"; else fail "F3 stops at 100% (98% -> $v%)"; fi
    xdotool key XF86AudioLowerVolume; sleep 1.3
    v=$(vol); if [ "$v" = 95 ]; then pass "XF86AudioLowerVolume media key works"; else fail "XF86AudioLowerVolume (100% -> $v%)"; fi
    echo "fkeys_sound=false" >> "$SETTINGS_INI"; sleep 2.5
    pactl set-sink-volume @DEFAULT_SINK@ 50%; xdotool key F3; sleep 1.3
    v=$(vol); if [ "$v" = 50 ]; then pass "fkeys_sound=false gives F1-F3 back to applications"; else fail "fkeys_sound=false (F3 still changed volume to $v%)"; fi
    sed -i '/^fkeys_sound=/d' "$SETTINGS_INI"; sleep 2
    # what labwc's key bindings run in the Wayland session, here on X11
    pactl set-sink-volume @DEFAULT_SINK@ 50%; c0=$(nlog "hde-panel: command 5 ")
    "$B/hde-hotkeys" --action volume-up; sleep 1.3
    v=$(vol)
    if [ "$v" = 55 ] && [ "$(nlog "hde-panel: command 5 ")" -gt "$c0" ]; then
        pass "hde-hotkeys --action volume-up (the Wayland key bindings) raises the volume and shows the OSD"
    else fail "hde-hotkeys --action volume-up raises the volume and shows the OSD (50% -> $v%)"; fi
else
    skip "no PulseAudio server: F1/F2/F3 volume tests"
fi

# ---------- 3b. F6 / F7 brightness, F8 Project window (Xvfb: one screen, no backlight) ----------
wait_osd() { i=0; while [ "$(popups)" -le "$BASE_POPUPS" ] && [ $i -lt 20 ]; do sleep 0.1; i=$((i + 1)); done; [ "$(popups)" -gt "$BASE_POPUPS" ]; }
soft() { xprop -root _HDE_BRIGHTNESS 2>/dev/null | sed -n 's/.*= //p'; }
"$B/hde-settings" --brightness > "$OUT/brightness.txt" 2>&1
binfo=$(head -n 1 "$OUT/brightness.txt")
echo "INFO: brightness in Xvfb: $binfo" | tee -a "$OUT/results.txt"
sleep 1.6
# why a key did nothing: hde-hotkeys writes the reason into the session log (a key another program holds, a brightness
# that cannot be changed and why). Printed into the results, which is what the CI publishes and the artifact keeps.
keys_reason() {
    grep -h "hde-hotkeys: \(already used by another program\|brightness \|.* is free again\|.* is taken\)" "$OUT/session.log" 2>/dev/null |
        tail -n 4 | sed 's/^/INFO:   /' >> "$OUT/results.txt"
    grep -h "hde-panel: command 6 " "$OUT/session.log" 2>/dev/null | tail -n 2 | sed 's/^/INFO:   /' >> "$OUT/results.txt"
    grep -h "hde-hotkeys" "$OUT/session.log" 2>/dev/null | tail -n 6 | sed 's/^/INFO:   /' >> "$OUT/results.txt"
}
case "$binfo" in
    "software dimming"*)
        xdotool key F6
        if wait_osd; then pass "F6 shows the brightness OSD"; else fail "F6 shows the brightness OSD"; keys_reason; fi
        shot 04b-osd-brightness
        sleep 1
        if [ "$(soft)" = 95 ]; then pass "F6 dims the screen (software dimming: no backlight in a VM) to 95%"
        else fail "F6 dims the screen to 95% (_HDE_BRIGHTNESS=$(soft))"; keys_reason; fi
        if ! grep -q "hde-panel: command 6 (time [0-9]*, arg 95)" "$OUT/session.log"; then keys_reason; fi
        check "... the panel got the level for its OSD" grep -q "hde-panel: command 6 (time [0-9]*, arg 95)" "$OUT/session.log"
        xdotool key F7; sleep 1.5
        if [ "$(soft)" = 100 ]; then pass "F7 brightens it again (100%)"; else fail "F7 brightens it again (_HDE_BRIGHTNESS=$(soft))"; keys_reason; fi
        echo "fkeys_display=false" >> "$SETTINGS_INI"; sleep 2.5
        xdotool key F6; sleep 1.5
        if [ "$(soft)" = 100 ]; then pass "fkeys_display=false gives F6/F7/F8 back to applications"
        else fail "fkeys_display=false gives F6/F7/F8 back to applications (F6 still dimmed: $(soft))"; keys_reason; fi
        sed -i '/^fkeys_display=/d' "$SETTINGS_INI"; sleep 2 ;;
    *)
        n0=$(grep -c "from HDE: Brightness" "$OUT/session.log")
        xdotool key F6; sleep 2
        n1=$(grep -c "from HDE: Brightness" "$OUT/session.log")
        if [ "$n1" -gt "$n0" ]; then pass "F6 where nothing can change the brightness: a notification says why (no error dialog)"
        else fail "F6 where nothing can change the brightness: a notification says why"; keys_reason; fi
        shot 04b-brightness-impossible ;;
esac

# ---------- 3c. software brightness is kept for the next session (hde-xsettings: $XDG_STATE_HOME/hde/state.ini) ----------
case "$binfo" in
    "software dimming"*)
        STATE_INI="$HOME/.local/state/hde/state.ini"
        "$B/hde-settings" --brightness 60 > /dev/null 2>&1; sleep 3
        if grep -q "^soft_brightness=60" "$STATE_INI" 2>/dev/null; then pass "the software brightness is written down for the next session (soft_brightness=60)"
        else fail "the software brightness is written down for the next session ($(tr '\n' ' ' < "$STATE_INI" 2>/dev/null))"; fi
        # a new login: the X server forgot it, the display service starts again
        xprop -root -remove _HDE_BRIGHTNESS; pkill -KILL -x hde-xsettings
        i=0; while [ "$i" -lt 60 ] && [ "$(soft)" != 60 ]; do sleep 0.2; i=$((i + 1)); done
        if [ "$(soft)" = 60 ]; then pass "... and put back when the display service starts (the next login): 60%"
        else fail "... and put back when the display service starts (_HDE_BRIGHTNESS=$(soft))"; fi
        check "... the session log says so" grep -q "hde-xsettings: software brightness 60% (as in the last session)" "$OUT/session.log"
        sleep 1
        "$B/hde-settings" --brightness 100 > /dev/null 2>&1; sleep 3
        check "100% is written down too (no dimming at the next login)" grep -q "^soft_brightness=100" "$STATE_INI" ;;
esac
xdotool key F8
i=0; while ! xdotool search --onlyvisible --name "^Project$" >/dev/null 2>&1 && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
if xdotool search --onlyvisible --name "^Project$" >/dev/null 2>&1; then
    pass "F8 opens the Project window (PC screen only / Duplicate / Extend / Second screen only)"
    sleep 1
    shot 04c-project-one-screen
    check "... it knows there is only one screen" grep -q "hde-settings: project: shown: 1 screen(s)" "$OUT/session.log"
    xdotool key F8; sleep 1.5
    nwin=$(xdotool search --onlyvisible --name "^Project$" 2>/dev/null | wc -l)
    if [ "$nwin" = 1 ] && grep -q "hde-settings: project: already open: next layout" "$OUT/session.log"; then
        pass "F8 again does not open a second Project window (it tells the open one to move on)"
    else fail "F8 again does not open a second Project window ($nwin windows)"; fi
    xdotool key Escape; sleep 1
    if xdotool search --onlyvisible --name "^Project$" >/dev/null 2>&1; then fail "Esc closes the Project window"
    else pass "Esc closes the Project window"; fi
else
    fail "F8 opens the Project window"
    grep -E "hde-(hotkeys|settings)" "$OUT/session.log" | tail -n 6 | sed 's/^/INFO:   /' | tee -a "$OUT/results.txt"
fi

# ---------- 4. notifications ----------
if command -v gdbus >/dev/null 2>&1; then
    info=$(gdbus call --session --dest org.freedesktop.Notifications --object-path /org/freedesktop/Notifications \
           --method org.freedesktop.Notifications.GetServerInformation 2>&1)
    case "$info" in *"HDE Notifications"*) pass "notification daemon answers on D-Bus" ;; *) fail "notification daemon answers on D-Bus ($info)" ;; esac
fi
if command -v notify-send >/dev/null 2>&1; then
    n0=$(popups)
    notify-send -a "Smoke test" -i dialog-information "Hello from HDE" "Notifications work — <b>bold</b> and <i>italic</i> body text"
    sleep 1; n1=$(popups); shot 05-notification
    if [ "$n1" -gt "$n0" ]; then pass "notify-send shows a popup"; else fail "notify-send shows a popup ($n0 -> $n1)"; fi
fi

# ---------- 4b. screenshots: HDE's own hde-screenshot (no scrot needed), PrtSc belongs to HDE ----------
SHOTS="$HOME/Pictures/Screenshots"
nshots() { ls "$SHOTS"/*.png 2>/dev/null | wc -l; }
pngsize() {
    python3 -c 'import struct, sys
d = open(sys.argv[1], "rb").read(24)
print("%dx%d" % struct.unpack(">II", d[16:24]) if d[:8] == b"\x89PNG\r\n\x1a\n" else "not-png")' "$1" 2>/dev/null || echo missing
}
newest_shot() { ls -t "$SHOTS"/*.png 2>/dev/null | head -n1; }
check "hde-screenshot is built" test -x "$B/hde-screenshot"
hk=$(grep -n "starting system hotkeys" "$OUT/session.log" | head -n1 | cut -d: -f1)
wl=$(grep -n "starting window manager" "$OUT/session.log" | head -n1 | cut -d: -f1)
if [ -n "$hk" ] && [ -n "$wl" ] && [ "$hk" -lt "$wl" ]; then pass "hde-hotkeys starts before the window manager (so the WM cannot take PrtSc, Super+E, ...)"
else fail "hde-hotkeys starts before the window manager (hotkeys at log line ${hk:-?}, WM at ${wl:-?})"; fi
if grep -q "system hotkeys.*not ready" "$OUT/session.log"; then fail "hde-hotkeys tells hde-session when its keys are grabbed (HDE_READY_FD)"
else pass "hde-hotkeys tells hde-session when its keys are grabbed (HDE_READY_FD)"; fi
n0=$(nshots); xdotool key Print; sleep 3; n1=$(nshots)
if [ "$n1" -gt "$n0" ]; then pass "Print saves a screenshot with the built-in tool ($n0 -> $n1 in ~/Pictures/Screenshots)"
else
    fail "Print saves a screenshot with the built-in tool ($n0 -> $n1)"
    grep -E "hde-(hotkeys|screenshot)" "$OUT/session.log" | tail -n 6 | sed 's/^/INFO:   /' | tee -a "$OUT/results.txt"
fi
sz=$(pngsize "$(newest_shot)")
if [ "$sz" = 1280x800 ]; then pass "Print: the screenshot is a full-screen PNG ($sz)"; else fail "Print: the screenshot is a full-screen PNG ($sz)"; fi
shot 05b-screenshot-notification
if grep -q "hde-screenshot: notification [1-9]" "$OUT/session.log"; then pass "a notification announces the screenshot (Open / Show in Folder)"
else fail "a notification announces the screenshot"; fi
owner=$($XT selection-owner CLIPBOARD)
targets=$($XT selection-targets CLIPBOARD 2>/dev/null)
if [ "$owner" != 0 ] && running hde-screenshot && echo " $targets " | grep -q " image/png "; then
    pass "the screenshot is on the clipboard as an image (served by hde-screenshot)"
else fail "the screenshot is on the clipboard as an image (owner=$owner, targets: $targets)"; fi
# Ctrl+Print: clipboard only (no file), like GNOME — and no Print combination is left over for a WM binding
n0=$(nshots); cb0=$($XT selection-owner CLIPBOARD)
xdotool key ctrl+Print; sleep 3
n1=$(nshots); cb1=$($XT selection-owner CLIPBOARD); targets=$($XT selection-targets CLIPBOARD 2>/dev/null)
if [ "$n1" = "$n0" ] && [ "$cb1" != 0 ] && [ "$cb1" != "$cb0" ] && echo " $targets " | grep -q " image/png "; then
    pass "Ctrl+Print copies a screenshot to the clipboard only (image/png, no file saved)"
else fail "Ctrl+Print copies a screenshot to the clipboard only (files $n0 -> $n1, owner $cb0 -> $cb1, targets: $targets)"; fi
if grep -q "hde-notify: notification [0-9]* from Screenshot: Screenshot copied \[image\]" "$OUT/session.log"; then
    pass "a notification with a preview says the screenshot was copied"
else fail "a notification with a preview says the screenshot was copied"; fi
shot 05b2-clipboard-notification
n0=$(nshots); xdotool key shift+Print; sleep 1.5
if xdotool search --onlyvisible --name "hde-screenshot area" >/dev/null 2>&1; then
    pass "Shift+Print shows the area selection overlay"
    shot 05c-area-overlay
    xdotool key Escape; sleep 1
    n1=$(nshots)
    if [ "$n1" = "$n0" ] && ! xdotool search --onlyvisible --name "hde-screenshot area" >/dev/null 2>&1; then
        pass "Esc cancels the area selection (nothing saved)"
    else fail "Esc cancels the area selection ($n0 -> $n1 files)"; fi
else
    fail "Shift+Print shows the area selection overlay"
fi
"$B/hde-screenshot" --area --file "$OUT/area.png" --no-notify --no-clipboard > "$OUT/area.log" 2>&1 &
AREA=$!
sleep 1.5
xdotool mousemove 200 150 mousedown 1; sleep 0.2; xdotool mousemove 300 220; sleep 0.2; xdotool mousemove 400 300; sleep 0.3
shot 05d-area-drag
xdotool mouseup 1; sleep 1.5
sz=$(pngsize "$OUT/area.png")
if [ "$sz" = 200x150 ]; then pass "dragging an area saves exactly that area (200x150)"; else fail "dragging an area saves exactly that area (got $sz)"; fi
kill "$AREA" 2>/dev/null
# the mouse pointer in the picture (XFixes): only when asked for (--pointer, the Screenshot window, screenshot_pointer)
xdotool mousemove 640 400; sleep 0.4
"$B/hde-screenshot" --file "$OUT/nopointer.png" --no-notify --no-clipboard > "$OUT/nopointer.log" 2>&1
"$B/hde-screenshot" --pointer --file "$OUT/pointer.png" --no-notify --no-clipboard > "$OUT/pointer.log" 2>&1
if grep -q "hde-screenshot: pointer at 640,400 .* drawn into the picture" "$OUT/pointer.log"; then
    pass "hde-screenshot --pointer draws the mouse pointer into the picture ($(sed -n 's/^hde-screenshot: pointer at //p' "$OUT/pointer.log"))"
else fail "hde-screenshot --pointer draws the mouse pointer into the picture ($(tr '\n' ' ' < "$OUT/pointer.log"))"; fi
if command -v compare >/dev/null 2>&1; then
    d=$(compare -metric AE "$OUT/nopointer.png" "$OUT/pointer.png" null: 2>&1 | sed 's/[^0-9].*//')
    if [ "${d:-0}" -gt 20 ] && [ "${d:-0}" -lt 5000 ]; then pass "... the two pictures differ only where the pointer is ($d pixels)"
    else fail "... the two pictures differ only where the pointer is (${d:-?} pixels)"; fi
fi
check "... without it there is no pointer (the default)" sh -c "! grep -q 'pointer at' '$OUT/nopointer.log'"
if ! command -v scrot >/dev/null 2>&1; then
    echo "screenshot_tool=scrot" >> "$SETTINGS_INI"; sleep 2.5
    n0=$(nshots); xdotool key Print; sleep 3; n1=$(nshots)
    if [ "$n1" -gt "$n0" ] && grep -q "screenshot_tool=scrot is not installed; using hde-screenshot" "$OUT/session.log"; then
        pass "screenshot_tool=scrot without scrot installed falls back to the built-in tool (no error)"
    else fail "screenshot_tool=scrot without scrot installed falls back to the built-in tool ($n0 -> $n1)"; fi
    sed -i '/^screenshot_tool=/d' "$SETTINGS_INI"; sleep 2
fi

# the Screenshot window: Start menu > Screenshot (hde-screenshot --ui)
"$B/hde-screenshot" --ui > "$OUT/screenshot-ui.log" 2>&1 &
SUI=$!
i=0; while ! xdotool search --onlyvisible --name "^Screenshot$" >/dev/null 2>&1 && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
if xdotool search --onlyvisible --name "^Screenshot$" >/dev/null 2>&1; then
    pass "the Screenshot window opens (whole screen / active window / area, delay)"
    sleep 1
    shot 05k-screenshot-window
    xdotool search --onlyvisible --name "^Screenshot$" windowactivate --sync >/dev/null 2>&1
    n0=$(nshots); xdotool key Return; sleep 3; n1=$(nshots)
    if [ "$n1" -gt "$n0" ] && grep -q "hde-screenshot: window: saved .* (1280x800)" "$OUT/screenshot-ui.log"; then
        pass "Take Screenshot saves the whole screen and shows it with Copy / Save As / Open / Show in Folder"
    else fail "Take Screenshot in the Screenshot window ($n0 -> $n1 files; $(tail -n 2 "$OUT/screenshot-ui.log" | tr '\n' ' '))"; fi
    shot 05l-screenshot-result
    xdotool key Escape; sleep 0.8
    if xdotool search --onlyvisible --name "^Screenshot$" >/dev/null 2>&1; then fail "Esc closes the Screenshot window"
    else pass "Esc closes the Screenshot window"; fi
else
    fail "the Screenshot window opens"
fi
kill "$SUI" 2>/dev/null

# ---------- 4c. desktop icons: visible selection frame + each icon's own context menu ----------
# Fresh profile, icons sorted by name in the first column: Home y=16, aaa-folder y=124, bbb-notes.txt y=232,
# hde.desktop y=340 (x=16, cells 92x100). (22, y+40) is inside a cell but beside the picture and the label.
px() { $XT pixel "$1" "$2" 2>/dev/null || echo "0 0 0"; }
cdist() { python3 -c 'import sys
a = [int(v) for v in sys.argv[1].split()]; b = [int(v) for v in sys.argv[2].split()]
print(sum(abs(p - q) for p, q in zip(a, b)))' "$1" "$2"; }
icon_menu_log() { grep -F "hde-desktop: icon menu for" "$OUT/session.log" | tail -n1; }
away() { xdotool mousemove 250 650; sleep 0.4; }     # empty desktop, away from icons and dialogs
away
bg=$(px 22 272); bg_folder=$(px 22 164); bg_link=$(px 22 380)
xdotool mousemove 62 262 click 1; sleep 0.3; away
sel=$(px 22 272)
if [ "$(cdist "$bg" "$sel")" -ge 60 ]; then pass "clicking a desktop icon shows a clearly visible selection frame (pixel $bg -> $sel)"
else fail "clicking a desktop icon shows a clearly visible selection frame (pixel $bg -> $sel)"; fi
shot 05e-icon-selected
xdotool click 1; sleep 0.5
clr=$(px 22 272)
if [ "$(cdist "$bg" "$clr")" -le 12 ]; then pass "clicking the empty desktop removes the selection frame"
else fail "clicking the empty desktop removes the selection frame ($bg -> $clr)"; fi
xdotool mousemove 62 262; sleep 0.5; hov=$(px 22 272); away
if [ "$(cdist "$bg" "$hov")" -ge 15 ]; then pass "hovering a desktop icon highlights it"; else fail "hovering a desktop icon highlights it ($bg -> $hov)"; fi
# rubber band from the empty desktop across aaa-folder and bbb-notes.txt (not Home, not hde.desktop)
xdotool mousemove 130 120 mousedown 1; sleep 0.2; xdotool mousemove 70 200; sleep 0.2; xdotool mousemove 10 250; sleep 0.3
shot 05e2-rubber-band
xdotool mouseup 1; sleep 0.3; away
if [ "$(cdist "$bg_folder" "$(px 22 164)")" -ge 60 ] && [ "$(cdist "$bg" "$(px 22 272)")" -ge 60 ] && [ "$(cdist "$bg_link" "$(px 22 380)")" -le 12 ]; then
    pass "dragging a rubber band on the desktop selects the icons it touches"
else fail "dragging a rubber band on the desktop selects the icons it touches"; fi
xdotool click 1; sleep 0.4

nd0=$(grep -c "hde-desktop: desktop menu" "$OUT/session.log")
xdotool mousemove 62 262 click 3; sleep 1.2
shot 05f-icon-menu
m=$(icon_menu_log)
case "$m" in
    *"bbb-notes.txt: Open | Open With | Cut | Copy | Rename… | Move to Trash | Properties")
        pass "right-click on a file icon opens the icon's own menu (Open, Open With, Cut, Copy, Rename, Trash, Properties)" ;;
    *) fail "right-click on a file icon opens the icon's own menu (log: ${m:-nothing})" ;;
esac
nd1=$(grep -c "hde-desktop: desktop menu" "$OUT/session.log")
if [ "$nd1" = "$nd0" ]; then pass "right-click on an icon does not open the desktop menu"; else fail "right-click on an icon opened the desktop menu"; fi
xdotool key Escape; sleep 0.5
xdotool mousemove 250 650 click 3; sleep 1.2
nd2=$(grep -c "hde-desktop: desktop menu" "$OUT/session.log")
if [ "$nd2" -gt "$nd1" ]; then pass "right-click on the empty desktop still opens the desktop menu"; else fail "right-click on the empty desktop still opens the desktop menu"; fi
shot 05g-desktop-menu
xdotool key Escape; sleep 0.5
xdotool mousemove 62 154 click 3; sleep 1.2
case "$(icon_menu_log)" in
    *"aaa-folder: Open | Open With | Open in Terminal | Cut | Copy | Rename… | Move to Trash | Properties") pass "a folder's icon menu also has Open in Terminal" ;;
    *) fail "a folder's icon menu also has Open in Terminal ($(icon_menu_log))" ;;
esac
xdotool key Escape; sleep 0.5
xdotool mousemove 62 46 click 3; sleep 1.2
case "$(icon_menu_log)" in
    *"$HOME: Open | Open With | Open in Terminal | Properties") pass "the Home icon's menu has no Cut / Rename / Trash" ;;
    *) fail "the Home icon's menu has no Cut / Rename / Trash ($(icon_menu_log))" ;;
esac
xdotool key Escape; sleep 0.5
# Properties (mnemonic P) of the launcher
xdotool mousemove 62 370 click 3; sleep 1; xdotool key p; sleep 1.5
if xdotool search --onlyvisible --name "hde.desktop.*Properties" >/dev/null 2>&1; then
    pass "Properties in the icon menu opens a Properties window"
    shot 05h-properties
    xdotool key Escape; sleep 0.8
    if xdotool search --onlyvisible --name "hde.desktop.*Properties" >/dev/null 2>&1; then fail "Esc closes the Properties window"; fi
else
    fail "Properties in the icon menu opens a Properties window"
fi
# Rename (mnemonic R): bbb-notes.txt -> zzz-notes.txt (then sorted last: y=340)
xdotool mousemove 62 262 click 3; sleep 1
case "$(icon_menu_log)" in
    *bbb-notes.txt:*)
        xdotool key r; sleep 1.5
        if xdotool search --onlyvisible --name "^Rename$" >/dev/null 2>&1; then
            shot 05i-rename
            xdotool key ctrl+a; xdotool type --delay 30 "zzz-notes.txt"; xdotool key Return; sleep 1.5
            if [ -f "$HOME/Desktop/zzz-notes.txt" ] && [ ! -e "$HOME/Desktop/bbb-notes.txt" ]; then pass "Rename… in the icon menu renames the file"
            else fail "Rename… in the icon menu renames the file ($(ls "$HOME/Desktop" | tr '\n' ' '))"; fi
        else
            fail "Rename… in the icon menu opens the Rename dialog"
        fi ;;
    *) fail "Rename…: the right-click did not open the menu of bbb-notes.txt"; xdotool key Escape ;;
esac
# Move to Trash (mnemonic M)
xdotool mousemove 62 370 click 3; sleep 1
case "$(icon_menu_log)" in
    *zzz-notes.txt:*)
        xdotool key m; sleep 1.5
        if [ -f "$XDG_DATA_HOME/Trash/files/zzz-notes.txt" ] && [ ! -e "$HOME/Desktop/zzz-notes.txt" ]; then
            pass "Move to Trash in the icon menu moves the file to the Trash"
        else fail "Move to Trash in the icon menu moves the file to the Trash"; fi ;;
    *) fail "Move to Trash: the right-click did not open the menu of zzz-notes.txt ($(icon_menu_log))"; xdotool key Escape ;;
esac
# Copy (mnemonic C) a folder, then Paste (mnemonic P) from the desktop menu: recursive copy with a free name
xdotool mousemove 62 154 click 3; sleep 1; xdotool key c; sleep 0.5
xdotool mousemove 250 650 click 3; sleep 1; xdotool key p; sleep 2
if [ -f "$HOME/Desktop/aaa-folder (copy)/inside.txt" ]; then pass "Copy + Paste duplicates a folder with its contents as \"aaa-folder (copy)\""
else fail "Copy + Paste duplicates a folder with its contents ($(ls "$HOME/Desktop" | tr '\n' ' '))"; fi
shot 05j-after-paste
# keyboard: the pasted folder is selected -> Delete moves it to the Trash
xdotool key Delete; sleep 1.5
if [ -d "$XDG_DATA_HOME/Trash/files/aaa-folder (copy)" ] && [ ! -e "$HOME/Desktop/aaa-folder (copy)" ]; then
    pass "the pasted items are selected and the Delete key moves them to the Trash"
else fail "the pasted items are selected and the Delete key moves them to the Trash"; fi

# ---------- 5. Settings: Wi-Fi, Bluetooth ----------
"$B/hde-settings" network > "$OUT/settings.log" 2>&1 &
SETTINGS=$!
sleep 4
check "Settings window opens" xdotool search --onlyvisible --name "Hyggshi Settings"
shot 06-settings-wifi
if grep -q "device wifi list" "$OUT/nmcli.log" 2>/dev/null; then pass "Network page asks NetworkManager for the Wi-Fi list"; else fail "Network page asks NetworkManager for the Wi-Fi list"; fi

# Super while an application has focus (the most common real-world case)
n0=$(popups); xdotool key super; sleep 1.5; n1=$(popups)
if [ "$n1" -gt "$n0" ]; then pass "Super opens the Start menu while an application window has focus ($n0 -> $n1)"
else
    fail "Super opens the Start menu while an application window has focus ($n0 -> $n1)"
    grep -E "hde-(panel|hotkeys):" "$OUT/session.log" | tail -n 14 | sed 's/^/INFO:   /' | tee -a "$OUT/results.txt"
fi
shot 06b-menu-over-app
xdotool key Escape; sleep 0.8

# Wi-Fi connection with a password: click the "Neighbor 5G" row (fixed position: 1020x700 window centered on a 1280x800 screen)
xdotool mousemove 600 436 click 1; sleep 1.5
if xdotool search --onlyvisible --name "Wi-Fi Network Authentication" >/dev/null 2>&1; then
    pass "choosing a secured Wi-Fi network asks for its password"
    shot 06c-wifi-password
    xdotool type --delay 40 "correct-horse"; xdotool key Return; sleep 2.5
    if grep -q "PW-STDIN-LEN: 13" "$OUT/nmcli.log"; then pass "Wi-Fi password is handed to nmcli on stdin"; else fail "Wi-Fi password is handed to nmcli on stdin"; fi
    if grep "^ARGS:" "$OUT/nmcli.log" | grep -q "correct-horse"; then fail "Wi-Fi password must not appear on the nmcli command line"
    else pass "Wi-Fi password never appears on the command line (ps)"; fi
    if grep -q "ARGS: --ask -w 45 device wifi connect Neighbor 5G ifname wlan0" "$OUT/nmcli.log"; then pass "nmcli connects to the chosen network on the right device"
    else fail "nmcli connects to the chosen network ($(grep 'wifi connect' "$OUT/nmcli.log" | tail -n1))"; fi
    shot 06d-wifi-connected
else
    fail "choosing a secured Wi-Fi network asks for its password"
fi
xdotool mousemove 600 542 click 1; sleep 1.5
if xdotool search --onlyvisible --name "Wi-Fi Network Authentication" >/dev/null 2>&1; then
    xdotool type --delay 40 "wrong-password"; xdotool key Return; sleep 2.5
    if xdotool search --onlyvisible --name "Wi-Fi Network Authentication" >/dev/null 2>&1; then
        pass "a wrong Wi-Fi password asks again"
        shot 06e-wifi-wrong-password
        xdotool key Escape; sleep 1.5
        if grep -q "connection delete id Office: Guest" "$OUT/nmcli.log"; then pass "cancelling leaves no broken Wi-Fi profile behind"
        else fail "cancelling leaves no broken Wi-Fi profile behind"; fi
    else
        fail "a wrong Wi-Fi password asks again"
    fi
else
    fail "second secured network asks for its password"
fi
"$B/hde-settings" bluetooth; sleep 2.5; shot 07-settings-bluetooth
if [ -n "$BLUEZ_MOCK" ]; then
    bz PairDevice "'hci0'" "'AA:BB:CC:DD:EE:01'" || bz PairDevice "'hci0'" "'AA:BB:CC:DD:EE:01'" 9536; sleep 1.5
    shot 07b-bluetooth-live-update
    bprop() { gdbus call --system --dest org.bluez --object-path "/org/bluez/hci0/dev_$1" \
                  --method org.freedesktop.DBus.Properties.Get org.bluez.Device1 "$2" 2>/dev/null; }
    case "$(bprop 11_22_33_44_55_66 Paired)" in *true*) pass "Bluetooth: paired + connected device is listed (BlueZ mock)" ;;
        *) skip "Bluetooth mock could not pair the test device" ;; esac
    # Pair through Settings itself: the "Pair" button of "Pixel 8" (the Other devices card has 1 row left).
    # The row heights follow the font of the system, so the button is looked for around the place it sits with the
    # font the numbers were written for (x=1078: the right edge of the card, all rows end in their button).
    for y in 556 552 560 548 564 544 568 540 572; do
        xdotool mousemove 1078 "$y" click 1; sleep 2.5
        bprop AA_BB_CC_DD_EE_02 Paired | grep -q true && break
    done
    sleep 1.5
    shot 07c-bluetooth-paired-by-settings
    if bprop AA_BB_CC_DD_EE_02 Paired | grep -q true; then
        pass "Settings pairs a Bluetooth device (Device1.Pair)"
        if bprop AA_BB_CC_DD_EE_02 Trusted | grep -q true; then pass "paired device is marked trusted (auto-reconnect)"; else fail "paired device is marked trusted"; fi
        # dbusmock's Device1.Connect only emits PropertiesChanged without changing the value returned by Get -> check the call in the mock log
        if grep -Eq "^[0-9.]+ Connect( |$)" "$OUT/bluez-mock.log"; then pass "Settings connects the device after pairing (Device1.Connect)"
        else fail "Settings connects the device after pairing (Device1.Connect)"; fi
    else
        fail "Settings pairs a Bluetooth device (the Pair button of the card, 1078x540..572 — see shot 07c)"
        grep "hde-settings: bt device" "$OUT/settings.log" | tail -n 3 | cut -c1-400 | sed 's/^/INFO: /' >> "$OUT/results.txt"
    fi
    if grep -q "AgentManager1\|RegisterAgent" "$OUT/bluez-mock.log" 2>/dev/null; then :; fi
fi
"$B/hde-settings" keyboard; sleep 2; shot 08-settings-keyboard
xdotool mousemove 760 600; for i in 1 2 3 4 5 6 7 8 9 10; do xdotool click 5; done; sleep 0.6
shot 08b-settings-keyboard-screenshots
n0=$(nshots); xdotool key alt+Print; sleep 3; n1=$(nshots)
sz=$(pngsize "$(newest_shot)"); sw=${sz%x*}; sh=${sz#*x}
if [ "$n1" -gt "$n0" ] && [ "$sw" -ge 980 ] 2>/dev/null && [ "$sw" -le 1120 ] && [ "$sh" -ge 660 ] && [ "$sh" -le 790 ]; then
    pass "Alt+Print captures only the active window ($sz; Settings is 1020x700)"
else fail "Alt+Print captures only the active window (got $sz, files $n0 -> $n1)"; fi

# ---------- 5c. touchpad settings ----------
# Xvfb has no touchpad: give the XTEST pointer the properties of a libinput touchpad (driver defaults: classic
# scrolling, no tapping). The real libinput/synaptics drivers and real swipes are tested by tests/input-test.sh.
FAKE_TP="pointer:Virtual core XTEST pointer"
NAT="libinput Natural Scrolling Enabled"
TAPP="libinput Tapping Enabled"
tp_prop() { xinput list-props "$FAKE_TP" 2>/dev/null | sed -n "s/^[[:space:]]*$1 ([0-9]*):[[:space:]]*//p" | head -n 1; }
tp_wait() { i=0; while [ $i -lt 50 ]; do [ "$(tp_prop "$1")" = "$2" ] && return 0; sleep 0.1; i=$((i + 1)); done; return 1; }
# widget NAME LOG -> "X Y" = centre of a widget, from the HDE_DEBUG log of hde-settings (its last position)
widget() {
    sed -n "s/^hde-settings: widget $1 at \([0-9-]*\),\([0-9-]*\) \([0-9]*\)x\([0-9]*\)$/\1 \2 \3 \4/p" "$2" | tail -n 1 |
        awk '{ printf "%d %d\n", $1 + $3 / 2, $2 + $4 / 2 }'
}
# shellcheck disable=SC2046  # "X Y" -> two arguments
click_widget() { set -- $(widget "$1" "$2"); [ -n "${2:-}" ] && xdotool mousemove "$1" "$2" click 1; }
tp_window() { xdotool search --onlyvisible --name "^Touchpad scrolling$" >/dev/null 2>&1; }
# notification popups (bottom right) could be over a button: wait until they are gone (at most 10 s)
wait_popups() { i=0; while [ "$(popups)" -gt "$BASE_POPUPS" ] && [ $i -lt 40 ]; do sleep 0.25; i=$((i + 1)); done; }
XS_PID=$(pgrep -x hde-xsettings | head -n 1)
if command -v xinput >/dev/null 2>&1 && [ -n "$XS_PID" ] &&
   xinput set-prop --type=int --format=8 "$FAKE_TP" "$TAPP" 0 2>/dev/null &&
   xinput set-prop --type=int --format=8 "$FAKE_TP" "$NAT" 0 2>/dev/null; then
    # a. another program (a WM with its own touchpad settings, a script, xinput) changes the touchpad: HDE sets it back
    check "a touchpad appears with classic scrolling: hde-xsettings applies Settings at once (like a phone)" tp_wait "$NAT" 1
    check "... and tap to click" tp_wait "$TAPP" 1
    xinput set-prop "$FAKE_TP" "$NAT" 0
    check "another program turns natural scrolling off: hde-xsettings sets it back" tp_wait "$NAT" 1
    check "... and logs it in the session log" grep -q \
        "hde-xsettings: input (changed by another program, set back): Virtual core XTEST pointer (touchpad, libinput): natural scrolling on" \
        "$OUT/session.log"

    # b. Settings > Input on its own (hde-xsettings paused: only Settings can change the device now)
    kill -STOP "$XS_PID"
    xinput set-prop "$FAKE_TP" "$TAPP" 0; xinput set-prop "$FAKE_TP" "$NAT" 0
    "$B/hde-settings" input; sleep 2.5
    check "opening Settings > Input applies the scroll direction to a touchpad (default: like a phone)" tp_wait "$NAT" 1
    check "opening Settings > Input applies tap to click (default on)" tp_wait "$TAPP" 1
    if grep -q "hde-settings: input device: Virtual core XTEST pointer: Touchpad · libinput driver · scrolls like a phone · tap to click on" "$OUT/settings.log"; then
        pass "Settings > Input lists the touchpad with its real state"
    else fail "Settings > Input lists the touchpad with its real state"; fi
    shot 08c-settings-input
    if click_widget input-wheel "$OUT/settings.log"; then
        sleep 1.2
        if tp_wait "$NAT" 0 && grep -q "^natural_scroll=false" "$SETTINGS_INI"; then
            pass "choosing the 'Like a mouse wheel' card switches the touchpad to the classic direction at once"
        else fail "choosing the 'Like a mouse wheel' card switches the touchpad to the classic direction at once"; fi
        shot 08d-settings-input-mouse-wheel
        click_widget input-phone "$OUT/settings.log"; sleep 1.2
        check "choosing 'Like a phone' turns natural scrolling on again" tp_wait "$NAT" 1
    else
        fail "the scroll direction cards are on Settings > Input (see shot 08c)"
    fi

    # c. "Try both…": the Touchpad scrolling window with its test page
    wait_popups
    if click_widget input-try "$OUT/settings.log" && sleep 2 && tp_window; then
        pass "'Try both…' opens the Touchpad scrolling window"
        sleep 0.6
        # shellcheck disable=SC2046
        set -- $(widget setup-test-page "$OUT/settings.log")
        if [ -n "${2:-}" ]; then
            xdotool mousemove "$1" "$2"; sleep 0.3; xdotool click --repeat 3 --delay 120 5; sleep 1
            check "the test page tells which way the page moved (wheel down: toward the end)" \
                grep -q "hde-settings: touchpad setup: test page: line [0-9]* -> [0-9]* (toward the end)" "$OUT/settings.log"
        else fail "the test page is in the Touchpad scrolling window"; fi
        shot 08e-touchpad-setup
        click_widget setup-wheel "$OUT/settings.log"; sleep 1.2
        check "the window's 'Like a mouse wheel' applies at once" tp_wait "$NAT" 0
        check "... and is saved (natural_scroll=false)" grep -q "^natural_scroll=false" "$SETTINGS_INI"
        shot 08f-touchpad-setup-mouse-wheel
        click_widget setup-done "$OUT/settings.log"; sleep 1
        if tp_window; then fail "Done closes the Touchpad scrolling window"; else pass "Done closes the Touchpad scrolling window"; fi
        check "the choice is remembered: the window is not opened at the next login" \
            grep -q "^touchpad_direction_chosen=true" "$SETTINGS_INI"
        shot 08g-settings-input-after-window
    else
        fail "'Try both…' opens the Touchpad scrolling window (see shot 08c)"
        xdotool key Escape 2>/dev/null
    fi

    # d. hde-settings --apply (login) while hde-xsettings is still paused
    sed -i 's/^natural_scroll=.*/natural_scroll=true/' "$SETTINGS_INI"
    xinput set-prop "$FAKE_TP" "$NAT" 0
    "$B/hde-settings" --apply > "$OUT/settings-apply.log" 2>&1
    check "hde-settings --apply (login) applies the scroll direction" tp_wait "$NAT" 1
    kill -CONT "$XS_PID"

    # e. settings.ini changes: hde-xsettings applies them
    sed -i 's/^natural_scroll=.*/natural_scroll=false/' "$SETTINGS_INI"
    check "natural_scroll=false in settings.ini: hde-xsettings switches the touchpad to the classic direction" \
        tp_wait "$NAT" 0
    sed -i 's/^natural_scroll=.*/natural_scroll=true/' "$SETTINGS_INI"
    check "natural_scroll=true: back to natural scrolling" tp_wait "$NAT" 1

    # f. the first login with a touchpad opens the window by itself, once (hde-session runs this 2 s after login)
    timeout 20 "$B/hde-settings" --touchpad-setup=auto > "$OUT/touchpad-setup-auto.log" 2>&1
    check "once a direction was chosen, the Touchpad scrolling window does not open at login" \
        grep -q "touchpad setup: not shown, the direction was chosen already" "$OUT/touchpad-setup-auto.log"
    sed -i '/^touchpad_direction_chosen=/d' "$SETTINGS_INI"
    "$B/hde-settings" --touchpad-setup=auto > "$OUT/touchpad-setup-auto.log" 2>&1 &
    TS=$!
    sleep 3
    if tp_window; then
        pass "first login with a touchpad: the Touchpad scrolling window opens by itself"
        shot 08h-touchpad-setup-first-login
        wait_popups
        click_widget setup-done "$OUT/touchpad-setup-auto.log"; sleep 1.2
        if kill -0 "$TS" 2>/dev/null; then fail "Done closes the window opened at login"; kill "$TS" 2>/dev/null
        else pass "Done closes the window opened at login"; fi
    else
        fail "first login with a touchpad: the Touchpad scrolling window opens by itself"
        kill "$TS" 2>/dev/null
    fi
    # g. scrolling over the panel's volume icon follows the fingers: up = louder, also with natural scrolling, where the
    #    X driver turns a two-finger swipe UP into "scroll down" (button 5) so that pages follow the fingers
    # shellcheck disable=SC2046
    set -- $(sed -n 's/^hde-panel: widget volume at \([0-9-]*\),\([0-9-]*\) \([0-9]*\)x\([0-9]*\)$/\1 \2 \3 \4/p' \
        "$OUT/session.log" | tail -n 1 | awk '{ printf "%d %d\n", $1 + $3 / 2, $2 + $4 / 2 }')
    if [ -n "${2:-}" ] && pactl info >/dev/null 2>&1; then
        sed -i 's/^natural_scroll=.*/natural_scroll=true/' "$SETTINGS_INI"
        tp_wait "$NAT" 1
        pactl set-sink-volume @DEFAULT_SINK@ 50%; pactl set-sink-mute @DEFAULT_SINK@ 0
        wait_popups
        xdotool mousemove "$1" "$2"; sleep 1.5
        xdotool click 5; sleep 1.3
        v=$(vol)
        if [ "$v" = 55 ]; then pass "natural scrolling: a swipe UP over the volume icon (it arrives as 'scroll down') turns the volume UP (50% -> $v%)"
        else fail "natural scrolling: a swipe UP over the volume icon (it arrives as 'scroll down') turns the volume UP (50% -> $v%)"; fi
        xdotool click 4; sleep 1.3
        v=$(vol)
        if [ "$v" = 50 ]; then pass "natural scrolling: a swipe DOWN over the volume icon turns the volume down (55% -> $v%)"
        else fail "natural scrolling: a swipe DOWN over the volume icon turns the volume down (55% -> $v%)"; fi
        sed -i 's/^natural_scroll=.*/natural_scroll=false/' "$SETTINGS_INI"
        tp_wait "$NAT" 0; sleep 1.3
        xdotool click 4; sleep 1.3
        v=$(vol)
        if [ "$v" = 55 ]; then pass "classic direction: a swipe (or wheel) UP over the volume icon turns the volume up (50% -> $v%)"
        else fail "classic direction: a swipe (or wheel) UP over the volume icon turns the volume up (50% -> $v%)"; fi
        grep 'hde-panel: volume: scroll' "$OUT/session.log" | tail -n 3 | sed 's/^/INFO:   /' >> "$OUT/results.txt"
        sed -i 's/^natural_scroll=.*/natural_scroll=true/' "$SETTINGS_INI"
        tp_wait "$NAT" 1
        xdotool mousemove 640 400
    else
        skip "volume icon scrolling (no PulseAudio, or the panel did not report the volume icon)"
    fi
    xinput delete-prop "$FAKE_TP" "$NAT" 2>/dev/null
    xinput delete-prop "$FAKE_TP" "$TAPP" 2>/dev/null
else
    skip "touchpad settings (needs xinput)"
fi
"$B/hde-settings" appearance; sleep 2; shot 09-settings-appearance-light
sidebar "Light mode"
"$B/hde-settings" display; sleep 2.5; shot 09b-settings-display
"$B/hde-settings" about; sleep 3.5; shot 09c-settings-about
xdotool mousemove 760 520; for i in 1 2 3 4 5 6 7; do xdotool click 5; done; sleep 1; shot 09d-settings-about-memory
if grep -q "hde-settings: about: HDE uses " "$OUT/settings.log"; then
    pass "Settings > About shows how much memory HDE uses ($(sed -n 's/^hde-settings: about: HDE uses \([^(]*\).*/\1/p' "$OUT/settings.log" | head -n 1))"
else fail "Settings > About shows how much memory HDE uses"; fi
"$B/hde-settings" --about > "$OUT/about.txt" 2>&1
if grep -q "^Desktop memory now: " "$OUT/about.txt" && grep -q " hde-panel " "$OUT/about.txt"; then
    pass "hde-settings --about: $(sed -n 's/^Desktop memory now: //p' "$OUT/about.txt")"
else fail "hde-settings --about lists the memory of the desktop's programs"; fi
sed -n '/^Desktop memory now/,$p' "$OUT/about.txt" | sed 's/^/INFO: /' >> "$OUT/results.txt"
# where the desktop's memory goes (largest mappings, PSS in kB; "[anon]" = the heap)
dp=$(pgrep -x hde-desktop | head -n 1)
if [ -n "$dp" ] && [ -r "/proc/$dp/smaps" ]; then
    awk '/^[0-9a-f]+-[0-9a-f]+ /{n=$6; if (n == "") n="[anon]"} /^Pss:/{p[n]+=$2} END{for (k in p) printf "%d %s\n", p[k], k}' \
        "/proc/$dp/smaps" | sort -rn | head -n 8 | sed 's/^/INFO: hde-desktop PSS kB: /' >> "$OUT/results.txt"
fi

# ---------- 6. Dark mode ----------
"$B/hde-settings" --style dark > "$OUT/style-dark.log" 2>&1
sleep 3
$XT xsettings > "$OUT/xsettings-dark.txt" 2>&1
if grep -q '^Net/ThemeName=Adwaita-dark$' "$OUT/xsettings-dark.txt"; then pass "Dark mode is published live over XSETTINGS (Adwaita-dark)"
else fail "Dark mode is published live over XSETTINGS ($(grep ThemeName "$OUT/xsettings-dark.txt"))"; fi
check "Dark mode writes gtk-3.0/settings.ini" grep -q "gtk-application-prefer-dark-theme=true" "$XDG_CONFIG_HOME/gtk-3.0/settings.ini"
check "an Adwaita-dark GTK theme is available" sh -c "[ -f '$XDG_DATA_HOME/themes/Adwaita-dark/gtk-3.0/gtk.css' ] || [ -d /usr/share/themes/Adwaita-dark ]"
shot 10-dark-mode
"$B/hde-settings" windows; sleep 2.5; shot 11-settings-windows-dark
sidebar "Dark mode"
"$B/hde-settings" network; sleep 2.5; shot 12-settings-wifi-dark
xdotool key super; sleep 1.2; shot 13-start-menu-dark; xdotool key Escape; sleep 0.5
"$B/hde-settings" --style light > "$OUT/style-light.log" 2>&1
sleep 3
$XT xsettings > "$OUT/xsettings-light.txt" 2>&1
if grep -q '^Net/ThemeName=Adwaita$' "$OUT/xsettings-light.txt"; then pass "switching back to Light mode works live"
else fail "switching back to Light mode ($(grep ThemeName "$OUT/xsettings-light.txt"))"; fi
# the accent colour follows the GTK theme ("Automatic", the default): Yaru draws its switches and selections orange,
# so HDE's own highlights (the sidebar of Settings, the Start menu...) are orange too, not HDE's blue next to them
if [ -d /usr/share/themes/Yaru-dark ]; then
    sed -i 's/^gtk_theme=.*/gtk_theme=Yaru/' "$SETTINGS_INI"
    "$B/hde-settings" --style dark > "$OUT/style-yaru.log" 2>&1
    sleep 3
    check "Yaru + Dark mode: the GTK theme is Yaru-dark" grep -q "Yaru-dark" "$OUT/style-yaru.log"
    "$B/hde-settings" appearance; sleep 2.5
    if grep -q "^hde-settings: accent: automatic #e95420 (GTK theme Yaru-dark)" "$OUT/settings.log"; then
        pass "the Automatic accent is the colour of the GTK theme (Yaru-dark: orange #e95420, like its own widgets)"
    else fail "the Automatic accent is the colour of the GTK theme ($(grep '^hde-settings: accent: ' "$OUT/settings.log" | tail -n 1))"; fi
    shot 11b-settings-yaru-dark
    sidebar "Yaru dark"
    sed -i 's/^gtk_theme=.*/gtk_theme=Adwaita/' "$SETTINGS_INI"
    "$B/hde-settings" --style light > "$OUT/style-light2.log" 2>&1
    sleep 3
    if grep -q "^hde-settings: accent: automatic #[0-9a-f]* (GTK theme Adwaita)" "$OUT/settings.log"; then pass "... and back with Adwaita: the accent follows ($(grep '^hde-settings: accent: ' "$OUT/settings.log" | tail -n 1 | sed 's/^hde-settings: accent: //'))"
    else fail "... and back with Adwaita: the accent follows ($(grep '^hde-settings: accent: ' "$OUT/settings.log" | tail -n 1))"; fi
else
    skip "the Automatic accent with Yaru (yaru-theme-gtk is not installed)"
fi
kill $SETTINGS 2>/dev/null

# ---------- 6b. the Start menu: search, keyboard, the app's menu (favorites, pin to panel), Kickoff and classic ----------
rm -f "$OUT/test-app-launched"
n0=$(popups); xdotool key super; sleep 1.2
xdotool type --delay 80 "test app"; sleep 1.2
if grep -q "start menu: search 'test app': 1 result(s), best: HDE Test App" "$OUT/session.log"; then
    pass "searching 'test app' finds HDE Test App only (every word must match)"
else fail "searching 'test app' finds HDE Test App ($(grep "start menu: search 'test app'" "$OUT/session.log" | tail -n 1))"; fi
xdotool key Menu; sleep 1
if grep -q "start menu: menu of HDE Test App: Add to Favorites | Pin to Panel | Add to Desktop" "$OUT/session.log"; then
    pass "the Menu key opens the app's own menu (Add to Favorites, Pin to Panel, Add to Desktop)"
else fail "the Menu key opens the app's own menu"; fi
shot 15a-start-menu-app-menu
xdotool key f; sleep 1.5
if grep -q "^menu_favorites=.*hde-test-app.desktop" "$SETTINGS_INI"; then pass "Add to Favorites adds the app to the favorites (menu_favorites)"
else fail "Add to Favorites adds the app to the favorites ($(grep '^menu_favorites' "$SETTINGS_INI"))"; fi
check "... and the menu shows the new favorite at once" grep -q "start menu: favorites: .*hde-test-app.desktop" "$OUT/session.log"
xdotool key Menu; sleep 1; xdotool key p; sleep 2
if grep -q "^panel_launchers=hde-test-app.desktop" "$SETTINGS_INI" && grep -q "hde-panel: pinned apps: HDE Test App" "$OUT/session.log"; then
    pass "Pin to Panel puts a button for the app on the panel"
else fail "Pin to Panel puts a button for the app on the panel ($(grep '^panel_launchers' "$SETTINGS_INI"))"; fi
shot 15b-start-menu-favorite-pinned
xdotool key Return; sleep 2
if [ -f "$OUT/test-app-launched" ]; then pass "Enter starts the app found (HDE Test App ran)"; else fail "Enter starts the app found"; fi
n1=$(popups); if [ "$n1" -le "$n0" ]; then pass "the menu closes after starting an app"; else fail "the menu closes after starting an app ($n1 popups)"; fi
xdotool key super; sleep 1.2
c0=$(nlog "hde-panel: start menu: category ")
xdotool key Left; sleep 0.3; xdotool key Down; sleep 0.3; xdotool key Down; sleep 0.8
c1=$(nlog "hde-panel: start menu: category ")
if [ "$c1" -ge "$((c0 + 2))" ]; then pass "Left / Down move through the categories with the keyboard ($(grep 'start menu: category ' "$OUT/session.log" | tail -n 1 | sed 's/.*category //'))"
else fail "Left / Down move through the categories with the keyboard ($c0 -> $c1)"; fi
shot 15c-start-menu-category
# the mouse over a category opens it (no click needed); the menu's place comes from its log line
geo=$(sed -n 's/.*start menu: shown (modern) at \([0-9]*\),\([0-9]*\) \([0-9]*\)x\([0-9]*\).*/\1 \2/p' "$OUT/session.log" | tail -n 1)
mx=${geo% *}; my=${geo#* }
if [ -n "$mx" ]; then
    c0=$(nlog "hde-panel: start menu: category ")
    xdotool mousemove "$((mx + 300))" "$((my + 160))"; sleep 0.3; xdotool mousemove "$((mx + 300))" "$((my + 200))"; sleep 1
    c1=$(nlog "hde-panel: start menu: category ")
    if [ "$c1" -gt "$c0" ]; then pass "moving the mouse over a category opens it ($(grep 'start menu: category ' "$OUT/session.log" | tail -n 1 | sed 's/.*category //'))"
    else fail "moving the mouse over a category opens it (menu at $mx,$my)"; fi
    shot 15c2-start-menu-hover
fi
xdotool key Escape; sleep 0.6
echo "menu_style=kickoff" >> "$SETTINGS_INI"; sleep 2.5
n0=$(popups); xdotool key super; sleep 1.5; n1=$(popups)
m=$(grep "hde-panel: start menu: shown" "$OUT/session.log" | tail -n 1)
case "$m" in *"shown (kickoff)"*) pass "menu_style=kickoff: the KDE Plasma-like menu ($n0 -> $n1 popups)" ;;
    *) fail "menu_style=kickoff: the KDE Plasma-like menu (${m:-nothing})" ;; esac
shot 15d-start-menu-kickoff
xdotool key Tab; sleep 1
check "Kickoff: Tab shows the Places tab" grep -q "start menu: tab places" "$OUT/session.log"
shot 15e-start-menu-kickoff-places
xdotool key Escape; sleep 0.8
sed -i 's/^menu_style=.*/menu_style=classic/' "$SETTINGS_INI"; sleep 2.5
n0=$(popups); xdotool key super; sleep 1.2; n1=$(popups)
if [ "$n1" -gt "$n0" ] && [ "$(nlog "menu popup by keyboard")" -gt 0 ]; then pass "menu_style=classic: the drop-down menu ($n0 -> $n1 popups)"
else fail "menu_style=classic: the drop-down menu ($n0 -> $n1 popups)"; fi
shot 15f-start-menu-classic
xdotool type --delay 120 "sett"; sleep 1.5
check "classic menu: typing opens the search window" xdotool search --onlyvisible --name "Search applications"
xdotool key Escape; sleep 0.8
sed -i '/^menu_style=/d' "$SETTINGS_INI"; sleep 2.5

# ---------- 6c. Settings > Panel: top of the screen, height, items, extensions, the Start button ----------
"$B/hde-settings" panel > "$OUT/settings-panel.log" 2>&1 &
SETTINGS=$!
sleep 3.5; shot 16a-settings-panel
l=$(grep "^hde-settings: panel measured: " "$OUT/settings-panel.log" | tail -n 1)
case "$l" in
    *"panel 0,766 1280x34, 34 px reserved at the bottom; windows get 1280x766 at 0,0: fits")
        pass "Settings > Panel > Screen: ${l#hde-settings: panel measured: }" ;;
    *) fail "Settings > Panel > Screen measures the screen and the panel (${l:-nothing logged})" ;;
esac
wait_popups
c0=$(grep -c "^hde-settings: panel measured: .*: fits" "$OUT/settings-panel.log")
if click_widget panel-measure "$OUT/settings-panel.log"; then
    sleep 2.8
    c1=$(grep -c "^hde-settings: panel measured: .*: fits" "$OUT/settings-panel.log")
    if grep -q "^hde-panel: asked to measure again: panel at 0,766 1280x34" "$OUT/session.log" && [ "$c1" -gt "$c0" ]; then
        pass "Settings > Panel > Screen > Measure again: the panel measures the screen again, Settings shows the result ($c0 -> $c1)"
    else fail "Settings > Panel > Screen > Measure again ($(grep -h 'measure again' "$OUT/session.log" "$OUT/settings-panel.log" | tail -n 2 | tr '\n' '|') $c0 -> $c1)"; fi
else fail "Settings > Panel > Screen: the Measure again button (not found in the log)"; fi
xdotool mousemove 760 500; for i in 1 2 3 4 5 6 7 8; do xdotool click 5; done; sleep 0.6; shot 16b-settings-panel-more
"$B/hde-settings" startmenu; sleep 2.5; shot 16c-settings-startmenu
xdotool mousemove 760 500; for i in 1 2 3 4 5 6 7 8; do xdotool click 5; done; sleep 0.6; shot 16d-settings-startmenu-more
kill "$SETTINGS" 2>/dev/null; sleep 0.5
python3 - "$SETTINGS_INI" <<'EOF'
import sys
p = sys.argv[1]
s = open(p).read().rstrip("\n") + "\n"
s = s.replace("[settings]\n", "[settings]\npanel_position=top\npanel_size=40\npanel_show_run=false\n"
              "panel_floating=true\npanel_inset=20\npanel_spacing=10\npanel_shadow=true\npanel_rounded=true\npanel_hover=false\n"
              "menu_button_icon=os\nmenu_button_label=Start\npanel_applets=t1;c1;\n", 1)
s += "\n[applet:t1]\ntype=command\nlabel=Test\ncommand=echo HDE-EXT-OK\ninterval=5\n\n[applet:c1]\ntype=cpu\nlabel=CPU\ninterval=2\n"
open(p, "w").write(s)
EOF
sleep 3
check "panel_position=top, panel_size=40: the panel moves to the top, 40 px high" grep -q "hde-panel: settings changed: panel at 0,0 1280x40" "$OUT/session.log"
check "floating style, inset, spacing and effect switches load from settings.ini" \
    grep -q "hde-panel: visual: floating, inset 20px, spacing 10px, effects: shadow rounded hover-off" "$OUT/session.log"
if grep -q "^hde-panel: CSS:" "$OUT/session.log"; then fail "panel appearance CSS loads without errors"
else pass "panel appearance CSS loads without errors"; fi
pgeo "0 0 1280 40" "... its window really is there"
check "... the desktop icons make room for it" grep -q "hde-desktop: panel now takes 40 px at the top, 0 px at the bottom" "$OUT/session.log"
if grep "hde-panel: settings applied: top, 40px" "$OUT/session.log" | tail -n 1 | grep -q "items: menu desktop launchers taskbar"; then
    pass "panel_show_run=false hides the Run button"
else fail "panel_show_run=false hides the Run button ($(grep 'settings applied' "$OUT/session.log" | tail -n 1))"; fi
check "an extension shows the output of a command (echo HDE-EXT-OK)" grep -q "hde-panel: extension t1 (command): HDE-EXT-OK" "$OUT/session.log"
check "the built-in processor extension works (no tool needed)" grep -q "hde-panel: extension c1 (cpu): CPU" "$OUT/session.log"
if grep -q "hde-panel: start button: Start, " "$OUT/session.log"; then
    pass "the Start button shows the logo of the system and the label 'Start' ($(grep 'start button: Start' "$OUT/session.log" | tail -n 1 | sed 's/.*Start, //'))"
else fail "the Start button shows the logo of the system and the label 'Start'"; fi
shot 16e-panel-top
xdotool key super; sleep 1.5
if grep "hde-panel: start menu: shown" "$OUT/session.log" | tail -n 1 | grep -q "shown (modern) at [0-9]*,44 "; then
    pass "with the panel at the top the Start menu opens below it"
else fail "with the panel at the top the Start menu opens below it ($(grep 'start menu: shown' "$OUT/session.log" | tail -n 1))"; fi
shot 16f-start-menu-under-top-panel
xdotool key Escape; sleep 0.6
if command -v notify-send >/dev/null 2>&1; then
    notify-send -a "Smoke test" "Panel at the top" "Notifications come from the top right now"; sleep 1.2
    shot 16g-notification-top-panel
fi
"$B/hde-settings" display > "$OUT/settings-max.log" 2>&1 &
SMAX=$!
sleep 3.5
maxwin "panel at the top, 40 px" top 40
shot 16g2-maximized-under-top-panel
python3 - "$SETTINGS_INI" <<'EOF'
import sys
p = sys.argv[1]
s = open(p).read()
s = s.replace("panel_size=40\n", "panel_size=28\npanel_opacity=80\npanel_taskbar_labels=false\nclock_24h=false\n", 1)
open(p, "w").write(s)
EOF
sleep 3
check "panel_size=28: a thin panel" grep -q "hde-panel: settings changed: panel at 0,0 1280x28" "$OUT/session.log"
pgeo "0 0 1280 28" "... its window really is 28 px high (a lower panel shrinks: it used to stay 40 px)"
check "... the clock on one line" grep -q "hde-panel: clock: time and date on one line (a thin panel, 28 px)" "$OUT/session.log"
maxwin "panel at the top, 28 px" top 28
shot 16h-panel-thin-top
# The bug of the screenshot: a panel made higher, then lower again, kept its height and hung below the bottom of the
# screen (libwnck's workspace switcher keeps the height it last had as its minimum). 56 px at the bottom, then 34.
python3 - "$SETTINGS_INI" <<'EOF'
import sys
p = sys.argv[1]
s = open(p).read()
s = s.replace("panel_position=top\n", "panel_position=bottom\n", 1).replace("panel_size=28\n", "panel_size=56\n", 1)
open(p, "w").write(s)
EOF
sleep 3
pgeo "0 744 1280 56" "panel_size=56 at the bottom: 56 px high, its bottom edge on the bottom of the screen"
maxwin "panel at the bottom, 56 px" bottom 744
shot 16i-panel-high-bottom
kill "$SMAX" 2>/dev/null; sleep 0.5
python3 - "$SETTINGS_INI" <<'EOF'
import sys, re
p = sys.argv[1]
s = open(p).read()
s = s.split("\n[applet:")[0].rstrip("\n") + "\n"
s = re.sub(r"(?m)^(panel_position|panel_size|panel_show_run|menu_button_icon|menu_button_label|panel_applets|panel_opacity|"
           r"panel_floating|panel_inset|panel_spacing|panel_shadow|panel_rounded|panel_hover|panel_taskbar_labels|clock_24h)=.*\n", "", s)
open(p, "w").write(s)
EOF
sleep 3
check "back to the defaults: the panel returns to the bottom, 34 px" grep -q "hde-panel: settings changed: panel at 0,766 1280x34" "$OUT/session.log"
pgeo "0 766 1280 34" "... 34 px high again after 56 px: nothing of it below the edge of the screen"
st=$($XT cardinals "$($XT root-window _HDE_PANEL_WINDOW)" _NET_WM_STRUT_PARTIAL 2>/dev/null)
case "$st" in
    "0 0 0 34 "*) pass "... and it reserves 34 px at the bottom for itself (_NET_WM_STRUT_PARTIAL $st)" ;;
    *) fail "... and it reserves 34 px at the bottom for itself (_NET_WM_STRUT_PARTIAL ${st:-none})" ;;
esac
l=$(grep "hde-panel: clock: " "$OUT/session.log" | tail -n 1)
case "$l" in
    *"the date under the time"*)
        pass "... the date under the time ${l#hde-panel: clock: the date under the time }" ;;
    *"time and date on one line (2 lines need "*)
        # the date under the time is what a 34 px panel does *when both lines fit in it*: with a taller font (a
        # distribution's own, or HDE_SMOKE_FONT) the panel says how much room it would need and keeps one line —
        # an item higher than the panel would be cut off. Both are the panel measuring itself and deciding.
        n_need=$(printf '%s' "$l" | sed -n 's/.*2 lines need \([0-9]*\) px, the panel has \([0-9]*\).*/\1/p')
        n_room=$(printf '%s' "$l" | sed -n 's/.*2 lines need \([0-9]*\) px, the panel has \([0-9]*\).*/\2/p')
        if [ -n "$n_need" ] && [ -n "$n_room" ] && [ "$n_need" -gt "$n_room" ]; then
            pass "... the date still on one line: the font of this system needs ${n_need} px for two lines and the panel has ${n_room} (it never grows: an item higher than the panel would be cut)"
        else
            fail "... the date under the time or on one line with a reason (${l:-nothing logged})"
        fi ;;
    *) fail "... the date under the time (${l:-nothing logged})" ;;
esac
shot 16j-panel-default-again
check "... and the desktop icons follow" grep -q "hde-desktop: panel now takes 0 px at the top, 34 px at the bottom" "$OUT/session.log"

# labwc's configuration for the Wayland session, written from settings.ini (tests/wayland-test.sh runs it for real)
echo "fkeys_sound=false" >> "$SETTINGS_INI"
"$B/hde-settings" --wayland-config "$OUT/labwc-config" > "$OUT/labwc-config.log" 2>&1
RCX="$OUT/labwc-config/rc.xml"
if grep -q "hde-panel --menu" "$RCX" && grep -q 'key="Print".*hde-hotkeys --action screenshot' "$RCX" && ! grep -q 'key="F3"' "$RCX" &&
   grep -q '<naturalScroll>yes</naturalScroll>' "$RCX" && [ -s "$OUT/labwc-config/themerc-override" ]; then
    pass "hde-settings --wayland-config writes labwc's rc.xml from settings.ini (Super, PrtSc; fkeys_sound=false: no F1-F3)"
else fail "hde-settings --wayland-config writes labwc's rc.xml from settings.ini ($(cat "$OUT/labwc-config.log"))"; fi
sed -i '/^fkeys_sound=/d' "$SETTINGS_INI"

# ---------- 6d. About: the logo of the system and of its base (os-release), the About window ----------
l=$(grep "hde-settings: about: .*logo from" "$OUT/settings.log" | head -n 1)
case "$l" in
    *"logo from badge"*|"") fail "Settings > About finds a logo for this system (${l:-nothing logged})" ;;
    *) pass "Settings > About shows the logo of this system: $(echo "$l" | sed 's/^hde-settings: about: //')" ;;
esac
for osr in hyggshios debian mint foo; do
    HDE_OS_RELEASE="$OUT/os-release-$osr" "$B/hde-settings" --about-window > "$OUT/about-$osr.log" 2>&1 &
    AW=$!
    sleep 2.5
    shot "17-about-window-$osr"
    kill "$AW" 2>/dev/null
done
if grep -q 'about: Hyggshi OS 1.0 "Sen Vàng" (ID=hyggshios, ID_LIKE=debian, .*): logo from \(built in: hyggshios\|icon theme: distributor-logo\); based on Debian 13 (trixie), its logo from [^b]' "$OUT/about-hyggshios.log"; then
    pass "Hyggshi OS (ID=hyggshios): its own logo, 'Based on Debian 13 (trixie)' with the Debian logo"
else fail "Hyggshi OS: its own logo and 'Based on Debian 13 (trixie)' ($(grep 'about:' "$OUT/about-hyggshios.log" | head -n 1))"; fi
check "Debian (ID=debian): the Debian logo" grep -q "about: Debian GNU/Linux 13 (trixie) (ID=debian, .*): logo from [^b]" "$OUT/about-debian.log"
if grep -q "about: Linux Mint 22.1 (ID=linuxmint, ID_LIKE=ubuntu debian, .*): logo from [^b][^;]*; based on Ubuntu 24.04 LTS (noble), its logo from [^b]" "$OUT/about-mint.log"; then
    pass "Linux Mint: the Mint logo, 'Based on Ubuntu 24.04 LTS (noble)' with the Ubuntu logo"
else fail "Linux Mint: the Mint logo and its Ubuntu base ($(grep 'about:' "$OUT/about-mint.log" | head -n 1))"; fi
check "an unknown system gets a badge with its first letter (F) in its ANSI_COLOR" grep -q "about: Foo OS 2 (ID=foo, .*): logo from badge: F" "$OUT/about-foo.log"
HDE_OS_RELEASE="$OUT/os-release-hyggshios" "$B/hde-settings" --about > "$OUT/about-hyggshios.txt" 2>&1
check "hde-settings --about tells the base too (Based on: Debian 13 (trixie))" grep -q "^Based on: Debian 13 (trixie)" "$OUT/about-hyggshios.txt"
grep -h "hde-settings: about" "$OUT"/about-*.log | sed 's/^/INFO: /' >> "$OUT/results.txt"

# ---------- 6e. the Control Center, the battery panel, the panel's right-click menus ----------
# pwidget NAME -> "CX CY X W": a widget of the panel or of its pop-ups (HDE_DEBUG: "hde-panel: widget NAME at X,Y WxH")
pwidget() {
    grep -F "hde-panel: widget $1 at " "$OUT/session.log" | tail -n 1 |
        sed -n 's/^hde-panel: widget .* at \([0-9-]*\),\([0-9-]*\) \([0-9]*\)x\([0-9]*\)$/\1 \2 \3 \4/p' |
        awk '{ printf "%d %d %d %d\n", $1 + $3 / 2, $2 + $4 / 2, $1, $3 }'
}
# shellcheck disable=SC2046  # "X Y ..." -> arguments
pclick() { set -- $(pwidget "$1"); [ -n "${2:-}" ] && xdotool mousemove "$1" "$2" click 1; }
cclog() { grep "hde-panel: control center: $1" "$OUT/session.log" | tail -n 1; }
xdotool mousemove 640 400
if command -v notify-send >/dev/null 2>&1; then
    notify-send -a "Smoke test" -i mail-unread "Two new messages" "For the Control Center test"
    notify-send -a "Calendar" -i x-office-calendar "Meeting at 15:00" "Room 2, with the HDE team"
fi
wait_popups
cc0=$(nlog "control center: shown")
# shellcheck disable=SC2046
set -- $(pwidget volume)
if [ -n "${2:-}" ]; then xdotool mousemove "$1" "$2" click 1; how="clicking the volume icon"
else "$B/hde-panel" --control-center; how="hde-panel --control-center"; fi
sleep 2
if [ "$(nlog "control center: shown")" -gt "$cc0" ]; then pass "$how opens the Control Center ($(cclog 'shown' | sed 's/.*shown //'))"
else fail "$how opens the Control Center"; fi
shot 18a-control-center
check "... Wi-Fi tile: on, connected to Home WiFi (NetworkManager)" grep -q "control center: tile wifi: on (Home WiFi)" "$OUT/session.log"
if [ -n "$BLUEZ_MOCK" ]; then
    check "... Bluetooth tile: on, Galaxy Buds2 connected (BlueZ)" grep -q "control center: tile bluetooth: on (Galaxy Buds2)" "$OUT/session.log"
fi
check "... Do Not Disturb, Dark mode and Night Light tiles" sh -c "grep -q 'control center: tile dnd: off' '$OUT/session.log' &&
    grep -q 'control center: tile dark: o' '$OUT/session.log' && grep -q 'control center: tile night: off' '$OUT/session.log'"
check "... the brightness slider (software dimming in Xvfb)" grep -q "control center: brightness: software dimming: [0-9]*%" "$OUT/session.log"
check "... it lists the notifications (the 2 new ones and the earlier ones)" \
    grep -q "control center: notifications: \([2-9]\|[1-9][0-9]\) (" "$OUT/session.log"
check "... the battery in its footer" grep -q "hde-panel: widget cc-battery at " "$OUT/session.log"
grep "hde-panel: control center: tile" "$OUT/session.log" | sed 's/^/INFO: /' | sort -u | head -n 9 >> "$OUT/results.txt"
pclick cc-tile-dnd; sleep 1
check "the Do Not Disturb tile turns Do Not Disturb on (dnd=true)" grep -q "^dnd=true" "$SETTINGS_INI"
pclick cc-tile-dnd; sleep 1
check "... and off again" grep -q "^dnd=false" "$SETTINGS_INI"
if pactl info >/dev/null 2>&1; then
    check "the volume slider shows the volume" grep -q "control center: volume: [0-9]*%" "$OUT/session.log"
    pactl set-sink-volume @DEFAULT_SINK@ 50%; sleep 3
    # shellcheck disable=SC2046
    set -- $(pwidget cc-volume-scale)
    if [ -n "${4:-}" ]; then
        xdotool mousemove "$(($3 + $4 * 30 / 100))" "$2" click 1; sleep 1.5
        v=$(vol)
        if [ "${v:-0}" -ge 20 ] && [ "${v:-0}" -le 40 ]; then pass "clicking the volume slider at 30% sets the volume (50% -> $v%)"
        else fail "clicking the volume slider at 30% sets the volume (50% -> $v%)"; fi
        pactl set-sink-volume @DEFAULT_SINK@ 50%
    else fail "the volume slider is in the Control Center (no widget position logged)"; fi
fi
if [ "$(soft)" != "" ] || grep -q "control center: brightness: software" "$OUT/session.log"; then
    # shellcheck disable=SC2046
    set -- $(pwidget cc-brightness-scale)
    if [ -n "${4:-}" ]; then
        xdotool mousemove "$(($3 + $4 * 60 / 100))" "$2" click 1; sleep 2
        b=$(soft)
        if [ "${b:-0}" -ge 55 ] && [ "${b:-0}" -le 75 ]; then pass "the brightness slider dims the screen (software dimming: $b%)"
        else fail "the brightness slider dims the screen (_HDE_BRIGHTNESS=$b)"; fi
        xdotool mousemove "$(($3 + $4 - 16))" "$2" click 1; sleep 2        # (the theme's padding around the trough: not clickable)
        b=$(soft)
        if [ "${b:-0}" -ge 90 ]; then pass "... and brightens it back at the right end ($b%)"
        else fail "... and brightens it back at the right end (_HDE_BRIGHTNESS=$b)"; fi
        "$B/hde-settings" --brightness 100 >/dev/null 2>&1
    else fail "the brightness slider is in the Control Center (no widget position logged)"; fi
fi
# the Wi-Fi list right in the Control Center, a password asked in the list
"$B/hde-panel" --control-center=wifi; sleep 3
w=$(cclog "wifi: [0-9]* network")
case "$w" in
    *"Home WiFi* 82% saved secured"*"Neighbor 5G 70% secured"*) pass "the Wi-Fi page lists the networks (saved ones first, one row per name)" ;;
    *) fail "the Wi-Fi page lists the networks (${w:-nothing logged})" ;;
esac
shot 18b-cc-wifi
p0=$(grep -c "PW-STDIN-LEN: 13" "$OUT/nmcli.log")
pclick "cc-wifi-Neighbor 5G"; sleep 1.2
shot 18c-cc-wifi-password
xdotool type --delay 40 "correct-horse"; xdotool key Return; sleep 3
p1=$(grep -c "PW-STDIN-LEN: 13" "$OUT/nmcli.log")
if [ "$p1" -gt "$p0" ] && grep -q "control center: wifi: connected to Neighbor 5G" "$OUT/session.log"; then
    pass "a new secured network asks for its password right in the list, then connects (password on nmcli's stdin)"
else fail "a new secured network asks for its password in the list ($p0 -> $p1; $(cclog 'wifi: c'))"; fi
if grep "^ARGS:" "$OUT/nmcli.log" | grep -q "correct-horse"; then fail "the Control Center must not put the password on the command line"
else pass "... the password is never on the command line"; fi
if [ -n "$BLUEZ_MOCK" ]; then
    "$B/hde-panel" --control-center=bluetooth; sleep 2.5
    check "the Bluetooth page lists the paired devices" grep -q "control center: bluetooth: on, [0-9]* paired device(s): .*Galaxy Buds2 (connected)" "$OUT/session.log"
    shot 18d-cc-bluetooth
fi
if pactl info >/dev/null 2>&1; then
    def0=$(pactl info | sed -n 's/^Default Sink: //p')
    pactl load-module module-null-sink sink_name=hde_hdmi sink_properties=device.description=HDMI-Test >/dev/null 2>&1
    pactl load-module module-null-sink sink_name=hde_usb sink_properties=device.description=USB-Headset-Test >/dev/null 2>&1
    pactl set-default-sink hde_hdmi
    pacat -p --raw --format=s16le --rate=8000 --channels=1 < /dev/zero > /dev/null 2>&1 &
    PACAT=$!
    sleep 1
    "$B/hde-panel" --control-center=sound; sleep 3
    check "the Sound page lists the output devices" grep -q "control center: sound: outputs: .*HDMI-Test\*, USB-Headset-Test\|control center: sound: outputs: .*USB-Headset-Test, .*HDMI-Test\*" "$OUT/session.log"
    check "... and the apps playing sound, each with its own volume" grep -q "control center: sound: outputs: .*apps: [^ ]* [0-9]*%" "$OUT/session.log"
    check "... shown in the list (a row with a slider)" grep -q "hde-panel: widget cc-app-" "$OUT/session.log"
    shot 18e-cc-sound
    pclick cc-sink-hde_usb; sleep 2
    if [ "$(pactl info | sed -n 's/^Default Sink: //p')" = hde_usb ]; then pass "choosing another output device makes it the default one"
    else fail "choosing another output device makes it the default one ($(pactl info | sed -n 's/^Default Sink: //p'))"; fi
    if pactl list short sink-inputs | grep -q "	$(pactl list short sinks | awk '$2 == "hde_usb" {print $1}')	"; then
        pass "... and the app playing moves to it"
    else fail "... and the app playing moves to it ($(pactl list short sink-inputs | tr '\t\n' ' ;'))"; fi
    [ -n "$def0" ] && pactl set-default-sink "$def0"
    kill "$PACAT" 2>/dev/null
    pactl unload-module module-null-sink 2>/dev/null
fi
"$B/hde-panel" --notifications; sleep 1.5
shot 18f-cc-notifications
pclick cc-notif-clear; sleep 1
check "Clear all removes the notifications" grep -q "control center: notifications: cleared" "$OUT/session.log"
h0=$(nlog "control center: hidden"); xdotool key Escape; sleep 0.8
if [ "$(nlog "control center: hidden")" -gt "$h0" ]; then pass "Esc closes the Control Center"; else fail "Esc closes the Control Center"; fi
c0=$(nlog "control center: shown"); xdotool key super+a; sleep 1.5
if [ "$(nlog "control center: shown")" -gt "$c0" ]; then pass "Super+A opens the Control Center"; else fail "Super+A opens the Control Center"; fi
xdotool key Escape; sleep 0.6

# the battery panel (the fake battery of HDE_POWER_SUPPLY_DIR)
check "the battery icon shows the laptop battery" grep -q "hde-panel: widget battery at " "$OUT/session.log"
b0=$(nlog "battery: shown")
pclick battery; sleep 2
if [ "$(nlog "battery: shown")" -gt "$b0" ]; then pass "clicking the battery icon opens the battery panel"
else
    fail "clicking the battery icon opens the battery panel"
    "$B/hde-panel" --battery; sleep 2
fi
if grep -q "hde-panel: battery: BAT0 82% Discharging, 5.2 W, 9 h 09 min left, health 91% (58.0 of 63.7 Wh), 123 cycles" "$OUT/session.log"; then
    pass "the battery panel: charge, draw, time left, health, cycles ($(grep 'hde-panel: battery: BAT0' "$OUT/session.log" | tail -n 1 | sed 's/.*battery: //'))"
else fail "the battery panel shows the details ($(grep 'hde-panel: battery: ' "$OUT/session.log" | tail -n 2 | tr '\n' ' '))"; fi
if [ -n "$PPD_MOCK" ] && grep -q "hde-panel: battery: power mode balanced" "$OUT/session.log"; then
    pass "... the power mode (power-profiles-daemon): Balanced"
    pclick bat-mode-power-saver; sleep 1.5
    pm=$(gdbus call --system --dest net.hadess.PowerProfiles --object-path /net/hadess/PowerProfiles \
         --method org.freedesktop.DBus.Properties.Get net.hadess.PowerProfiles ActiveProfile 2>&1)
    case "$pm" in *power-saver*) pass "... Power Saver switches the power mode" ;; *) fail "... Power Saver switches the power mode ($pm)" ;; esac
elif [ -n "$PPD_MOCK" ]; then
    fail "the battery panel shows the power mode ($(grep 'battery: power mode' "$OUT/session.log" | tail -n 1))"
fi
shot 18g-battery-panel
xdotool key Escape; sleep 0.6

# the battery saver and the low-battery warnings (src/hde-powersave.c, fed by the battery readings every 5 s), and
# Settings > Power; the fake battery of HDE_POWER_SUPPLY_DIR is drained and plugged in
PSD=$HDE_POWER_SUPPLY_DIR
bat() {     # CAPACITY STATUS AC_ONLINE (the energy left follows the capacity: 1% of the 58 Wh full charge = 580000 µWh)
    printf '%s\n' "$1" > "$PSD/BAT0/capacity"; printf '%s\n' "$(($1 * 580000))" > "$PSD/BAT0/energy_now"
    printf '%s\n' "$2" > "$PSD/BAT0/status"; printf '%s\n' "$3" > "$PSD/AC/online"
}
ppd() {
    if [ -n "${1:-}" ]; then
        gdbus call --system --dest net.hadess.PowerProfiles --object-path /net/hadess/PowerProfiles \
            --method org.freedesktop.DBus.Properties.Set net.hadess.PowerProfiles ActiveProfile "<'$1'>" > /dev/null 2>&1
    else
        gdbus call --system --dest net.hadess.PowerProfiles --object-path /net/hadess/PowerProfiles \
            --method org.freedesktop.DBus.Properties.Get net.hadess.PowerProfiles ActiveProfile 2>&1 | tr -dc 'a-z-'
    fi
}
wait_nlog() { i=0; while [ "$i" -lt "${3:-90}" ]; do [ "$(nlog "$1")" -gt "$2" ] && return 0; sleep 0.1; i=$((i + 1)); done; return 1; }
ini_set() {     # KEY VALUE, in the [settings] group of settings.ini
    if grep -q "^$1=" "$SETTINGS_INI"; then sed -i "s|^$1=.*|$1=$2|" "$SETTINGS_INI"
    else sed -i "/^\[settings\]\$/a $1=$2" "$SETTINGS_INI"; fi
}
[ -n "$PPD_MOCK" ] && ppd balanced
"$B/hde-settings" --brightness 100 > /dev/null 2>&1
ini_set battery_saver true; ini_set battery_saver_level 20; sleep 2.5
c0=$(nlog "hde-panel: battery saver: on")
bat 18 Discharging 0
if wait_nlog "hde-panel: battery saver: on" "$c0" 90; then pass "the battery saver turns on at 20% on battery ($(grep 'hde-panel: battery saver: on' "$OUT/session.log" | tail -n 1 | sed 's/.*saver: //'))"
else fail "the battery saver turns on at 20% on battery ($(grep 'hde-panel: battery saver' "$OUT/session.log" | tail -n 2 | tr '\n' ' '))"; fi
sleep 2
if [ -n "$PPD_MOCK" ]; then
    if [ "$(ppd)" = power-saver ]; then pass "... it switches to the Power Saver mode (power-profiles-daemon)"
    else fail "... it switches to the Power Saver mode ($(ppd))"; fi
fi
case "$binfo" in "software dimming"*)
    if [ "$(soft)" = 70 ]; then pass "... it dims the screen to 70% of its brightness"
    else fail "... it dims the screen to 70% of its brightness (_HDE_BRIGHTNESS=$(soft))"; fi ;;
esac
if wait_nlog "hde-notify: notification [0-9]* from Power: Battery saver is on" 0 30; then pass "... a notification says so"
else fail "... a notification says so"; fi
check "... the extensions refresh less often" grep -q "hde-panel: extensions: refreshed 3 times less often (battery saver)" "$OUT/session.log"
shot 19a-battery-saver
"$B/hde-settings" --power > "$OUT/power-cli.txt" 2>&1
check "hde-settings --power: the battery and the battery saver (on now)" grep -q "^Battery saver: at 20% (on now)" "$OUT/power-cli.txt"
"$B/hde-settings" power > "$OUT/settings-power.log" 2>&1 &
SPW=$!
i=0; while ! xdotool search --onlyvisible --name "Hyggshi Settings" >/dev/null 2>&1 && [ "$i" -lt 60 ]; do sleep 0.1; i=$((i + 1)); done
sleep 2.5
shot 19b-settings-power
if [ -n "$PPD_MOCK" ]; then
    check "Settings > Power shows the power mode (Power Saver, from power-profiles-daemon)" \
        grep -q "hde-settings: power mode power-saver (.*balanced" "$OUT/settings-power.log"
fi
kill "$SPW" 2>/dev/null; sleep 0.5
c0=$(nlog "hde-panel: power: battery low"); n0=$(nlog "from Power: Battery low")
bat 9 Discharging 0
if wait_nlog "hde-panel: power: battery low" "$c0" 90; then pass "'Battery low' at 10% ($(grep 'hde-panel: power: battery low' "$OUT/session.log" | tail -n 1 | sed 's/.*power: //'))"
else fail "'Battery low' at 10%"; fi
# (the notification goes through D-Bus: it is logged a moment later)
if wait_nlog "hde-notify: notification [0-9]* from Power: Battery low" "$n0" 30; then pass "... as a notification"
else fail "... as a notification"; fi
c0=$(nlog "hde-panel: power: battery critically low"); n0=$(nlog "from Power: Battery critically low")
bat 4 Discharging 0
if wait_nlog "hde-panel: power: battery critically low" "$c0" 90; then pass "'Battery critically low' at 5%"
else fail "'Battery critically low' at 5%"; fi
if wait_nlog "hde-notify: notification [0-9]* from Power: Battery critically low" "$n0" 30; then pass "... as a notification that stays (critical)"
else fail "... as a notification that stays (critical)"; fi
sleep 1
shot 19c-battery-critical
c0=$(nlog "hde-panel: battery saver: off")
bat 4 Charging 1
if wait_nlog "hde-panel: battery saver: off (plugged in)" "$c0" 90; then pass "plugged in: the battery saver turns off"
else fail "plugged in: the battery saver turns off ($(grep 'hde-panel: battery saver' "$OUT/session.log" | tail -n 1))"; fi
sleep 2.5
if [ -n "$PPD_MOCK" ]; then
    if [ "$(ppd)" = balanced ]; then pass "... the power mode is Balanced again"; else fail "... the power mode is Balanced again ($(ppd))"; fi
fi
case "$binfo" in "software dimming"*)
    if [ "$(soft)" = 100 ]; then pass "... and the brightness 100% again"; else fail "... and the brightness 100% again (_HDE_BRIGHTNESS=$(soft))"; fi ;;
esac
bat 82 Discharging 0; printf '47600000\n' > "$PSD/BAT0/energy_now"
sed -i '/^battery_saver=/d; /^battery_saver_level=/d' "$SETTINGS_INI"
xdotool key Escape; sleep 0.5
# GNOME's touchpad settings follow HDE's (Mutter / Muffin apply them themselves); the keyfile backend stands in for dconf
if gsettings list-schemas 2>/dev/null | grep -qx org.gnome.desktop.peripherals.touchpad; then
    ns0=$(sed -n 's/^natural_scroll=//p' "$SETTINGS_INI" | tail -n 1)
    ini_set natural_scroll false
    GSETTINGS_BACKEND=keyfile "$B/hde-settings" --apply > "$OUT/gsettings-apply.log" 2>&1
    v=$(GSETTINGS_BACKEND=keyfile gsettings get org.gnome.desktop.peripherals.touchpad natural-scroll 2>&1)
    if [ "$v" = false ]; then pass "natural_scroll=false: org.gnome.desktop.peripherals.touchpad natural-scroll follows (false)"
    else fail "natural_scroll=false: GNOME's natural-scroll follows ($v; $(tr '\n' ' ' < "$OUT/gsettings-apply.log"))"; fi
    ini_set natural_scroll true
    GSETTINGS_BACKEND=keyfile "$B/hde-settings" --apply >> "$OUT/gsettings-apply.log" 2>&1
    v=$(GSETTINGS_BACKEND=keyfile gsettings get org.gnome.desktop.peripherals.touchpad natural-scroll 2>&1)
    t=$(GSETTINGS_BACKEND=keyfile gsettings get org.gnome.desktop.peripherals.touchpad tap-to-click 2>&1)
    if [ "$v" = true ] && [ "$t" = true ]; then pass "... true again, and tap-to-click too (true)"
    else fail "... true again, and tap-to-click too ($v, $t)"; fi
    if [ -n "$ns0" ]; then ini_set natural_scroll "$ns0"; else sed -i '/^natural_scroll=/d' "$SETTINGS_INI"; fi
else
    skip "gsettings-desktop-schemas is not installed: GNOME's touchpad settings"
fi

# right-click menus with icons: the panel, the Start button, a status icon
xdotool mousemove 760 783 click 3; sleep 1
m=$(grep "hde-panel: panel menu: " "$OUT/session.log" | tail -n 1)
case "$m" in
    *"Control Center | Panel Settings… | Start Menu Settings… | Position | Size | Show on the panel"*) pass "right-click on the panel: Control Center, Panel Settings, Start Menu Settings, position, size, items" ;;
    *) fail "right-click on the panel opens its menu (${m:-nothing logged})" ;;
esac
shot 18h-panel-menu
xdotool key Escape; sleep 0.5
xdotool mousemove 30 783 click 3; sleep 1
m=$(grep "hde-panel: start button menu: " "$OUT/session.log" | tail -n 1)
case "$m" in
    *"Open the Start menu | Menu layout | Button icon | Button label | Start Menu Settings… | Panel Settings…"*) pass "right-click on the Start button: its layout, icon and label, Start Menu Settings" ;;
    *) fail "right-click on the Start button opens its menu (${m:-nothing logged})" ;;
esac
xdotool key i; sleep 0.8
shot 18i-start-button-icons
xdotool key h; sleep 2
if grep -q "^menu_button_icon=hde" "$SETTINGS_INI" && grep -q "hde-panel: start button: Menu, HDE logo" "$OUT/session.log"; then
    pass "... choosing the HDE logo there changes the Start button at once"
else fail "... choosing the HDE logo there changes the Start button ($(grep '^menu_button_icon' "$SETTINGS_INI"))"; fi
shot 18j-start-button-hde-logo
sed -i '/^menu_button_icon=/d' "$SETTINGS_INI"; sleep 2
check "the default Start button shows the logo of the system" sh -c "grep 'hde-panel: start button: Menu, ' '$OUT/session.log' | tail -n 1 | grep -vq 'HDE logo'"
# shellcheck disable=SC2046
set -- $(pwidget volume)
if [ -n "${2:-}" ]; then
    xdotool mousemove "$1" "$2" click 3; sleep 1
    check "right-click on the volume icon: mute, microphone, devices and apps, settings" \
        grep -q "hde-panel: status menu vol: Mute | Microphone on / off | Sound: devices and apps | Sound settings…" "$OUT/session.log"
    shot 18k-volume-menu
    xdotool key Escape; sleep 0.5
fi
xdotool mousemove 640 400

# ---------- 6f. Hyggshi Files, the file manager of HDE (hde-files/) ----------
FT="$HOME/FilesTest"
FL="$OUT/files.log"
mkdir -p "$FT/docs" "$FT/pics"
printf 'hello\n' > "$FT/notes.txt"
printf 'not shown\n' > "$FT/.hidden-file"
head -c 300000 /dev/zero > "$FT/big.bin"
python3 - "$FT/red.png" "$FT/pics/photo-red.png" <<'EOF'
import struct, sys, zlib
w, h = 64, 48
raw = b''.join(b'\x00' + b'\xe0\x10\x10' * w for _ in range(h))
def chunk(t, d):
    return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
png = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
       + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))
for p in sys.argv[1:]:
    open(p, 'wb').write(png)
EOF
# what `make install` sets up: its menu entry, and folders opening in it in HDE sessions (hde-mimeapps.list)
sed "s|@PREFIX@/bin/hde-files|$B/hde-files|g" "$HERE/../hde-files/hde-files.desktop" > "$XDG_DATA_HOME/applications/hde-files.desktop"
cp "$HERE/../hde-files/hde-mimeapps.list" "$XDG_DATA_HOME/applications/hde-mimeapps.list"
fnum() { fnum_n=$(grep -cF -- "$1" "$FL" 2>/dev/null); echo "${fnum_n:-0}"; }
fwait() { fw_i=0; while [ "$fw_i" -lt "${2:-30}" ]; do grep -qF -- "$1" "$FL" 2>/dev/null && return 0; sleep 0.2; fw_i=$((fw_i + 1)); done; return 1; }
# shellcheck disable=SC2329  # (called through check)
fmore() { fm_i=0; while [ "$fm_i" -lt "${3:-30}" ]; do [ "$(fnum "$1")" -gt "$2" ] && return 0; sleep 0.2; fm_i=$((fm_i + 1)); done; return 1; }
flast() { grep -F -- "$1" "$FL" 2>/dev/null | tail -n 1; }
# fitem NAME: "X Y W H" of the item on the screen (hde-files logs where its items are with HDE_DEBUG)
fitem() { sed -n "s/^hde-files: item $1 at \([0-9]*\),\([0-9]*\) \([0-9]*\)x\([0-9]*\)$/\1 \2 \3 \4/p" "$FL" | tail -n 1; }
fmenu() { flast "hde-files: menu: " | sed 's/^hde-files: menu: //'; }

FAILS0=$FAILS
v=$("$B/hde-files" --version 2>&1)
case "$v" in "hde-files (Hyggshi Files) "*) pass "hde-files --version: $v" ;; *) fail "hde-files --version ($v)" ;; esac
"$B/hde-files" "$FT" > "$FL" 2>&1 &
FILES=$!
if fwait "hde-files: folder ~/FilesTest: 6 items (1 hidden), 5 shown" 50; then
    pass "Hyggshi Files opens a folder: 5 items shown, the hidden one not ($(flast 'folder ~/FilesTest:' | sed 's/.*FilesTest: //'))"
else fail "Hyggshi Files opens a folder ($(flast 'folder ~/FilesTest'))"; fi
sleep 1
FW=$(xdotool search --onlyvisible --name "^FilesTest - Hyggshi Files$" 2>/dev/null | head -n 1)
if [ -n "$FW" ]; then
    pass "its window is named after the folder (FilesTest - Hyggshi Files)"
    xdotool windowactivate "$FW" 2>/dev/null || xdotool windowfocus "$FW" 2>/dev/null
else fail "a window \"FilesTest - Hyggshi Files\" ($(xdotool search --onlyvisible --name "Hyggshi Files" getwindowname 2>/dev/null | tr '\n' ' '))"; fi
sleep 1.5
# shellcheck disable=SC2046  # "X Y W H" -> four arguments
set -- $(fitem red.png)
if [ -n "${4:-}" ]; then
    c=$(px $(($1 + $3 / 2)) $(($2 + 46)))
    if [ "$(echo "$c" | awk '{ print ($1 > 150 && $2 < 90 && $3 < 90) }')" = 1 ]; then pass "a picture shows as its thumbnail (pixel $c)"
    else fail "a picture shows as its thumbnail (pixel $c at $(($1 + $3 / 2)),$(($2 + 46)); item $*)"; fi
else fail "a picture shows as its thumbnail (red.png: no place logged)"; fi
check "... kept in ~/.cache/thumbnails for the other programs too" sh -c "ls '$XDG_CACHE_HOME/thumbnails/normal/'*.png"
shot 21a-files
xdotool key ctrl+h; sleep 0.8
check "Ctrl+H shows the hidden files" fwait "hde-files: hidden files: shown (6 shown)" 10
xdotool key ctrl+h; sleep 0.6
check "... Ctrl+H again hides them" fwait "hde-files: hidden files: hidden (5 shown)" 10
xdotool key ctrl+2; sleep 1.2
check "Ctrl+2: the list (Name, Size, Type, Modified)" fwait "hde-files: view: list" 10
shot 21b-files-list
xdotool key ctrl+1; sleep 1
check "Ctrl+1: the icons again" fwait "hde-files: view: icons" 10
# new folder
xdotool key ctrl+shift+n
if fwait "hde-files: dialog: New Folder (New Folder)" 15; then
    sleep 0.5; xdotool key ctrl+a; xdotool type --delay 40 "Projects"; xdotool key Return; sleep 1.2
    if [ -d "$FT/Projects" ]; then pass "Ctrl+Shift+N makes a new folder (named Projects in its dialog)"
    else fail "Ctrl+Shift+N makes a new folder ($(ls "$FT" | tr '\n' ' '))"; fi
    check "... and selects it" fwait "hde-files: selected: file://$FT/Projects" 10
else fail "Ctrl+Shift+N opens the New Folder dialog"; fi
# type-ahead, rename (only the name is selected, the extension stays). Rename from the menu (Menu key, R): F2 is the
# volume-down key of HDE in this session (F1-F3 sound keys on); F2 renames when they are off
xdotool type --delay 60 "no"; sleep 0.6
check "typing selects the item whose name begins with it (type-ahead)" fwait "hde-files: type-ahead 'no': notes.txt" 10
xdotool key Menu; sleep 1
case "$(fmenu)" in *"| Rename… |"*) xdotool key r ;; *) xdotool key Escape ;; esac
if fwait "hde-files: dialog: Rename File (notes.txt)" 15; then
    sleep 0.6; shot 21c-files-rename
    xdotool type --delay 40 "readme"; xdotool key Return; sleep 1.2
    if [ -f "$FT/readme.txt" ] && [ ! -e "$FT/notes.txt" ]; then pass "Rename… renames, the extension kept: notes.txt -> readme.txt"
    else fail "Rename… renames (notes.txt -> readme.txt: $(ls "$FT" | tr '\n' ' '))"; fi
else fail "Rename… (Menu key, R) opens the Rename dialog ($(fmenu))"; fi
# copy + paste in the same folder, undo
sleep 0.5; xdotool key ctrl+c; sleep 0.5
check "Ctrl+C copies the selected file" fwait "hde-files: clipboard: copy 1 item(s): readme.txt" 10
xdotool key ctrl+v; sleep 2
check "... the status bar shows the operation with a progress bar and Cancel" fwait "hde-files: progress shown: " 5
case "$(flast 'progress shown: ')" in *"[bar, Cancel]") ;; *) fail "... with a progress bar and Cancel ($(flast 'progress shown: '))" ;; esac
if [ -f "$FT/readme (copy).txt" ]; then pass "Ctrl+V in the same folder makes \"readme (copy).txt\""
else fail "Ctrl+V in the same folder makes \"readme (copy).txt\" ($(ls "$FT" | tr '\n' ' '))"; fi
xdotool key ctrl+z; sleep 2
if [ ! -e "$FT/readme (copy).txt" ] && [ -f "$XDG_DATA_HOME/Trash/files/readme (copy).txt" ]; then
    pass "Ctrl+Z undoes the copy (the copy goes to the trash)"
else fail "Ctrl+Z undoes the copy ($(flast 'undo:'))"; fi
# Delete -> trash; the Trash; Restore
xdotool type --delay 60 "bi"; sleep 0.6; xdotool key Delete; sleep 1.5
if [ -f "$XDG_DATA_HOME/Trash/files/big.bin" ] && [ -f "$XDG_DATA_HOME/Trash/info/big.bin.trashinfo" ] && [ ! -e "$FT/big.bin" ]; then
    pass "Delete moves the selected file to the trash (with its .trashinfo)"
else fail "Delete moves the selected file to the trash ($(flast 'job:'))"; fi
xdotool key ctrl+l; sleep 0.6; xdotool type --delay 30 "trash:///"; xdotool key Return; sleep 2
if fwait "hde-files: folder Trash: " 15 && [ -n "$(fitem big.bin)" ]; then
    pass "the Trash (trash:///, no GVfs needed) lists what was deleted ($(flast 'folder Trash:' | sed 's/.*Trash: //'))"
else fail "the Trash lists what was deleted ($(flast 'folder Trash'))"; fi
check "... with the trash bar: Restore, Empty Trash" fwait "hde-files: trash bar: Restore, Empty Trash" 5
shot 21d-files-trash
xdotool type --delay 60 "bi"; sleep 0.6; xdotool key Menu; sleep 1
case "$(fmenu)" in
    "Restore | Delete Permanently | Properties") pass "the menu of an item in the trash: Restore, Delete Permanently, Properties" ;;
    *) fail "the menu of an item in the trash ($(fmenu))" ;;
esac
xdotool key r; sleep 1.5
if [ -f "$FT/big.bin" ] && [ ! -e "$XDG_DATA_HOME/Trash/files/big.bin" ]; then
    pass "Restore puts it back where it was ($(flast 'restored:' | sed 's/.*restored: //'))"
else fail "Restore puts it back where it was ($(flast 'restored'))"; fi
n=$(fnum "hde-files: go: ~/FilesTest")
xdotool key alt+Left; sleep 1.2
check "Alt+Left goes back to the folder" fmore "hde-files: go: ~/FilesTest" "$n" 15
sleep 0.8
# right-click menus: an item, the empty folder
# shellcheck disable=SC2046
set -- $(fitem readme.txt)
if [ -n "${4:-}" ]; then
    xdotool mousemove $(($1 + $3 / 2)) $(($2 + 30)) click 3; sleep 1
    # the menu is drawn a moment after the click: read it again instead of failing on a first look that caught half of it
    for _ in 1 2 3 4 5 6; do
        case "$(fmenu)" in *"Compress | Properties"*) break ;; esac
        sleep 0.5
    done
    case "$(fmenu)" in
        *"| Cut | Copy | Copy Path | Rename… | Make Link | Move to Trash | Delete Permanently | Compress | Properties")
            pass "right-click on a file: Open, Open With…, Cut, Copy, Rename, Trash, Compress, Properties ($(fmenu | cut -d'|' -f1))" ;;
        *) fail "right-click on a file ($(fmenu))" ;;
    esac
    shot 21e-files-menu
    xdotool key Escape; sleep 0.5
else fail "right-click on a file (readme.txt: no place logged)"; fi
# shellcheck disable=SC2046
set -- $($XT geometry "$FW" 2>/dev/null)
if [ -n "${4:-}" ]; then
    xdotool mousemove $(($1 + $3 - 80)) $(($2 + $4 - 90)) click 3; sleep 1
    case "$(fmenu)" in
        "New Folder… | New Document | Paste | Select All | Open in Terminal | Show Hidden Files | View | Sort By | Bookmark This Folder | Properties")
            pass "right-click on the empty folder: New Folder, New Document, Paste, Select All, Terminal, Hidden files, View, Sort" ;;
        *) fail "right-click on the empty folder ($(fmenu))" ;;
    esac
    xdotool key Escape; sleep 0.5
fi
# properties of a folder
xdotool type --delay 60 "pi"; sleep 0.6; xdotool key alt+Return; sleep 1.5
if xdotool search --onlyvisible --name "^pics Properties$" >/dev/null 2>&1 && fwait "hde-files: properties: pics: Folder" 10; then
    pass "Alt+Enter shows the Properties of the folder"
else fail "Alt+Enter shows the Properties of the folder ($(flast 'properties:'))"; fi
check "... with the size of what it holds" fwait "hde-files: properties: size " 15
shot 21f-files-properties
xdotool key Escape; sleep 0.6
# search in the subfolders
xdotool key ctrl+f; sleep 0.6; xdotool type --delay 80 "red"; sleep 2
if fwait "hde-files: search 'red' in ~/FilesTest: 2 result(s)" 15; then
    pass "Ctrl+F searches the folder and its subfolders (red.png, pics/photo-red.png)"
else fail "Ctrl+F searches the folder and its subfolders ($(flast "search 'red'"))"; fi
xdotool key ctrl+2; sleep 1; shot 21g-files-search; xdotool key ctrl+1; sleep 0.5
xdotool key Escape; sleep 1.2
# opening a folder with a double click, Backspace
# shellcheck disable=SC2046
set -- $(fitem docs)
if [ -n "${4:-}" ]; then
    xdotool mousemove $(($1 + $3 / 2)) $(($2 + 30)) click --repeat 2 --delay 90 1; sleep 1.5
    check "double-click on a folder opens it" fwait "hde-files: go: ~/FilesTest/docs" 10
    n=$(fnum "hde-files: go: ~/FilesTest")
    xdotool key BackSpace; sleep 1.2
    check "Backspace goes back" fmore "hde-files: go: ~/FilesTest" "$n" 10
else fail "double-click on a folder (docs: no place logged)"; fi
xdotool key ctrl+t; sleep 1
check "Ctrl+T opens a tab" fwait "hde-files: tab opened (2 tabs)" 10
shot 21h-files-tabs
xdotool key ctrl+w; sleep 0.8
check "Ctrl+W closes it" fwait "hde-files: tab closed (1 tabs)" 10
# "Show in Folder" of other programs (org.freedesktop.FileManager1), hde-files --select
gdbus call --session --dest org.freedesktop.FileManager1 --object-path /org/freedesktop/FileManager1 \
    --method org.freedesktop.FileManager1.ShowItems "['file://$FT/pics/photo-red.png']" "" > "$OUT/fm1.txt" 2>&1
sleep 1.5
check "org.freedesktop.FileManager1.ShowItems (Show in Folder): the folder, the file selected" \
    fwait "hde-files: show items: ~/FilesTest/pics (photo-red.png)" 10
"$B/hde-files" --select "$FT/red.png" > "$OUT/files-select.log" 2>&1
sleep 1.5
check "hde-files --select FILE, in the Hyggshi Files already running" fwait "hde-files: show items: ~/FilesTest (red.png)" 10
# Dark mode, live
"$B/hde-settings" --style dark > /dev/null 2>&1; sleep 3
PW=$(xdotool search --onlyvisible --name "^pics - Hyggshi Files$" 2>/dev/null | head -n 1)
[ -n "$PW" ] && xdotool windowactivate "$PW" 2>/dev/null; sleep 1
shot 21i-files-dark
# shellcheck disable=SC2046
set -- $($XT geometry "${PW:-$FW}" 2>/dev/null)
if [ -n "${4:-}" ]; then
    c=$(px $(($1 + $3 - 60)) $(($2 + $4 - 90)))
    if [ "$(echo "$c" | awk '{ print $1 + $2 + $3 }')" -lt 300 ]; then pass "Hyggshi Files follows Dark mode at once (view $c)"
    else fail "Hyggshi Files follows Dark mode at once (view $c)"; fi
fi
"$B/hde-settings" --style light > /dev/null 2>&1; sleep 2
"$B/hde-files" --quit > /dev/null 2>&1
i=0; while [ "$i" -lt 30 ] && kill -0 "$FILES" 2>/dev/null; do sleep 0.2; i=$((i + 1)); done
if kill -0 "$FILES" 2>/dev/null; then fail "hde-files --quit closes Hyggshi Files"; kill "$FILES" 2>/dev/null
else pass "hde-files --quit closes Hyggshi Files"; fi
if grep -q 'CRITICAL' "$FL"; then
    fail "Hyggshi Files logged no GTK criticals ($(grep 'CRITICAL' "$FL" | sed 's/.*CRITICAL \*\*: [0-9:.]*: //' | sort | uniq -c | tr '\n' ';' | cut -c1-400) after: $(grep -B5 -m1 'CRITICAL' "$FL" | grep -v -e '^hde-files: item ' -e 'CRITICAL' | tr '\n' '|' | cut -c1-700))"
else pass "Hyggshi Files logged no GTK criticals"; fi
grep -E "CRITICAL|WARNING" "$FL" | sort | uniq -c | sort -rn | head -n 6 | sed 's/^ */INFO: files.log: /' >> "$OUT/results.txt"
if [ "$FAILS" -gt "$FAILS0" ]; then           # what Hyggshi Files did, for the failures above
    grep -v "^hde-files: item " "$FL" | tail -n 70 | sed 's/^/INFO: files.log: /' >> "$OUT/results.txt"
fi
# the desktop: a double click on a folder opens it in Hyggshi Files (the default file manager of HDE sessions)
n=$(nlog "hde-files: window opened: ~/Desktop/aaa-folder")
xdotool mousemove 62 154 click --repeat 2 --delay 100 1; sleep 3.5
if [ "$(nlog "hde-files: window opened: ~/Desktop/aaa-folder")" -gt "$n" ]; then
    pass "double-clicking a folder on the desktop opens it in Hyggshi Files"
else fail "double-clicking a folder on the desktop opens it in Hyggshi Files ($(grep -E 'hde-desktop: no default handler|hde-files' "$OUT/session.log" | tail -n 2 | tr '\n' ' '))"; fi
shot 21j-files-from-desktop
"$B/hde-files" --quit > /dev/null 2>&1; sleep 1

# ---------- 7. WM switch without logging out ----------
if command -v openbox >/dev/null 2>&1 && command -v metacity >/dev/null 2>&1; then
    sed -i 's/^wm=.*/wm=openbox/' "$SETTINGS_INI"
    "$B/hde-session" wm >/dev/null 2>&1; sleep 7
    check "live WM switch: Openbox is running" pgrep -x openbox
    "$B/hde-settings" --about > "$OUT/about-openbox.txt" 2>&1
    echo "INFO: memory with Openbox: $(sed -n 's/^Desktop memory now: //p' "$OUT/about-openbox.txt"); $(grep -E '^  (openbox|hde-panel|hde-desktop) ' "$OUT/about-openbox.txt" | tr -s ' ' | tr '\n' ';')" \
        | tee -a "$OUT/results.txt"
    check "live WM switch: Metacity has stopped" sh -c "! pgrep -x metacity"
    check "panel survives the WM switch" running hde-panel
    pgeo "0 766 1280 34" "Openbox: the panel at the bottom edge, 34 px high"
    echo "INFO: with Openbox: $(grep "^hde-panel: measured: " "$OUT/session.log" | tail -n 1)" | tee -a "$OUT/results.txt"
    # the setup of the bug report: Openbox, the panel at the top, a maximized window (its title bar was under the panel)
    sed -i 's/^\[settings\]$/[settings]\npanel_position=top/' "$SETTINGS_INI"; sleep 3
    pgeo "0 0 1280 34" "Openbox: the panel at the top"
    "$B/hde-settings" display > "$OUT/settings-openbox.log" 2>&1 &
    SOB=$!
    sleep 3.5
    maxwin "Openbox, panel at the top" top 34
    shot 20a-openbox-maximized-top-panel
    sed -i '/^panel_position=top$/d' "$SETTINGS_INI"; sleep 3
    pgeo "0 766 1280 34" "Openbox: the panel back at the bottom edge"
    maxwin "Openbox, panel at the bottom" bottom 766
    shot 20b-openbox-maximized-bottom-panel
    kill "$SOB" 2>/dev/null; sleep 0.5
    if [ -f "$XDG_CONFIG_HOME/openbox/rc.xml" ]; then
        n0=$(nshots); xdotool key Print; sleep 3; n1=$(nshots)
        if [ -e "$OUT/wm-print-pressed" ]; then fail "Openbox: its own rc.xml Print binding fired (HDE must keep PrtSc)"
        elif [ "$n1" -gt "$n0" ]; then pass "Openbox with Print bound in rc.xml: Print still takes HDE's screenshot ($n0 -> $n1)"
        else fail "Openbox with Print bound in rc.xml: Print did nothing ($n0 -> $n1)"; fi
        # The situation of a session started by an OLDER hde-session (WM first) or of hde-hotkeys restarted after the
        # WM grabbed Print: HDE must say so, and take Print back by itself as soon as the WM gives way.
        hk=$(pgrep -x hde-hotkeys | head -n1)
        kill -KILL "$hk" 2>/dev/null; openbox --reconfigure >/dev/null 2>&1; sleep 4   # Openbox grabs Print; hde-session restarts hde-hotkeys
        if grep -q "hde-hotkeys: already used by another program (window manager: Openbox):.* Print" "$OUT/session.log"; then
            pass "hde-hotkeys notices that the window manager (Openbox) holds Print"
        else
            fail "hde-hotkeys notices that the window manager (Openbox) holds Print"
            grep -E "hde-hotkeys|hotkeys" "$OUT/session.log" | tail -n 5 | sed 's/^/INFO:   /' | tee -a "$OUT/results.txt"
        fi
        if grep -q "hde-notify: notification [0-9]* from HDE: Print key taken by another program" "$OUT/session.log"; then
            pass "a notification explains that Print is taken and what to do (log out and back in)"
        else fail "a notification explains that Print is taken"; fi
        shot 15b-print-taken-notification
        rm -f "$OUT/wm-print-pressed"; xdotool key Print; sleep 1.5
        if [ -e "$OUT/wm-print-pressed" ]; then echo "INFO: (setup) meanwhile Print runs Openbox's own rc.xml binding, as in the bug report" | tee -a "$OUT/results.txt"
        else echo "INFO: (setup) Openbox's rc.xml Print binding did not run" | tee -a "$OUT/results.txt"; fi
        rm -f "$OUT/wm-print-pressed"
        "$B/hde-session" wm >/dev/null 2>&1; sleep 6                    # wm=openbox again: a new Openbox replaces the old one
        if grep -q "hde-hotkeys: Print is free again: handled by HDE now" "$OUT/session.log"; then
            pass "when the window manager is replaced, HDE takes Print back before the new one can grab it"
        else fail "when the window manager is replaced, HDE takes Print back before the new one can grab it"; fi
        n0=$(nshots); xdotool key Print; sleep 3; n1=$(nshots)
        if [ ! -e "$OUT/wm-print-pressed" ] && [ "$n1" -gt "$n0" ]; then pass "after that, Print takes HDE's screenshot again ($n0 -> $n1)"
        else fail "after that, Print takes HDE's screenshot again (files $n0 -> $n1, Openbox binding fired: $([ -e "$OUT/wm-print-pressed" ] && echo yes || echo no))"; fi
    fi
    sed -i 's/^wm=.*/wm=metacity/' "$SETTINGS_INI"
    "$B/hde-session" wm >/dev/null 2>&1; sleep 7
    check "live WM switch back: Metacity is running" pgrep -x metacity
    check "live WM switch back: Openbox has stopped" sh -c "! pgrep -x openbox"
    pgeo "0 766 1280 34" "Metacity again: the panel at the bottom edge, 34 px high"
else
    skip "live WM switch (needs both openbox and metacity)"
fi

# ---------- 8. restart after a crash ----------
pid=$(pgrep -x hde-panel | head -n1)
if [ -n "$pid" ]; then
    kill -SEGV "$pid"
    new=""; i=0
    while [ "$i" -lt 75 ]; do
        new=$(pgrep -x hde-panel | head -n1)
        [ -n "$new" ] && [ "$new" != "$pid" ] && break
        sleep 0.2; i=$((i + 1))
    done
    if [ -n "$new" ] && [ "$new" != "$pid" ]; then pass "crashed panel is restarted by hde-session (after $((i * 200)) ms)"
    else
        fail "crashed panel is restarted by hde-session"
        grep -E "hde-session: .*(panel|restart)" "$OUT/session.log" | tail -n 5 | sed 's/^/INFO:   /' | tee -a "$OUT/results.txt"
        pgrep -a -x hde-panel | sed 's/^/INFO:   still running: /' | tee -a "$OUT/results.txt"
    fi
fi
shot 14-after-restart

if grep -q "Failed to execute child process" "$OUT/session.log"; then
    fail "no 'Failed to execute child process' errors during the session"
    grep "Failed to execute child process" "$OUT/session.log" | head -n 3 | sed 's/^/INFO:   /' | tee -a "$OUT/results.txt"
else pass "no 'Failed to execute child process' errors during the session"; fi

# ---------- 9. logout ----------
kill -TERM "$SESSION"
# up to 15 s for a program of the session to be gone; the FAIL says what is still running and from where
stopped() {
    name=$1; i=0
    while [ "$i" -lt 75 ]; do pgrep -x "$name" >/dev/null 2>&1 || return 0; sleep 0.2; i=$((i + 1)); done
    return 1
}
logout_check() {
    name=$1
    if stopped "$name"; then pass "logout stops $name"
    else
        fail "logout stops $name"
        pgrep -a -x "$name" | sed 's/^/INFO:   still running: /' | tee -a "$OUT/results.txt"
    fi
}
logout_check hde-panel
logout_check hde-hotkeys
logout_check hde-xsettings
if stopped metacity && stopped openbox; then pass "logout stops the window manager"
else
    fail "logout stops the window manager"
    pgrep -a -x metacity | sed 's/^/INFO:   still running: /' | tee -a "$OUT/results.txt"
    pgrep -a -x openbox  | sed 's/^/INFO:   still running: /' | tee -a "$OUT/results.txt"
fi
grep -E "hde-(panel|desktop|settings).*(CRITICAL|WARNING)" "$OUT/session.log" "$OUT/settings.log" > "$OUT/gtk-warnings.txt" 2>/dev/null
grep -E "(Gtk|GLib|GLib-GObject|Gdk|Wnck)-(CRITICAL|WARNING)" "$OUT/session.log" "$OUT/settings.log" >> "$OUT/gtk-warnings.txt" 2>/dev/null
n=$(sort -u "$OUT/gtk-warnings.txt" | wc -l)
echo "INFO: $n GTK/GLib warning line(s) in the logs (see gtk-warnings.txt)" | tee -a "$OUT/results.txt"
sort -u "$OUT/gtk-warnings.txt" | head -n 8 | sed 's/^/INFO:   /' | tee -a "$OUT/results.txt"
pulseaudio -k >/dev/null 2>&1
[ -n "$BLUEZ_MOCK" ] && kill "$BLUEZ_MOCK" 2>/dev/null
[ -n "$PPD_MOCK" ] && kill "$PPD_MOCK" 2>/dev/null
[ -n "$SYSBUS_PID" ] && kill "$SYSBUS_PID" 2>/dev/null
[ "$FAILS" -gt 100 ] && FAILS=100
exit "$FAILS"
