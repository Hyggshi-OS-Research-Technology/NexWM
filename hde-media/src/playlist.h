/* playlist.h — hde-media: the music and the video (the second part of the multimedia of HDE, after the pictures of
 * viewer.c). This header is the *logic* of the player; the window that shows it and starts the engine is
 * player.h/player.c (HdeMediaPlayer there, HdePlayer here — the state, not the widgets). What this header shares with
 * the tests (tests/player-test.c): the list of what is played, the tags read from the files themselves, and the state
 * of the playback (which track, what follows, shuffle, repeat, volume).
 *
 * Everything here is plain C + POSIX, no GTK and no glib, for the same reason as media.h: `make check-unit` builds and
 * runs all of it without a display, with a fake clock and a fixed shuffle seed, so the parts that are easy to get
 * wrong (the order, the end of the list, the repeat modes, the tags of a file) are tested on any machine. What is
 * *played* — decoding — is the business of the window: gstreamer / mpv / ffplay when the machine has one of them
 * (the "hybrid" engine of HDE), and nothing but a clear message when it has none.
 *
 * The rules the rest of the folder follows (and what tests/player-test.c checks):
 *   - a playlist file (.m3u/.m3u8/.pls) is read the way other players write it, relative paths included;
 *   - the tags come from the file itself (ID3v2/ID3v1, Ogg Vorbis/Opus comments, FLAC, WAV): no library needed;
 *   - the user's Next/Previous keys wrap around the list; the end of a track honours `repeat` (and only then ends
 *     the playback);
 *   - shuffle never plays the same track twice in a row, and with a fixed seed it is reproducible.
 */
#ifndef HDE_PLAYLIST_H
#define HDE_PLAYLIST_H

#include <stddef.h>
#include <stdint.h>

/* what a file is, by extension: the window shows a picture for the audio, a video for the video, and a speaker icon
 * for anything else */
typedef enum {
    HDE_MEDIA_KIND_UNKNOWN = 0,
    HDE_MEDIA_KIND_AUDIO,
    HDE_MEDIA_KIND_VIDEO
} HdeMediaKind;

/* one thing to play, with what its own tags say about it (title/artist/album are never NULL: without tags they are
 * the file name / "" / "", so the window never has to check) */
typedef struct {
    char         *path;
    char         *title;
    char         *artist;
    char         *album;
    int           track;        /* 0 = no track number in the tags */
    double        duration;     /* seconds, 0 = unknown (a stream, or a format we do not read the length of) */
    HdeMediaKind  kind;
} HdeTrack;

typedef struct {
    HdeTrack *items;
    size_t    n, cap;
} HdePlaylist;

void  hde_playlist_init(HdePlaylist *p);
void  hde_playlist_free(HdePlaylist *p);
void  hde_playlist_clear(HdePlaylist *p);                       /* empty it, keep what was allocated */
long  hde_playlist_add(HdePlaylist *p, const char *path);       /* the index of the new entry, or -1 (memory) */
long  hde_playlist_scan(HdePlaylist *p, const char *dir, int recursive);   /* the music and videos of a folder */
long  hde_playlist_find(const HdePlaylist *p, const char *path);           /* -1 when it is not in the list */
int   hde_playlist_copy(HdePlaylist *dst, const HdePlaylist *src);         /* 0 ok, -1 memory */

/* a playlist file: .m3u, .m3u8 (with the #EXTINF lines other players write) or .pls. Returns how many entries were
 * added, -1 when the file is not a playlist we read, -2 when it cannot be read at all. */
long  hde_playlist_read(HdePlaylist *p, const char *file);
int   hde_playlist_write(const HdePlaylist *p, const char *file);          /* 0 ok (an extended .m3u) */
int   hde_media_is_playlist(const char *name);                             /* by extension */

/* the tags of one file, read by hand (no tag library): ID3v2 and ID3v1 in an MP3, the Vorbis comment of an Ogg
 * Vorbis / Opus file, the same in a FLAC (plus its STREAMINFO for the length), the fmt/data chunks of a WAV, and for
 * an MP3 without a TLEN frame an estimate from the first frame header. Fills what it finds, and always leaves
 * title/artist/album as strings (the file name when the file has no tags): 0 when the file was read (even with no
 * tags at all), -1 when it cannot be opened — in which case the strings are set just the same. */
int   hde_media_read_tags(HdeTrack *t);

const char *hde_media_kind_name(HdeMediaKind kind);             /* "audio" / "video" / "file", for the logs */
HdeMediaKind hde_media_kind_of(const char *name);               /* by extension */
int   hde_media_is_media(const char *name);                     /* audio or video: what the player opens */

/* "3:07", "1:02:03"; seconds < 0 (unknown) gives "-:--". Returns what it wrote. */
size_t hde_media_format_time(char *buf, size_t n, double seconds);
/* the line the panel and the window show: "Artist — Title", or the file name when there are no tags */
size_t hde_playlist_now_playing(const HdePlaylist *p, long index, char *buf, size_t n);

/* ------------------------------------------------------------------ the state of the playback */
typedef enum {
    HDE_REPEAT_OFF = 0,     /* the end of the last track stops the playback */
    HDE_REPEAT_ALL,         /* ... goes back to the first one */
    HDE_REPEAT_ONE          /* ... plays the same one again */
} HdeRepeat;

typedef struct {
    HdePlaylist list;
    long        index;        /* the track that plays now, -1 = none */
    int         playing;      /* it was started and was not stopped */
    int         paused;
    HdeRepeat   repeat;
    int         shuffle;
    double      volume;       /* 0 .. 1 */
    int         muted;
    uint32_t    seed;         /* the shuffle's state: tests/player-test.c sets it so the order is reproducible */
    int64_t     started_ms;   /* when the current track started: the caller passes the clock, the test fakes it */
} HdePlayer;

void  hde_player_init(HdePlayer *pl);
void  hde_player_free(HdePlayer *pl);
/* play track `index` (clamped into the list). Returns the index it plays, or -1 when the list is empty. */
long  hde_player_start(HdePlayer *pl, long index, int64_t now_ms);
/* the Next/Previous keys: they wrap around the list, and with shuffle they pick another one at random (never the same
 * one twice in a row). Returns the index to play, or -1 when there is nothing to play. */
long  hde_player_next(HdePlayer *pl, int direction, int64_t now_ms);
/* the end of a track: 0 when the playback stops (repeat off, last track), else 1 with `index` set to what plays next
 * (repeat one: the same track again) */
int   hde_player_advance(HdePlayer *pl, int64_t now_ms);
void  hde_player_stop(HdePlayer *pl);
double hde_player_set_volume(HdePlayer *pl, double v);          /* clamped to 0 .. 1, returns what it kept */
double hde_player_volume_step(HdePlayer *pl, int direction);    /* the +/- and the wheel: steps of 5 % */

#endif /* HDE_PLAYLIST_H */
