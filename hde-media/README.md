# Hyggshi Media (`hde-media`)

The multimedia of HDE: the pictures first, then the music and the video. This folder is the whole of it — the window
(`viewer.c`), the logic that has nothing to do with a window (`gallery.c`, `media.h`) and, as the work goes on, the
player and the screen recorder.

```
hde-media PICTURE...          the pictures given (the folder of the first one is the list the arrows walk through)
hde-media TRACK...            (player, next step) music and video: the list, the tags, the playing
hde-media FOLDER              the pictures of a folder
hde-media                     the Pictures folder of this user
hde-media -s FOLDER           start with the slideshow running
hde-media -i 2 FOLDER         seconds per picture in the slideshow (0.5 .. 600, default 5)
hde-media --sort date FOLDER  the order: name (natural: img2 before img10), date or size
hde-media -r FOLDER           also the pictures of the sub-folders (8 deep at most)
hde-media -f PICTURE          start full screen
hde-media --version, --help
```

Keys: `Left`/`Right`/`PageUp`/`PageDown` previous and next, `Home`/`End` the first and the last, `+`/`-` zoom, `0` fit,
`1` as it is on disk, `r` and `Shift+R` rotate, `s` the slideshow, `Space` the next picture, `i` more about the picture
(folder, date, size), `f`/`F11` full screen, `Ctrl+O` open a picture, `Escape` leaves full screen (again: closes), `q`
closes. The mouse wheel zooms, a double click goes full screen, dragging moves a picture that is bigger than the
window, and a picture dropped on the window opens it.

Running `hde-media` again while it is open shows the new picture in the window that is already there (one process, one
window — what the file manager's "Open With" expects). In HDE sessions `hde-mimeapps.list` makes it the program that
opens pictures.

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
| `src/player.h` | What the player promises: the list of what is played, the tags of a file, the state of the playback (which track, what follows, shuffle, repeat, the volume). Plain C too. |
| `src/playlist.c` | The implementation of that: folder scans, the `.m3u`/`.m3u8`/`.pls` files other players write (relative paths included), and the tags read by hand — ID3v2.2/2.3/2.4 (all four text encodings, unsynchronisation) and ID3v1, the Vorbis comment of Ogg Vorbis/Opus/FLAC, FLAC's STREAMINFO, WAV's fmt/data chunks, and an estimate from the first frame of an MP3 with no TLEN. |
| `hde-media.desktop` | The menu entry and the types of pictures that open here. |

## Tests

```sh
make check-unit            # tests/media-test.c and tests/player-test.c: no display, no sound card needed
make check-media           # tests/media-test.sh: the window driven for real (Xvfb + Metacity), pixels included
```

`tests/player-test.c` (92 checks) writes its own tiny files — an ID3v2.3 tag with a TLEN frame, a title in UTF-16, an
ID3v1 tag, an Ogg Vorbis comment header, a FLAC STREAMINFO, a WAV of a known size — and checks what the reader makes of
them, the `.m3u`/`.pls` round trip, and the playback: next/previous wrapping, the end of a list, repeat off/all/one,
shuffle (with a fixed seed, so the same order comes back), the volume steps.

The second one needs `xvfb xdotool metacity imagemagick dbus-x11`, opens a folder of solid-coloured pictures of known
sizes and checks what the log says and what is on the screen: the arrows (wrapping included), the zoom keys and the
mouse wheel, the rotation, the slideshow running by itself, full screen and Escape, a second `hde-media` reusing the
open window, one process, `q` closing with 0.

## What comes next in this folder

The player's *window* (music and video, subtitles, the playlist panel) on top of `playlist.c`, with the hybrid engine:
what HDE can do itself (the pictures through gdk-pixbuf, WAV, the tags, the playlists — all of that is here and tested)
and GStreamer, mpv or ffplay — whichever of them is installed — for everything else, with a clear message when none of
them is. Then the panel showing what is playing (HDE's own MPRIS interface), the multimedia keys of `hde-hotkeys`, the
volume on the screen, and a screen recorder.
