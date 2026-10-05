/* hde-commands.h — shared shell commands (header-only, no X11/GTK dependency) so that hotkeys, panel,
 * Settings and backend do the same thing in the same way. */
#ifndef HDE_COMMANDS_H
#define HDE_COMMANDS_H

/* ---------------- shared shell commands ----------------
 * Volume: PulseAudio/PipeWire-pulse (pactl) -> PipeWire (wpctl) -> ALSA (amixer).
 * Raising the volume unmutes automatically and stops at 100% (no overshoot as with continuous scrolling). */
#define HDE_SH_HAVE_PACTL "command -v pactl >/dev/null 2>&1 && pactl info >/dev/null 2>&1"

#define HDE_SH_VOLUME_MUTE \
    "if " HDE_SH_HAVE_PACTL "; then pactl set-sink-mute @DEFAULT_SINK@ toggle; " \
    "elif command -v wpctl >/dev/null 2>&1; then wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle; " \
    "else amixer -q -D pulse sset Master toggle 2>/dev/null || amixer -q sset Master toggle; fi"

#define HDE_SH_VOLUME_DOWN \
    "if " HDE_SH_HAVE_PACTL "; then pactl set-sink-volume @DEFAULT_SINK@ -5%; " \
    "elif command -v wpctl >/dev/null 2>&1; then wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%-; " \
    "else amixer -q sset Master 5%-; fi"

#define HDE_SH_VOLUME_UP \
    "if " HDE_SH_HAVE_PACTL "; then pactl set-sink-mute @DEFAULT_SINK@ 0; " \
    "v=$(pactl get-sink-volume @DEFAULT_SINK@ 2>/dev/null | grep -o '[0-9]*%' | head -n1 | tr -d %); " \
    "if [ -z \"$v\" ]; then pactl set-sink-volume @DEFAULT_SINK@ +5%; " \
    "elif [ \"$v\" -lt 95 ]; then pactl set-sink-volume @DEFAULT_SINK@ +5%; " \
    "elif [ \"$v\" -lt 100 ]; then pactl set-sink-volume @DEFAULT_SINK@ 100%; fi; " \
    "elif command -v wpctl >/dev/null 2>&1; then wpctl set-mute @DEFAULT_AUDIO_SINK@ 0; " \
    "wpctl set-volume -l 1.0 @DEFAULT_AUDIO_SINK@ 5%+; " \
    "else amixer -q sset Master 5%+ unmute; fi"

#define HDE_SH_MIC_MUTE \
    "if " HDE_SH_HAVE_PACTL "; then pactl set-source-mute @DEFAULT_SOURCE@ toggle; " \
    "elif command -v wpctl >/dev/null 2>&1; then wpctl set-mute @DEFAULT_AUDIO_SOURCE@ toggle; " \
    "else amixer -q sset Capture toggle; fi"

/* Set an absolute volume: use with printf (argument %d = percent). */
#define HDE_SH_VOLUME_SET_FMT \
    "if " HDE_SH_HAVE_PACTL "; then pactl set-sink-volume @DEFAULT_SINK@ %d%%; " \
    "elif command -v wpctl >/dev/null 2>&1; then wpctl set-volume @DEFAULT_AUDIO_SINK@ %d%%; " \
    "else amixer -q sset Master %d%%; fi"

/* Prints "<percent> <muted 0|1>" for the default output volume, or nothing if there is no audio. */
#define HDE_SH_VOLUME_GET \
    "if " HDE_SH_HAVE_PACTL "; then " \
    "v=$(pactl get-sink-volume @DEFAULT_SINK@ 2>/dev/null | grep -o '[0-9]*%' | head -n1 | tr -d %); " \
    "m=$(pactl get-sink-mute @DEFAULT_SINK@ 2>/dev/null | grep -c yes); " \
    "if [ -z \"$v\" ]; then " \
    "  s=$(pactl info 2>/dev/null | sed -n 's/^Default Sink: //p'); " \
    "  v=$(pactl list sinks 2>/dev/null | awk -v s=\"$s\" '/^[ \\t]*Name: /{n=$2} /^[ \\t]*Volume:/{if(n==s){for(i=1;i<=NF;i++)if($i~/%$/){gsub(\"%\",\"\",$i);print $i;exit}}}'); " \
    "  m=$(pactl list sinks 2>/dev/null | awk -v s=\"$s\" '/^[ \\t]*Name: /{n=$2} /^[ \\t]*Mute:/{if(n==s){print ($2==\"yes\")?1:0;exit}}'); " \
    "fi; [ -n \"$v\" ] && echo \"$v ${m:-0}\"; " \
    "elif command -v wpctl >/dev/null 2>&1; then " \
    "wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null | awk '{printf \"%d %d\\n\", $2*100+0.5, /MUTED/?1:0}'; " \
    "elif command -v amixer >/dev/null 2>&1; then " \
    "amixer get Master 2>/dev/null | awk -F'[][]' '/%/{v=$2; m=($0~/\\[off\\]/)?1:0} END{if(v!=\"\"){sub(\"%\",\"\",v); print v, m}}'; fi"

#define HDE_SH_MIC_GET \
    "if " HDE_SH_HAVE_PACTL "; then pactl get-source-mute @DEFAULT_SOURCE@ 2>/dev/null | grep -c yes; " \
    "elif command -v wpctl >/dev/null 2>&1; then wpctl get-volume @DEFAULT_AUDIO_SOURCE@ 2>/dev/null | grep -c MUTED; " \
    "else amixer get Capture 2>/dev/null | grep -c '\\[off\\]'; fi"

/* Brightness: prints the percentage after the change. Exits with code 3 if no tool is available. */
#define HDE_SH_BRIGHTNESS_UP \
    "if command -v brightnessctl >/dev/null 2>&1; then brightnessctl -q set 5%+; " \
    "brightnessctl -m 2>/dev/null | head -n1 | cut -d, -f4 | tr -d %; " \
    "elif command -v light >/dev/null 2>&1; then light -A 5; light -G | cut -d. -f1; " \
    "elif command -v xbacklight >/dev/null 2>&1; then xbacklight -inc 5; xbacklight -get | cut -d. -f1; " \
    "else exit 3; fi"

#define HDE_SH_BRIGHTNESS_DOWN \
    "if command -v brightnessctl >/dev/null 2>&1; then brightnessctl -q -n1 set 5%- 2>/dev/null || brightnessctl -q set 5%-; " \
    "brightnessctl -m 2>/dev/null | head -n1 | cut -d, -f4 | tr -d %; " \
    "elif command -v light >/dev/null 2>&1; then light -U 5; light -G | cut -d. -f1; " \
    "elif command -v xbacklight >/dev/null 2>&1; then xbacklight -dec 5; xbacklight -get | cut -d. -f1; " \
    "else exit 3; fi"

/* Screen lock: try real lockers first. `loginctl lock-session` only works when
 * some program listens to logind, so it comes last. */
#define HDE_SH_LOCK \
    "if command -v light-locker-command >/dev/null 2>&1 && light-locker-command -l >/dev/null 2>&1; then :; " \
    "elif command -v xscreensaver-command >/dev/null 2>&1 && xscreensaver-command -lock >/dev/null 2>&1; then :; " \
    "elif command -v xfce4-screensaver-command >/dev/null 2>&1 && xfce4-screensaver-command --lock >/dev/null 2>&1; then :; " \
    "elif command -v mate-screensaver-command >/dev/null 2>&1 && mate-screensaver-command --lock >/dev/null 2>&1; then :; " \
    "elif command -v cinnamon-screensaver-command >/dev/null 2>&1 && cinnamon-screensaver-command --lock >/dev/null 2>&1; then :; " \
    "elif command -v dm-tool >/dev/null 2>&1 && [ -n \"$XDG_SEAT_PATH\" ]; then dm-tool lock; " \
    "elif command -v i3lock >/dev/null 2>&1; then i3lock -c 1e222a; " \
    "elif command -v slock >/dev/null 2>&1; then slock; " \
    "elif command -v xsecurelock >/dev/null 2>&1; then xsecurelock; " \
    "elif command -v xdg-screensaver >/dev/null 2>&1 && xdg-screensaver lock >/dev/null 2>&1; then :; " \
    "else loginctl lock-session; fi"

#define HDE_SH_TERMINAL \
    "for t in x-terminal-emulator gnome-terminal xfce4-terminal mate-terminal tilix konsole lxterminal " \
    "qterminal terminator alacritty kitty xterm; do " \
    "if command -v \"$t\" >/dev/null 2>&1; then exec \"$t\"; fi; done; exit 127"

#define HDE_SH_FILES \
    "for f in thunar pcmanfm nautilus nemo caja dolphin pcmanfm-qt; do " \
    "if command -v \"$f\" >/dev/null 2>&1; then exec \"$f\" \"$HOME\"; fi; done; exec xdg-open \"$HOME\""

#endif
