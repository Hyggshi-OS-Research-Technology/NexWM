#!/bin/sh
# tests/player-window-test.sh — Hyggshi Media (hde-media), the player window (the second part of HDE's multimedia): a
# real X server (Xvfb) with Metacity, a folder of (very small) songs, a stand-in for mpv, and the program driven by the
# keyboard the way a user drives it.
#
# What it checks, on top of tests/player-test.c (the plain-C checks of the list, the tags, the state of the playback and
# the command line of each engine): that a song opens the player and starts playing; that the *command line* the engine
# is given is the one engine.c built (the file, --no-video, --input-ipc-server); that the window really talks to mpv
# over that socket (pause, seek, the volume, the position) — the seek judged against where mpv said the track was, and
# kept inside the end of a track; the transport keys (next/previous with the wrap-around,
# stop, play); the volume, mute, shuffle and repeat keys; that a second hde-media hands its song to the window that is
# already open (one process, one window); that a track that ends plays the next one, that the end of the list stops, and
# that repeat all goes back to the first; that with no engine at all the window says so instead of doing nothing; and
# that closing the window ends the process with 0 (and takes the sound down with it); and, for a video, the subtitles
# next to it: that they are found and named in the log, that "v" hides and shows them again over mpv's socket, and that
# a video with nothing next to it says so.
#
# Needs: Xvfb, xdotool, Metacity, python3 (tests/fake-mpv.py), dbus-run-session, playerctl.
#   sudo apt install xvfb xdotool metacity python3 dbus-x11 playerctl
#   make && sh tests/player-window-test.sh
# Output: $HDE_TEST_OUT (default /tmp/hde-player): results.txt, player.log, engine.log, shot-*.png
set -u
BUILD=${BUILD:-build}
B=$(cd "$BUILD" 2>/dev/null && pwd) || { echo "build dir '$BUILD' not found"; exit 2; }
[ -x "$B/hde-media" ] || { echo "$B/hde-media is not built (make, or make -C hde-media)"; exit 2; }
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${HDE_TEST_OUT:-/tmp/hde-player}
DISP=${HDE_PLAYER_TEST_DISPLAY:-:20}
TITLE="Hyggshi Media"
HOME_DIR="$OUT/home"
MUSIC="$HOME_DIR/Music"
MUSIC2="$HOME_DIR/More"
FILMS="$HOME_DIR/Films"
FAKEBIN="$OUT/fakebin"
PLAYER_LOG="$OUT/player.log"
ENGINE_LOG="$OUT/engine.log"
MPV_DURATION=600      # the length the stand-in says a track has: long, so a seek of five seconds has room (section 9
                      # makes it short on purpose: there, a seek has to stop at the end of the track)

if [ -z "${HDE_PLAYER_INNER:-}" ]; then
    rm -rf "$OUT"
    mkdir -p "$MUSIC/sub" "$MUSIC2" "$FILMS" "$HOME_DIR/.config/hde" "$OUT/run" "$OUT/empty" "$FAKEBIN"
    chmod 700 "$OUT/run"
    : > "$OUT/results.txt"
    for t in Xvfb xdotool metacity dbus-run-session python3 playerctl; do
        command -v $t >/dev/null 2>&1 || { echo "FAIL: missing $t" | tee -a "$OUT/results.txt"; exit 2; }
    done
    # The songs: the stand-in for mpv plays nothing, so the bytes do not matter — the names do, and the extension,
    # which is what makes hde-media give a file to the player instead of the picture viewer. sub/song4.mp3 is what the
    # second invocation hands over, and More/ is the list the end of a track is tried on.
    printf 'not really an mp3\n' > "$MUSIC/song1.mp3"
    printf 'nor this one\n' > "$MUSIC/song2.mp3"
    printf 'nor that one\n' > "$MUSIC/song3.mp3"
    printf 'nothing here\n' > "$MUSIC/notes.txt"
    printf 'and this one\n' > "$MUSIC/sub/song4.mp3"
    printf 'first of two\n' > "$MUSIC2/one.mp3"
    printf 'second of two\n' > "$MUSIC2/two.mp3"
    printf 'not really a video\n' > "$FILMS/clip.mp4"
    printf '1\n00:00:01,000 --> 00:00:02,000\nHello\n' > "$FILMS/clip.srt"   # the subtitles "next to" the clip
    printf 'not really a video either\n' > "$FILMS/zebra.mp4"                 # a video with nothing next to it
    # the engine the window finds in $PATH: an empty executable file is not enough (hde_media_find_program only looks at
    # $PATH, but the window *runs* it), so it is the python stand-in, under the name mpv
    ln -sf "$HERE/fake-mpv.py" "$FAKEBIN/mpv"
    chmod +x "$HERE/fake-mpv.py"

    export HOME="$HOME_DIR" XDG_CONFIG_HOME="$HOME_DIR/.config" XDG_CACHE_HOME="$HOME_DIR/.cache"
    export XDG_DATA_HOME="$HOME_DIR/.local/share" XDG_RUNTIME_DIR="$OUT/run"
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

    HDE_PLAYER_INNER=1 dbus-run-session -- sh "$0"
    rc=$?
    kill $METACITY $XVFB 2>/dev/null
    echo "== results ($OUT/results.txt)"
    cat "$OUT/results.txt"
    exit $rc
fi

# ============================ inside Xvfb + Metacity + a session bus ============================
FAILS=0
pass() { echo "PASS: $*" | tee -a "$OUT/results.txt"; }
fail() { echo "FAIL: $*" | tee -a "$OUT/results.txt"; FAILS=$((FAILS + 1)); }
skip() { echo "SKIP: $*" | tee -a "$OUT/results.txt"; }
check() { desc=$1; shift; if "$@" >/dev/null 2>&1; then pass "$desc"; else fail "$desc"; fi; }
shot() { command -v import >/dev/null 2>&1 && import -display "$DISPLAY" -window root "$OUT/shot-$1.png" 2>/dev/null; return 0; }

: > "$PLAYER_LOG"
: > "$ENGINE_LOG"
PLAYER_PID=
PLAYER_WINDOW=
HOTKEYS_PID=
stop_hotkeys() {
    if [ -n "$HOTKEYS_PID" ]; then
        kill -TERM "$HOTKEYS_PID" 2>/dev/null || :
        wait "$HOTKEYS_PID" 2>/dev/null || :
        HOTKEYS_PID=
    fi
}
trap stop_hotkeys 0
trap 'exit 143' 1 2 15

since() { sed -n "$(($1 + 1)),\$p" "$PLAYER_LOG"; }
mark() { wc -l < "$PLAYER_LOG" | tr -d ' '; }
# wait_new MARK PATTERN SECONDS: the lines added since MARK come to contain PATTERN
wait_new() {
    i=0
    while [ "$i" -lt $(($3 * 4)) ]; do
        since "$1" | grep -q -e "$2" && return 0
        if [ -n "${PLAYER_PID:-}" ]; then kill -0 "$PLAYER_PID" 2>/dev/null || return 1; fi
        sleep 0.25; i=$((i + 1))
    done
    return 1
}
# check_log PATTERN WHAT: the whole run of the program, not one key
check_log() {
    if grep -q -e "$1" "$PLAYER_LOG"; then pass "$2"
    else fail "$2 (no '$1' in $PLAYER_LOG)"; fi
}
# player_start HOLDS ARGS...: run the player with the stand-in mpv in front of $PATH; HOLDS is how long the stand-in
# "plays" before exiting (its exit IS the end of a track). MPV_DURATION is the length the file is said to have.
player_start() {
    hold=$1; shift
    PLAYER_WINDOW=
    HDE_FAKE_MPV_SECONDS="$hold" HDE_FAKE_MPV_DURATION="${MPV_DURATION:-600}" HDE_FAKE_MPV_LOG="$ENGINE_LOG" \
        PATH="$FAKEBIN:$PATH" "$B/hde-media" "$@" >> "$PLAYER_LOG" 2>&1 &
    PLAYER_PID=$!
    for i in $(seq 1 80); do
        PLAYER_WINDOW=$(xdotool search --onlyvisible --name "$TITLE" 2>/dev/null | head -n 1)
        [ -n "$PLAYER_WINDOW" ] && break
        kill -0 "$PLAYER_PID" 2>/dev/null || break
        sleep 0.25
    done
    [ -n "$PLAYER_WINDOW" ] && sleep 0.8          # let it paint, and let the socket come up
    [ -n "$PLAYER_WINDOW" ]
}
# player_key KEY: the key, in the window of the player
player_key() {
    [ -n "${PLAYER_WINDOW:-}" ] && xdotool windowactivate --sync "$PLAYER_WINDOW" >/dev/null 2>&1
    xdotool key --clearmodifiers --delay 60 "$1" >/dev/null 2>&1
    sleep 0.4
}
# player_quit: q, then the process has to be gone (and with it whatever it was running)
player_quit() {
    player_key q
    i=0
    while [ "$i" -lt 24 ]; do
        [ -n "${PLAYER_PID:-}" ] || break
        kill -0 "$PLAYER_PID" 2>/dev/null || break
        sleep 0.25; i=$((i + 1))
    done
    if [ -n "${PLAYER_PID:-}" ] && kill -0 "$PLAYER_PID" 2>/dev/null; then
        kill -TERM "$PLAYER_PID" 2>/dev/null
        return 1
    fi
    wait "$PLAYER_PID" 2>/dev/null
    return 0
}

# ---------- 0. what the program says about itself ----------
if "$B/hde-media" --help 2>&1 | grep -q -- "--play"; then pass "--help knows the player (--play)"
else fail "--help mentions the player"; fi
out=$(PATH="$FAKEBIN:$PATH" "$B/hde-media" --play "$OUT/empty" 2>&1)
rc=$?
case "$out" in
*"nothing to play"*) [ "$rc" = 2 ] && pass "an empty folder: 'nothing to play' and status 2" \
                                       || fail "an empty folder says nothing to play, with status 2 (got $rc)" ;;
*) fail "an empty folder says nothing to play (said: $(printf '%s' "$out" | head -n 1))" ;;
esac

# ---------- 1. a song opens the player and plays ----------
m0=$(mark)
if player_start 60 --play "$MUSIC"; then pass "the player window opens for a folder of songs"
else fail "the player window opens for a folder of songs"; fi
if wait_new "$m0" "playing 1/3: song1.mp3" 10; then pass "it starts playing the first song"
else fail "it starts playing the first song ($(since "$m0" | tr '\n' '|' | cut -c1-220))"; fi
shot 1-first-song
name=$(xdotool getwindowname "$PLAYER_WINDOW" 2>/dev/null)
case "$name" in
*"song1.mp3"*"$TITLE"*) pass "the window title says what plays ($name)" ;;
*) fail "the window title says what plays (got '$name')" ;;
esac
check_log "engine: mpv" "the engine it found is named in the log"
if grep -q -- "--no-video" "$ENGINE_LOG"; then pass "the song is handed to mpv with --no-video (no black window)"
else fail "mpv gets --no-video (engine.log: $(head -n 1 "$ENGINE_LOG" | cut -c1-200))"; fi
if grep -q -e "--input-ipc-server=$OUT/run" "$ENGINE_LOG"; then pass "mpv is given a socket in XDG_RUNTIME_DIR to listen on"
else fail "mpv is given --input-ipc-server (engine.log: $(head -n 1 "$ENGINE_LOG" | cut -c1-200))"; fi
if grep -e "ARGS" "$ENGINE_LOG" | tail -n 1 | grep -q -e "song1.mp3$"; then pass "the file is the last word of the command line"
else fail "the file is the last argument of the command line"; fi
if grep -q -e "--wid=" "$ENGINE_LOG"; then fail "a sound gets no --wid (there is no video to draw)"
else pass "a sound gets no --wid (there is no video to draw)"; fi
if wait_new "$m0" "mpv is listening" 5; then pass "the window connects to mpv's socket"
else fail "the window connects to mpv's socket"; fi
if grep -q '"get_property","duration"' "$ENGINE_LOG"; then pass "it asks mpv how long the track is (the tags have nothing here)"
else fail "it asks mpv for the duration"; fi

# ---------- MPRIS: playerctl and desktop media controls ----------
if playerctl --list-all 2>/dev/null | grep -Fxq "hde-media"; then
    pass "the player is listed on MPRIS as hde-media"
else fail "the player is listed on MPRIS as hde-media"; fi
status=$(playerctl --player=hde-media status 2>/dev/null)
[ "$status" = "Playing" ] && pass "MPRIS reports the active track as Playing" \
    || fail "MPRIS reports Playing (got '${status:-nothing}')"
title=$(playerctl --player=hde-media metadata xesam:title 2>/dev/null)
case "$title" in *song1.mp3*) pass "MPRIS publishes the current track title ($title)" ;;
*) fail "MPRIS publishes song1.mp3 as the title (got '${title:-nothing}')" ;; esac
m=$(mark)
if playerctl --player=hde-media pause >/dev/null 2>&1 && wait_new "$m" "paused" 5 && grep -q '"pause",true' "$ENGINE_LOG"; then
    pass "playerctl pause reaches mpv over MPRIS"
else fail "playerctl pause reaches mpv over MPRIS"; fi
status=$(playerctl --player=hde-media status 2>/dev/null)
[ "$status" = "Paused" ] && pass "MPRIS reports the paused state" || fail "MPRIS reports Paused (got '${status:-nothing}')"
m=$(mark)
if playerctl --player=hde-media play >/dev/null 2>&1 && wait_new "$m" "playing on" 5; then
    pass "playerctl play resumes the track over MPRIS"
else fail "playerctl play resumes the track over MPRIS"; fi
m=$(mark)
if playerctl --player=hde-media next >/dev/null 2>&1 && wait_new "$m" "playing 2/3: song2.mp3" 5; then
    pass "playerctl next changes the current track"
else fail "playerctl next changes the current track"; fi
m=$(mark)
if playerctl --player=hde-media previous >/dev/null 2>&1 && wait_new "$m" "playing 1/3: song1.mp3" 5; then
    pass "playerctl previous changes back to the first track"
else fail "playerctl previous changes back to the first track"; fi

# F4 is HDE's unmodified play/pause key; it uses the same MPRIS command as the hardware media key.
if [ -x "$B/hde-hotkeys" ]; then
    HDE_DEBUG=1 "$B/hde-hotkeys" > "$OUT/hotkeys.log" 2>&1 &
    HOTKEYS_PID=$!
    for i in $(seq 1 30); do
        grep -q 'fkeys_sound=1' "$OUT/hotkeys.log" && break
        kill -0 "$HOTKEYS_PID" 2>/dev/null || break
        sleep 0.1
    done
    m=$(mark); player_key F4
    if wait_new "$m" "paused" 5 && [ "$(playerctl --player=hde-media status 2>/dev/null)" = Paused ]; then
        pass "F4 pauses the player through playerctl"
    else fail "F4 pauses the player through playerctl"; fi
    m=$(mark); player_key F4
    if wait_new "$m" "playing on" 5 && [ "$(playerctl --player=hde-media status 2>/dev/null)" = Playing ]; then
        pass "F4 again resumes the player through playerctl"
    else fail "F4 again resumes the player through playerctl"; fi

    echo 'fkeys_sound=false' >> "$XDG_CONFIG_HOME/hde/settings.ini"
    kill -HUP "$HOTKEYS_PID" 2>/dev/null || :
    for i in $(seq 1 30); do
        grep -q 'fkeys_sound=0' "$OUT/hotkeys.log" && break
        kill -0 "$HOTKEYS_PID" 2>/dev/null || break
        sleep 0.1
    done
    m=$(mark); player_key F4
    if grep -q 'fkeys_sound=0' "$OUT/hotkeys.log" && ! since "$m" | grep -q 'paused' &&
       [ "$(playerctl --player=hde-media status 2>/dev/null)" = Playing ]; then
        pass "fkeys_sound=false releases F4 without pausing the player"
    else fail "fkeys_sound=false releases F4 without pausing the player"; fi
    sed -i '/^fkeys_sound=/d' "$XDG_CONFIG_HOME/hde/settings.ini"
    stop_hotkeys

    # The labwc F4 binding runs this one-shot action; MPRIS needs no Wayland connection or X display.
    m=$(mark); WAYLAND_DISPLAY=media-action-test "$B/hde-hotkeys" --action play
    if wait_new "$m" "paused" 5; then pass "the Wayland F4 action pauses through playerctl"
    else fail "the Wayland F4 action pauses through playerctl"; fi
    m=$(mark); WAYLAND_DISPLAY=media-action-test "$B/hde-hotkeys" --action play
    if wait_new "$m" "playing on" 5; then pass "the Wayland F4 action resumes through playerctl"
    else fail "the Wayland F4 action resumes through playerctl"; fi
else
    skip "hde-hotkeys is not built: F4 media binding checks"
fi

# ---------- 2. the transport ----------
m1=$(mark); player_key Right
if wait_new "$m1" "playing 2/3: song2.mp3" 5; then pass "Right plays the next song"
else fail "Right plays the next song"; fi
if grep -e "ARGS" "$ENGINE_LOG" | tail -n 1 | grep -q "song2.mp3"; then pass "the next song is what mpv is now given"
else fail "mpv is given the next song"; fi
m2=$(mark); player_key Left
if wait_new "$m2" "playing 1/3: song1.mp3" 5; then pass "Left plays the previous song"
else fail "Left plays the previous song"; fi
m3=$(mark); player_key Left
if wait_new "$m3" "playing 3/3: song3.mp3" 5; then pass "Left on the first song wraps around to the last"
else fail "Left wraps around to the last song"; fi

# ---------- 3. pause, seek, the volume: mpv's socket ----------
m4=$(mark); player_key space
if wait_new "$m4" "paused" 5 && grep -q '"pause",true' "$ENGINE_LOG"; then pass "Space pauses (mpv is told over the socket)"
else fail "Space pauses mpv over the socket"; fi
m5=$(mark); player_key space
if wait_new "$m5" "playing on" 5 && grep -q '"pause",false' "$ENGINE_LOG"; then pass "Space again plays on"
else fail "Space again plays on"; fi
m6=$(mark); player_key ctrl+Right
if wait_new "$m6" "seek " 5; then
    # The seek is relative to where the track is, not the absolute 5 s, so the test needs to know where the track was:
    # the command mpv was given, and the position it reported just before that (the window asks for it twice a second,
    # and the stand-in writes every position down as "TIME<TAB>3.250").
    seek_line=$(grep -n '"seek",[0-9.]*' "$ENGINE_LOG" | tail -n 1 | cut -d: -f1)
    [ -n "$seek_line" ] || seek_line=1     # nothing there: the check below fails, it does not blow up
    seek_to=$(sed -n "${seek_line}p" "$ENGINE_LOG" | grep -o '"seek",[0-9.]*' | cut -d, -f2)
    was_at=$(awk -v n="$seek_line" 'NR < n && $1 == "TIME" { t = $2 } END { if (t != "") print t }' "$ENGINE_LOG")
    if [ -n "$seek_to" ] && [ -n "$was_at" ] && awk "BEGIN { d = $seek_to - $was_at; exit !(d > 4 && d < 6) }"; then
        pass "Ctrl+Right asks mpv to move five seconds further (${was_at}s in, ${seek_to}s out)"
    else fail "Ctrl+Right seeks five seconds further (mpv last said ${was_at:-?}s, the window asked for ${seek_to:-?}s)"; fi
else fail "Ctrl+Right does not reach mpv (nothing was seeked in $ENGINE_LOG)"; fi
m7=$(mark); player_key Up
if wait_new "$m7" "volume 85 %" 5 && grep -q '"set_property","volume"' "$ENGINE_LOG"; then pass "Up is +5 % of the volume, and mpv is told"
else fail "Up raises the volume by 5 % and tells mpv"; fi
m8=$(mark); player_key Down
if wait_new "$m8" "volume 80 %" 5; then pass "Down is -5 %"
else fail "Down lowers the volume by 5 %"; fi
m9=$(mark); player_key m
if wait_new "$m9" "muted" 5 && grep -q '"set_property","mute",true' "$ENGINE_LOG"; then pass "m mutes"
else fail "m mutes"; fi
m10=$(mark); player_key m
if wait_new "$m10" "sound on" 5; then pass "m again unmutes"
else fail "m again unmutes"; fi

# ---------- 4. shuffle and repeat ----------
m11=$(mark); player_key z
if wait_new "$m11" "shuffle on" 5; then pass "z turns shuffle on"
else fail "z turns shuffle on"; fi
m12=$(mark); player_key z
if wait_new "$m12" "shuffle off" 5; then pass "z again turns it off"
else fail "z again turns shuffle off"; fi
m13=$(mark); player_key r
if wait_new "$m13" "repeat all" 5; then pass "r: repeat all"
else fail "r: repeat all"; fi
m14=$(mark); player_key r
if wait_new "$m14" "repeat one" 5; then pass "r: repeat one"
else fail "r: repeat one"; fi
m15=$(mark); player_key r
if wait_new "$m15" "repeat off" 5; then pass "r: repeat off again"
else fail "r: repeat off again"; fi

# ---------- 5. a second hde-media hands its song to the same window ----------
m16=$(mark)
PATH="$FAKEBIN:$PATH" "$B/hde-media" "$MUSIC/sub/song4.mp3" >> "$PLAYER_LOG" 2>&1
if wait_new "$m16" "added to the list" 5 && wait_new "$m16" "playing 4/4: song4.mp3" 5; then
    pass "a second hde-media hands its song to the window that is already open (4 in the list)"
else fail "a second hde-media hands its song to the open window"; fi
n=$(xdotool search --onlyvisible --name "$TITLE" 2>/dev/null | wc -l | tr -d ' ')
[ "$n" = 1 ] && pass "one window, not two" || fail "one window, not two (found $n)"

# ---------- 6. stop, play again ----------
m17=$(mark); player_key s
if wait_new "$m17" "stopped" 5; then pass "s stops"
else fail "s stops"; fi
sleep 1
if pgrep -f "$FAKEBIN/mpv" >/dev/null 2>&1; then fail "stopping takes the engine (mpv) down with it"
else pass "stopping takes the engine (mpv) down with it"; fi
m18=$(mark); player_key space
if wait_new "$m18" "playing 4/4: song4.mp3" 5; then pass "Play starts it again"
else fail "Play starts it again"; fi
shot 2-playing
if player_quit; then pass "q closes the window and ends the process with 0"
else fail "q closes the window and ends the process"; fi
if pgrep -f "$FAKEBIN/mpv" >/dev/null 2>&1; then fail "closing the window leaves no engine behind"
else pass "closing the window leaves no engine behind"; fi

# ---------- 7. the end of a track, and the end of the list ----------
m19=$(mark)
if player_start 1 --play "$MUSIC2"; then pass "the player opens on a two-song folder"
else fail "the player opens on a two-song folder"; fi
if wait_new "$m19" "playing 1/2: one.mp3" 10; then pass "the first song plays"
else fail "the first song plays"; fi
if wait_new "$m19" "one.mp3 finished" 10 && wait_new "$m19" "playing 2/2: two.mp3" 10; then
    pass "a track that ends plays the next one by itself"
else fail "a track that ends plays the next one"; fi
if wait_new "$m19" "end of the list, stopping" 10; then pass "the end of the last track (repeat off) stops"
else fail "the end of the list stops the playback"; fi
m20=$(mark); player_key r
if wait_new "$m20" "repeat all" 5; then pass "repeat all (r once)"
else fail "repeat all (r once)"; fi
m21=$(mark); player_key space
if wait_new "$m21" "playing 2/2: two.mp3" 5 && wait_new "$m21" "playing 1/2: one.mp3" 10; then
    pass "with repeat all the end of the list goes back to the first"
else fail "with repeat all the end of the list goes back to the first"; fi
if player_quit; then pass "q ends the run (repeat all included)"
else fail "q ends the run (repeat all included)"; fi

# ---------- 8. no engine at all ----------
m22=$(mark)
PATH="$OUT/empty" "$B/hde-media" --play "$MUSIC" >> "$PLAYER_LOG" 2>&1 &
PLAYER_PID=$!
PLAYER_WINDOW=
for i in $(seq 1 60); do
    PLAYER_WINDOW=$(xdotool search --onlyvisible --name "$TITLE" 2>/dev/null | head -n 1)
    [ -n "$PLAYER_WINDOW" ] && break
    sleep 0.25
done
sleep 0.5
if wait_new "$m22" "engine: none" 5; then pass "with nothing installed the window says the engine is none"
else fail "with nothing installed the window says so"; fi
if wait_new "$m22" "install mpv" 5; then pass "... and what to install (mpv, or ffplay, or gst-launch-1.0)"
else fail "... and what to install"; fi
if wait_new "$m22" "playing" 2; then fail "and it plays nothing (there is nothing to play with)"
else pass "and it plays nothing (there is nothing to play with)"; fi
shot 3-no-engine
if player_quit; then pass "the window closes with 0 even without an engine"
else fail "the window closes with 0 even without an engine"; fi

# ---------- 9. a video is drawn inside the window (mpv --wid, X11) ----------
: > "$ENGINE_LOG"
MPV_DURATION=2.5          # a short clip: the seek below has to stop at its end
m23=$(mark)
if player_start 60 --play "$FILMS"; then pass "a video opens the player too"
else fail "a video opens the player too"; fi
if wait_new "$m23" "playing 1/2: clip.mp4" 10; then pass "the video plays"
else fail "the video plays"; fi
if wait_new "$m23" "the video is drawn in this window" 5; then
    pass "mpv is told to draw it inside the window (an X11 session)"
else fail "mpv is told to draw the video inside the window"; fi
if grep -q -e "--wid=[0-9]" "$ENGINE_LOG"; then pass "the command line carries the X id of the area"
else fail "the command line carries the X id of the area (engine.log: $(head -n 1 "$ENGINE_LOG" | cut -c1-200))"; fi
if wait_new "$m23" "subtitles: clip.srt" 5; then pass "the subtitles next to the video are found and named (clip.srt)"
else fail "the subtitles next to the video are named"; fi
m24=$(mark); player_key v
if wait_new "$m24" "subtitles off" 5 && grep -q '"sub-visibility",false' "$ENGINE_LOG"; then
    pass "v hides them (mpv is told over the socket)"
else fail "v hides the subtitles (mpv told nothing: $(grep -c 'sub-visibility' "$ENGINE_LOG" 2>/dev/null) of those)"; fi
m25=$(mark); player_key v
if wait_new "$m25" "subtitles on" 5 && grep -q '"sub-visibility",true' "$ENGINE_LOG"; then pass "v again: they are back"
else fail "v again shows the subtitles"; fi
# five seconds on from a 2.5 s clip is its end: a seek is kept inside the track, it does not run past it
m26=$(mark); player_key ctrl+Right
if wait_new "$m26" "seek " 5; then
    seek_line=$(grep -n '"seek",[0-9.]*' "$ENGINE_LOG" | tail -n 1 | cut -d: -f1)
    [ -n "$seek_line" ] || seek_line=1     # nothing there: the check below fails, it does not blow up
    seek_to=$(sed -n "${seek_line}p" "$ENGINE_LOG" | grep -o '"seek",[0-9.]*' | cut -d, -f2)
    if [ -n "$seek_to" ] && awk "BEGIN { exit !($seek_to > 2.4 && $seek_to < 2.6) }"; then
        pass "a seek stops at the end of the track (the clip is 2.5 s, it asked for ${seek_to}s)"
    else fail "a seek stops at the end of the track (asked for '${seek_to:-nothing}' in $ENGINE_LOG)"; fi
else fail "the video phase reaches mpv with a seek too"; fi
m27=$(mark); player_key Right
if wait_new "$m27" "playing 2/2: zebra.mp4" 5; then pass "Right: the second video"
else fail "Right: the second video"; fi
if wait_new "$m27" "no subtitles next to zebra.mp4" 5; then pass "a video with nothing next to it says so"
else fail "a video with nothing next to it says so"; fi
shot 4-video
if player_quit; then pass "q closes the video window with 0"
else fail "q closes the video window with 0"; fi

if [ "$FAILS" -gt 100 ]; then FAILS=100; fi
exit "$FAILS"
