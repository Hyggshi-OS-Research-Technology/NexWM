#!/bin/sh
# tests/input-test.sh — HDE touchpad / mouse settings on a REAL X server: Xorg (dummy video driver) with the libinput
# and synaptics input drivers and virtual uinput devices: a laptop-like touchpad, a second touchpad plugged in later,
# a wheel mouse, and a touchpad driven by the synaptics driver.
# Checks that HDE applies Settings > Input — on a fresh account, live, after hotplug and after a remove/re-add
# (suspend/resume) — and that a two-finger swipe UP really moves the content UP (natural scrolling, the default).
#
# Needs root through sudo (Xorg, /dev/uinput, a file in /usr/share/X11/xorg.conf.d): meant for CI, not a desktop.
#   sudo apt install xserver-xorg-core xserver-xorg-video-dummy xserver-xorg-input-libinput \
#                    xserver-xorg-input-synaptics xinput x11-utils python3-evdev
#   make && sh tests/input-test.sh
# Output: $HDE_TEST_OUT (default /tmp/hde-input): results.txt, Xorg.log, xsettings.log, shot-*.png
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
BUILD=${BUILD:-build}
B=$(cd "$BUILD" 2>/dev/null && pwd) || { echo "build dir '$BUILD' not found"; exit 2; }
OUT=${HDE_TEST_OUT:-/tmp/hde-input}
DISP=${HDE_INPUT_DISPLAY:-:7}
XT="python3 $HERE/xtool.py"
TP="HDE Test Touchpad"
TP2="HDE Test Touchpad 2"
MOUSE="HDE Test Mouse"
SYN="HDE Test Synaptics Pad"
NAT="libinput Natural Scrolling Enabled"
TAP="libinput Tapping Enabled"
CONF=/usr/share/X11/xorg.conf.d/99-hde-input-test.conf

rm -rf "$OUT"
mkdir -p "$OUT/home/.config/hde"
: > "$OUT/results.txt"
FAILS=0
pass() { echo "PASS: $*" | tee -a "$OUT/results.txt"; }
fail() { echo "FAIL: $*" | tee -a "$OUT/results.txt"; FAILS=$((FAILS + 1)); }
info() { echo "INFO: $*" | tee -a "$OUT/results.txt"; }
check() { desc=$1; shift; if "$@" >/dev/null 2>&1; then pass "$desc"; else fail "$desc"; fi; }

for t in Xorg xinput python3 sudo timeout; do
    command -v $t >/dev/null 2>&1 || { fail "missing $t"; exit 2; }
done
sudo -n true 2>/dev/null || { fail "needs password-less sudo"; exit 2; }
python3 -c "import evdev" 2>/dev/null || { fail "missing python3-evdev"; exit 2; }
sudo -n modprobe uinput 2>/dev/null
[ -e /dev/uinput ] || { fail "/dev/uinput is missing (kernel module uinput)"; exit 2; }
info "kernel $(uname -r); $(dpkg-query -W -f '${Package} ${Version}, ' xserver-xorg-core xserver-xorg-input-libinput \
    xserver-xorg-input-synaptics 'libinput1*' python3-evdev 2>/dev/null)"

# ---------- X server ----------
# The synaptics package makes every touchpad use synaptics (70-synaptics.conf); pin each test device to its driver
# with an InputClass read after it.
sudo -n tee "$CONF" > /dev/null <<'EOF'
Section "InputClass"
    Identifier "HDE input test: libinput touchpads"
    MatchProduct "HDE Test Touchpad"
    MatchDevicePath "/dev/input/event*"
    Driver "libinput"
EndSection
Section "InputClass"
    Identifier "HDE input test: synaptics touchpad"
    MatchProduct "HDE Test Synaptics"
    MatchDevicePath "/dev/input/event*"
    Driver "synaptics"
EndSection
EOF
cat > "$OUT/xorg.conf" <<'EOF'
Section "ServerFlags"
    Option "AutoAddDevices" "true"
    Option "AutoEnableDevices" "true"
    Option "AutoAddGPU" "false"
    Option "DontVTSwitch" "true"
EndSection
Section "Device"
    Identifier "dummy"
    Driver "dummy"
    VideoRam 32768
EndSection
Section "Monitor"
    Identifier "monitor"
    HorizSync 5.0 - 1000.0
    VertRefresh 5.0 - 200.0
    Modeline "1280x800" 83.50 1280 1352 1480 1680 800 803 809 831 -hsync +vsync
EndSection
Section "Screen"
    Identifier "screen"
    Device "dummy"
    Monitor "monitor"
    DefaultDepth 24
    SubSection "Display"
        Depth 24
        Modes "1280x800"
        Virtual 1280 800
    EndSubSection
EndSection
EOF
sudo -n Xorg "$DISP" -config "$OUT/xorg.conf" -noreset -nolisten tcp -ac -logfile "$OUT/Xorg.log" \
    > "$OUT/xorg-output.log" 2>&1 &
XORG=$!
export DISPLAY="$DISP" HOME="$OUT/home" XDG_CONFIG_HOME="$OUT/home/.config" LANG=C.UTF-8 NO_AT_BRIDGE=1
unset DBUS_SESSION_BUS_ADDRESS XDG_CURRENT_DESKTOP WAYLAND_DISPLAY
for i in $(seq 1 100); do xinput list >/dev/null 2>&1 && break; sleep 0.1; done
VIN=""
cleanup() {
    [ -n "$VIN" ] && timeout 3 sh -c "echo quit > '$OUT/vinput.fifo'" 2>/dev/null
    pkill -x hde-xsettings 2>/dev/null
    sudo -n kill "$XORG" 2>/dev/null
    sleep 1
    sudo -n rm -f "$CONF"
}
if ! xinput list >/dev/null 2>&1; then
    fail "Xorg (dummy video driver) starts"
    tail -n 30 "$OUT/Xorg.log" "$OUT/xorg-output.log" 2>/dev/null | sed 's/^/INFO:   /' | tee -a "$OUT/results.txt"
    cleanup
    exit 1
fi
pass "Xorg with the dummy video driver is running ($DISP)"

# ---------- virtual devices ----------
mkfifo "$OUT/vinput.fifo"
: > "$OUT/vinput.resp"
sudo -n python3 "$HERE/vinput.py" serve "$OUT/vinput.fifo" "$OUT/vinput.resp" > "$OUT/vinput.log" 2>&1 &
VIN=$!
vin() {     # vin COMMAND...: run a vinput.py command, wait for its answer
    n0=$(wc -l < "$OUT/vinput.resp")
    timeout 5 sh -c 'echo "$1" > "$2"' _ "$*" "$OUT/vinput.fifo" || { info "vinput: no answer to '$*'"; return 1; }
    i=0
    while [ "$(wc -l < "$OUT/vinput.resp")" -le "$n0" ] && [ $i -lt 100 ]; do sleep 0.05; i=$((i + 1)); done
    r=$(tail -n 1 "$OUT/vinput.resp")
    case "$r" in "ok "*) return 0 ;; esac
    info "vinput: $r"
    return 1
}
has_dev() { xinput list --name-only 2>/dev/null | grep -qxF "$1"; }
wait_dev() { i=0; while [ $i -lt 80 ]; do has_dev "$1" && return 0; sleep 0.1; i=$((i + 1)); done; return 1; }
wait_gone() { i=0; while [ $i -lt 80 ]; do has_dev "$1" || return 0; sleep 0.1; i=$((i + 1)); done; return 1; }
prop() { xinput list-props "pointer:$1" 2>/dev/null | sed -n "s/^[[:space:]]*$2 ([0-9]*):[[:space:]]*//p" | head -n 1; }
wait_prop() {   # DEVICE PROPERTY VALUE [tenths of a second]
    i=0
    while [ $i -lt "${4:-50}" ]; do [ "$(prop "$1" "$2")" = "$3" ] && return 0; sleep 0.1; i=$((i + 1)); done
    return 1
}
wait_prop_re() {   # DEVICE PROPERTY EXTENDED-REGEX
    i=0
    while [ $i -lt 50 ]; do prop "$1" "$2" | grep -Eq "$3" && return 0; sleep 0.1; i=$((i + 1)); done
    return 1
}
driver_of() { grep -E "\(II\) Using input driver '[a-z]+' for '$1'" "$OUT/Xorg.log" | tail -n 1 | sed "s/.*driver '\([a-z]*\)'.*/\1/"; }
# scroll DEVICE up|down [swipe|wheel] -> "up=N down=N left=N right=N" seen by a full-screen X window
scroll() {
    rm -f "$OUT/scroll.txt"
    $XT scroll-watch 2.5 > "$OUT/scroll.txt" 2>&1 &
    sp=$!
    i=0
    while ! grep -q '^ready' "$OUT/scroll.txt" 2>/dev/null && [ $i -lt 60 ]; do sleep 0.05; i=$((i + 1)); done
    sleep 0.2
    vin "${3:-swipe}" "$1" "$2"
    wait $sp
    tail -n 1 "$OUT/scroll.txt"
}
# "up" (buttons 4: the view scrolls up, the content moves DOWN), "down" (buttons 5: the content moves UP) or "none"
direction() {
    u=$(echo "$1" | sed -n 's/.*up=\([0-9]*\).*/\1/p'); dn=$(echo "$1" | sed -n 's/.*down=\([0-9]*\).*/\1/p')
    if [ "${u:-0}" -gt 0 ] && [ "${dn:-0}" -eq 0 ]; then echo up
    elif [ "${dn:-0}" -gt 0 ] && [ "${u:-0}" -eq 0 ]; then echo down
    elif [ "${u:-0}" -eq 0 ] && [ "${dn:-0}" -eq 0 ]; then echo none
    else echo mixed; fi
}
INI="$XDG_CONFIG_HOME/hde/settings.ini"
set_ini() {   # KEY VALUE — what Hyggshi Settings writes when a switch is flipped
    [ -f "$INI" ] || printf '[settings]\n' > "$INI"
    if grep -q "^$1=" "$INI"; then sed -i "s|^$1=.*|$1=$2|" "$INI"; else echo "$1=$2" >> "$INI"; fi
}

# devices present at login
vin add-touchpad "$TP"
vin add-mouse "$MOUSE"
vin add-touchpad "$SYN"
for dev in "$TP" "$MOUSE" "$SYN"; do check "X server sees the virtual device '$dev'" wait_dev "$dev"; done
sleep 1
info "X drivers: $TP=$(driver_of "$TP"), $MOUSE=$(driver_of "$MOUSE"), $SYN=$(driver_of "$SYN")"
check "'$TP' uses the libinput driver" test "$(driver_of "$TP")" = libinput
check "'$SYN' uses the synaptics driver" test "$(driver_of "$SYN")" = synaptics

# what a fresh account had before this fix: the drivers' defaults
before="natural scrolling=$(prop "$TP" "$NAT"), tapping=$(prop "$TP" "$TAP")"
r=$(scroll "$TP" up)
info "before HDE applies anything (driver defaults): $before; two-finger swipe up -> $r ($(direction "$r"): 'up' means the content moves DOWN, against the fingers — the reported bug)"

# ---------- login: hde-xsettings with no settings.ini at all (fresh account) ----------
rm -f "$INI"
PATH=/nonexistent HDE_DEBUG=1 "$B/hde-xsettings" > "$OUT/xsettings.log" 2>&1 &
XS=$!
check "touchpad: natural scrolling is ON after login (default, without the xinput program)" wait_prop "$TP" "$NAT" 1
check "touchpad: tap to click is ON after login (default)" wait_prop "$TP" "$TAP" 1
check "mouse: wheel direction untouched (natural scrolling off)" wait_prop "$MOUSE" "$NAT" 0 10
ok=1; wait_prop_re "$SYN" "Synaptics Scrolling Distance" '^-[0-9]+, -[0-9]+$' || ok=0
v=$(prop "$SYN" "Synaptics Scrolling Distance")
if [ $ok = 1 ]; then pass "synaptics touchpad: natural scrolling ON (negative scrolling distance: $v)"
else fail "synaptics touchpad: natural scrolling ON (negative scrolling distance: $v)"; fi
ok=1; wait_prop_re "$SYN" "Synaptics Tap Action" ', 1, 3, 2$' || ok=0
v=$(prop "$SYN" "Synaptics Tap Action")
if [ $ok = 1 ]; then pass "synaptics touchpad: tap to click ON (1/2/3-finger tap = left/right/middle button: $v)"
else fail "synaptics touchpad: tap to click ON (1/2/3-finger tap = left/right/middle button: $v)"; fi
if grep -q "hde-xsettings: input device [0-9]*: $TP (touchpad, libinput): natural scrolling on, tap to click on" "$OUT/xsettings.log"; then
    pass "hde-xsettings logs each device and its state (~/.xsession-errors)"
else fail "hde-xsettings logs each device and its state"; fi

r=$(scroll "$TP" up); d=$(direction "$r")
if [ "$d" = down ]; then pass "two-finger swipe UP moves the content UP with the fingers (the view scrolls down: $r)"
else fail "two-finger swipe UP moves the content UP with the fingers (got $d: $r)"; fi
r=$(scroll "$TP" down); d=$(direction "$r")
if [ "$d" = up ]; then pass "two-finger swipe DOWN moves the content DOWN with the fingers ($r)"
else fail "two-finger swipe DOWN moves the content DOWN with the fingers (got $d: $r)"; fi
r=$(scroll "$SYN" up); d=$(direction "$r")
case "$d" in
    down) pass "synaptics touchpad: swipe UP moves the content UP ($r)" ;;
    up|mixed) fail "synaptics touchpad: swipe UP moves the content UP (got $d: $r)" ;;
    *) info "synaptics touchpad: the virtual swipe produced no scrolling ($r) — not checked" ;;
esac

# ---------- hotplug ----------
vin add-touchpad "$TP2"
check "X server sees a touchpad plugged in later" wait_dev "$TP2"
check "a touchpad plugged in later gets natural scrolling" wait_prop "$TP2" "$NAT" 1
check "a touchpad plugged in later gets tap to click" wait_prop "$TP2" "$TAP" 1
vin remove "$TP"
wait_gone "$TP"
sleep 0.5
vin add-touchpad "$TP"
wait_dev "$TP"
check "touchpad removed and added again (as after suspend/resume): natural scrolling is applied again" wait_prop "$TP" "$NAT" 1
r=$(scroll "$TP" up); d=$(direction "$r")
if [ "$d" = down ]; then pass "after the re-add, swipe UP still moves the content UP ($r)"
else fail "after the re-add, swipe UP still moves the content UP (got $d: $r)"; fi

# ---------- live changes (Settings writes settings.ini, hde-xsettings applies it) ----------
set_ini natural_scroll false
check "natural scrolling switched off: the touchpad goes back to the classic direction" wait_prop "$TP" "$NAT" 0
check "natural scrolling switched off: on the second touchpad too" wait_prop "$TP2" "$NAT" 0
check "natural scrolling switched off: synaptics touchpad (positive scrolling distance)" \
    wait_prop_re "$SYN" "Synaptics Scrolling Distance" '^[0-9]+, [0-9]+$'
r=$(scroll "$TP" up); d=$(direction "$r")
if [ "$d" = up ]; then pass "classic direction: swipe UP scrolls the view up, the content moves down ($r)"
else fail "classic direction: swipe UP scrolls the view up (got $d: $r)"; fi
set_ini natural_scroll true
check "natural scrolling switched on again" wait_prop "$TP" "$NAT" 1
set_ini tap_to_click false
check "tap to click switched off" wait_prop "$TP" "$TAP" 0
check "tap to click switched off: synaptics touchpad" wait_prop_re "$SYN" "Synaptics Tap Action" ', 0, 0, 0$'
set_ini tap_to_click true
check "tap to click switched on again" wait_prop "$TP" "$TAP" 1
set_ini mouse_natural_scroll true
check "mouse natural scrolling switched on" wait_prop "$MOUSE" "$NAT" 1
check "mouse natural scrolling does not change the touchpads" wait_prop "$TP" "$NAT" 1 5
r=$(scroll "$MOUSE" up wheel); d=$(direction "$r")
if [ "$d" = down ]; then pass "mouse with natural scrolling: wheel up scrolls the view down ($r)"
else fail "mouse with natural scrolling: wheel up scrolls the view down (got $d: $r)"; fi
set_ini mouse_natural_scroll false
check "mouse natural scrolling switched off" wait_prop "$MOUSE" "$NAT" 0
r=$(scroll "$MOUSE" up wheel); d=$(direction "$r")
if [ "$d" = up ]; then pass "mouse: wheel up scrolls the view up ($r)"
else fail "mouse: wheel up scrolls the view up (got $d: $r)"; fi
set_ini pointer_speed 0.75
check "pointer speed applies to the touchpad (libinput Accel Speed 0.5)" wait_prop "$TP" "libinput Accel Speed" 0.500000
check "pointer speed applies to the mouse" wait_prop "$MOUSE" "libinput Accel Speed" 0.500000
set_ini pointer_acceleration false
check "pointer acceleration off = flat profile" wait_prop_re "$MOUSE" "libinput Accel Profile Enabled" '^0, 1(, 0)*$'
set_ini pointer_acceleration true
check "pointer acceleration on = adaptive profile" wait_prop_re "$MOUSE" "libinput Accel Profile Enabled" '^1, 0(, 0)*$'

# something else changes a device behind HDE's back: SIGHUP re-applies everything
xinput set-prop "pointer:$TP" "$NAT" 0
kill -HUP "$XS"
check "SIGHUP: hde-xsettings re-applies the settings" wait_prop "$TP" "$NAT" 1

# ---------- hde-settings --apply (login path when hde-xsettings is not running) ----------
kill "$XS" 2>/dev/null
wait "$XS" 2>/dev/null
xinput set-prop "pointer:$TP" "$NAT" 0
xinput set-prop "pointer:$TP" "$TAP" 0
PATH=/nonexistent "$B/hde-settings" --apply > "$OUT/settings-apply.log" 2>&1
check "hde-settings --apply sets natural scrolling (no xinput program needed)" wait_prop "$TP" "$NAT" 1 10
check "hde-settings --apply sets tap to click" wait_prop "$TP" "$TAP" 1 10
check "hde-settings --apply logs what it changed" grep -q "hde-settings: input: $TP (touchpad, libinput): natural scrolling on, tap to click on" "$OUT/settings-apply.log"

# ---------- the Input page lists the devices ----------
if command -v dbus-run-session >/dev/null 2>&1; then
    HDE_DEBUG=1 dbus-run-session -- "$B/hde-settings" input > "$OUT/settings.log" 2>&1 &
    SP=$!
    sleep 4
    command -v import >/dev/null 2>&1 && import -display "$DISPLAY" -window root "$OUT/shot-20-settings-input.png" 2>/dev/null
    if grep -q "hde-settings: input device: $TP: Touchpad · libinput driver · natural scrolling on · tap to click on" "$OUT/settings.log" &&
       grep -q "hde-settings: input device: $SYN: Touchpad · synaptics driver" "$OUT/settings.log" &&
       grep -q "hde-settings: input device: $MOUSE: Mouse · libinput driver" "$OUT/settings.log"; then
        pass "Settings > Input lists the touchpads and the mouse with their state"
    else fail "Settings > Input lists the touchpads and the mouse with their state"; fi
    kill "$SP" 2>/dev/null
    pkill -x hde-settings 2>/dev/null
fi

[ "$FAILS" -gt 0 ] && { echo "--- hde-xsettings log (end)"; tail -n 25 "$OUT/xsettings.log"; } | sed 's/^/INFO:   /' >> "$OUT/results.txt"
cleanup
echo "== results ($OUT/results.txt)"
cat "$OUT/results.txt"
[ "$FAILS" -gt 100 ] && FAILS=100
exit "$FAILS"
