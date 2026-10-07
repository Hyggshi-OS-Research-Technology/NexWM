#!/bin/sh
# tests/media-test.sh — Hyggshi Media (hde-media), the picture viewer (the first part of HDE's multimedia): a real X
# server (Xvfb) with Metacity, a folder of pictures of known colours and sizes, and the program driven by the keyboard
# the way a user drives it.
#
# What it checks, on top of tests/media-test.c (the plain-C checks of the list, the order, the zoom ladder and the
# slideshow clock): --version / --help / a bad option / an empty folder; that a picture opens with its folder as the
# list; the arrow keys and Home/End (wrapping included); that the picture really is on the screen (a pixel of it);
# the zoom keys and the mouse wheel; the rotation; the slideshow running by itself; full screen and Escape; the second
# hde-media handing its picture to the window that is already open (one process); -r, --sort and the Pictures folder
# of the user when no folder is given; and that closing the window ends the process with 0.
#
# Needs: Xvfb, xdotool, Metacity, ImageMagick (convert), python3 + libX11 (tests/xtool.py), dbus-run-session.
#   sudo apt install xvfb xdotool metacity imagemagick python3 dbus-x11
#   make && sh tests/media-test.sh
# Output: $HDE_TEST_OUT (default /tmp/hde-media): results.txt, media.log, shot-*.png
set -u
BUILD=${BUILD:-build}
B=$(cd "$BUILD" 2>/dev/null && pwd) || { echo "build dir '$BUILD' not found"; exit 2; }
[ -x "$B/hde-media" ] || { echo "$B/hde-media is not built (make, or make -C hde-media)"; exit 2; }
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${HDE_TEST_OUT:-/tmp/hde-media}
DISP=${HDE_MEDIA_TEST_DISPLAY:-:19}
XT="python3 $HERE/xtool.py"
TITLE="Hyggshi Media"
PIC="$OUT/home/Pictures"

if [ -z "${HDE_MEDIA_INNER:-}" ]; then
    rm -rf "$OUT"
    mkdir -p "$PIC/sub" "$OUT/home/.config/hde" "$OUT/run" "$OUT/empty"
    chmod 700 "$OUT/run"
    : > "$OUT/results.txt"
    for t in Xvfb xdotool convert metacity dbus-run-session python3; do
        command -v $t >/dev/null 2>&1 || { echo "FAIL: missing $t" | tee -a "$OUT/results.txt"; exit 2; }
    done
    # The pictures the viewer is pointed at: solid colours of known sizes, so a screenshot pixel can tell which one is
    # on the screen and how big it is drawn. notes.txt is not a picture, .hidden.png is hidden, sub/ is a sub-folder.
    convert -size 1200x800 xc:'#cc2222' "$PIC/a.png"          # 1200x800, red    (the biggest)
    convert -size 1600x400 xc:'#22aa44' "$PIC/b.png"          # 1600x400, green  (the widest)
    convert -size 300x200  xc:'#2244cc' "$PIC/c.png"          # 300x200,  blue   (the smallest: fit never enlarges)
    convert -size 400x400  xc:'#eecc22' "$PIC/sub/d.png"      # 400x400,  yellow (only with -r)
    printf 'not a picture\n' > "$PIC/notes.txt"
    printf 'not a picture\n' > "$PIC/sub/notes.txt"
    printf 'hidden\n' > "$PIC/.hidden.png"
    # dates of a.png > c.png > b.png, so --sort date must give b, c, a
    touch -t 202403010000 "$PIC/a.png"
    touch -t 202401010000 "$PIC/b.png"
    touch -t 202402010000 "$PIC/c.png"

    export HOME="$OUT/home" XDG_CONFIG_HOME="$OUT/home/.config" XDG_CACHE_HOME="$OUT/home/.cache"
    export XDG_DATA_HOME="$OUT/home/.local/share" XDG_RUNTIME_DIR="$OUT/run"
    export LANG=C.UTF-8 NO_AT_BRIDGE=1 GDK_BACKEND=x11
    unset WAYLAND_DISPLAY HDE_SESSION_PID HDE_WM DBUS_SESSION_BUS_ADDRESS XDG_CURRENT_DESKTOP DISPLAY

    Xvfb "$DISP" -screen 0 1280x800x24 -nolisten tcp > "$OUT/xvfb.log" 2>&1 &
    XVFB=$!
    export DISPLAY="$DISP"
    for i in $(seq 1 60); do xdotool getdisplaygeometry >/dev/null 2>&1 && break; sleep 0.2; done
    xdotool getdisplaygeometry >/dev/null 2>&1 || { echo "FAIL: Xvfb $DISP does not answer" | tee -a "$OUT/results.txt"; exit 2; }
    metacity --sm-disable --replace > "$OUT/metacity.log" 2>&1 &
    METACITY=$!
    sleep 1

    HDE_MEDIA_INNER=1 dbus-run-session -- sh "$0"
    rc=$?
    kill $METACITY $XVFB 2>/dev/null
    echo "== results ($OUT/results.txt)"
    cat "$OUT/results.txt"
    exit $rc
fi

# ================================ inside Xvfb + Metacity + a session bus ================================
FAILS=0
pass() { echo "PASS: $*" | tee -a "$OUT/results.txt"; }
fail() { echo "FAIL: $*" | tee -a "$OUT/results.txt"; FAILS=$((FAILS + 1)); }
skip() { echo "SKIP: $*" | tee -a "$OUT/results.txt"; }
check() { desc=$1; shift; if "$@" >/dev/null 2>&1; then pass "$desc"; else fail "$desc"; fi; }
shot() { command -v import >/dev/null 2>&1 && import -display "$DISPLAY" -window root "$OUT/shot-$1.png" 2>/dev/null; return 0; }

MEDIA_LOG="$OUT/media.log"
: > "$MEDIA_LOG"
MEDIA_PID=
MEDIA_RC=

# the lines the log got since MARK (a number from log_mark) — what the last key did
since() { sed -n "$(($1 + 1)),\$p" "$MEDIA_LOG"; }
log_mark() { wc -l < "$MEDIA_LOG" | tr -d ' '; }
# check_new MARK PATTERN WHAT: the lines added since MARK contain PATTERN
check_new() {
    if since "$1" | grep -q -e "$2"; then pass "$3"
    else fail "$3 (nothing matching '$2' in: $(since "$1" | tr '\n' '|' | cut -c1-220))"; fi
}
# check_log PATTERN WHAT: the log contains PATTERN (a whole run, not one key)
check_log() {
    if grep -q -e "$1" "$MEDIA_LOG"; then pass "$2"; else fail "$2 (no '$1' in $MEDIA_LOG)"; fi
}
# media_start ARGS...: run the viewer, log appended, window waited for (MEDIA_WINDOW set)
media_start() {
    # the window of a run that was just stopped is still on its way out: wait for it, or the pixel checks would read it
    i=0
    while [ -n "$(xdotool search --onlyvisible --name "$TITLE" 2>/dev/null)" ] && [ "$i" -lt 24 ]; do
        sleep 0.25; i=$((i + 1))
    done
    "$B/hde-media" "$@" >> "$MEDIA_LOG" 2>&1 &
    MEDIA_PID=$!
    MEDIA_WINDOW=
    for i in $(seq 1 60); do
        MEDIA_WINDOW=$(xdotool search --onlyvisible --name "$TITLE" 2>/dev/null | head -n 1)
        [ -n "$MEDIA_WINDOW" ] && break
        kill -0 "$MEDIA_PID" 2>/dev/null || break
        sleep 0.25
    done
    [ -n "$MEDIA_WINDOW" ] && sleep 0.7          # let it paint before a pixel is read
    [ -n "$MEDIA_WINDOW" ]
}
# media_wait PATTERN SECONDS: wait for a line of the log
media_wait() {
    i=0
    while [ "$i" -lt $(($2 * 4)) ]; do
        grep -q -e "$1" "$MEDIA_LOG" && return 0
        sleep 0.25; i=$((i + 1))
    done
    return 1
}
# media_key KEY: the key, in the window of the viewer
media_key() {
    [ -n "${MEDIA_WINDOW:-}" ] && xdotool windowactivate --sync "$MEDIA_WINDOW" >/dev/null 2>&1
    xdotool key --clearmodifiers --delay 60 "$1" >/dev/null 2>&1
    sleep 0.4
}
media_stop() {
    [ -n "${MEDIA_PID:-}" ] || return 0
    kill "$MEDIA_PID" 2>/dev/null
    wait "$MEDIA_PID" 2>/dev/null
    MEDIA_PID=
    sleep 0.3
}
# the window as the X server has it: sets WX WY WW WH
media_geometry() {
    eval "$(xdotool getwindowgeometry --shell "$MEDIA_WINDOW" 2>/dev/null)"
    WX=${X:-0}; WY=${Y:-0}; WW=${WIDTH:-0}; WH=${HEIGHT:-0}
}
# the colour of the middle of the window (where the picture is, when it fills it)
media_pixel() {
    media_geometry
    set -- $($XT pixel $((WX + WW / 2)) $((WY + WH / 2)) 2>/dev/null)
    PX=${1:-0}; PY=${2:-0}; PZ=${3:-0}
}

# ---------------------------------------------------------------- the command line
v=$("$B/hde-media" --version 2>&1)
case "$v" in
"hde-media (Hyggshi Media) "*) pass "--version says what this is: $v" ;;
*) fail "--version (got '${v:-nothing}')" ;;
esac
h=$("$B/hde-media" --help 2>&1)
case "$h" in
*"--slideshow"*"Keys:"*) pass "--help explains the options and the keys" ;;
*) fail "--help (got '$(echo "$h" | head -n 1)')" ;;
esac
"$B/hde-media" --not-an-option > "$OUT/bad-option.log" 2>&1
rc=$?
if [ "$rc" = 1 ] && grep -q "unknown option" "$OUT/bad-option.log"; then pass "an option that is not understood: exit 1, and it says why"
else fail "an option that is not understood (exit $rc, log '$(head -n 1 "$OUT/bad-option.log")')"; fi
"$B/hde-media" "$OUT/empty" > "$OUT/empty.log" 2>&1
rc=$?
if [ "$rc" = 2 ] && grep -q "nothing to show" "$OUT/empty.log"; then pass "an empty folder: exit 2, and it says there is nothing to show"
else fail "an empty folder (exit $rc, log '$(head -n 1 "$OUT/empty.log")')"; fi
grep -q 'image/png' "$HERE/../hde-media/hde-media.desktop" && grep -q 'MimeType=image/' "$HERE/../hde-media/hde-media.desktop" \
    && pass "the menu entry says which pictures open here (MimeType=image/…)" \
    || fail "the menu entry says which pictures open here"

# ---------------------------------------------------------------- opening a picture
media_start "$PIC/a.png" || fail "the viewer starts (no window)"
MEDIA_WINDOW=$(xdotool search --onlyvisible --name "$TITLE" 2>/dev/null | head -n 1)
[ -n "$MEDIA_WINDOW" ] && pass "a window opens for a picture (and only one: $(xdotool search --onlyvisible --name "$TITLE" | wc -l | tr -d ' '))" \
    || fail "a window opens for a picture"
media_wait "showing 1/3: a.png (1200x800, fit " 5 && pass "the picture is shown, and its folder is the list (1/3)" \
    || fail "the picture is shown ($(tail -n 3 "$MEDIA_LOG" | tr '\n' '|'))"
check "the window title says the picture and where it is in the list" \
    sh -c "xdotool getwindowname '$MEDIA_WINDOW' | grep -q 'a.png (1/3)'"
shot 1-open

# the pixels: the middle of the window is the red of a.png
ok=0
for i in 1 2 3 4 5 6; do
    media_pixel
    if [ "$PX" -ge 150 ] && [ "$PY" -le 90 ] && [ "$PZ" -le 90 ]; then ok=1; break; fi
    sleep 0.4
done
[ "$ok" = 1 ] && pass "the picture is really on the screen (middle of the window: $PX $PY $PZ, the red of a.png)" \
    || fail "the picture is on the screen (middle of the window: $PX $PY $PZ, expected red)"

# ---------------------------------------------------------------- stepping through the pictures
m=$(log_mark); media_key Right
check_new "$m" "showing 2/3: b.png (1600x400, fit " "Right: the next picture (b.png)"
m=$(log_mark); media_key Right
check_new "$m" "showing 3/3: c.png (300x200, fit 100 %)" "Right again: the last picture, drawn as it is (a small picture is not blown up)"
m=$(log_mark); media_key Right
check_new "$m" "showing 1/3: a.png" "Right once more: back to the first picture (the list wraps around)"
m=$(log_mark); media_key End
check_new "$m" "showing 3/3: c.png" "End: the last picture"
m=$(log_mark); media_key Home
check_new "$m" "showing 1/3: a.png" "Home: the first picture"
m=$(log_mark); media_key Left
check_new "$m" "showing 3/3: c.png" "Left from the first picture: the last one (wraps around the other way)"
m=$(log_mark); media_key Home
check_new "$m" "showing 1/3: a.png" "Home again (the checks below start from the first picture)"

# ---------------------------------------------------------------- the zoom
m=$(log_mark); media_key 1
check_new "$m" "^hde-media: zoom 100 %" "1: the picture as it is on disk (100 %)"
m=$(log_mark); media_key plus
check_new "$m" "^hde-media: zoom 125 %" "+: one step further in (125 %)"
m=$(log_mark); media_key minus
check_new "$m" "^hde-media: zoom 100 %" "-: one step back (100 %)"
m=$(log_mark); media_key minus; media_key minus
check_new "$m" "^hde-media: zoom 75 %" "- twice more: 75 %"
m=$(log_mark); media_key 0
check_new "$m" "^hde-media: zoom fit" "0: back to fitting the window"
m=$(log_mark)
media_pixel                                     # make sure we are over the picture
xdotool mousemove "$((WX + WW / 2))" "$((WY + WH / 2))" click 4 >/dev/null 2>&1
sleep 0.5
check_new "$m" "^hde-media: zoom " "the mouse wheel zooms"
m=$(log_mark); media_key 0; media_key 0
check_new "$m" "^hde-media: zoom fit" "0 again: back to fit"

# ---------------------------------------------------------------- the rotation
m=$(log_mark); media_key r
check_new "$m" "^hde-media: rotated right (now 90°)" "r: a quarter turn to the right"
check_new "$m" "showing 1/3: a.png (800x1200, fit .*, rotated right 90°)" "the window shows the turned picture (the size is swapped)"
m=$(log_mark); media_key R
check_new "$m" "^hde-media: rotated left (now 0°)" "Shift+R: back the other way (0°)"
m=$(log_mark); media_key r; media_key r
check_new "$m" "rotated right (now 180°)" "twice more to the right: 180°"
m=$(log_mark); media_key r; media_key r
check_new "$m" "rotated left (now 0°)" "and twice more: back to where the file is"

# ---------------------------------------------------------------- the slideshow
m=$(log_mark); media_key s
check_new "$m" "^hde-media: slideshow on (5 s per picture)" "s: the slideshow starts (5 s, the default)"
m=$(log_mark); media_key s
check_new "$m" "^hde-media: slideshow off" "s again: the slideshow stops"
media_stop
m=$(log_mark)
media_start -s -i 1 "$PIC" && pass "the viewer starts with the slideshow already running (-s -i 1)" || fail "the viewer starts with -s -i 1"
check_log "^hde-media: slideshow on (1 s per picture)" "the log says the slideshow is on, one second per picture"
media_wait "^hde-media: slideshow: next picture" 6 && pass "the slideshow walks on by itself" \
    || fail "the slideshow walks on by itself"
sleep 0.5
check_log "showing 2/3: b.png" "the picture the slideshow moved on to is on the screen"
media_stop

# ---------------------------------------------------------------- full screen
media_start "$PIC/a.png" || fail "the viewer starts again"
media_wait "showing 1/3: a.png" 5
m=$(log_mark); media_key f
check_new "$m" "^hde-media: full screen on" "f: full screen"
media_geometry
if [ "$WW" = 1280 ] && [ "$WH" = 800 ]; then pass "full screen takes the whole screen (${WW}x${WH}, the toolbar hidden)"
else fail "full screen takes the whole screen (window ${WW}x${WH}, screen 1280x800)"; fi
case "$($XT wm-state "$MEDIA_WINDOW" 2>/dev/null)" in
*_NET_WM_STATE_FULLSCREEN*) pass "the window manager knows it is full screen (_NET_WM_STATE_FULLSCREEN)" ;;
*) fail "the window manager knows it is full screen (state: $($XT wm-state "$MEDIA_WINDOW" 2>/dev/null))" ;;
esac
shot 2-fullscreen
m=$(log_mark); media_key Escape
check_new "$m" "^hde-media: full screen off" "Escape: out of full screen"
media_geometry
if [ "$WW" = 1100 ] && [ "$WH" = 720 ]; then pass "the window is back to its normal size (${WW}x${WH}) and the toolbar is back"
else fail "the window is back to its normal size (got ${WW}x${WH}, expected 1100x720)"; fi

# ---------------------------------------------------------------- the second hde-media: one window, one process
m=$(log_mark)
"$B/hde-media" "$PIC/c.png" >> "$MEDIA_LOG" 2>&1 &
SECOND=$!
wait $SECOND 2>/dev/null
sleep 1
check_new "$m" "showing 3/3: c.png" "a second hde-media hands its picture to the window that is open"
n=$(pgrep -x hde-media 2>/dev/null | wc -l | tr -d ' ')
[ "$n" = 1 ] && pass "one hde-media process is enough ($n)" || fail "one hde-media process (found $n)"
[ "$(xdotool search --onlyvisible --name "$TITLE" 2>/dev/null | wc -l | tr -d ' ')" = 1 ] \
    && pass "one window, not two" || fail "one window (found $(xdotool search --onlyvisible --name "$TITLE" 2>/dev/null | wc -l | tr -d ' '))"

# ---------------------------------------------------------------- -r, --sort and the Pictures folder
m=$(log_mark); media_key i
sleep 0.3
[ -n "$(xdotool search --onlyvisible --name "$TITLE" 2>/dev/null)" ] && pass "i: the long status line does not close anything" \
    || fail "i: the long status line"
media_stop

media_start -r "$PIC" || fail "the viewer starts with -r"
media_wait "showing 1/4: a.png" 5 && pass "-r: the pictures of the sub-folders are in the list too (1/4)" \
    || fail "-r: the sub-folders are in the list ($(tail -n 2 "$MEDIA_LOG" | tr '\n' '|'))"
if grep -q "notes.txt" "$MEDIA_LOG"; then fail "the files that are not pictures are never in the list (notes.txt appeared)"; else pass "the files that are not pictures are left out, and so are the hidden ones (.hidden.png never appeared)"; fi
media_stop

media_start --sort date "$PIC" || fail "the viewer starts with --sort date"
media_wait "showing 1/3: b.png" 5 && pass "--sort date: the oldest picture first (b.png, 2024-01-01)" \
    || fail "--sort date: the oldest first ($(tail -n 2 "$MEDIA_LOG" | tr '\n' '|'))"
media_stop

# no folder at all: the Pictures folder of the user
m=$(log_mark)
media_start || fail "the viewer starts with no argument"
media_wait "showing 1/3: a.png" 5 && pass "with no argument: the Pictures folder of the user" \
    || fail "with no argument: the Pictures folder ($(tail -n 3 "$MEDIA_LOG" | tr '\n' '|'))"

# ---------------------------------------------------------------- closing
m=$(log_mark); media_key q
i=0
while [ "$i" -lt 20 ]; do
    [ -z "$(xdotool search --onlyvisible --name "$TITLE" 2>/dev/null)" ] && break
    sleep 0.25; i=$((i + 1))
done
if [ -z "$(xdotool search --onlyvisible --name "$TITLE" 2>/dev/null)" ]; then
    pass "q: the window closes"
else
    fail "q: the window closes (still there)"
    kill "$MEDIA_PID" 2>/dev/null            # never leave the test waiting for a window that will not close
fi
wait "$MEDIA_PID" 2>/dev/null
MEDIA_RC=$?
MEDIA_PID=
[ "$MEDIA_RC" = 0 ] && pass "the process ends with 0 when the window is closed" \
    || fail "the process ends with 0 (got ${MEDIA_RC:-?})"

if [ "$FAILS" = 0 ]; then
    echo "== media-test: all checks passed" | tee -a "$OUT/results.txt"
else
    echo "== media-test: $FAILS FAILED" | tee -a "$OUT/results.txt"
fi
[ "$FAILS" = 0 ]
