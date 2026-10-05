#!/bin/sh
# tests/smoke.sh — HDE smoke test: runs a whole HDE session in Xvfb, then checks the main features
# (Super key, F1/F2/F3, notifications, PrtSc screenshots, desktop icon frame + icon menu, Wi-Fi list,
#  live Dark mode, WM switch without logout, restart after a crash).
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
    sed "s|@PREFIX@/bin/hde-settings|$B/hde-settings|" "$HERE/../data/hyggshi-settings.desktop" \
        > "$OUT/home/.local/share/applications/hyggshi-settings.desktop"
    printf '[Desktop Entry]\nType=Link\nName=HDE Website\nURL=https://github.com/Hyggshi-OS-Research-Technology/NexWM\nIcon=web-browser\n' \
        > "$OUT/home/Desktop/hde.desktop"
    # desktop icons for the icon menu tests (sorted by name: Home, aaa-folder, bbb-notes.txt, hde.desktop)
    mkdir -p "$OUT/home/Desktop/aaa-folder"
    echo "inside" > "$OUT/home/Desktop/aaa-folder/inside.txt"
    echo "notes" > "$OUT/home/Desktop/bbb-notes.txt"
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
running() { pgrep -x "$1" >/dev/null 2>&1; }       # by process name (never matches the shell doing the check)
SETTINGS_INI="$XDG_CONFIG_HOME/hde/settings.ini"

if command -v pulseaudio >/dev/null 2>&1; then
    pulseaudio -D --exit-idle-time=-1 --log-target=file:"$OUT/pulse.log" >/dev/null 2>&1
    sleep 1.5
fi

# Fake BlueZ (python3-dbusmock) on a private "system bus": checks the Bluetooth device list
BLUEZ_MOCK=""
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
check "hde-xsettings owns _XSETTINGS_S0" $XT xsettings

# ---------- 2. Super key -> Start menu ----------
n0=$(popups); xdotool key super; sleep 1.2; n1=$(popups)
if [ "$n1" -gt "$n0" ]; then pass "Super key opens the Start menu ($n0 -> $n1 popups)"; else fail "Super key opens the Start menu ($n0 -> $n1 popups)"; fi
shot 02-start-menu
xdotool type --delay 120 "sett"; sleep 1.5
check "typing while the menu is open starts app search" xdotool search --onlyvisible --name "Search applications"
shot 03-app-search
xdotool key Escape; sleep 0.8
check "Escape closes app search" sh -c "! xdotool search --onlyvisible --name 'Search applications'"
xdotool key super; sleep 1.2; n1=$(popups); xdotool key super; sleep 1.2; n2=$(popups)
if [ "$n2" -lt "$n1" ]; then pass "pressing Super again closes the Start menu ($n1 -> $n2)"; else fail "pressing Super again closes the Start menu ($n1 -> $n2)"; fi
n0=$(popups); xdotool key super+s; sleep 1.5
check "Super+S opens app search" xdotool search --onlyvisible --name "Search applications"
n1=$(popups)
if [ "$n1" -le "$n0" ]; then pass "Super+<key> combination does not pop up the Start menu"; else fail "Super+<key> popped up the Start menu"; fi
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
else
    skip "no PulseAudio server: F1/F2/F3 volume tests"
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
if ! command -v scrot >/dev/null 2>&1; then
    echo "screenshot_tool=scrot" >> "$SETTINGS_INI"; sleep 2.5
    n0=$(nshots); xdotool key Print; sleep 3; n1=$(nshots)
    if [ "$n1" -gt "$n0" ] && grep -q "screenshot_tool=scrot is not installed; using hde-screenshot" "$OUT/session.log"; then
        pass "screenshot_tool=scrot without scrot installed falls back to the built-in tool (no error)"
    else fail "screenshot_tool=scrot without scrot installed falls back to the built-in tool ($n0 -> $n1)"; fi
    sed -i '/^screenshot_tool=/d' "$SETTINGS_INI"; sleep 2
fi

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
    # Pair through Settings itself: the "Pair" button of "Pixel 8" (the Other devices card has 1 row left)
    xdotool mousemove 1078 556 click 1; sleep 4
    shot 07c-bluetooth-paired-by-settings
    if bprop AA_BB_CC_DD_EE_02 Paired | grep -q true; then
        pass "Settings pairs a Bluetooth device (Device1.Pair)"
        if bprop AA_BB_CC_DD_EE_02 Trusted | grep -q true; then pass "paired device is marked trusted (auto-reconnect)"; else fail "paired device is marked trusted"; fi
        # dbusmock's Device1.Connect only emits PropertiesChanged without changing the value returned by Get -> check the call in the mock log
        if grep -Eq "^[0-9.]+ Connect( |$)" "$OUT/bluez-mock.log"; then pass "Settings connects the device after pairing (Device1.Connect)"
        else fail "Settings connects the device after pairing (Device1.Connect)"; fi
    else
        fail "Settings pairs a Bluetooth device (Pair button at 1078,556 — see shot 07c)"
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
# scrolling, no tapping). The real libinput/synaptics drivers are tested by tests/input-test.sh (Xorg + uinput).
FAKE_TP="pointer:Virtual core XTEST pointer"
tp_prop() { xinput list-props "$FAKE_TP" 2>/dev/null | sed -n "s/^[[:space:]]*$1 ([0-9]*):[[:space:]]*//p" | head -n 1; }
tp_wait() { i=0; while [ $i -lt 50 ]; do [ "$(tp_prop "$1")" = "$2" ] && return 0; sleep 0.1; i=$((i + 1)); done; return 1; }
if command -v xinput >/dev/null 2>&1 &&
   xinput set-prop --type=int --format=8 "$FAKE_TP" "libinput Tapping Enabled" 0 2>/dev/null &&
   xinput set-prop --type=int --format=8 "$FAKE_TP" "libinput Natural Scrolling Enabled" 0 2>/dev/null; then
    "$B/hde-settings" input; sleep 2.5
    check "opening Settings > Input applies natural scrolling to a touchpad (default on)" tp_wait "libinput Natural Scrolling Enabled" 1
    check "opening Settings > Input applies tap to click (default on)" tp_wait "libinput Tapping Enabled" 1
    if grep -q "hde-settings: input device: Virtual core XTEST pointer: Touchpad · libinput driver · natural scrolling on · tap to click on" "$OUT/settings.log"; then
        pass "Settings > Input lists the touchpad with its real state"
    else fail "Settings > Input lists the touchpad with its real state"; fi
    shot 08c-settings-input
    # flip the real "Natural scrolling" switch (the first switch of the page: find its blue "on" color)
    sw_y=""
    for y in $(seq 200 3 300); do
        set -- $($XT pixel 1074 "$y" 2>/dev/null)
        if [ "${3:-0}" -gt 180 ] && [ "${1:-255}" -lt 110 ] && [ "${2:-0}" -gt 100 ] && [ "${2:-0}" -lt 175 ]; then sw_y=$y; break; fi
    done
    if [ -n "$sw_y" ]; then
        xdotool mousemove 1090 "$sw_y" click 1; sleep 1.2
        if tp_wait "libinput Natural Scrolling Enabled" 0 && grep -q "^natural_scroll=false" "$SETTINGS_INI"; then
            pass "flipping the Natural scrolling switch switches the touchpad to the classic direction at once (switch at y=$sw_y)"
        else fail "flipping the Natural scrolling switch switches the touchpad to the classic direction at once (switch at y=$sw_y)"; fi
        shot 08d-settings-input-natural-off
        xdotool mousemove 1090 "$sw_y" click 1; sleep 1.2
        check "flipping it back turns natural scrolling on again" tp_wait "libinput Natural Scrolling Enabled" 1
    else
        fail "the Natural scrolling switch is visible at the top of Settings > Input (see shot 08c)"
    fi
    sed -i '/^natural_scroll=/d' "$SETTINGS_INI"; echo "natural_scroll=false" >> "$SETTINGS_INI"
    check "natural_scroll=false in settings.ini: hde-xsettings switches the touchpad to the classic direction" \
        tp_wait "libinput Natural Scrolling Enabled" 0
    sed -i 's/^natural_scroll=.*/natural_scroll=true/' "$SETTINGS_INI"
    check "natural_scroll=true: back to natural scrolling" tp_wait "libinput Natural Scrolling Enabled" 1
    xinput set-prop "$FAKE_TP" "libinput Natural Scrolling Enabled" 0
    "$B/hde-settings" --apply > "$OUT/settings-apply.log" 2>&1
    check "hde-settings --apply (login) applies natural scrolling" tp_wait "libinput Natural Scrolling Enabled" 1
    xinput delete-prop "$FAKE_TP" "libinput Natural Scrolling Enabled" 2>/dev/null
    xinput delete-prop "$FAKE_TP" "libinput Tapping Enabled" 2>/dev/null
else
    skip "touchpad settings (needs xinput)"
fi
"$B/hde-settings" appearance; sleep 2; shot 09-settings-appearance-light

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
"$B/hde-settings" network; sleep 2.5; shot 12-settings-wifi-dark
xdotool key super; sleep 1.2; shot 13-start-menu-dark; xdotool key Escape; sleep 0.5
"$B/hde-settings" --style light > "$OUT/style-light.log" 2>&1
sleep 3
$XT xsettings > "$OUT/xsettings-light.txt" 2>&1
if grep -q '^Net/ThemeName=Adwaita$' "$OUT/xsettings-light.txt"; then pass "switching back to Light mode works live"
else fail "switching back to Light mode ($(grep ThemeName "$OUT/xsettings-light.txt"))"; fi
kill $SETTINGS 2>/dev/null

# ---------- 7. WM switch without logging out ----------
if command -v openbox >/dev/null 2>&1 && command -v metacity >/dev/null 2>&1; then
    sed -i 's/^wm=.*/wm=openbox/' "$SETTINGS_INI"
    "$B/hde-session" wm >/dev/null 2>&1; sleep 7
    check "live WM switch: Openbox is running" pgrep -x openbox
    check "live WM switch: Metacity has stopped" sh -c "! pgrep -x metacity"
    check "panel survives the WM switch" running hde-panel
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
else
    skip "live WM switch (needs both openbox and metacity)"
fi

# ---------- 8. restart after a crash ----------
pid=$(pgrep -x hde-panel | head -n1)
if [ -n "$pid" ]; then
    kill -SEGV "$pid"; sleep 4
    new=$(pgrep -x hde-panel | head -n1)
    if [ -n "$new" ] && [ "$new" != "$pid" ]; then pass "crashed panel is restarted by hde-session"; else fail "crashed panel is restarted by hde-session"; fi
fi
shot 14-after-restart

if grep -q "Failed to execute child process" "$OUT/session.log"; then
    fail "no 'Failed to execute child process' errors during the session"
    grep "Failed to execute child process" "$OUT/session.log" | head -n 3 | sed 's/^/INFO:   /' | tee -a "$OUT/results.txt"
else pass "no 'Failed to execute child process' errors during the session"; fi

# ---------- 9. logout ----------
kill -TERM "$SESSION"; sleep 5
check "logout stops the panel" sh -c "! pgrep -x hde-panel"
check "logout stops hde-hotkeys" sh -c "! pgrep -x hde-hotkeys"
check "logout stops hde-xsettings" sh -c "! pgrep -x hde-xsettings"
check "logout stops the window manager" sh -c "! pgrep -x metacity && ! pgrep -x openbox"
grep -E "hde-(panel|desktop|settings).*(CRITICAL|WARNING)" "$OUT/session.log" "$OUT/settings.log" > "$OUT/gtk-warnings.txt" 2>/dev/null
grep -E "(Gtk|GLib|GLib-GObject|Gdk|Wnck)-(CRITICAL|WARNING)" "$OUT/session.log" "$OUT/settings.log" >> "$OUT/gtk-warnings.txt" 2>/dev/null
n=$(sort -u "$OUT/gtk-warnings.txt" | wc -l)
echo "INFO: $n GTK/GLib warning line(s) in the logs (see gtk-warnings.txt)" | tee -a "$OUT/results.txt"
sort -u "$OUT/gtk-warnings.txt" | head -n 8 | sed 's/^/INFO:   /' | tee -a "$OUT/results.txt"
pulseaudio -k >/dev/null 2>&1
[ -n "$BLUEZ_MOCK" ] && kill "$BLUEZ_MOCK" 2>/dev/null
[ -n "$SYSBUS_PID" ] && kill "$SYSBUS_PID" 2>/dev/null
[ "$FAILS" -gt 100 ] && FAILS=100
exit "$FAILS"
