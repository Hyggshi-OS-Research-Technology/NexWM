/* tests/player-test.c — the logic of the media player (hde-media/src/playlist.c) without a display and without a
 * sound card: the list of what is played, the order, the m3u/pls files, the tags read from the files themselves
 * (ID3v2/ID3v1, Ogg Vorbis, FLAC, WAV, an MP3 length estimated from its first frame) and the state of the playback
 * (next/previous, the end of the list, shuffle, repeat, the volume). All of it is plain C: built and run by
 * `make check-unit` on every machine.
 *
 *   cc -O2 -Wall -Wextra -std=c11 -Ihde-media/src -o build/player-test tests/player-test.c \
 *      hde-media/src/playlist.c hde-media/src/gallery.c -lm
 *
 * The files it writes are tiny and made by hand, so every value the tests expect is in this file: an ID3v2.3 tag with
 * a TLEN frame, an ID3v2.4 tag whose title is UTF-16 (Vietnamese, so the encoding really is exercised), an MP3 with
 * only an ID3v1 tag, an Ogg Vorbis comment header, a FLAC STREAMINFO, a WAV with a known byte rate and size, an
 * extended .m3u and a .pls.
 *
 * PASS/FAIL lines; exit status = failures.
 */
#define _POSIX_C_SOURCE 200809L

#include "player.h"
#include "media.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails, passes;

#define CHECK(cond, ...) do { \
        if (cond) { passes++; printf("PASS: player: "); } else { fails++; printf("FAIL: player: "); } \
        printf(__VA_ARGS__); printf("\n"); \
    } while (0)

static char root[256];

static void path_of(char *out, size_t n, const char *rel) { snprintf(out, n, "%s/%s", root, rel); }

static void write_bytes(const char *rel, const void *data, size_t n)
{
    char path[512];
    path_of(path, sizeof path, rel);
    FILE *f = fopen(path, "wb");
    if (!f) { printf("FAIL: player: cannot write %s\n", path); fails++; return; }
    if (n && fwrite(data, 1, n, f) != n) { printf("FAIL: player: short write to %s\n", path); fails++; }
    fclose(f);
}

/* ---- the little files ---------------------------------------------------------------- */

/* one ID3v2 frame: 4-char id, big-endian size, two flag bytes, then the text (encoding byte 3 = UTF-8) */
static size_t id3_frame(unsigned char *out, const char *id, const char *text)
{
    size_t len = strlen(text) + 1;
    memcpy(out, id, 4);
    out[4] = (unsigned char)(len >> 24); out[5] = (unsigned char)(len >> 16);
    out[6] = (unsigned char)(len >> 8);  out[7] = (unsigned char)(len);
    out[8] = out[9] = 0;
    out[10] = 3;                                       /* UTF-8 */
    memcpy(out + 11, text, len);
    return 10 + len;
}

/* a frame whose text is UTF-16 with a BOM: what a tag written on Windows looks like */
static size_t id3_frame_utf16(unsigned char *out, const char *id, const unsigned short *units, size_t n_units)
{
    size_t len = 1 + 2 + 2 * n_units;                  /* encoding byte + BOM + the text */
    memcpy(out, id, 4);
    out[4] = (unsigned char)(len >> 24); out[5] = (unsigned char)(len >> 16);
    out[6] = (unsigned char)(len >> 8);  out[7] = (unsigned char)(len);
    out[8] = out[9] = 0;
    out[10] = 1;                                       /* UTF-16 with a BOM */
    out[11] = 0xFF; out[12] = 0xFE;                    /* little-endian */
    for (size_t i = 0; i < n_units; i++) {
        out[13 + 2 * i] = (unsigned char)(units[i] & 0xFF);
        out[14 + 2 * i] = (unsigned char)(units[i] >> 8);
    }
    return 10 + len;
}

static size_t id3v2_tag(unsigned char *out, int major, const unsigned char *frames, size_t n_frames)
{
    memcpy(out, "ID3", 3);
    out[3] = (unsigned char)major;
    out[4] = 0;
    out[5] = 0;                                        /* no unsynchronisation, no extended header */
    out[6] = (unsigned char)((n_frames >> 21) & 0x7F);
    out[7] = (unsigned char)((n_frames >> 14) & 0x7F);
    out[8] = (unsigned char)((n_frames >> 7) & 0x7F);
    out[9] = (unsigned char)(n_frames & 0x7F);
    memcpy(out + 10, frames, n_frames);
    return 10 + n_frames;
}

static void write_id3v1(const char *rel, const char *title, const char *artist, const char *album, int track)
{
    unsigned char tail[128];
    memset(tail, 0, sizeof tail);
    memcpy(tail, "TAG", 3);
    memcpy(tail + 3, title, strlen(title));
    memcpy(tail + 33, artist, strlen(artist));
    memcpy(tail + 63, album, strlen(album));
    memcpy(tail + 93, "2024", 4);                       /* year */
    tail[97 + 28] = 0;                                  /* the comment field starts at 97 */
    tail[97 + 29] = (unsigned char)track;               /* ID3v1.1 puts the track number at its end */

    char path[512];
    path_of(path, sizeof path, rel);
    FILE *f = fopen(path, "ab");
    if (!f) { printf("FAIL: player: cannot append to %s\n", path); fails++; return; }
    fwrite(tail, 1, sizeof tail, f);
    fclose(f);
}

/* an MP3 with no ID3v2 tag at all: only the frame header (for the length) and an ID3v1 tag at the end */
static void write_raw_mp3(const char *rel, const char *title, const char *artist, const char *album, int track)
{
    unsigned char body[512];
    memset(body, 0x55, sizeof body);
    body[0] = 0xFF; body[1] = 0xFB; body[2] = 0x90; body[3] = 0x00;   /* MPEG1 layer 3, 128 kbps, 44100 Hz */
    write_bytes(rel, body, sizeof body);
    write_id3v1(rel, title, artist, album, track);
}

/* an MP3 with an ID3v2.3 tag (with a TLEN frame) and some audio after it */
static void write_mp3(const char *rel, const char *title, const char *artist, const char *album, const char *track, double seconds)
{
    unsigned char frames[1024];
    size_t n = 0;
    n += id3_frame(frames + n, "TIT2", title);
    n += id3_frame(frames + n, "TPE1", artist);
    n += id3_frame(frames + n, "TALB", album);
    n += id3_frame(frames + n, "TRCK", track);
    char len[32];
    snprintf(len, sizeof len, "%d", (int)(seconds * 1000));   /* TLEN is in milliseconds */
    n += id3_frame(frames + n, "TLEN", len);

    unsigned char tag[1200];
    size_t taglen = id3v2_tag(tag, 3, frames, n);
    write_bytes(rel, tag, taglen);

    /* some audio after the tag: a frame header only matters for the files without a TLEN frame */
    unsigned char audio[512];
    memset(audio, 0x55, sizeof audio);
    audio[0] = 0xFF; audio[1] = 0xFB; audio[2] = 0x90; audio[3] = 0x00;   /* MPEG1 layer 3, 128 kbps, 44100 Hz */

    char path[512];
    path_of(path, sizeof path, rel);
    FILE *f = fopen(path, "ab");
    if (f) { fwrite(audio, 1, sizeof audio, f); fclose(f); }
}

/* one Ogg page: "OggS", 27 bytes of header, the segment table, then the packet */
static size_t ogg_page(unsigned char *buf, int seq, int bos, const unsigned char *packet, size_t len)
{
    size_t n = 0;
    memcpy(buf + n, "OggS", 4); n += 4;
    buf[n++] = 0;                                      /* version */
    buf[n++] = (unsigned char)(bos ? 2 : 0);           /* the first page of the stream */
    for (int i = 0; i < 8; i++) buf[n++] = 0;          /* granule position */
    buf[n++] = 1; buf[n++] = 0; buf[n++] = 0; buf[n++] = 0;    /* serial */
    buf[n++] = (unsigned char)seq; buf[n++] = 0; buf[n++] = 0; buf[n++] = 0;
    for (int i = 0; i < 4; i++) buf[n++] = 0;          /* CRC: our reader does not check it */
    buf[n++] = 1;                                      /* one segment */
    buf[n++] = (unsigned char)len;
    memcpy(buf + n, packet, len); n += len;
    return n;
}

static void write_ogg(const char *rel, const char *title, const char *artist)
{
    unsigned char buf[1024];
    size_t n = 0;
    unsigned char ident[64];
    size_t il = 0;
    memcpy(ident + il, "\x01vorbis", 7); il += 7;       /* the identification header: not the comments */
    memset(ident + il, 0, 23); il += 23;
    n += ogg_page(buf + n, 0, 1, ident, il);
    unsigned char comment[512];
    size_t cn = 0;
    memcpy(comment + cn, "\x03vorbis", 7); cn += 7;     /* the comment header */
    const char *vendor = "hde-test";
    unsigned vlen = (unsigned)strlen(vendor);
    comment[cn++] = (unsigned char)vlen; comment[cn++] = 0; comment[cn++] = 0; comment[cn++] = 0;
    memcpy(comment + cn, vendor, vlen); cn += vlen;
    unsigned count = 2;
    comment[cn++] = (unsigned char)count; comment[cn++] = 0; comment[cn++] = 0; comment[cn++] = 0;
    char title_kv[128], artist_kv[128];
    snprintf(title_kv, sizeof title_kv, "TITLE=%s", title);
    snprintf(artist_kv, sizeof artist_kv, "ARTIST=%s", artist);
    const char *kv[2] = { title_kv, artist_kv };
    for (int i = 0; i < 2; i++) {
        unsigned l = (unsigned)strlen(kv[i]);
        comment[cn++] = (unsigned char)l; comment[cn++] = (unsigned char)(l >> 8); comment[cn++] = 0; comment[cn++] = 0;
        memcpy(comment + cn, kv[i], l); cn += l;
    }
    n += ogg_page(buf + n, 1, 0, comment, cn);
    write_bytes(rel, buf, n);
}

static void write_flac(const char *rel, unsigned rate, unsigned long long samples)
{
    unsigned char buf[64];
    memset(buf, 0, sizeof buf);
    memcpy(buf, "fLaC", 4);
    buf[4] = 0x80;                                      /* last metadata block, type 0 = STREAMINFO */
    buf[5] = 0; buf[6] = 0; buf[7] = 34;
    unsigned char *b = buf + 8;
    b[10] = (unsigned char)((rate >> 12) & 0xFF);
    b[11] = (unsigned char)((rate >> 4) & 0xFF);
    b[12] = (unsigned char)(((rate & 0x0F) << 4) | (1 << 1));   /* 2 channels, 16 bits per sample */
    b[13] = (unsigned char)(((samples >> 32) & 0x0F) | (1 << 4));
    b[14] = (unsigned char)((samples >> 24) & 0xFF);
    b[15] = (unsigned char)((samples >> 16) & 0xFF);
    b[16] = (unsigned char)((samples >> 8) & 0xFF);
    b[17] = (unsigned char)(samples & 0xFF);
    write_bytes(rel, buf, 8 + 34);
}

static void write_wav(const char *rel, unsigned byte_rate, unsigned data_bytes)
{
    unsigned char head[44];
    memset(head, 0, sizeof head);
    memcpy(head, "RIFF", 4);
    unsigned riff = 36 + data_bytes;
    head[4] = (unsigned char)riff; head[5] = (unsigned char)(riff >> 8); head[6] = (unsigned char)(riff >> 16); head[7] = (unsigned char)(riff >> 24);
    memcpy(head + 8, "WAVEfmt ", 8);
    unsigned fmt = 16;
    head[16] = (unsigned char)fmt; head[17] = head[18] = head[19] = 0;
    head[20] = 1; head[21] = 0;                         /* PCM */
    head[22] = 2; head[23] = 0;                         /* 2 channels */
    unsigned rate = 44100;
    head[24] = (unsigned char)rate; head[25] = (unsigned char)(rate >> 8); head[26] = (unsigned char)(rate >> 16); head[27] = (unsigned char)(rate >> 24);
    head[28] = (unsigned char)byte_rate; head[29] = (unsigned char)(byte_rate >> 8);
    head[30] = (unsigned char)(byte_rate >> 16); head[31] = (unsigned char)(byte_rate >> 24);
    memcpy(head + 36, "data", 4);
    head[40] = (unsigned char)data_bytes; head[41] = (unsigned char)(data_bytes >> 8);
    head[42] = (unsigned char)(data_bytes >> 16); head[43] = (unsigned char)(data_bytes >> 24);
    write_bytes(rel, head, sizeof head);
}

/* ---- the tests ------------------------------------------------------------------------ */

static void test_kinds(void)
{
    CHECK(hde_media_kind_of("a.mp3") == HDE_MEDIA_KIND_AUDIO, "a .mp3 is audio");
    CHECK(hde_media_kind_of("A.FLAC") == HDE_MEDIA_KIND_AUDIO, "a .FLAC is audio, whatever the case");
    CHECK(hde_media_kind_of("film.mkv") == HDE_MEDIA_KIND_VIDEO, "a .mkv is video");
    CHECK(hde_media_kind_of("clip.MP4") == HDE_MEDIA_KIND_VIDEO, "a .MP4 is video");
    CHECK(hde_media_kind_of("notes.txt") == HDE_MEDIA_KIND_UNKNOWN, "a .txt is neither");
    CHECK(hde_media_kind_of("noextension") == HDE_MEDIA_KIND_UNKNOWN, "a file without an extension is neither");
    CHECK(hde_media_is_media("song.opus") && !hde_media_is_media("song.opus.txt"), "the last extension decides");
    CHECK(hde_media_is_playlist("list.m3u") && hde_media_is_playlist("LIST.PLS") && !hde_media_is_playlist("song.mp3"),
          "a playlist is .m3u / .m3u8 / .pls");
    CHECK(!strcmp(hde_media_kind_name(HDE_MEDIA_KIND_AUDIO), "audio"), "the kind has a name for the logs");
}

static void test_time(void)
{
    char buf[32];
    hde_media_format_time(buf, sizeof buf, -1);
    CHECK(!strcmp(buf, "-:--"), "an unknown length is '-:--' (got %s)", buf);
    hde_media_format_time(buf, sizeof buf, 0);
    CHECK(!strcmp(buf, "0:00"), "0 s is '0:00' (got %s)", buf);
    hde_media_format_time(buf, sizeof buf, 7);
    CHECK(!strcmp(buf, "0:07"), "7 s is '0:07' (got %s)", buf);
    hde_media_format_time(buf, sizeof buf, 187);
    CHECK(!strcmp(buf, "3:07"), "187 s is '3:07' (got %s)", buf);
    hde_media_format_time(buf, sizeof buf, 3600);
    CHECK(!strcmp(buf, "1:00:00"), "an hour is '1:00:00' (got %s)", buf);
    hde_media_format_time(buf, sizeof buf, 3723.4);
    CHECK(!strcmp(buf, "1:02:03"), "3723 s is '1:02:03' (got %s)", buf);
}

static void test_scan(void)
{
    HdePlaylist p;
    hde_playlist_init(&p);
    long n = hde_playlist_scan(&p, root, 0);
    CHECK(n == 7, "the folder holds the seven audio/video files (got %ld)", n);
    CHECK(p.n == 7, "and the list has them (%zu)", p.n);
    if (p.n == 7) {
        CHECK(!strcmp(hde_media_basename(p.items[0].path), "clip.mp4"), "sorted by name: clip.mp4 first (got %s)",
              hde_media_basename(p.items[0].path));
        CHECK(!strcmp(hde_media_basename(p.items[6].path), "utf16.mp3"), "utf16.mp3 last (got %s)",
              hde_media_basename(p.items[6].path));
        int notes = 0, hidden = 0, sub = 0;
        for (size_t i = 0; i < p.n; i++) {
            const char *b = hde_media_basename(p.items[i].path);
            if (!strcmp(b, "notes.txt")) notes = 1;
            if (!strcmp(b, ".hidden.mp3")) hidden = 1;
            if (!strcmp(b, "song9.mp3")) sub = 1;
        }
        CHECK(!notes, "the files that are not music or video are left out (notes.txt)");
        CHECK(!hidden, "the hidden files are left out (.hidden.mp3)");
        CHECK(!sub, "without -r the sub-folders are left out (sub/song9.mp3)");
    }
    hde_playlist_free(&p);

    hde_playlist_init(&p);
    n = hde_playlist_scan(&p, root, 1);
    CHECK(n == 8, "with the sub-folders there are eight (got %ld)", n);
    hde_playlist_free(&p);

    hde_playlist_init(&p);
    CHECK(hde_playlist_scan(&p, "/nonexistent/hde", 0) == -1, "a folder that cannot be read says so (-1)");
    hde_playlist_free(&p);
}

static void test_tags(void)
{
    HdeTrack t;
    memset(&t, 0, sizeof t);

    /* the ID3v2.3 tag, TLEN included */
    t.path = (char *)malloc(512);
    path_of(t.path, 512, "song1.mp3");
    CHECK(hde_media_read_tags(&t) == 0, "an MP3 with an ID3v2.3 tag can be read");
    CHECK(!strcmp(t.title, "First Song"), "TIT2 is the title (got %s)", t.title);
    CHECK(!strcmp(t.artist, "The Band"), "TPE1 is the artist (got %s)", t.artist);
    CHECK(!strcmp(t.album, "Album One"), "TALB is the album (got %s)", t.album);
    CHECK(t.track == 3, "TRCK is the track number (got %d)", t.track);
    CHECK(t.duration > 179.9 && t.duration < 180.1, "TLEN gives the length: 180 s (got %.1f)", t.duration);
    CHECK(t.kind == HDE_MEDIA_KIND_AUDIO, "and the file is known to be audio");
    free(t.path);

    /* a UTF-16 title */
    memset(&t, 0, sizeof t);
    t.path = (char *)malloc(512);
    path_of(t.path, 512, "utf16.mp3");
    CHECK(hde_media_read_tags(&t) == 0, "an MP3 whose title is UTF-16 can be read");
    CHECK(!strcmp(t.title, "Cà phê sáng"), "the UTF-16 title comes out as UTF-8 (got %s)", t.title);
    free(t.path);

    /* ID3v1 only, and a length estimated from the first frame */
    memset(&t, 0, sizeof t);
    t.path = (char *)malloc(512);
    path_of(t.path, 512, "song2.mp3");
    CHECK(hde_media_read_tags(&t) == 0, "an MP3 with only an ID3v1 tag can be read");
    CHECK(!strcmp(t.title, "Old Song"), "ID3v1 gives the title (got %s)", t.title);
    CHECK(!strcmp(t.artist, "Someone"), "ID3v1 gives the artist (got %s)", t.artist);
    CHECK(!strcmp(t.album, "Old Album"), "ID3v1 gives the album (got %s)", t.album);
    CHECK(t.track == 7, "ID3v1.1 gives the track number (got %d)", t.track);
    /* 512 bytes of audio + the 128 bytes of the tag, at 128 kbps: (640 * 8) / 128000 ≈ 0.04 s */
    CHECK(t.duration > 0.02 && t.duration < 0.08, "the length of an MP3 without TLEN comes from its first frame (got %.3f)",
          t.duration);
    free(t.path);

    /* an Ogg Vorbis comment header */
    memset(&t, 0, sizeof t);
    t.path = (char *)malloc(512);
    path_of(t.path, 512, "tune.ogg");
    CHECK(hde_media_read_tags(&t) == 0, "an Ogg Vorbis file can be read");
    CHECK(!strcmp(t.title, "Ogg Tune"), "the Vorbis comment gives the title (got %s)", t.title);
    CHECK(!strcmp(t.artist, "Someone"), "and the artist (got %s)", t.artist);
    free(t.path);

    /* a WAV: the byte rate and the size of the data chunk */
    memset(&t, 0, sizeof t);
    t.path = (char *)malloc(512);
    path_of(t.path, 512, "sound.wav");
    CHECK(hde_media_read_tags(&t) == 0, "a WAV can be read");
    CHECK(t.duration > 1.99 && t.duration < 2.01, "176400 bytes a second and 352800 bytes of audio: 2 s (got %.2f)", t.duration);
    free(t.path);

    /* a FLAC: STREAMINFO */
    memset(&t, 0, sizeof t);
    t.path = (char *)malloc(512);
    path_of(t.path, 512, "long.flac");
    CHECK(hde_media_read_tags(&t) == 0, "a FLAC can be read");
    CHECK(t.duration > 89.9 && t.duration < 90.1, "44100 Hz and 90 s of samples: 90 s (got %.2f)", t.duration);
    free(t.path);

    /* a file that is not there */
    memset(&t, 0, sizeof t);
    t.path = (char *)"/nonexistent/hde/no.mp3";
    CHECK(hde_media_read_tags(&t) == -1, "a file that cannot be opened says so (-1)");
}

static void test_playlist_files(void)
{
    HdePlaylist p;
    hde_playlist_init(&p);
    long n = hde_playlist_read(&p, "/dev/null");
    CHECK(n == -1, "a file that is not a playlist is not read as one (-1), got %ld", n);
    hde_playlist_free(&p);

    hde_playlist_init(&p);
    char m3u[512];
    path_of(m3u, sizeof m3u, "list.m3u");
    n = hde_playlist_read(&p, m3u);
    CHECK(n == 3, "the .m3u gives three entries (got %ld)", n);
    if (n == 3) {
        char want[512];
        path_of(want, sizeof want, "song1.mp3");
        CHECK(!strcmp(p.items[0].path, want), "the relative path is resolved against the folder of the playlist (%s)",
              p.items[0].path);
        CHECK(!strcmp(p.items[0].title, "From The Playlist"), "the #EXTINF title is used (got %s)", p.items[0].title);
        CHECK(p.items[0].duration > 122.9 && p.items[0].duration < 123.1, "the #EXTINF length too (got %.1f)",
              p.items[0].duration);
        CHECK(!strcmp(p.items[1].path, "/tmp/hde-player-test-absolute.mp3"), "an absolute path is kept as it is (%s)",
              p.items[1].path);
        CHECK(!strcmp(hde_media_basename(p.items[2].path), "tune.ogg"), "and the last one is tune.ogg");
    }
    hde_playlist_free(&p);

    /* what we write, we can read back */
    hde_playlist_init(&p);
    char m3u_out[512];
    path_of(m3u_out, sizeof m3u_out, "written.m3u");
    CHECK(hde_playlist_write(&p, root) != 0, "writing to a path that is a folder fails (got 0)");
    hde_playlist_free(&p);

    hde_playlist_init(&p);
    path_of(m3u, sizeof m3u, "list.m3u");
    hde_playlist_read(&p, m3u);
    path_of(m3u_out, sizeof m3u_out, "written.m3u");
    CHECK(hde_playlist_write(&p, m3u_out) == 0, "the list can be written as an extended .m3u");
    HdePlaylist back;
    hde_playlist_init(&back);
    CHECK(hde_playlist_read(&back, m3u_out) == 3, "and read back with the same number of entries");
    CHECK(back.n == p.n, "the same count (%zu)", back.n);
    hde_playlist_free(&back);
    hde_playlist_free(&p);

    /* .pls */
    hde_playlist_init(&p);
    char pls[512];
    path_of(pls, sizeof pls, "list.pls");
    n = hde_playlist_read(&p, pls);
    CHECK(n == 2, "the .pls gives two entries (got %ld)", n);
    if (n == 2) {
        CHECK(!strcmp(hde_media_basename(p.items[0].path), "sound.wav"), "File1 first (%s)", p.items[0].path);
        CHECK(!strcmp(p.items[0].title, "Pls Song"), "Title1 is used (got %s)", p.items[0].title);
        CHECK(p.items[0].duration > 1.9 && p.items[0].duration < 2.1, "Length1 is used (got %.1f)", p.items[0].duration);
        CHECK(!strcmp(hde_media_basename(p.items[1].path), "tune.ogg"), "File2 second (%s)", p.items[1].path);
    }
    hde_playlist_free(&p);
}

static void test_playback(void)
{
    HdePlayer pl;
    hde_player_init(&pl);
    hde_playlist_add(&pl.list, "/music/a.mp3");
    hde_playlist_add(&pl.list, "/music/b.mp3");
    hde_playlist_add(&pl.list, "/music/c.mp3");

    CHECK(pl.index == -1 && !pl.playing, "nothing plays before the first start");
    CHECK(hde_player_start(&pl, 0, 1000) == 0 && pl.playing && pl.started_ms == 1000, "start: the first track plays");
    CHECK(hde_player_next(&pl, 1, 2000) == 1, "Next: the second one");
    CHECK(hde_player_next(&pl, 1, 3000) == 2, "Next: the third one");
    CHECK(hde_player_next(&pl, 1, 4000) == 0, "Next at the end: the first one again (the keys wrap around)");
    CHECK(hde_player_next(&pl, -1, 5000) == 2, "Previous at the beginning: the last one (they wrap the other way)");
    CHECK(hde_player_next(&pl, -1, 6000) == 1, "Previous: the second one");

    /* the end of a track */
    hde_player_start(&pl, 2, 7000);
    pl.repeat = HDE_REPEAT_OFF;
    CHECK(hde_player_advance(&pl, 8000) == 0 && !pl.playing, "the end of the last track stops the playback (repeat off)");
    hde_player_start(&pl, 2, 9000);
    pl.repeat = HDE_REPEAT_ALL;
    CHECK(hde_player_advance(&pl, 10000) == 1 && pl.index == 0, "with repeat all it goes back to the first track");
    hde_player_start(&pl, 1, 11000);
    pl.repeat = HDE_REPEAT_ONE;
    CHECK(hde_player_advance(&pl, 12000) == 1 && pl.index == 1 && pl.started_ms == 12000,
          "with repeat one the same track starts again");
    hde_player_start(&pl, 0, 13000);
    pl.repeat = HDE_REPEAT_ALL;
    CHECK(hde_player_advance(&pl, 14000) == 1 && pl.index == 1, "in the middle of the list it is simply the next track");

    /* shuffle: the same seed, the same order; and never the same track twice in a row */
    pl.repeat = HDE_REPEAT_OFF;
    pl.shuffle = 1;
    pl.seed = 12345;
    hde_player_start(&pl, 0, 15000);
    long a = hde_player_next(&pl, 1, 16000);
    long b = hde_player_next(&pl, 1, 17000);
    long c = hde_player_next(&pl, 1, 18000);
    CHECK(a != 0 && b != a && c != b, "shuffle never plays the same track twice in a row (%ld %ld %ld)", a, b, c);
    CHECK(a >= 0 && a < 3 && b >= 0 && b < 3 && c >= 0 && c < 3, "and it always picks one of the list");
    pl.seed = 12345;
    hde_player_start(&pl, 0, 19000);
    CHECK(hde_player_next(&pl, 1, 20000) == a, "the same seed gives the same order (the test can check it)");
    pl.shuffle = 0;

    /* stop, and an empty list */
    hde_player_stop(&pl);
    CHECK(!pl.playing, "stop: nothing plays");
    HdePlayer empty;
    hde_player_init(&empty);
    CHECK(hde_player_start(&empty, 0, 1) == -1, "starting an empty playlist gives -1");
    CHECK(hde_player_next(&empty, 1, 1) == -1, "and Next too");
    hde_player_free(&empty);

    /* the volume */
    pl.muted = 1;
    CHECK(hde_player_set_volume(&pl, 1.5) == 1.0, "a volume above 100 %% is kept at 100 %%");
    CHECK(hde_player_set_volume(&pl, -1) == 0.0, "and below 0 %% at 0 %%");
    CHECK(hde_player_set_volume(&pl, 0.8) == 0.8, "80 %% is 80 %%");
    CHECK(hde_player_volume_step(&pl, 1) > 0.84 && hde_player_volume_step(&pl, 1) > 0.89, "the +/- keys step by 5 %%");
    for (int i = 0; i < 30; i++) hde_player_volume_step(&pl, 1);
    CHECK(pl.volume == 1.0, "step up all the way: exactly 100 %% (got %.2f)", pl.volume);
    for (int i = 0; i < 30; i++) hde_player_volume_step(&pl, -1);
    CHECK(pl.volume == 0.0, "step down all the way: exactly 0 %% (got %.2f)", pl.volume);
    hde_player_set_volume(&pl, 0.5);
    CHECK(!pl.muted, "turning the volume up takes the mute off");

    hde_player_free(&pl);
}

static void test_titles(void)
{
    HdePlaylist p;
    hde_playlist_init(&p);
    hde_playlist_add(&p, "/music/Track One.mp3");
    char buf[256];
    hde_playlist_now_playing(&p, 0, buf, sizeof buf);
    CHECK(!strcmp(buf, "Track One.mp3"), "without tags the file name is what plays (got %s)", buf);
    free(p.items[0].title);
    p.items[0].title = hde_media_strdup("A Song");
    p.items[0].artist = hde_media_strdup("The Band");
    hde_playlist_now_playing(&p, 0, buf, sizeof buf);
    CHECK(!strcmp(buf, "The Band — A Song"), "with tags it is 'Artist — Title' (got %s)", buf);
    hde_playlist_now_playing(&p, 7, buf, sizeof buf);
    CHECK(!strcmp(buf, "nothing playing"), "and an index that is not in the list says so (got %s)", buf);

    HdePlaylist copy;
    CHECK(hde_playlist_copy(&copy, &p) == 0, "a playlist can be copied");
    CHECK(copy.n == 1 && !strcmp(copy.items[0].title, "A Song") && !strcmp(copy.items[0].artist, "The Band"),
          "the copy has the tags too (%s — %s)", copy.items[0].artist, copy.items[0].title);
    hde_playlist_free(&copy);
    hde_playlist_free(&p);
}

int main(void)
{
    snprintf(root, sizeof root, "/tmp/hde-player-test-%d", (int)getpid());
    char cmd[512];
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    if (system(cmd) != 0) { /* nothing to clean */ }
    snprintf(cmd, sizeof cmd, "mkdir -p %s/sub", root);
    if (system(cmd) != 0) { printf("FAIL: player: cannot create %s\n", root); return 1; }

    write_mp3("song1.mp3", "First Song", "The Band", "Album One", "3", 180.0);
    write_raw_mp3("song2.mp3", "Old Song", "Someone", "Old Album", 7);

    unsigned char frames[256];
    size_t n = 0;
    /* "Cà phê sáng" in UTF-16LE, one code unit each */
    static const unsigned short units[] = { 'C', 0x00E0, ' ', 'p', 'h', 0x00EA, ' ', 's', 0x00E1, 'n', 'g' };
    n += id3_frame_utf16(frames + n, "TIT2", units, sizeof units / sizeof units[0]);
    unsigned char tag[512];
    size_t taglen = id3v2_tag(tag, 4, frames, n);
    write_bytes("utf16.mp3", tag, taglen);

    write_ogg("tune.ogg", "Ogg Tune", "Someone");
    write_wav("sound.wav", 176400, 352800);              /* 2 s of CD-quality stereo */
    write_flac("long.flac", 44100, 44100ULL * 90);       /* 90 s */
    write_bytes("clip.mp4", "video", 5);
    write_bytes("notes.txt", "not music\n", 10);
    write_bytes(".hidden.mp3", "hidden\n", 7);
    write_bytes("sub/song9.mp3", "nine\n", 5);

    static const char *m3u_text =
        "#EXTM3U\n"
        "# a comment\n"
        "#EXTINF:123,From The Playlist\n"
        "song1.mp3\n"
        "/tmp/hde-player-test-absolute.mp3\n"
        "tune.ogg\n";
    write_bytes("list.m3u", m3u_text, strlen(m3u_text));

    static const char *pls_text =
        "[playlist]\n"
        "File2=tune.ogg\n"
        "Title1=Pls Song\n"
        "File1=sound.wav\n"
        "Length1=2\n"
        "NumberOfEntries=2\n"
        "Version=2\n";
    write_bytes("list.pls", pls_text, strlen(pls_text));

    test_kinds();
    test_time();
    test_scan();
    test_tags();
    test_playlist_files();
    test_playback();
    test_titles();

    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    if (system(cmd) != 0) { /* nothing to clean */ }

    printf("\nplayer-test: %d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
