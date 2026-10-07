# Hyggshi Media (`hde-media`)

The multimedia of HDE: the pictures first, then the music and the video. This folder is the whole of it — the two
windows (`viewer.c` for the pictures, `player.c` for the music and the video), the logic that has nothing to do with a
window (`gallery.c`, `playlist.c`, `engine.c`) and, as the work goes on, the screen recorder.

```
hde-media PICTURE...          the pictures given (the folder of the first one is the list the arrows walk through)
hde-media FOLDER              the pictures of a folder
hde-media                     the Pictures folder of this user
hde-media -s FOLDER           start with the slideshow running
hde-media -i 2 FOLDER         seconds per picture in the slideshow (0.5 .. 600, default 5)
hde-media --sort date FOLDER  the order: name (natural: img2 before img10), date or size
hde-media -r FOLDER           also the pictures of the sub-folders (8 deep at most)
hde-media -f PICTURE          start full screen
hde-media SONG...             play them (the player window)
hde-media --play FOLDER       play the music and the video of a folder (with -r: of its sub-folders too)
hde-media --play              play the Music folder of this user
hde-media --version, --help
```

A song or a video opens the player, a folder the picture viewer, and a `.m3u`/`.m3u8`/`.pls` list the player: what is
given decides (a folder is a folder of pictures unless `--play` says otherwise).

Keys: `Left`/`Right`/`PageUp`/`PageDown` previous and next, `Home`/`End` the first and the last, `+`/`-` zoom, `0` fit,
`1` as it is on disk, `r` and `Shift+R` rotate, `s` the slideshow, `Space` the next picture, `i` more about the picture
(folder, date, size), `f`/`F11` full screen, `Ctrl+O` open a picture, `Escape` leaves full screen (again: closes), `q`
closes. The mouse wheel zooms, a double click goes full screen, dragging moves a picture that is bigger than the
window, and a picture dropped on the window opens it.

Player keys: `Space` play/pause (mpv only: the others cannot be paused), `Left`/`Right` (or `b`/`n`) previous and next,
`Ctrl+Left`/`Ctrl+Right` five seconds back or forward, `s` stop, `Up`/`Down` or `+`/`-` the volume, `m` mute, `z`
shuffle, `r` repeat (off, all, one), `v` the subtitles (mpv only, and only a video), `q` closes the window — which stops
the sound with it.

A video is drawn *inside* this window when mpv is the engine and the session is X11 (mpv is given the X id of the black
area above the seek bar); with ffplay, gst-launch-1.0, or on Wayland, it opens a window of its own.

Subtitles are mpv's business: it loads the ones next to a video (`clip.srt`, and `clip.en.srt` when there is a language
in the name) and the ones stored inside it (an `.mkv` with three subtitle tracks) by itself. What HDE does with them is
look for the sidecar (`playlist.c`, `hde_media_subtitle_for`) and say in one line per video what it found — `subtitles:
clip.srt` or `no subtitles next to clip.mp4` — and let `v` show or hide them over the socket while the video plays. The
other engines show no subtitles at all, and the window says so instead of pretending.

Running `hde-media` again while it is open shows the new picture in the window that is already there, and hands what it
was given to the player if that is what is open (one process, one window per kind — what the file manager's "Open With"
expects). In HDE sessions `hde-mimeapps.list` makes it the program that opens pictures, music and video.

## Build and install

```sh
make                       # at the top of the repository: builds everything
make -C hde-media          # or just this: needs libgtk-3-dev
sudo make install          # the top-level Makefile installs the program, its menu entry and the default viewer list
```

The theme follows HDE: Light / Dark and the accent colour chosen in Settings, live while the window is open.

## What is in here

| File | What it is |
| --- | --- |
| `src/media.h` | What the folder promises: the picture list, the order, the state of what is shown. Plain C (POSIX only), so all of it is tested without a display. |
| `src/gallery.c` | The implementation of that: folder scans (hidden files left out, sub-folders 8 deep with `-r`), the natural order (digit runs compare as numbers), the zoom ladder (5 % .. 1600 %), the slideshow clock, the human sizes. |
| `src/viewer.c` | The window: a drawing area painted with cairo, the toolbar, the status line, the keys, the wheel, the dragging, the dropped pictures. |
| `src/main.c` | The program: the options, the one running window a second `hde-media` hands its picture to, the theme and its CSS. |
| `src/player.h`, `src/player.c` | The player window: what plays now, the seek bar, the transport, the list, the status line — and, for a video with mpv on X11, the black area mpv is told to draw in (`--wid`). It starts the engine and follows the list; it decodes nothing itself. |
| `src/playlist.h` | What the player promises: the list of what is played, the tags of a file, the state of the playback (which track, what follows, shuffle, repeat, the volume). Plain C too (the window is `player.c`). |
| `src/playlist.c` | The implementation of that: folder scans, the `.m3u`/`.m3u8`/`.pls` files other players write (relative paths included), and the tags read by hand — ID3v2.2/2.3/2.4 (all four text encodings, unsynchronisation) and ID3v1, the Vorbis comment of Ogg Vorbis/Opus/FLAC, FLAC's STREAMINFO, WAV's fmt/data chunks, and an estimate from the first frame of an MP3 with no TLEN. |
| `src/engine.h`, `src/engine.c` | The hybrid engine's decision half: which of mpv / ffplay / gst-launch-1.0 / paplay / aplay this machine has, the command line that follows from it for a given file, the `file://` URI gst-launch-1.0 wants, and what to tell the user when none of them is installed. It starts nothing — the window does that. |
| `hde-media.desktop` | The menu entry and the types of pictures that open here. |

## Tests

```sh
make check-unit            # tests/media-test.c and tests/player-test.c: no display, no sound card needed
make check-media           # tests/media-test.sh and tests/player-window-test.sh: the windows driven for real
```

`tests/player-test.c` (133 checks) writes its own tiny files — an ID3v2.3 tag with a TLEN frame, a title in UTF-16, an
ID3v1 tag, an Ogg Vorbis comment header, a FLAC STREAMINFO, a WAV of a known size — and checks what the reader makes of
them, the `.m3u`/`.pls` round trip, and the playback: next/previous wrapping, the end of a list, repeat off/all/one,
shuffle (with a fixed seed, so the same order comes back), the volume steps (85 % down is 80 % — the awkward case a
double turns into 0.7999999999999999, which used to come out as 79 %). The engine is checked with a fake `$PATH`
of empty executable files — mpv wins over ffplay over gst-launch-1.0, a WAV goes to paplay or aplay when one of them is
there, a sound never gets a window from ffplay, a video never goes to the sound server, a `-weird.mp3` is handed over as
`./-weird.mp3`, and the URI of `/nhạc/Cà phê.mp3` comes out byte for byte.

`tests/player-window-test.sh` needs `xvfb xdotool metacity python3 dbus-x11` and *no* player at all: a stand-in for mpv
(`tests/fake-mpv.py`, an empty script that writes down what it was given, listens on the `--input-ipc-server` socket and
answers the JSON the window sends — pause, seek, the volume, how far into the track it is) plays songs that last one
second each, so the whole transport can be driven by the keyboard: the next and previous keys (and the wrap-around),
stop and play again, the volume and mute, shuffle, repeat (off → all → one), a second `hde-media` handing its song to
the window that is already open, a track that ends playing the next one by itself, the end of the list stopping, closing
the window with 0 and taking mpv down with it — and a run with no engine in `$PATH` at all, which has to say what to
install instead of doing nothing. Every check reads either the `hde-media: player: ` lines of the program or what the
stand-in was told over the socket. The stand-in writes every position it reports down as well (`TIME	3.250`), and that
is what lets the test judge a seek exactly: `Ctrl+Right` has to ask mpv for where the track *is* plus five seconds (the
absolute 5 s would be a different bug), and on a 2.5-second video the same key has to stop at the 2.5 s end of the track
instead of running past it. The video of the last phase also comes with a `clip.srt` next to it and a second video
(`zebra.mp4`) that has none: the log has to name the one and say there is nothing next to the other, and `v` has to send
mpv the one command that hides the subtitles and, again, the one that shows them.

The picture-viewer test needs `xvfb xdotool metacity imagemagick dbus-x11`, opens a folder of solid-coloured pictures of known
sizes and checks what the log says and what is on the screen: the arrows (wrapping included), the zoom keys and the
mouse wheel, the rotation, the slideshow running by itself, full screen and Escape, a second `hde-media` reusing the
open window, one process, `q` closing with 0.

## The engine (what plays it)

HDE decodes nothing itself. It plays the pictures through gdk-pixbuf, hands an uncompressed sound (WAV, AU, AIFF) to the
sound server's own player, and gives everything else to the first of these that is installed:

| | | |
| --- | --- | --- |
| `mpv` | everything | the best of them: with its IPC socket the window can pause, seek, set the volume and show or hide the subtitles; on X11 a video can even be drawn inside the window (everywhere else it opens its own) |
| `ffplay` | everything | part of ffmpeg, its own window, no control but stopping it |
| `gst-launch-1.0` | everything | part of GStreamer, its own window, no control but stopping it |
| `paplay` | sound only | PulseAudio/PipeWire's own player, for WAV/AU/AIFF |
| `aplay` | sound only | ALSA's own player, for WAV/AU/AIFF |

With none of them installed the window says so and gives the two commands (`sudo dnf install mpv` /
`sudo apt install mpv`) instead of staying silent.

## What comes next in this folder

The panel showing what is playing (HDE's own MPRIS interface), the multimedia keys of `hde-hotkeys`, the volume on the
screen, and a screen recorder.
