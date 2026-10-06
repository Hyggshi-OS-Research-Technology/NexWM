#!/bin/sh
# tests/display-test.sh — F6/F7 brightness and F8 "Project" on a REAL X server with several screens: Xorg with the
# dummy video driver, whose RandR 1.2 outputs DUMMY0..DUMMY15 (xf86-video-dummy >= 0.4.0, Ubuntu 24.04) behave like
# real connectors: DUMMY0 is the computer's screen, DUMMY1 a second screen "plugged in" (connected once it got a mode).
# A whole HDE session runs (hde-session, Metacity): F8 opens the Project window, F8 again moves on, Enter applies;
# Extend / Duplicate / Second screen only / PC screen only really change the screens (checked with xrandr); the panel
# and the desktop follow; "Second screen only" goes back by itself when nobody keeps it; F6/F7 dim the screens through
# the gamma ramps (no backlight in a VM); Night Light; the layout chosen with F8 comes back at login; a screen plugged
# in opens the Project window; a laptop backlight (a fake /sys/class/backlight); the memory the desktop uses.
#
# Needs root through sudo (Xorg): meant for CI, not a desktop.
#   sudo apt install xserver-xorg-core xserver-xorg-video-dummy x11-xserver-utils x11-utils xdotool metacity dbus-x11
#   make && sh tests/display-test.sh
# Output: $HDE_TEST_OUT (default /tmp/hde-display): results.txt, session.log, Xorg.log, shot-*.png
# shellcheck disable=SC2329  # helpers called through check()
set -u
BUILD=${BUILD:-build}
B=$(cd "$BUILD" 2>/dev/null && pwd) || { echo "build dir '$BUILD' not found"; exit 2; }
OUT=${HDE_TEST_OUT:-/tmp/hde-display}
DISP=${HDE_DISPLAY_TEST_DISPLAY:-:8}

if [ -z "${HDE_DISPLAY_INNER:-}" ]; then
    rm -rf "$OUT"
    mkdir -p "$OUT/home/.config/hde" "$OUT/home/Desktop" "$OUT/run"
    chmod 700 "$OUT/run"
    : > "$OUT/results.txt"
    for t in Xorg xrandr xdotool sudo dbus-run-session; do
        command -v $t >/dev/null 2>&1 || { echo "FAIL: missing $t" | tee -a "$OUT/results.txt"; exit 2; }
    done
    sudo -n true 2>/dev/null || { echo "FAIL: needs password-less sudo" | tee -a "$OUT/results.txt"; exit 2; }
    echo "INFO: $(dpkg-query -W -f '${Package} ${Version}, ' xserver-xorg-core xserver-xorg-video-dummy metacity 2>/dev/null)" \
        | tee -a "$OUT/results.txt"
    cat > "$OUT/xorg.conf" <<'EOF'
Section "ServerFlags"
    Option "AutoAddDevices" "false"
    Option "AutoAddGPU" "false"
    Option "DontVTSwitch" "true"
EndSection
Section "Device"
    Identifier "dummy"
    Driver "dummy"
    VideoRam 262144
EndSection
Section "Monitor"
    Identifier "monitor"
    HorizSync 5.0 - 1000.0
    VertRefresh 5.0 - 200.0
    Modeline "1280x800" 83.50 1280 1352 1480 1680 800 803 809 831 -hsync +vsync
    Modeline "1024x768" 65.00 1024 1048 1184 1344 768 771 777 806 -hsync -vsync
EndSection
Section "Screen"
    Identifier "screen"
    Device "dummy"
    Monitor "monitor"
    DefaultDepth 24
    SubSection "Display"
        Depth 24
        Modes "1280x800" "1024x768"
    EndSubSection
EndSection
EOF
    # shellcheck disable=SC2024  # the output file belongs to the user on purpose
    sudo -n Xorg "$DISP" -config "$OUT/xorg.conf" -noreset -nolisten tcp -ac -logfile "$OUT/Xorg.log" \
        > "$OUT/xorg-output.log" 2>&1 &
    XORG=$!
    export DISPLAY="$DISP"
    for i in $(seq 1 100); do xrandr >/dev/null 2>&1 && break; sleep 0.1; done
    if ! xrandr >/dev/null 2>&1; then
        echo "FAIL: Xorg (dummy video driver) starts" | tee -a "$OUT/results.txt"
        tail -n 30 "$OUT/Xorg.log" | sed 's/^/INFO:   /' | tee -a "$OUT/results.txt"
        sudo -n kill "$XORG" 2>/dev/null
        exit 1
    fi
    export HOME="$OUT/home" XDG_CONFIG_HOME="$OUT/home/.config" XDG_CACHE_HOME="$OUT/home/.cache"
    export XDG_DATA_HOME="$OUT/home/.local/share" XDG_RUNTIME_DIR="$OUT/run" LANG=C.UTF-8 NO_AT_BRIDGE=1
    unset WAYLAND_DISPLAY HDE_SESSION_PID HDE_WM DBUS_SESSION_BUS_ADDRESS XDG_CURRENT_DESKTOP
    printf '[settings]\nwm=metacity\n' > "$OUT/home/.config/hde/settings.ini"
    HDE_DISPLAY_INNER=1 dbus-run-session -- sh "$0"
    rc=$?
    sudo -n kill "$XORG" 2>/dev/null
    sleep 1
    echo "== results ($OUT/results.txt)"
    cat "$OUT/results.txt"
    exit $rc
fi

# ===================== inside dbus-run-session =====================
FAILS=0
pass() { echo "PASS: $*" | tee -a "$OUT/results.txt"; }
fail() { echo "FAIL: $*" | tee -a "$OUT/results.txt"; FAILS=$((FAILS + 1)); }
info() { echo "INFO: $*" | tee -a "$OUT/results.txt"; }
check() { desc=$1; shift; if "$@" >/dev/null 2>&1; then pass "$desc"; else fail "$desc"; fi; }
shot() { command -v import >/dev/null 2>&1 && import -display "$DISPLAY" -window root "$OUT/shot-$1.png" 2>/dev/null; }
INI="$XDG_CONFIG_HOME/hde/settings.ini"
LOG="$OUT/session.log"
set_ini() { if grep -q "^$1=" "$INI"; then sed -i "s|^$1=.*|$1=$2|" "$INI"; else echo "$1=$2" >> "$INI"; fi; }
# geom OUTPUT -> "WxH+X+Y" when it is on, "" when off
geom() { xrandr 2>/dev/null | sed -n "s/^$1 connected \(primary \)\{0,1\}\([0-9]*x[0-9]*+[0-9]*+[0-9]*\).*/\2/p"; }
screen_size() { xrandr 2>/dev/null | sed -n 's/^Screen 0:.*current \([0-9]*\) x \([0-9]*\),.*/\1x\2/p'; }
connected() { xrandr 2>/dev/null | grep -q "^$1 connected"; }
verbose_field() { xrandr --verbose 2>/dev/null | awk -v o="$1" -v f="$2:" '$1 == o { on = 1; next } /^[A-Za-z]/ && $2 ~ /connected/ { on = 0 } on && $1 == f { print $2; exit }'; }
wait_geom() {   # OUTPUT EXPECTED(""=off or a regex) [tenths]
    i=0
    while [ "$i" -lt "${3:-60}" ]; do
        g=$(geom "$1")
        if [ -z "$2" ]; then [ -z "$g" ] && return 0; else echo "$g" | grep -Eq "^$2$" && return 0; fi
        sleep 0.1; i=$((i + 1))
    done
    return 1
}
win_visible() { xdotool search --onlyvisible --name "$1" >/dev/null 2>&1; }
wait_win() { i=0; while [ "$i" -lt "${2:-60}" ]; do win_visible "$1" && return 0; sleep 0.1; i=$((i + 1)); done; return 1; }
wait_nowin() { i=0; while [ "$i" -lt "${2:-60}" ]; do win_visible "$1" || return 0; sleep 0.1; i=$((i + 1)); done; return 1; }
wait_log() { i=0; while [ "$i" -lt "${2:-60}" ]; do grep -q "$1" "$LOG" 2>/dev/null && return 0; sleep 0.1; i=$((i + 1)); done; return 1; }
nlog() { nlog_n=$(grep -c "$1" "$LOG" 2>/dev/null); echo "${nlog_n:-0}"; }
wait_more() { i=0; while [ "$i" -lt "${3:-60}" ]; do [ "$(nlog "$1")" -gt "$2" ] && return 0; sleep 0.1; i=$((i + 1)); done; return 1; }
# widget NAME [LOG] -> "X Y", the centre of a widget of hde-settings (HDE_DEBUG: "hde-settings: widget NAME at ...")
widget() {
    sed -n "s/^hde-settings: widget $1 at \([0-9-]*\),\([0-9-]*\) \([0-9]*\)x\([0-9]*\)$/\1 \2 \3 \4/p" "${2:-$LOG}" | tail -n 1 |
        awk '{ printf "%d %d\n", $1 + $3 / 2, $2 + $4 / 2 }'
}
# shellcheck disable=SC2046  # "X Y" -> two arguments
click_widget() { set -- $(widget "$1" "${2:-$LOG}"); [ -n "${2:-}" ] && xdotool mousemove "$1" "$2" click 1; }
root_prop() { xprop -root "$1" 2>/dev/null | sed -n 's/.*= \([0-9]*\)$/\1/p'; }
# xrandr prints the brightness as "0.95", "0.60", "1.0": as a whole percentage
pct() { echo "${1:-0}" | awk '{ printf "%d", $1 * 100 + 0.5 }'; }

info "xrandr at start: $(xrandr 2>/dev/null | grep -E '^(Screen|DUMMY[0-2] )' | tr '\n' ';' | cut -c1-400)"
if ! connected DUMMY0 || ! xrandr 2>/dev/null | grep -q "^DUMMY1 "; then
    info "this dummy driver has no RandR 1.2 outputs (xf86-video-dummy < 0.4.0): screen tests skipped"
    exit 0
fi
pass "Xorg with the dummy driver has RandR outputs (DUMMY0 on: $(geom DUMMY0))"

# a second screen is "plugged in": the dummy driver reports an output as connected once it had a mode (a disconnected
# output has no modes: give it one first, like xpra does)
plug() {    # OUTPUT RIGHT_OF [off]
    xrandr --addmode "$1" 1024x768 2>> "$OUT/xrandr.log"
    xrandr --output "$1" --mode 1024x768 --right-of "$2" 2>> "$OUT/xrandr.log"
    [ "${3:-}" = off ] && xrandr --output "$1" --off 2>> "$OUT/xrandr.log"
    return 0
}
plug DUMMY1 DUMMY0 off
sleep 0.5
if connected DUMMY1 && [ -z "$(geom DUMMY1)" ]; then pass "a second screen (DUMMY1) is connected and off, like a monitor just plugged in"
else
    fail "a second screen (DUMMY1) is connected and off ($(xrandr | grep '^DUMMY1 '))"
    sed 's/^/INFO:   xrandr: /' "$OUT/xrandr.log" | tee -a "$OUT/results.txt"
fi

export HDE_DEBUG=1 HDE_DISPLAY_CONFIRM_SECONDS=5
"$B/hde-session" > "$LOG" 2>&1 &
SESSION=$!
export HDE_SESSION_PID=$SESSION
sleep 7
shot 00-session
for p in hde-panel hde-desktop hde-hotkeys hde-xsettings; do check "$p is running" pgrep -x $p; done
check "the display service sees both screens at login" grep -q "hde-xsettings: displays: DUMMY0 1280x800+0+0 (PC screen), DUMMY1 off" "$LOG"

"$B/hde-settings" --displays > "$OUT/displays-1.txt" 2>&1
if grep -q "^Layout: PC screen only" "$OUT/displays-1.txt" && grep -q "DUMMY1: DUMMY1, off" "$OUT/displays-1.txt"; then
    pass "hde-settings --displays: PC screen only, DUMMY1 connected and off"
else fail "hde-settings --displays ($(tr '\n' ';' < "$OUT/displays-1.txt"))"; fi

# ---------- 1. F8 opens the Project window, F8 again moves on, Enter applies ----------
xdotool key F8
if wait_win "^Project$"; then
    pass "F8 opens the Project window"
    sleep 0.8
    shot 01-project
    check "... it lists 2 screens and the layout in use (PC screen only)" wait_log "hde-settings: project: shown: 2 screen(s): .*layout now: PC screen only"
    n0=$(nlog "hde-settings: project: selected"); xdotool key F8
    if wait_more "hde-settings: project: selected Duplicate" 0 30; then pass "F8 again moves to the next choice (Duplicate) instead of opening a second window"
    else fail "F8 again moves to the next choice (Duplicate)"; fi
    check "... and the second F8 only told the open window (one Project window)" wait_log "hde-settings: project: already open: next layout"
    xdotool key F8; sleep 0.6
    check "F8 a third time: Extend" wait_log "hde-settings: project: selected Extend"
    shot 02-project-extend-selected
    xdotool key Return
    if wait_geom DUMMY1 "[0-9]+x[0-9]+\+1280\+0" 60 && [ "$(geom DUMMY0)" = "1280x800+0+0" ]; then
        pass "Enter applies Extend: DUMMY1 to the right of DUMMY0 ($(geom DUMMY0), $(geom DUMMY1), desktop $(screen_size))"
    else fail "Enter applies Extend (DUMMY0 $(geom DUMMY0), DUMMY1 $(geom DUMMY1))"; fi
    check "the Project window closes after applying" wait_nowin "^Project$"
    check "the panel stays on the primary screen (DUMMY0) after the change" wait_log "hde-panel: screens changed: panel at 0,766 1280x34"
    w=$(screen_size)
    check "the desktop window covers the whole extended desktop ($w)" wait_log "hde-desktop: screens changed: desktop $w"
    sleep 1.5
    shot 03-extended
else
    fail "F8 opens the Project window"
    grep -E "hde-(hotkeys|settings)" "$LOG" | tail -n 8 | sed 's/^/INFO:   /' | tee -a "$OUT/results.txt"
fi
"$B/hde-xsettings" --status > "$OUT/status-extend.txt" 2>&1
check "hde-xsettings --status shows the screens and the layout (Extend)" grep -q "^Displays: .*(layout: Extend)" "$OUT/status-extend.txt"
check "the choice is remembered for these screens (display_mode=extend, display_outputs=DUMMY0,DUMMY1)" \
    sh -c "grep -q '^display_mode=extend' '$INI' && grep -q '^display_outputs=DUMMY0,DUMMY1' '$INI'"

# ---------- 2. F6 / F7: software dimming of every screen (no backlight here) ----------
br0=$(verbose_field DUMMY0 Brightness)
xdotool key F6; sleep 1
b0=$(verbose_field DUMMY0 Brightness); b1=$(verbose_field DUMMY1 Brightness)
if [ "$(pct "$b0")" = 95 ] && [ "$(pct "$b1")" = 95 ]; then pass "F6 dims both screens to 95% (gamma: DUMMY0 $b0, DUMMY1 $b1; was $br0)"
else fail "F6 dims both screens to 95% (DUMMY0 $b0, DUMMY1 $b1, was $br0)"; fi
check "... remembered for every HDE program (_HDE_BRIGHTNESS = 95)" sh -c "[ \"\$(xprop -root _HDE_BRIGHTNESS 2>/dev/null | sed -n 's/.*= //p')\" = 95 ]"
if [ "$(root_prop _HDE_BRIGHTNESS)" != 95 ]; then info "_HDE_BRIGHTNESS: $(xprop -root _HDE_BRIGHTNESS 2>&1)"; fi
shot 04-osd-brightness
xdotool key F6; sleep 0.6; xdotool key F6; sleep 1
b0=$(verbose_field DUMMY0 Brightness)
if [ "$(pct "$b0")" = 85 ]; then pass "F6 twice more: 85%"; else fail "F6 twice more: 85% (got $b0)"; fi
check "the session log tells how (software dimming)" grep -q "hde-hotkeys: brightness down: software dimming: 85%" "$LOG"
xdotool key F7; sleep 0.6; xdotool key F7; sleep 0.6; xdotool key F7; sleep 0.6; xdotool key F7; sleep 1
b0=$(verbose_field DUMMY0 Brightness)
if [ "$(pct "$b0")" = 100 ]; then pass "F7 brightens again up to 100% (stops there)"; else fail "F7 brightens again up to 100% (got $b0)"; fi
"$B/hde-settings" --brightness 60 > "$OUT/brightness-cli.txt" 2>&1
b1=$(verbose_field DUMMY1 Brightness)
if grep -q "software dimming: 60%" "$OUT/brightness-cli.txt" && [ "$(pct "$b1")" = 60 ]; then pass "hde-settings --brightness 60: $(head -n1 "$OUT/brightness-cli.txt")"
else fail "hde-settings --brightness 60 ($(tr '\n' ';' < "$OUT/brightness-cli.txt"), DUMMY1 $b1)"; fi

# ---------- 3. Night Light ----------
set_ini night_light true
i=0; while [ "$i" -lt 40 ]; do g=$(verbose_field DUMMY0 Gamma); [ -n "$g" ] && [ "$g" != "1.0:1.0:1.0" ] && break; sleep 0.1; i=$((i + 1)); done
if [ "$g" != "1.0:1.0:1.0" ] && [ -n "$g" ]; then pass "Night Light on: warmer colours on the screens (gamma $g), together with the dimming ($(verbose_field DUMMY0 Brightness))"
else fail "Night Light on: warmer colours (gamma ${g:-?})"; fi
shot 05-night-light
set_ini night_light false
"$B/hde-settings" --brightness 100 > /dev/null 2>&1
sleep 1.5
g=$(verbose_field DUMMY0 Gamma); b0=$(verbose_field DUMMY0 Brightness)
if [ "$g" = "1.0:1.0:1.0" ] && [ "$(pct "$b0")" = 100 ]; then pass "Night Light off + 100%: the screens are back to normal"
else fail "Night Light off + 100%: back to normal (gamma $g, brightness $b0)"; fi

# ---------- 4. Duplicate (without a window) ----------
"$B/hde-settings" --display-mode duplicate > "$OUT/duplicate.txt" 2>&1
g0=$(geom DUMMY0); g1=$(geom DUMMY1)
if [ -n "$g0" ] && [ "$g0" = "$g1" ] && echo "$g0" | grep -q "+0+0$"; then pass "hde-settings --display-mode duplicate: the same picture on both ($g0)"
else fail "Duplicate (DUMMY0 $g0, DUMMY1 $g1: $(cat "$OUT/duplicate.txt"))"; fi
sleep 1.5
shot 06-duplicate

close_project() { win_visible "^Project$" && xdotool key Escape && sleep 0.5; return 0; }
# ---------- 5. Second screen only: nobody keeps it -> back by itself ----------
close_project
xdotool key F8
if wait_win "^Project$"; then
    sleep 0.6
    xdotool key 4
    if wait_geom DUMMY0 "" 40 && wait_geom DUMMY1 "[0-9]+x[0-9]+\+0\+0" 20; then pass "4 in the Project window: Second screen only (DUMMY0 off, DUMMY1 $(geom DUMMY1))"
    else fail "4 in the Project window: Second screen only (DUMMY0 $(geom DUMMY0), DUMMY1 $(geom DUMMY1))"; fi
    if wait_win "^Keep these display settings\?$" 40; then
        pass "the PC screen is off: 'Keep these display settings?' asks"
        sleep 1.2
        shot 07-keep-settings
        if wait_geom DUMMY0 "[0-9]+x[0-9]+\+0\+0" 90; then pass "no answer: back to the previous layout after the countdown (DUMMY0 $(geom DUMMY0), DUMMY1 $(geom DUMMY1))"
        else fail "no answer: back to the previous layout after the countdown (DUMMY0 still off)"; fi
        check "... and the session log says so" wait_log "hde-settings: project: no answer: going back"
    else fail "the PC screen is off: 'Keep these display settings?' asks"; fi
else fail "F8 opens the Project window (2nd time)"; fi
sleep 1

# ---------- 6. Second screen only, kept ----------
close_project
xdotool key F8
if wait_win "^Project$"; then
    sleep 0.6
    click_widget project-second
    if wait_win "^Keep these display settings\?$" 40; then
        sleep 1.5
        n0=$(nlog "hde-settings: project: kept")
        click_widget project-keep
        if wait_more "hde-settings: project: kept" "$n0" 30; then pass "clicking 'Keep changes' keeps Second screen only"
        else fail "clicking 'Keep changes' keeps Second screen only"; fi
        sleep 7
        if [ -z "$(geom DUMMY0)" ] && [ -n "$(geom DUMMY1)" ]; then pass "... still Second screen only after the countdown time"
        else fail "... still Second screen only after the countdown time (DUMMY0 $(geom DUMMY0))"; fi
        check "the panel moved to the screen in use (DUMMY1)" grep -q "hde-panel: screens changed: panel at 0,[0-9]* [0-9]*x34 (primary screen [0-9]*x[0-9]*+0+0, 1 screen(s))" "$LOG"
        shot 08-second-only
    else fail "Second screen only by clicking its tile asks to keep it"; fi
else fail "F8 opens the Project window (3rd time)"; fi

# ---------- 7. PC screen only with Super+P ----------
close_project
xdotool key super+p
if wait_win "^Project$"; then
    pass "Super+P opens the Project window too"
    check "... showing that Second screen only is in use" wait_log "hde-settings: project: shown: 2 screen(s): .*layout now: Second screen only"
    sleep 0.6
    xdotool key 1
    if wait_geom DUMMY1 "" 40 && wait_geom DUMMY0 "[0-9]+x[0-9]+\+0\+0" 20; then pass "1: PC screen only (DUMMY0 $(geom DUMMY0), DUMMY1 off)"
    else fail "1: PC screen only (DUMMY0 $(geom DUMMY0), DUMMY1 $(geom DUMMY1))"; fi
else fail "Super+P opens the Project window"; fi
sleep 1.5

# ---------- 8. the layout chosen with F8 comes back at login ----------
close_project
set_ini display_mode extend
pkill -x hde-xsettings; sleep 0.5
"$B/hde-xsettings" > "$OUT/xsettings-login.log" 2>&1 &
if wait_geom DUMMY1 "[0-9]+x[0-9]+\+1280\+0" 60 || wait_geom DUMMY1 "[0-9]+x[0-9]+\+[0-9]+\+0" 10; then
    pass "at login the layout chosen earlier for these screens comes back (Extend: DUMMY1 $(geom DUMMY1))"
else fail "at login the layout chosen earlier comes back (DUMMY1 $(geom DUMMY1))"; fi
check "... and hde-xsettings logs it" grep -q "hde-xsettings: displays: the layout chosen earlier for DUMMY0,DUMMY1 (Extend applied)" "$OUT/xsettings-login.log"

# ---------- 9. a screen plugged in: the Project window asks ----------
plug DUMMY2 DUMMY1
xrandr > /dev/null 2>&1      # the dummy driver says "connected" at the next look, a real screen raises a hotplug event
if wait_win "^Project$" 80; then
    pass "a third screen (DUMMY2) plugged in: the Project window opens by itself"
    check "... saying which screen is new" grep -q "hde-xsettings: displays: connected: DUMMY2 (display_connect=ask)" "$OUT/xsettings-login.log"
    sleep 1
    shot 09-new-screen
    xdotool key Escape
    check "Esc closes it" wait_nowin "^Project$"
else
    fail "a third screen (DUMMY2) plugged in: the Project window opens by itself"
    tail -n 5 "$OUT/xsettings-login.log" | sed 's/^/INFO:   /' | tee -a "$OUT/results.txt"
fi
"$B/hde-settings" --display-mode pc > /dev/null 2>&1
sleep 1

# ---------- 10. a laptop backlight (fake /sys/class/backlight) ----------
BL="$OUT/backlight/hde_test_bl"
mkdir -p "$BL"
echo 1000 > "$BL/max_brightness"; echo 500 > "$BL/brightness"; echo raw > "$BL/type"
out=$(HDE_BACKLIGHT_DIR="$OUT/backlight" "$B/hde-settings" --brightness +10 2>&1)
if [ "$(cat "$BL/brightness")" = 600 ] && echo "$out" | grep -q "backlight hde_test_bl: 60%"; then pass "a backlight is used first: 50% +10 -> 60% ($out)"
else fail "a backlight is used first: 50% +10 -> 60% (file $(cat "$BL/brightness"): $out)"; fi
HDE_BACKLIGHT_DIR="$OUT/backlight" "$B/hde-settings" --brightness 20 > /dev/null 2>&1
check "--brightness 20 sets the backlight to 200 of 1000" sh -c "[ \"\$(cat '$BL/brightness')\" = 200 ]"
HDE_BACKLIGHT_DIR="$OUT/backlight" "$B/hde-settings" --brightness 0 > /dev/null 2>&1
check "the backlight never goes all the way off (0% -> 10 of 1000)" sh -c "[ \"\$(cat '$BL/brightness')\" = 10 ]"
chmod 444 "$BL/brightness"
out=$(HDE_BACKLIGHT_DIR="$OUT/backlight" "$B/hde-settings" --brightness 80 2>&1)
if echo "$out" | grep -q "software dimming" && echo "$out" | grep -q "could not be changed"; then
    pass "a backlight HDE may not write (no logind session here): software dimming instead, and it says why"
else fail "an unwritable backlight falls back to software dimming ($out)"; fi
"$B/hde-settings" --brightness 100 > /dev/null 2>&1

# ---------- 10b. desktop monitors: their own brightness over DDC/CI (a fake ddcutil) ----------
mkdir -p "$OUT/fakebin"
cat > "$OUT/fakebin/ddcutil" <<'DDC'
#!/bin/sh
# fake ddcutil: one monitor that answers DDC/CI on /dev/i2c-7, one that does not
st=$HDE_FAKE_DDC
echo "ARGS: $*" >> "$st.log"
case "$*" in
    "detect --terse") printf 'Display 1\n   I2C bus:  /dev/i2c-7\n   Monitor:  DEL:DELL U2415:CFV9N7\n\nInvalid display\n   I2C bus:  /dev/i2c-8\n' ;;
    "--bus 7 getvcp 10 --terse") printf 'VCP 10 C %s 100\n' "$(cat "$st" 2>/dev/null || echo 50)" ;;
    "--bus 7 setvcp 10 "*" --noverify") echo "$5" > "$st" ;;
    *) exit 1 ;;
esac
DDC
chmod +x "$OUT/fakebin/ddcutil"
export HDE_FAKE_DDC="$OUT/ddc-brightness"
rm -f "$XDG_RUNTIME_DIR/hde-ddc.cache"
out=$(PATH="$OUT/fakebin:$PATH" "$B/hde-settings" --brightness 40 2>&1)
if [ "$(cat "$HDE_FAKE_DDC" 2>/dev/null)" = 40 ] && echo "$out" | grep -q "DDC/CI DELL U2415: 40%"; then
    pass "no backlight, ddcutil installed: the monitor's own brightness over DDC/CI ($out)"
else fail "the monitor's own brightness over DDC/CI ($out; $(tr '\n' ';' < "$HDE_FAKE_DDC.log" 2>/dev/null))"; fi
out=$(PATH="$OUT/fakebin:$PATH" "$B/hde-settings" --brightness -10 2>&1)
if [ "$(cat "$HDE_FAKE_DDC" 2>/dev/null)" = 30 ]; then pass "... a step down: 30% ($out)"; else fail "... a step down: 30% ($out)"; fi
n=$(grep -c '^ARGS: detect' "$HDE_FAKE_DDC.log" 2>/dev/null)
if [ "$n" = 1 ]; then pass "... the monitors are looked for once (cached for these screens)"
else fail "... the monitors are looked for once (cached for these screens): $n times"; fi
b0=$(verbose_field DUMMY0 Brightness)
if [ "$(pct "$b0")" = 100 ]; then pass "... and no software dimming on top of it (gamma $b0)"
else fail "... and no software dimming on top of it (gamma $b0)"; fi
out=$(PATH="$OUT/fakebin:$PATH" HDE_DDC=0 "$B/hde-settings" --brightness 2>&1)
case "$out" in "software dimming"*) pass "HDE_DDC=0: software dimming again" ;; *) fail "HDE_DDC=0: software dimming again ($out)" ;; esac
rm -f "$XDG_RUNTIME_DIR/hde-ddc.cache"

# ---------- 10c. the resolution of a screen (Settings > Display > Resolution, hde-settings --display-set) ----------
"$B/hde-settings" --display-mode extend > /dev/null 2>&1; sleep 1
info "screens now: DUMMY0 $(geom DUMMY0), DUMMY1 $(geom DUMMY1), desktop $(screen_size); DUMMY0 can: $(xrandr 2>/dev/null | awk '/^DUMMY0 /{on=1;next} /^[A-Za-z]/{on=0} on{printf "%s ", $1}')"
out=$("$B/hde-settings" --display-set DUMMY0 1024x768 2>&1)
if wait_geom DUMMY0 "1024x768\+0\+0" 30 && wait_geom DUMMY1 "[0-9]+x[0-9]+\+1024\+0" 20; then
    pass "--display-set DUMMY0 1024x768: smaller, and DUMMY1 on its right moves along (DUMMY1 $(geom DUMMY1), desktop $(screen_size))"
else fail "--display-set DUMMY0 1024x768 (DUMMY0 $(geom DUMMY0), DUMMY1 $(geom DUMMY1): $out)"; fi
check "... remembered for the next logins (display_modes=DUMMY0=1024x768@...)" grep -q "^display_modes=DUMMY0=1024x768@" "$INI"
"$B/hde-settings" --display-set DUMMY1 1024x768 left > "$OUT/display-set-left.txt" 2>&1
if xrandr 2>/dev/null | grep -q "^DUMMY1 .*(normal left"; then
    if wait_geom DUMMY1 "768x1024\+1024\+0" 30; then pass "--display-set DUMMY1 1024x768 left: portrait ($(geom DUMMY1), desktop $(screen_size))"
    else fail "--display-set DUMMY1 1024x768 left: portrait (DUMMY1 $(geom DUMMY1): $(tr '\n' ' ' < "$OUT/display-set-left.txt"))"; fi
elif grep -q "cannot be turned" "$OUT/display-set-left.txt"; then
    pass "--display-set DUMMY1 ... left: this driver cannot rotate, and HDE says so ($(tr '\n' ' ' < "$OUT/display-set-left.txt"))"
else fail "--display-set DUMMY1 ... left on a driver that cannot rotate ($(tr '\n' ' ' < "$OUT/display-set-left.txt"))"; fi
"$B/hde-settings" --display-set DUMMY1 auto normal > /dev/null 2>&1
"$B/hde-settings" --display-set DUMMY0 auto > /dev/null 2>&1
if wait_geom DUMMY0 "1280x800\+0\+0" 30 && wait_geom DUMMY1 "[0-9]+x[0-9]+\+1280\+0" 20; then
    pass "--display-set ... auto: the native resolution again (DUMMY0 $(geom DUMMY0), DUMMY1 $(geom DUMMY1))"
else fail "--display-set ... auto: the native resolution again (DUMMY0 $(geom DUMMY0), DUMMY1 $(geom DUMMY1))"; fi
check "... and the choice is forgotten" sh -c "! grep -q '^display_modes=.*DUMMY0=' '$INI'"
"$B/hde-settings" --display-mode pc > /dev/null 2>&1; sleep 1
# at login: the resolution chosen in Settings comes back
set_ini display_modes "DUMMY0=1024x768@60.00/normal"
pkill -x hde-xsettings; sleep 0.5
"$B/hde-xsettings" > "$OUT/xsettings-login2.log" 2>&1 &
if wait_geom DUMMY0 "1024x768\+0\+0" 60; then pass "at login the resolution chosen in Settings comes back (DUMMY0 $(geom DUMMY0))"
else fail "at login the resolution chosen in Settings comes back (DUMMY0 $(geom DUMMY0))"; fi
check "... and hde-xsettings logs it" grep -q "hde-xsettings: displays: the resolutions chosen in Settings: DUMMY0 1024x768@" "$OUT/xsettings-login2.log"
set_ini display_modes ""
"$B/hde-settings" --display-set DUMMY0 auto > /dev/null 2>&1; sleep 1
# the Display page: another resolution from the list, nobody keeps it -> back by itself; then one that is kept
pkill -x hde-settings 2>/dev/null; sleep 0.5
SLOG="$OUT/settings-res.log"         # (not appended to the session log: hde-session's own writes would cover it)
"$B/hde-settings" display > "$SLOG" 2>&1 &
SRES=$!
if wait_win "Hyggshi Settings" 60; then
    sleep 2
    shot 13-settings-display-resolution
    if [ -n "$(widget display-resolution "$SLOG")" ]; then
        click_widget display-resolution "$SLOG"; sleep 0.8
        xdotool key Down; sleep 0.3; xdotool key Return
        if wait_win "^Keep these display settings\?$" 40 && ! wait_geom DUMMY0 "1280x800\+0\+0" 5; then
            pass "a new resolution from the list applies at once and asks to keep it (DUMMY0 $(geom DUMMY0))"
            sleep 1; shot 14-keep-resolution
            if wait_geom DUMMY0 "1280x800\+0\+0" 90; then pass "... nobody answers: back to 1280x800 after the countdown"
            else fail "... nobody answers: back to 1280x800 after the countdown (DUMMY0 $(geom DUMMY0))"; fi
            check "... and not remembered" sh -c "! grep -q '^display_modes=.*DUMMY0=' '$INI'"
            sleep 1.5
            click_widget display-resolution "$SLOG"; sleep 0.8
            xdotool key Down; sleep 0.3; xdotool key Return
            if wait_win "^Keep these display settings\?$" 40; then
                sleep 1.5
                n0=$(grep -c "hde-settings: project: kept" "$SLOG")
                click_widget project-keep "$SLOG"
                i=0; while [ "$i" -lt 30 ] && [ "$(grep -c "hde-settings: project: kept" "$SLOG")" -le "$n0" ]; do sleep 0.1; i=$((i + 1)); done
                sleep 1
                g=$(geom DUMMY0)
                if [ -n "$g" ] && [ "$g" != "1280x800+0+0" ] && grep -q "^display_modes=DUMMY0=${g%%+*}@" "$INI"; then
                    pass "'Keep changes' keeps it and remembers it for the next logins ($g)"
                else fail "'Keep changes' keeps it and remembers it ($g; $(grep '^display_modes' "$INI"))"; fi
            else fail "a second choice from the list asks again"; fi
        else fail "a new resolution from the list applies and asks to keep it (DUMMY0 $(geom DUMMY0); $(grep 'hde-settings: display' "$SLOG" | tail -n 2 | tr '\n' ' '))"; fi
    else fail "the Display page has a resolution list (no widget position logged)"; fi
else fail "Settings opens at the Display page"; fi
kill "$SRES" 2>/dev/null; sleep 0.5
set_ini display_modes ""
"$B/hde-settings" --display-set DUMMY0 auto > /dev/null 2>&1; sleep 1

# ---------- 11. Settings: Display and About ----------
"$B/hde-settings" display > "$OUT/settings.log" 2>&1 &
SETTINGS=$!
sleep 4
check "Settings opens" wait_win "Hyggshi Settings"
shot 10-settings-display
"$B/hde-settings" about; sleep 3
shot 11-settings-about
xdotool mousemove 760 520; for i in 1 2 3 4 5 6 7; do xdotool click 5; done; sleep 1
shot 12-settings-about-memory
grep "hde-settings: about:" "$OUT/settings.log" | head -n 1 | sed 's/^/INFO: /' | tee -a "$OUT/results.txt"
kill $SETTINGS 2>/dev/null

# ---------- 12. memory used by the desktop (real Xorg, Metacity) ----------
"$B/hde-settings" --about > "$OUT/about.txt" 2>&1
if grep -q "^Desktop memory now: " "$OUT/about.txt" && grep -q "hde-panel" "$OUT/about.txt"; then
    pass "hde-settings --about measures the memory of the desktop: $(sed -n 's/^Desktop memory now: //p' "$OUT/about.txt")"
else fail "hde-settings --about measures the memory of the desktop"; fi
sed -n '/^Desktop memory now/,$p' "$OUT/about.txt" | sed 's/^/INFO: /' | tee -a "$OUT/results.txt"
grep -E "^(Model|Processor|Memory|Graphics|Window manager|Display server|Screens):" "$OUT/about.txt" | sed 's/^/INFO: /' | tee -a "$OUT/results.txt"

if grep -q "Failed to execute child process" "$LOG"; then fail "no 'Failed to execute child process' errors"
else pass "no 'Failed to execute child process' errors"; fi
kill -TERM "$SESSION"; sleep 4
pkill -x hde-xsettings 2>/dev/null
[ "$FAILS" -gt 100 ] && FAILS=100
exit "$FAILS"
