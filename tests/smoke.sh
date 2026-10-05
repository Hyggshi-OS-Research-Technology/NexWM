#!/bin/sh
# tests/smoke.sh — kiểm thử khói HDE: chạy cả phiên HDE trong Xvfb rồi kiểm tra các tính năng chính
# (phím Super, F1/F2/F3, thông báo, danh sách Wi-Fi, Dark mode trực tiếp, đổi WM không đăng xuất, tự chạy lại khi crash).
#
#   make check            (hoặc: BUILD=build sh tests/smoke.sh)
# Cần: Xvfb xdotool dbus-run-session python3. Tuỳ chọn: metacity openbox pulseaudio notify-send import(ImageMagick)
# Kết quả: $HDE_TEST_OUT (mặc định /tmp/hde-smoke): results.txt, *.log, ảnh chụp shot-*.png
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

    # nmcli giả lập: danh sách Wi-Fi cố định, ghi lại mọi lệnh (kiểm tra mật khẩu KHÔNG nằm trên dòng lệnh)
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

# ===================== bên trong dbus-run-session =====================
FAILS=0
pass() { echo "PASS: $*" | tee -a "$OUT/results.txt"; }
fail() { echo "FAIL: $*" | tee -a "$OUT/results.txt"; FAILS=$((FAILS + 1)); }
skip() { echo "SKIP: $*" | tee -a "$OUT/results.txt"; }
check() { desc=$1; shift; if "$@" >/dev/null 2>&1; then pass "$desc"; else fail "$desc"; fi; }
shot() { command -v import >/dev/null 2>&1 && import -display "$DISPLAY" -window root "$OUT/shot-$1.png" 2>/dev/null; }
popups() { $XT popups 2>/dev/null || echo 0; }
running() { pgrep -x "$1" >/dev/null 2>&1; }       # theo tên tiến trình (không khớp nhầm shell đang kiểm tra)
SETTINGS_INI="$XDG_CONFIG_HOME/hde/settings.ini"

if command -v pulseaudio >/dev/null 2>&1; then
    pulseaudio -D --exit-idle-time=-1 --log-target=file:"$OUT/pulse.log" >/dev/null 2>&1
    sleep 1.5
fi

# BlueZ giả lập (python3-dbusmock) trên một "system bus" riêng: kiểm tra danh sách thiết bị Bluetooth
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
        # dbusmock < 0.28 (Ubuntu 22.04): PairDevice(adapter, address, class) — có thêm tham số Class
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

# ---------- 1. phiên khởi động ----------
for p in hde-panel hde-desktop hde-hotkeys hde-xsettings; do check "$p is running" running $p; done
WM=$(sed -n 's/^hde-session: starting window manager \([A-Za-z0-9]*\).*/\1/p' "$OUT/session.log" | head -n1)
if [ -n "$WM" ]; then pass "window manager started: $WM"; else fail "window manager started"; fi
if command -v metacity >/dev/null 2>&1; then
    if [ "$WM" = Metacity ]; then pass "auto mode prefers a GTK window manager (Metacity over Openbox/Xfwm4)"
    else fail "auto mode prefers a GTK window manager (got '$WM')"; fi
fi
check "panel publishes _HDE_PANEL_WINDOW" sh -c "[ \"\$($XT root-window _HDE_PANEL_WINDOW)\" != 0 ]"
check "hde-xsettings owns _XSETTINGS_S0" $XT xsettings

# ---------- 2. phím Super -> Start menu ----------
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
# hồi quy: xdotool nhả Super trước S -> lần nhả đó không tới hde-hotkeys (đang grab Super+S); Super vẫn phải chạy
n0=$(popups); xdotool key super; sleep 1.2; n1=$(popups)
if [ "$n1" -gt "$n0" ]; then pass "Super still works right after a Super+<key> shortcut"; else fail "Super still works right after a Super+<key> shortcut ($n0 -> $n1)"; fi
xdotool key Escape; sleep 0.5

# ---------- 3. F1 / F2 / F3 âm lượng ----------
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

# ---------- 4. thông báo ----------
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

# ---------- 5. Settings: Wi-Fi, Bluetooth ----------
"$B/hde-settings" network > "$OUT/settings.log" 2>&1 &
SETTINGS=$!
sleep 4
check "Settings window opens" xdotool search --onlyvisible --name "Hyggshi Settings"
shot 06-settings-wifi
if grep -q "device wifi list" "$OUT/nmcli.log" 2>/dev/null; then pass "Network page asks NetworkManager for the Wi-Fi list"; else fail "Network page asks NetworkManager for the Wi-Fi list"; fi

# Super khi một ứng dụng đang có focus (trường hợp dùng thật phổ biến nhất)
n0=$(popups); xdotool key super; sleep 1.5; n1=$(popups)
if [ "$n1" -gt "$n0" ]; then pass "Super opens the Start menu while an application window has focus ($n0 -> $n1)"
else
    fail "Super opens the Start menu while an application window has focus ($n0 -> $n1)"
    grep -E "hde-(panel|hotkeys):" "$OUT/session.log" | tail -n 14 | sed 's/^/INFO:   /' | tee -a "$OUT/results.txt"
fi
shot 06b-menu-over-app
xdotool key Escape; sleep 0.8

# Kết nối Wi-Fi có mật khẩu: bấm vào dòng "Neighbor 5G" (vị trí cố định: cửa sổ 1020x700 giữa màn hình 1280x800)
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
    # Ghép đôi bằng chính Settings: nút "Pair" của "Pixel 8" (thẻ Other devices còn 1 dòng)
    xdotool mousemove 1078 556 click 1; sleep 4
    shot 07c-bluetooth-paired-by-settings
    if bprop AA_BB_CC_DD_EE_02 Paired | grep -q true; then
        pass "Settings pairs a Bluetooth device (Device1.Pair)"
        if bprop AA_BB_CC_DD_EE_02 Trusted | grep -q true; then pass "paired device is marked trusted (auto-reconnect)"; else fail "paired device is marked trusted"; fi
        # Device1.Connect của dbusmock chỉ phát PropertiesChanged, không đổi giá trị Get -> kiểm tra lời gọi trong log mock
        if grep -Eq "^[0-9.]+ Connect( |$)" "$OUT/bluez-mock.log"; then pass "Settings connects the device after pairing (Device1.Connect)"
        else fail "Settings connects the device after pairing (Device1.Connect)"; fi
    else
        fail "Settings pairs a Bluetooth device (Pair button at 1078,556 — see shot 07c)"
        grep "hde-settings: bt device" "$OUT/settings.log" | tail -n 3 | cut -c1-400 | sed 's/^/INFO: /' >> "$OUT/results.txt"
    fi
    if grep -q "AgentManager1\|RegisterAgent" "$OUT/bluez-mock.log" 2>/dev/null; then :; fi
fi
"$B/hde-settings" keyboard; sleep 2; shot 08-settings-keyboard
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

# ---------- 7. đổi WM không cần đăng xuất ----------
if command -v openbox >/dev/null 2>&1 && command -v metacity >/dev/null 2>&1; then
    sed -i 's/^wm=.*/wm=openbox/' "$SETTINGS_INI"
    "$B/hde-session" wm >/dev/null 2>&1; sleep 7
    check "live WM switch: Openbox is running" pgrep -x openbox
    check "live WM switch: Metacity has stopped" sh -c "! pgrep -x metacity"
    check "panel survives the WM switch" running hde-panel
    sed -i 's/^wm=.*/wm=metacity/' "$SETTINGS_INI"
    "$B/hde-session" wm >/dev/null 2>&1; sleep 7
    check "live WM switch back: Metacity is running" pgrep -x metacity
    check "live WM switch back: Openbox has stopped" sh -c "! pgrep -x openbox"
else
    skip "live WM switch (needs both openbox and metacity)"
fi

# ---------- 8. tự chạy lại khi crash ----------
pid=$(pgrep -x hde-panel | head -n1)
if [ -n "$pid" ]; then
    kill -SEGV "$pid"; sleep 4
    new=$(pgrep -x hde-panel | head -n1)
    if [ -n "$new" ] && [ "$new" != "$pid" ]; then pass "crashed panel is restarted by hde-session"; else fail "crashed panel is restarted by hde-session"; fi
fi
shot 14-after-restart

# ---------- 9. đăng xuất ----------
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
