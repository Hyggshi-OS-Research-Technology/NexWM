/*
 * engine.h — what actually plays the sound and the video.
 *
 * HDE does not decode MP3 or MP4 itself: it plays the light things on its own (the pictures through gdk-pixbuf, the
 * WAV through the sound server's own player) and hands everything else to what the machine already has — mpv, ffplay
 * (ffmpeg) or gst-launch-1.0 (GStreamer). This file is the choice and the command line that follows from it: which of
 * them is installed, what running one looks like for a given file, and what to tell the user when none of them is.
 * Plain C (POSIX only), like gallery.c — no GTK, no GLib — so tests/player-test.c can check all of it, and the window
 * (player.c) only has to start the process.
 */
#ifndef HDE_MEDIA_ENGINE_H
#define HDE_MEDIA_ENGINE_H

#include "player.h" /* HdeMediaKind */

typedef enum {
    HDE_MEDIA_ENGINE_NONE = 0, /* nothing installed: the window says what to install */
    HDE_MEDIA_ENGINE_MPV,      /* mpv: the best of the three — it can be told to pause/seek/goto through its socket */
    HDE_MEDIA_ENGINE_FFPLAY,   /* ffplay: part of ffmpeg, its own window, no control but stop */
    HDE_MEDIA_ENGINE_GST,      /* gst-launch-1.0: part of GStreamer, its own window, no control but stop */
    HDE_MEDIA_ENGINE_PAPLAY,   /* the sound server's player (PulseAudio/PipeWire): sound only, WAV and friends */
    HDE_MEDIA_ENGINE_APLAY     /* ALSA's own player: sound only */
} HdeMediaEngineKind;

typedef struct {
    HdeMediaEngineKind kind;
    char *program;   /* the path that was found in $PATH */
    char **argv;     /* NULL terminated, argv[0] = program — ready for exec */
    int can_seek;    /* 1: the engine listens on an IPC socket (mpv) and can be told what to do */
    int sound_only;  /* 1: it cannot show video (paplay, aplay) */
} HdeMediaEngine;

/* What $PATH offers, best first: mpv, then ffplay, then gst-launch-1.0. HDE_MEDIA_ENGINE_NONE when there is nothing. */
HdeMediaEngineKind hde_media_engine_find(void);

/* Same, for a sound file: a WAV is played by the sound server itself (paplay, aplay) when one of them is there —
   lighter, and it does not care about the codecs of a file that needs none. Falls back to the three above. */
HdeMediaEngineKind hde_media_engine_find_for(HdeMediaKind file_kind, const char *path);

/* "mpv", "ffplay", "gst-launch-1.0", "paplay", "aplay", or "none" — for the log and the status line. */
const char *hde_media_engine_kind_name(HdeMediaEngineKind kind);

/* The command line for one file, or NULL when kind is NONE. file_kind tells video from sound (ffplay needs to be told:
   without -nodisp it opens a black window for an MP3). wid is an X11 window id to put the video in (0: a window of its
   own — mpv is the only one that can be embedded). ipc, when not NULL, is the socket mpv should listen on; then
   can_seek is 1 and the window can pause, seek and set the volume through it. */
HdeMediaEngine *hde_media_engine_new(HdeMediaEngineKind kind, const char *path, HdeMediaKind file_kind,
                                     unsigned long wid, const char *ipc);
void hde_media_engine_free(HdeMediaEngine *engine);

/* The whole command as one line, quoted for a human ("mpv --no-video --really-quiet '/música/Cà phê.mp3'"). */
char *hde_media_engine_line(const HdeMediaEngine *engine);

/* file:///... for a path, with the bytes that would confuse a parser written as %XX (a space, a '#', anything that is
   not unreserved). gst-launch-1.0 needs the URI, the others are given the path itself. */
char *hde_media_file_uri(const char *path);

/* Where a program is, or NULL: $PATH, and a name with a '/' in it is used as it is. */
char *hde_media_find_program(const char *name);

/* What to tell the user when nothing is installed (one line, English, with the two package commands). */
const char *hde_media_engine_hint(void);

#endif /* HDE_MEDIA_ENGINE_H */
