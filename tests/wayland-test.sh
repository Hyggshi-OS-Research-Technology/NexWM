#!/bin/sh
# tests/wayland-test.sh — the "HDE (Wayland)" session for real: `hde-start --wayland` starts labwc (headless backend,
# pixman renderer: no screen or GPU needed) with HDE inside, then checks the panel and the desktop as layer-shell
# surfaces, the Start menu (D-Bus command, the Super key and Ctrl+Esc through labwc's key bindings, typing), the
# taskbar (a real window), Show Desktop, the volume key, notifications, Settings and About on Wayland, PrtSc with grim,
# the panel moved to the top, labwc's configuration following settings.ini, and logging out (labwc stops).
#
#   BUILD=build sh tests/wayland-test.sh
# Needs: labwc grim wtype dbus-run-session (dbus) zenity notify-send imagemagick; HDE built with libgtk-layer-shell-dev.
# Output: $HDE_TEST_OUT (default /tmp/hde-wayland): results.txt, session.log, shot-*.png
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
BUILD=${BUILD:-build}
B=$(cd "$BUILD" 2>/dev/null && pwd) || { echo "build dir '$BUILD' not found"; exit 2; }
OUT=${HDE_TEST_OUT:-/tmp/hde-wayland}

if [ -z "${HDE_WL_INNER:-}" ]; then
    for t in labwc grim wtype dbus-run-session; do
        command -v $t >/dev/null 2>&1 || { echo "missing $t (apt install labwc grim wtype dbus)"; exit 2; }
    done
    rm -rf "$OUT"
    mkdir -p "$OUT/home/.config/hde" "$OUT/home/Desktop" "$OUT/home/.local/share/applications" "$OUT/run"
    chmod 700 "$OUT/run"
    printf '[settings]\n' > "$OUT/home/.config/hde/settings.ini"
    sed "s|@PREFIX@/bin/hde-settings|$B/hde-settings|" "$HERE/../data/hyggshi-settings.desktop" \
        > "$OUT/home/.local/share/applications/hyggshi-settings.desktop"
    echo "notes" > "$OUT/home/Desktop/notes.txt"
    export HOME="$OUT/home" XDG_CONFIG_HOME="$OUT/home/.config" XDG_CACHE_HOME="$OUT/home/.cache"
    export XDG_DATA_HOME="$OUT/home/.local/share" XDG_RUNTIME_DIR="$OUT/run" LANG=C.UTF-8 NO_AT_BRIDGE=1
    export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
    export PATH="$B:$PATH" HDE_DEBUG=1
    unset DISPLAY WAYLAND_DISPLAY HDE_SESSION_PID DBUS_SESSION_BUS_ADDRESS XDG_CURRENT_DESKTOP GDK_BACKEND
    HDE_WL_INNER=1 dbus-run-session -- sh "$0"
    rc=$?
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
LOG="$XDG_CACHE_HOME/hde/session.log"
INI="$XDG_CONFIG_HOME/hde/settings.ini"
shot() { grim "$OUT/shot-wl-$1.png" 2>/dev/null || info "grim could not take shot $1"; }   # wl-: not mixed up with make check's
wait_log() { i=0; while [ "$i" -lt "${2:-60}" ]; do grep -q "$1" "$LOG" 2>/dev/null && return 0; sleep 0.1; i=$((i + 1)); done; return 1; }
nlog() { nlog_n=$(grep -c "$1" "$LOG" 2>/dev/null); echo "${nlog_n:-0}"; }
wait_more() { i=0; while [ "$i" -lt "${3:-60}" ]; do [ "$(nlog "$1")" -gt "$2" ] && return 0; sleep 0.1; i=$((i + 1)); done; return 1; }
# a live process of that name; zombies do not count: in a container nothing reaps the orphans of labwc (it double-forks
# what its key bindings run), a real system's init does
running() {
    for rp in $(pgrep -x "$1" 2>/dev/null); do
        rs=$(awk '{print $3}' "/proc/$rp/stat" 2>/dev/null)
        [ -n "$rs" ] && [ "$rs" != Z ] && return 0
    done
    return 1
}
pixel() { convert "$1" -format "%[fx:int(255*r)] %[fx:int(255*g)] %[fx:int(255*b)]" -crop "1x1+$2+$3" info: 2>/dev/null; }

info "labwc: $(labwc --version 2>/dev/null | head -n 1); gtk-layer-shell: $(pkg-config --modversion gtk-layer-shell-0 2>/dev/null || echo ?)"
sh "$HERE/../data/hde-start" --wayland > "$OUT/hde-start.out" 2>&1 &
START=$!
i=0
while [ $i -lt 100 ]; do
    sock=""
    for f in "$XDG_RUNTIME_DIR"/wayland-[0-9]*; do
        case "$f" in *.lock) ;; *) [ -S "$f" ] && sock=$(basename "$f") ;; esac
    done
    [ -n "$sock" ] && break
    sleep 0.2; i=$((i + 1))
done
if [ -z "${sock:-}" ]; then
    fail "labwc starts (no Wayland socket in XDG_RUNTIME_DIR)"
    cp "$LOG" "$OUT/session.log" 2>/dev/null
    sed 's/^/INFO:   /' "$LOG" 2>/dev/null | tail -n 30 >> "$OUT/results.txt"
    exit 1
fi
export WAYLAND_DISPLAY="$sock"
wait_log "hde-panel: taskbar: Wayland" 100
wait_log "hde-desktop: Wayland: desktop on the background layer" 50
sleep 2

# ---------- 1. the session ----------
check "hde-start --wayland starts labwc ($sock)" running labwc
for p in hde-panel hde-desktop; do check "$p is running inside labwc" running $p; done
check "hde-session runs as the session inside labwc" grep -q "^hde-session: HDE build .*, Wayland session inside labwc" "$LOG"
if ! running hde-hotkeys && ! running hde-xsettings && ! running metacity; then pass "no hde-hotkeys daemon / hde-xsettings / window manager on Wayland"
else fail "no hde-hotkeys daemon / hde-xsettings / window manager on Wayland"; fi
check "labwc's configuration is written from settings.ini (rc.xml, menu.xml, environment, themerc-override)" \
    grep -q "hde-settings: wayland: labwc .* configuration in .*/hde/labwc: rc.xml, menu.xml, environment, themerc-override" "$LOG"
RC="$XDG_CONFIG_HOME/hde/labwc/rc.xml"
if grep -q 'key="Super_L" onRelease="yes"' "$RC"; then pass "rc.xml: the Super key alone opens the Start menu (onRelease)"
elif grep -q 'key="W-space"' "$RC"; then pass "rc.xml: Super+Space opens the Start menu (labwc before 0.7.3 cannot bind Super alone)"
else fail "rc.xml binds a key to the Start menu"; fi
check "rc.xml: F1-F3 / media keys / PrtSc run hde-hotkeys --action" sh -c "grep -q 'key=\"F3\".*hde-hotkeys --action volume-up' '$RC' && grep -q 'key=\"Print\".*--action screenshot' '$RC'"
check "rc.xml: the touchpad settings (natural scrolling, tap to click)" sh -c "grep -q '<naturalScroll>yes</naturalScroll>' '$RC' && grep -q '<tap>yes</tap>' '$RC'"
check "the panel is a layer-shell surface at the bottom" grep -q "hde-panel: started: panel at the bottom, 34px high (Wayland layer shell" "$LOG"
check "the taskbar uses wlr-foreign-toplevel-management" grep -q "hde-panel: taskbar: Wayland (wlr-foreign-toplevel-management v[0-9])" "$LOG"
check "the desktop is on the background layer" grep -q "hde-desktop: Wayland: desktop on the background layer" "$LOG"
shot 01-session
if [ -f "$OUT/shot-wl-01-session.png" ]; then
    sz=$(identify -format "%wx%h" "$OUT/shot-wl-01-session.png" 2>/dev/null); W=${sz%x*}; H=${sz#*x}
    info "screen $sz"
    pb=$(pixel "$OUT/shot-wl-01-session.png" "$((W / 2))" "$((H - 4))"); pd=$(pixel "$OUT/shot-wl-01-session.png" "$((W / 2))" "$((H / 2))")
    case "$pb" in "0 0 0"|"") fail "the panel is drawn at the bottom of the screen (pixel $pb)" ;; *) pass "the panel is drawn at the bottom of the screen (pixel $pb)" ;; esac
    case "$pd" in "0 0 0"|"") fail "the desktop is drawn behind everything (pixel $pd)" ;; *) pass "the desktop is drawn behind everything (pixel $pd)" ;; esac
fi

# ---------- 2. the Start menu ----------
m0=$(nlog "start menu: shown")
"$B/hde-panel" --menu
if wait_more "start menu: shown" "$m0" 50; then pass "hde-panel --menu (D-Bus) opens the Start menu"; else fail "hde-panel --menu (D-Bus) opens the Start menu"; fi
check "... a layer-shell surface with the keyboard" grep -q "start menu: shown (modern) .*keyboard (layer shell)" "$LOG"
my=$(sed -n 's/.*start menu: shown (modern) at [0-9]*,\([0-9]*\) [0-9]*x\([0-9]*\).*/\1 \2/p' "$LOG" | tail -n 1)
if [ -n "$my" ] && [ "${my% *}" -gt 60 ]; then pass "... right above the panel at the bottom (y ${my% *}, ${my#* } high)"
else fail "... right above the panel at the bottom (${my:-no position})"; fi
sleep 1; shot 02-start-menu
wtype -s 400 -d 60 "sett"; sleep 1.5      # -s: a new virtual keyboard's keymap reaches the app first
if grep -q "start menu: search 'sett': [1-9][0-9]* result(s), best: Hyggshi Settings" "$LOG"; then pass "typing in the menu searches (sett -> Hyggshi Settings)"
else fail "typing in the menu searches ($(grep 'start menu: search' "$LOG" | tail -n 1))"; fi
shot 03-start-menu-search
h0=$(nlog "start menu: hidden"); wtype -k Escape; sleep 0.4; wtype -k Escape
if wait_more "start menu: hidden" "$h0" 30; then pass "Escape clears the search, then closes the menu"; else fail "Escape clears the search, then closes the menu"; fi
c0=$(nlog "hde-panel: command 1 ")
if grep -q 'key="Super_L" onRelease="yes"' "$RC"; then
    wtype -k Super_L; sleep 1.5
    if [ "$(nlog "hde-panel: command 1 ")" -gt "$c0" ] && wait_more "start menu: shown" "$m0" 10; then pass "the Super key (labwc key binding) opens the Start menu"
    else fail "the Super key (labwc key binding) opens the Start menu"; fi
    shot 04-start-menu-super
    c0=$(nlog "hde-panel: command 1 "); h0=$(nlog "start menu: hidden")
    wtype -k Super_L; sleep 1.2
    if wait_more "start menu: hidden" "$h0" 20; then pass "Super again closes it"; else fail "Super again closes it"; fi
fi
c0=$(nlog "hde-panel: command 1 ")
wtype -M ctrl -k Escape -m ctrl; sleep 1.5
if [ "$(nlog "hde-panel: command 1 ")" -gt "$c0" ]; then pass "Ctrl+Esc opens the Start menu too"; else fail "Ctrl+Esc opens the Start menu too"; fi
wtype -k Escape; sleep 0.5

# ---------- 3. the taskbar, Show Desktop ----------
if command -v zenity >/dev/null 2>&1; then
    t0=$(nlog "hde-panel: taskbar: + ")
    GDK_BACKEND=wayland zenity --info --title "Wayland test window" --text "HDE on Wayland" > /dev/null 2>&1 &
    ZEN=$!
    if wait_more "hde-panel: taskbar: + " "$t0" 80; then pass "a new window gets a taskbar button ($(grep 'taskbar: + ' "$LOG" | tail -n 1 | sed 's/.*taskbar: + //'))"
    else fail "a new window gets a taskbar button"; fi
    sleep 1; shot 05-window-taskbar
    "$B/hde-panel" --show-desktop
    if wait_log "taskbar: show desktop: minimized 1 window(s)" 30; then pass "Show Desktop minimizes the window"; else fail "Show Desktop minimizes the window"; fi
    sleep 0.6; shot 06-show-desktop
    "$B/hde-panel" --show-desktop
    if wait_log "taskbar: show desktop: restored 1 window(s)" 30; then pass "Show Desktop again brings it back"; else fail "Show Desktop again brings it back"; fi
    kill "$ZEN" 2>/dev/null
    if wait_log "hde-panel: taskbar: - " 50; then pass "a closed window leaves the taskbar"; else fail "a closed window leaves the taskbar"; fi
else
    info "no zenity: taskbar not tested with a window"
fi

# ---------- 4. keys through labwc: volume (OSD), PrtSc (grim) ----------
c0=$(nlog "hde-panel: command 5 ")
wtype -k XF86AudioRaiseVolume; sleep 1.5
if [ "$(nlog "hde-panel: command 5 ")" -gt "$c0" ]; then pass "the volume key runs hde-hotkeys --action volume-up, which shows the OSD (D-Bus)"
else fail "the volume key shows the OSD"; fi
shot 07-osd
mkdir -p "$HOME/Pictures/Screenshots"
n0=$(ls "$HOME/Pictures/Screenshots" 2>/dev/null | wc -l)
wtype -k Print; sleep 3
n1=$(ls "$HOME/Pictures/Screenshots" 2>/dev/null | wc -l)
f=$(ls -t "$HOME/Pictures/Screenshots"/*.png 2>/dev/null | head -n 1)
if [ "$n1" -gt "$n0" ] && [ -n "$f" ]; then pass "PrtSc saves a screenshot through grim ($(identify -format '%wx%h' "$f" 2>/dev/null))"
else fail "PrtSc saves a screenshot through grim ($n0 -> $n1; $(grep 'hde-screenshot\|hde-hotkeys' "$LOG" | tail -n 2 | tr '\n' ' '))"; fi

# ---------- 5. notifications, Settings and About on Wayland ----------
if command -v notify-send >/dev/null 2>&1; then
    notify-send -a "Wayland test" "Hello from HDE on Wayland" "Notifications are layer-shell surfaces"; sleep 1.2
    check "a notification arrives" grep -q "hde-notify: notification [0-9]* from Wayland test" "$LOG"
    shot 08-notification
fi
LABWC_PID=$(pgrep -x labwc | head -n 1) "$B/hde-settings" about > "$OUT/settings.log" 2>&1 &   # as if started from the menu
SET=$!
sleep 4
check "Settings runs on Wayland and knows it: Wayland (labwc)" grep -q "hde-settings: about: window manager: labwc (Wayland compositor); display server: Wayland (labwc" "$OUT/settings.log"
shot 09-settings-about
"$B/hde-settings" panel; sleep 2; shot 10-settings-panel
kill "$SET" 2>/dev/null
"$B/hde-settings" --about-window > "$OUT/about-window.log" 2>&1 &
AW=$!
sleep 2.5; shot 11-about-window; kill "$AW" 2>/dev/null

# ---------- 6. settings follow live: panel at the top, labwc reloads its configuration ----------
printf '[settings]\npanel_position=top\n' > "$INI"
if wait_log "hde-panel: settings changed: panel at the top, 34px high (Wayland layer shell" 50; then pass "panel_position=top moves the panel to the top"
else fail "panel_position=top moves the panel to the top"; fi
sleep 1; shot 12-panel-top
r0=$(nlog "reloads it")
printf '[settings]\npanel_position=top\nnatural_scroll=false\n' > "$INI"
if wait_more "reloads it" "$r0" 50 && grep -q '<naturalScroll>no</naturalScroll>' "$RC"; then
    pass "a changed setting (natural_scroll=false) rewrites rc.xml and labwc reloads it"
else fail "a changed setting rewrites rc.xml and labwc reloads it"; fi
printf '[settings]\n' > "$INI"; sleep 2

# ---------- 7. logout ----------
"$B/hde-session" logout >/dev/null 2>&1
# labwc is this shell's child ($START: hde-start -> exec hde-session -> exec labwc): reap it once it exited (a zombie
# still matches pgrep)
i=0
while [ $i -lt 75 ]; do
    st=$(awk '{print $3}' "/proc/$START/stat" 2>/dev/null)
    if [ -z "$st" ] || [ "$st" = Z ]; then break; fi
    sleep 0.2; i=$((i + 1))
done
st=$(awk '{print $3}' "/proc/$START/stat" 2>/dev/null)
if [ -z "$st" ] || [ "$st" = Z ]; then wait "$START" 2>/dev/null; fi
if ! running labwc && ! running hde-panel && ! running hde-desktop; then pass "logging out stops the session and labwc"
else
    fail "logging out stops the session and labwc (still running: $(pgrep -a 'labwc|hde-' 2>/dev/null | grep -v defunct | tr '\n' ';'))"
    tail -n 15 "$LOG" | sed 's/^/INFO:   /' >> "$OUT/results.txt"
fi
check "... as the log says" grep -q "hde-session: stopping the Wayland compositor" "$LOG"
cp "$LOG" "$OUT/session.log" 2>/dev/null
grep -E "(Gtk|GLib|Gdk)-(CRITICAL|WARNING)" "$LOG" | sort -u | head -n 8 | sed 's/^/INFO: warning: /' >> "$OUT/results.txt"
grep -E "^hde-(panel|session|desktop): .*(taskbar|layer|Wayland)" "$LOG" | head -n 8 | sed 's/^/INFO: /' >> "$OUT/results.txt"
[ "$FAILS" -gt 100 ] && FAILS=100
exit "$FAILS"
