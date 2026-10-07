/* playlist.c — hde-media: the list of what is played (music and video), the tags read from the files themselves, and
 * the state of the playback: plain C + POSIX, no GTK and no glib, because all of it is tested without a display by
 * tests/player-test.c (`make check-unit`).
 *
 * Why the tags are read here and not by a library: this is the "light part" of the hybrid engine of HDE (our own code
 * for what is small — the picture viewer, the tags, m3u/pls — and gstreamer / mpv / ffplay for what is heavy: the
 * decoding). A tag reader of a few hundred lines of C shows the title of every file of a folder without pulling in a
 * dependency, and the tests can build it on any machine.
 *
 * The formats read here, by hand:
 *   ID3v2.2/2.3/2.4   at the start of an MP3: TIT2/TPE1/TALB/TRCK/TLEN (TT2/TP1/TAL/TRK in 2.2), the four text
 *                     encodings (latin-1, UTF-16 with a BOM, UTF-16BE, UTF-8), unsynchronisation undone when the
 *                     header says so
 *   ID3v1             the last 128 bytes of an MP3 ("TAG"), including the track number in the comment field
 *   Vorbis comment    in an Ogg Vorbis / Opus / FLAC file: TITLE, ARTIST, ALBUM, TRACKNUMBER
 *   STREAMINFO        in a FLAC: the sample rate and the number of samples → how long it is
 *   fmt / data        in a WAV: the byte rate and the size of the audio → how long it is
 *   the first frame   of an MP3 without a TLEN frame: the bitrate → an estimate of how long it is
 */
#define _POSIX_C_SOURCE 200809L    /* opendir/readdir/stat/strcasecmp: HDE is built with -std=c11, POSIX is not implied */

#include "player.h"
#include "media.h"                 /* the shared helpers: hde_media_strdup, hde_media_extension, ... */

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/* ------------------------------------------------------------------ what is audio and what is video
 * gstreamer / mpv decide in the end (this list only says "this file could be played", so a folder listing can be
 * filtered without opening every file, and so the window knows whether to show a video or a speaker). */
static const char *const AUDIO[] = {
    "mp3", "mp2", "m4a", "m4b", "aac", "ogg", "oga", "opus", "flac", "wav", "wave", "wma", "mka", "spx", "ape",
    "wv", "mpc", "aiff", "aif", "aifc", "au", "snd", "mid", "midi", "mod", "xm", "s3m", "it", "ra", "amr", "ac3",
    "dts", "caf", "voc", "gsm", "mus",
    NULL
};
static const char *const VIDEO[] = {
    "mp4", "m4v", "mkv", "webm", "avi", "mov", "qt", "mpg", "mpeg", "mpe", "m2v", "ts", "mts", "m2ts", "vob",
    "ogv", "ogg", "wmv", "asf", "flv", "f4v", "3gp", "3g2", "divx", "rm", "rmvb", "y4m", "nut", "dv", "mxf",
    NULL
};
static const char *const PLAYLISTS[] = { "m3u", "m3u8", "pls", NULL };

static int in_list(const char *const *list, const char *ext)
{
    if (!ext || !*ext) return 0;
    for (int i = 0; list[i]; i++)
        if (!strcasecmp(list[i], ext)) return 1;
    return 0;
}

HdeMediaKind hde_media_kind_of(const char *name)
{
    const char *ext = hde_media_extension(name);
    if (in_list(AUDIO, ext)) return HDE_MEDIA_KIND_AUDIO;
    if (in_list(VIDEO, ext)) return HDE_MEDIA_KIND_VIDEO;
    return HDE_MEDIA_KIND_UNKNOWN;
}

int hde_media_is_media(const char *name) { return hde_media_kind_of(name) != HDE_MEDIA_KIND_UNKNOWN; }
int hde_media_is_playlist(const char *name) { return in_list(PLAYLISTS, hde_media_extension(name)); }

const char *hde_media_kind_name(HdeMediaKind kind)
{
    switch (kind) {
    case HDE_MEDIA_KIND_AUDIO: return "audio";
    case HDE_MEDIA_KIND_VIDEO: return "video";
    default: return "file";
    }
}

/* ------------------------------------------------------------------ the list */

void hde_playlist_init(HdePlaylist *p) { memset(p, 0, sizeof *p); }

void hde_playlist_clear(HdePlaylist *p)
{
    for (size_t i = 0; i < p->n; i++) {
        free(p->items[i].path);
        free(p->items[i].title);
        free(p->items[i].artist);
        free(p->items[i].album);
    }
    p->n = 0;
}

void hde_playlist_free(HdePlaylist *p)
{
    if (!p) return;
    hde_playlist_clear(p);
    free(p->items);
    p->items = NULL;
    p->cap = 0;
}

long hde_playlist_add(HdePlaylist *p, const char *path)
{
    if (!path || !*path) return -1;
    if (p->n == p->cap) {
        size_t cap = p->cap ? p->cap * 2 : 16;
        HdeTrack *items = realloc(p->items, cap * sizeof *items);
        if (!items) return -1;
        p->items = items;
        p->cap = cap;
    }
    HdeTrack *t = &p->items[p->n];
    memset(t, 0, sizeof *t);
    t->path = hde_media_strdup(path);
    t->title = hde_media_strdup(hde_media_basename(path));
    t->artist = hde_media_strdup("");
    t->album = hde_media_strdup("");
    if (!t->path || !t->title || !t->artist || !t->album) {     /* leave the list as it was */
        free(t->path); free(t->title); free(t->artist); free(t->album);
        memset(t, 0, sizeof *t);
        return -1;
    }
    t->kind = hde_media_kind_of(path);
    return (long)p->n++;
}

long hde_playlist_find(const HdePlaylist *p, const char *path)
{
    if (!p || !path) return -1;
    for (size_t i = 0; i < p->n; i++)
        if (!strcmp(p->items[i].path, path)) return (long)i;
    return -1;
}

int hde_playlist_copy(HdePlaylist *dst, const HdePlaylist *src)
{
    hde_playlist_init(dst);
    for (size_t i = 0; i < src->n; i++) {
        long at = hde_playlist_add(dst, src->items[i].path);
        if (at < 0) { hde_playlist_free(dst); return -1; }
        HdeTrack *d = &dst->items[at], *s = &src->items[i];
        free(d->title);  d->title  = hde_media_strdup(s->title);
        free(d->artist); d->artist = hde_media_strdup(s->artist);
        free(d->album);  d->album  = hde_media_strdup(s->album);
        if (!d->title || !d->artist || !d->album) {
            free(d->title); free(d->artist); free(d->album);
            d->title = hde_media_strdup(""); d->artist = hde_media_strdup(""); d->album = hde_media_strdup("");
        }
        d->track = s->track;
        d->duration = s->duration;
        d->kind = s->kind;
    }
    return 0;
}

/* the order of a folder: by name, the natural one (track2 before track10) */
static int track_cmp(const void *a, const void *b)
{
    const HdeTrack *x = a, *y = b;
    return hde_media_natural_cmp(x->path, y->path);
}

/* the sub-folders of a folder, downloaded into it; depth is how deep it may still go (0 = this folder only) */
static long scan_into(HdePlaylist *p, const char *dir, int depth, int *unreadable)
{
    DIR *d = opendir(dir);
    if (!d) { *unreadable = 1; return 0; }

    long added = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char *path = malloc(strlen(dir) + strlen(e->d_name) + 2);
        if (!path) break;
        sprintf(path, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                if (depth > 0) added += scan_into(p, path, depth - 1, unreadable);
            } else if (S_ISREG(st.st_mode) && hde_media_is_media(path)) {
                if (hde_playlist_add(p, path) >= 0) added++;
            }
        }
        free(path);
    }
    closedir(d);
    return added;
}

long hde_playlist_scan(HdePlaylist *p, const char *dir, int recursive)
{
    if (!dir) return -1;
    size_t before = p->n;
    int unreadable = 0;
    if (recursive) {
        long added = scan_into(p, dir, 8, &unreadable);
        if (unreadable && added == 0) return -1;
    } else {
        DIR *d = opendir(dir);
        if (!d) return -1;
        closedir(d);
        long added = scan_into(p, dir, 0, &unreadable);
        if (unreadable && added == 0) return -1;
    }
    if (p->n > before) qsort(p->items + before, p->n - before, sizeof *p->items, track_cmp);
    return (long)(p->n - before);
}

/* ------------------------------------------------------------------ strings and text encodings */

/* a string being built: the tag readers turn latin-1 / UTF-16 into UTF-8, and the tests check what comes out */
typedef struct {
    char  *s;
    size_t n, cap;
} SB;

static void sb_init(SB *b) { b->s = NULL; b->n = 0; b->cap = 0; }

static int sb_reserve(SB *b, size_t extra)
{
    if (b->n + extra + 1 <= b->cap) return 0;
    size_t cap = b->cap ? b->cap * 2 : 32;
    while (cap < b->n + extra + 1) cap *= 2;
    char *s = realloc(b->s, cap);
    if (!s) return -1;
    b->s = s;
    b->cap = cap;
    return 0;
}

static int sb_put_cp(SB *b, unsigned cp)
{
    if (sb_reserve(b, 4) != 0) return -1;
    char *o = b->s + b->n;
    if (cp < 0x80) {
        *o++ = (char)cp;
    } else if (cp < 0x800) {
        *o++ = (char)(0xC0 | (cp >> 6));
        *o++ = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        *o++ = (char)(0xE0 | (cp >> 12));
        *o++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *o++ = (char)(0x80 | (cp & 0x3F));
    } else {
        *o++ = (char)(0xF0 | (cp >> 18));
        *o++ = (char)(0x80 | ((cp >> 12) & 0x3F));
        *o++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *o++ = (char)(0x80 | (cp & 0x3F));
    }
    b->n = (size_t)(o - b->s);
    b->s[b->n] = 0;
    return 0;
}

static int sb_put_bytes(SB *b, const void *data, size_t len)
{
    if (sb_reserve(b, len) != 0) return -1;
    memcpy(b->s + b->n, data, len);
    b->n += len;
    b->s[b->n] = 0;
    return 0;
}

/* what the string became: never NULL, and never something the caller has to check for emptiness */
static char *sb_take(SB *b)
{
    char *s = b->s;
    b->s = NULL;
    b->n = b->cap = 0;
    return s ? s : hde_media_strdup("");
}

static void sb_free(SB *b) { free(b->s); sb_init(b); }

/* "  Artist  " → "Artist": tags are padded, and the spaces belong to the field */
static char *trim_tag(char *s)
{
    if (!s) return NULL;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/* one text frame of an ID3v2 tag: the first byte says how the rest is encoded */
static char *id3_text(const unsigned char *p, size_t n)
{
    SB b;
    sb_init(&b);
    if (n == 0) return sb_take(&b);
    unsigned enc = p[0];
    p++;
    n--;

    if (enc == 0 || enc == 3) {                    /* ISO-8859-1 or UTF-8 */
        for (size_t i = 0; i < n && p[i]; i++)
            if (sb_put_cp(&b, p[i]) != 0) break;
    } else {                                        /* UTF-16 with a BOM (1) or UTF-16BE (2) */
        int be = (enc == 2);
        size_t i = 0;
        if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) { be = 0; i = 2; }
        else if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) { be = 1; i = 2; }
        for (; i + 1 < n; i += 2) {
            unsigned u = be ? ((unsigned)p[i] << 8 | p[i + 1]) : ((unsigned)p[i + 1] << 8 | p[i]);
            if (u == 0) break;
            if (u >= 0xD800 && u <= 0xDBFF && i + 3 < n) {     /* a surrogate pair */
                unsigned lo = be ? ((unsigned)p[i + 2] << 8 | p[i + 3]) : ((unsigned)p[i + 3] << 8 | p[i + 2]);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    if (sb_put_cp(&b, 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00)) != 0) break;
                    i += 2;
                    continue;
                }
            }
            if (sb_put_cp(&b, u) != 0) break;
        }
    }
    return sb_take(&b);
}

static size_t read_be32(const unsigned char *p) { return ((size_t)p[0] << 24) | ((size_t)p[1] << 16) | ((size_t)p[2] << 8) | p[3]; }
static size_t read_synchsafe(const unsigned char *p) { return ((size_t)(p[0] & 0x7F) << 21) | ((size_t)(p[1] & 0x7F) << 14) | ((size_t)(p[2] & 0x7F) << 7) | (p[3] & 0x7F); }
static unsigned read_le32u(const unsigned char *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24); }

/* which field a frame holds (0 = one we do not read) */
static int id3_field(const char *id)
{
    if (!strcmp(id, "TIT2") || !strcmp(id, "TT2")) return 1;      /* title */
    if (!strcmp(id, "TPE1") || !strcmp(id, "TP1")) return 2;      /* artist */
    if (!strcmp(id, "TALB") || !strcmp(id, "TAL")) return 3;      /* album */
    if (!strcmp(id, "TRCK") || !strcmp(id, "TRK")) return 4;      /* track number */
    if (!strcmp(id, "TLEN") || !strcmp(id, "TLE")) return 5;      /* length, in milliseconds */
    return 0;
}

/* the whole ID3v2 tag at the start of the buffer; returns how many bytes it takes (0 when there is none) */
static size_t id3v2_tags(const unsigned char *d, size_t n, HdeTrack *t, int *track, double *len)
{
    if (n < 10 || memcmp(d, "ID3", 3) != 0) return 0;
    int major = d[3];
    size_t size = read_synchsafe(d + 6);
    if (size == 0 || 10 + size > n) size = n > 10 ? n - 10 : 0;   /* a truncated file: read what there is */

    unsigned char *body = NULL;
    const unsigned char *p = d + 10;
    size_t pn = size;
    if (d[5] & 0x80) {                       /* unsynchronisation: every 0xFF 0x00 was a 0xFF */
        body = malloc(size ? size : 1);
        if (!body) return 10 + size;
        size_t o = 0;
        for (size_t i = 0; i < size; i++) {
            body[o++] = p[i];
            if (p[i] == 0xFF && i + 1 < size && p[i + 1] == 0x00) i++;
        }
        p = body;
        pn = o;
    }
    if ((major == 3 || major == 4) && (d[5] & 0x40) && pn >= 4) {    /* extended header: skip it */
        size_t x = major == 4 ? read_synchsafe(p) : read_be32(p);
        if (x + 4 <= pn) { p += 4 + x; pn -= 4 + x; }
    }

    size_t idsz = major == 2 ? 3 : 4;
    size_t hdr = major == 2 ? 3 : 6;
    size_t i = 0;
    while (i + idsz + hdr <= pn) {
        char id[5] = {0};
        memcpy(id, p + i, idsz);
        if (!isupper((unsigned char)id[0])) break;      /* padding, or the end of the frames */
        size_t fsz;
        if (major == 2) fsz = ((size_t)p[i + 3] << 16) | ((size_t)p[i + 4] << 8) | p[i + 5];
        else if (major == 4) fsz = read_synchsafe(p + i + 4);
        else fsz = read_be32(p + i + 4);
        size_t body_off = i + idsz + hdr;
        if (fsz == 0 || body_off + fsz > pn) break;

        int what = id3_field(id);
        if (what >= 1 && what <= 5) {
            char *text = trim_tag(id3_text(p + body_off, fsz));
            if (text && *text) {
                switch (what) {
                case 1: free(t->title);  t->title  = hde_media_strdup(text); break;
                case 2: free(t->artist); t->artist = hde_media_strdup(text); break;
                case 3: free(t->album);  t->album  = hde_media_strdup(text); break;
                case 4: *track = atoi(text); break;
                case 5: *len = atof(text) / 1000.0; break;   /* TLEN is in milliseconds */
                default: break;
                }
            }
            free(text);
        }
        i = body_off + fsz;
    }
    free(body);
    return 10 + size;
}

/* ID3v1: the last 128 bytes of an MP3 */
static void id3v1_tags(const unsigned char *d, size_t n, HdeTrack *t, int *track)
{
    if (n < 128 || memcmp(d + n - 128, "TAG", 3) != 0) return;
    const unsigned char *p = d + n - 128 + 3;
    char field[31];
    const int off[3] = { 0, 30, 60 };
    char **dst[3] = { &t->title, &t->artist, &t->album };
    for (int i = 0; i < 3; i++) {
        memcpy(field, p + off[i], 30);
        field[30] = 0;
        char *s = trim_tag(field);
        if (*s) { free(*dst[i]); *dst[i] = hde_media_strdup(s); }
    }
    if (p[94 + 28] == 0 && p[94 + 29] > 0) *track = p[94 + 29];   /* ID3v1.1: the track, in the last bytes of the comment */
}

/* the Vorbis comment of an Ogg Vorbis / Opus / FLAC file: "KEY=value", keys in any case */
static void vorbis_comments(const unsigned char *p, size_t n, HdeTrack *t, int *track)
{
    if (n < 8) return;
    unsigned vendor = read_le32u(p);
    if (4 + (size_t)vendor + 4 > n) return;
    size_t at = 4 + (size_t)vendor;
    unsigned count = read_le32u(p + at);
    at += 4;
    for (unsigned i = 0; i < count && at + 4 <= n; i++) {
        unsigned len = read_le32u(p + at);
        at += 4;
        if ((size_t)len > n - at) break;
        const char *kv = (const char *)(p + at);
        const char *eq = memchr(kv, '=', len);
        if (eq) {
            size_t klen = (size_t)(eq - kv);
            char key[32];
            if (klen > 0 && klen < sizeof key) {
                size_t j = 0;
                for (; j < klen; j++) key[j] = (char)toupper((unsigned char)kv[j]);
                key[j] = 0;
                size_t vlen = len - klen - 1;
                SB b;
                sb_init(&b);
                if (vlen > 1024) vlen = 1024;              /* a comment is never shown in full: keep it sane */
                sb_put_bytes(&b, eq + 1, vlen);            /* the values are UTF-8 already */
                char *v = trim_tag(sb_take(&b));
                if (*v) {
                    if (!strcmp(key, "TITLE")) { free(t->title); t->title = hde_media_strdup(v); }
                    else if (!strcmp(key, "ARTIST")) { free(t->artist); t->artist = hde_media_strdup(v); }
                    else if (!strcmp(key, "ALBUM")) { free(t->album); t->album = hde_media_strdup(v); }
                    else if (!strcmp(key, "TRACKNUMBER")) *track = atoi(v);
                }
                free(v);
                sb_free(&b);
            }
        }
        at += len;
    }
}

/* FLAC: the metadata blocks at the start (STREAMINFO for the length, VORBIS_COMMENT for the tags) */
static void flac_tags(const unsigned char *d, size_t n, HdeTrack *t, int *track)
{
    size_t i = 4;                                  /* "fLaC" */
    while (i + 4 <= n) {
        int last = d[i] & 0x80;
        int type = d[i] & 0x7F;
        size_t len = ((size_t)d[i + 1] << 16) | ((size_t)d[i + 2] << 8) | d[i + 3];
        const unsigned char *b = d + i + 4;
        if (i + 4 + len > n) break;
        if (type == 0 && len >= 34) {              /* STREAMINFO */
            unsigned rate = ((unsigned)b[10] << 12) | ((unsigned)b[11] << 4) | (b[12] >> 4);
            unsigned long long samples = ((unsigned long long)(b[13] & 0x0F) << 32) |
                                         ((unsigned long long)b[14] << 24) | ((unsigned long long)b[15] << 16) |
                                         ((unsigned long long)b[16] << 8) | b[17];
            if (rate > 0 && samples > 0) t->duration = (double)samples / (double)rate;
        } else if (type == 4) {                    /* VORBIS_COMMENT */
            vorbis_comments(b, len, t, track);
        }
        if (last) break;
        i += 4 + len;
    }
}

/* WAV: the fmt chunk says how many bytes a second, the data chunk how many bytes */
static void wav_tags(const unsigned char *d, size_t n, HdeTrack *t)
{
    unsigned byte_rate = 0;
    unsigned long long data = 0;
    size_t i = 12;                                 /* "RIFF" ... "WAVE" */
    while (i + 8 <= n) {
        unsigned len = read_le32u(d + i + 4);
        if (!memcmp(d + i, "fmt ", 4) && len >= 12 && i + 16 <= n) byte_rate = read_le32u(d + i + 16);
        else if (!memcmp(d + i, "data", 4)) data = len;
        if (len == 0) break;
        i += 8 + (size_t)len + (len & 1);
    }
    if (byte_rate > 0 && data > 0) t->duration = (double)data / (double)byte_rate;
}

/* an MP3 without a TLEN frame: the first frame header says the bitrate. `audio` is how many bytes of audio there are
 * (the whole file minus its tags): a good estimate for the constant bitrate files that are most of a music folder,
 * better than showing 0:00 for everything. */
static double mp3_estimate(const unsigned char *d, size_t n, unsigned long long audio)
{
    static const int BR_V1L1[16] = { 0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0 };
    static const int BR_V1L2[16] = { 0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0 };
    static const int BR_V1L3[16] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 };
    static const int BR_V2L1[16] = { 0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0 };
    static const int BR_V2L23[16] = { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 };

    for (size_t i = 0; i + 4 <= n; i++) {
        if (d[i] != 0xFF || (d[i + 1] & 0xE0) != 0xE0) continue;
        int version = (d[i + 1] >> 3) & 0x03;      /* 3 = MPEG1, 2 = MPEG2, 0 = MPEG2.5, 1 = reserved */
        int layer = (d[i + 1] >> 1) & 0x03;        /* 3 = layer 1, 2 = layer 2, 1 = layer 3 */
        int bri = (d[i + 2] >> 4) & 0x0F;
        int sri = (d[i + 2] >> 2) & 0x03;
        if (version == 1 || layer == 0 || bri == 0 || bri == 15 || sri == 3) continue;
        const int *table = version == 3 ? (layer == 3 ? BR_V1L1 : (layer == 2 ? BR_V1L2 : BR_V1L3))
                                        : (layer == 3 ? BR_V2L1 : BR_V2L23);
        int kbps = table[bri];
        if (kbps <= 0) continue;
        return (double)audio * 8.0 / (kbps * 1000.0);
    }
    return 0;
}

int hde_media_read_tags(HdeTrack *t)
{
    if (!t || !t->path) return -1;
    FILE *f = fopen(t->path, "rb");
    if (!f) return -1;

    struct stat st;
    unsigned long long size = (fstat(fileno(f), &st) == 0 && st.st_size > 0) ? (unsigned long long)st.st_size : 0;
    size_t head_len = size < 65536 ? (size_t)size : 65536;
    unsigned char *head = malloc(head_len ? head_len : 1);
    if (!head) { fclose(f); return -1; }
    head_len = fread(head, 1, head_len, f);

    int track = 0;
    double len = 0;

    if (head_len >= 3 && !memcmp(head, "ID3", 3)) {
        size_t tag = id3v2_tags(head, head_len, t, &track, &len);
        if (len > 0) t->duration = len;
        else if (size > (unsigned long long)tag) {
            /* the length of an MP3 without a TLEN frame: an estimate from its first frame */
            size_t more = (size_t)(size - tag < 4096 ? size - tag : 4096);
            unsigned char *buf = malloc(more ? more : 1);
            if (buf) {
                if (fseek(f, (long)tag, SEEK_SET) == 0 && fread(buf, 1, more, f) == more)
                    t->duration = mp3_estimate(buf, more, size - tag);
                free(buf);
            }
        }
    } else if (head_len >= 4 && !memcmp(head, "OggS", 4)) {
        /* the comment header of a Vorbis or an Opus stream: "vorbis" then the comments, or "OpusTags" */
        for (size_t i = 0; i + 8 < head_len; i++) {
            /* the comment header of a Vorbis stream is the packet 0x03 followed by "vorbis"; 0x01 is the
             * identification header, which has no comments in it */
            if (head[i] == 0x03 && !memcmp(head + i + 1, "vorbis", 6)) { vorbis_comments(head + i + 7, head_len - i - 7, t, &track); break; }
            if (!memcmp(head + i, "OpusTags", 8)) { vorbis_comments(head + i + 8, head_len - i - 8, t, &track); break; }
        }
    } else if (head_len >= 4 && !memcmp(head, "fLaC", 4)) {
        flac_tags(head, head_len, t, &track);
    } else if (head_len >= 12 && !memcmp(head, "RIFF", 4) && !memcmp(head + 8, "WAVE", 4)) {
        wav_tags(head, head_len, t);
    } else if (!strcasecmp(hde_media_extension(t->path), "mp3")) {
        t->duration = mp3_estimate(head, head_len, size);
    }

    /* an MP3 may carry ID3v1 instead of (or as well as) ID3v2: the last 128 bytes hold the same fields */
    if (size >= 128 && fseek(f, (long)(size - 128), SEEK_SET) == 0) {
        unsigned char tail[128];
        if (fread(tail, 1, sizeof tail, f) == sizeof tail) id3v1_tags(tail, sizeof tail, t, &track);
    }
    fclose(f);
    free(head);

    if (track > 0) t->track = track;
    if (!t->kind) t->kind = hde_media_kind_of(t->path);
    /* what player.h promises: the three strings are there even when the file has no tags at all */
    if (!t->title) t->title = hde_media_strdup(hde_media_basename(t->path));
    if (!t->artist) t->artist = hde_media_strdup("");
    if (!t->album) t->album = hde_media_strdup("");
    return 0;
}

/* ------------------------------------------------------------------ what is playing, and the time */

size_t hde_media_format_time(char *buf, size_t n, double seconds)
{
    if (!buf || n == 0) return 0;
    if (seconds < 0) { snprintf(buf, n, "-:--"); return strlen(buf); }
    long s = (long)(seconds + 0.5);
    long h = s / 3600, m = (s % 3600) / 60, sec = s % 60;
    if (h > 0) snprintf(buf, n, "%ld:%02ld:%02ld", h, m, sec);
    else snprintf(buf, n, "%ld:%02ld", m, sec);
    return strlen(buf);
}

size_t hde_playlist_now_playing(const HdePlaylist *p, long index, char *buf, size_t n)
{
    if (!buf || n == 0) return 0;
    buf[0] = 0;
    if (!p || index < 0 || (size_t)index >= p->n) { snprintf(buf, n, "%s", "nothing playing"); return strlen(buf); }
    const HdeTrack *t = &p->items[index];
    const char *title = (t->title && *t->title) ? t->title : hde_media_basename(t->path);
    if (t->artist && *t->artist) snprintf(buf, n, "%s — %s", t->artist, title);
    else snprintf(buf, n, "%s", title);
    return strlen(buf);
}

/* ------------------------------------------------------------------ playlist files (.m3u, .m3u8, .pls) */

static char *resolve_from(const char *base_dir, const char *entry)
{
    if (entry[0] == '~' && entry[1] == '/') {
        const char *home = getenv("HOME");
        if (home && *home) {
            char *out = malloc(strlen(home) + strlen(entry));
            if (out) sprintf(out, "%s%s", home, entry + 1);
            return out;
        }
    }
    if (entry[0] == '/' || entry[0] == '~') return hde_media_strdup(entry);
    if (entry[0] == '.' && entry[1] == '/') entry += 2;
    size_t bl = strlen(base_dir), el = strlen(entry);
    char *out = malloc(bl + el + 2);
    if (out) sprintf(out, "%s/%s", base_dir, entry);
    return out;
}

static void dir_of(const char *file, char *buf, size_t n)
{
    const char *slash = strrchr(file, '/');
    if (!slash) { snprintf(buf, n, "."); return; }
    size_t len = (size_t)(slash - file);
    if (len == 0) len = 1;
    if (len >= n) len = n - 1;
    memcpy(buf, file, len);
    buf[len] = 0;
}

/* how deep a playlist may point at another playlist (a file that lists itself would otherwise never end) */
static int playlist_depth = 0;

long hde_playlist_read(HdePlaylist *p, const char *file)
{
    if (!file || !hde_media_is_playlist(file)) return -1;
    if (playlist_depth > 4) return -1;
    FILE *f = fopen(file, "rb");
    if (!f) return -2;

    playlist_depth++;
    char dir[4096];
    dir_of(file, dir, sizeof dir);
    long added = 0;
    char line[4096];
    int is_pls = !strcasecmp(hde_media_extension(file), "pls");

    /* an #EXTINF line of an extended .m3u applies to the path that follows it */
    double pending_len = 0;
    char  *pending_title = NULL;
    /* .pls: FileN=, TitleN= and LengthN= may come in any order, so they are collected and then added in order */
    struct { char *path; char *title; double len; } pls[512];
    int n_pls = 0;
    memset(pls, 0, sizeof pls);

    while (fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) continue;

        if (is_pls) {
            if (*s == '[') continue;                                  /* [playlist] */
            char *eq = strchr(s, '=');
            if (!eq) continue;
            *eq = 0;
            char *key = s, *val = eq + 1;
            static const char *const keys[] = { "File", "Title", "Length" };
            int kind = -1, n = 0;
            for (int k = 0; k < 3; k++) {
                size_t kl = strlen(keys[k]);
                if (!strncasecmp(key, keys[k], kl) && isdigit((unsigned char)key[kl])) { kind = k; n = atoi(key + kl); break; }
            }
            if (kind >= 0 && n > 0 && n <= 512) {          /* .pls entries are numbered from 1 */
                if (kind == 0) { free(pls[n - 1].path);  pls[n - 1].path  = hde_media_strdup(val); }
                else if (kind == 1) { free(pls[n - 1].title); pls[n - 1].title = hde_media_strdup(val); }
                else pls[n - 1].len = (double)atoi(val);
                if (n > n_pls) n_pls = n;
            }
            continue;
        }

        if (s[0] == '#') {
            if (!strncasecmp(s, "#EXTINF:", 8)) {
                char *rest = s + 8;
                pending_len = atof(rest);
                char *comma = strchr(rest, ',');
                free(pending_title);
                pending_title = comma && comma[1] ? hde_media_strdup(comma + 1) : NULL;
            }
            continue;
        }
        if (hde_media_is_playlist(s) && playlist_depth <= 4) {          /* a playlist inside a playlist */
            char *sub = resolve_from(dir, s);
            if (sub) {
                long n = hde_playlist_read(p, sub);
                if (n > 0) added += n;
                free(sub);
            }
            continue;
        }
        char *path = resolve_from(dir, s);
        if (!path) continue;
        long at = hde_playlist_add(p, path);
        if (at >= 0) {
            added++;
            if (pending_len > 0) p->items[at].duration = pending_len;
            if (pending_title && *pending_title) {
                free(p->items[at].title);
                p->items[at].title = hde_media_strdup(pending_title);
            }
        }
        free(path);
        pending_len = 0;
        free(pending_title);
        pending_title = NULL;
    }
    free(pending_title);

    for (int i = 0; i < n_pls; i++) {
        if (!pls[i].path) { free(pls[i].title); continue; }
        char *path = resolve_from(dir, pls[i].path);
        if (path && !hde_media_is_playlist(path)) {
            long at = hde_playlist_add(p, path);
            if (at >= 0) {
                added++;
                if (pls[i].len > 0) p->items[at].duration = pls[i].len;
                if (pls[i].title && *pls[i].title) {
                    free(p->items[at].title);
                    p->items[at].title = hde_media_strdup(pls[i].title);
                }
            }
        }
        free(path);
        free(pls[i].path);
        free(pls[i].title);
    }
    fclose(f);
    playlist_depth--;
    return added;
}

int hde_playlist_write(const HdePlaylist *p, const char *file)
{
    FILE *f = fopen(file, "wb");
    if (!f) return -1;
    fputs("#EXTM3U\n", f);
    for (size_t i = 0; i < p->n; i++) {
        const HdeTrack *t = &p->items[i];
        const char *title = (t->title && *t->title) ? t->title : hde_media_basename(t->path);
        if (t->artist && *t->artist) fprintf(f, "#EXTINF:%d,%s — %s\n", (int)(t->duration + 0.5), t->artist, title);
        else fprintf(f, "#EXTINF:%d,%s\n", (int)(t->duration + 0.5), title);
        fprintf(f, "%s\n", t->path);
    }
    fclose(f);
    return 0;
}

/* ------------------------------------------------------------------ the state of the playback */

void hde_player_init(HdePlayer *pl)
{
    memset(pl, 0, sizeof *pl);
    pl->index = -1;
    pl->repeat = HDE_REPEAT_OFF;
    pl->volume = 0.8;                 /* what most players start with: loud, but not the loudest */
    pl->seed = 0x9e3779b9u;
}

void hde_player_free(HdePlayer *pl)
{
    if (!pl) return;
    hde_playlist_free(&pl->list);
}

long hde_player_start(HdePlayer *pl, long index, int64_t now_ms)
{
    if (pl->list.n == 0) { pl->index = -1; pl->playing = 0; return -1; }
    if (index < 0) index = 0;
    if ((size_t)index >= pl->list.n) index = (long)pl->list.n - 1;
    pl->index = index;
    pl->playing = 1;
    pl->paused = 0;
    pl->started_ms = now_ms;
    return index;
}

/* the shuffle: xorshift32 — the same seed gives the same order, which is what makes it testable */
static uint32_t rnd(uint32_t *s)
{
    uint32_t x = *s ? *s : 0x1234567u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

static long shuffle_pick(HdePlayer *pl)
{
    size_t n = pl->list.n;
    if (n == 0) return -1;
    if (n == 1) return 0;
    uint32_t pick = rnd(&pl->seed) % (uint32_t)(n - 1);     /* among the others: never the same track twice */
    if (pl->index >= 0 && (long)pick >= pl->index) pick++;
    return (long)pick;
}

long hde_player_next(HdePlayer *pl, int direction, int64_t now_ms)
{
    size_t n = pl->list.n;
    if (n == 0) { pl->index = -1; pl->playing = 0; return -1; }
    if (pl->index < 0) return hde_player_start(pl, 0, now_ms);
    if (pl->shuffle && n > 1) return hde_player_start(pl, shuffle_pick(pl), now_ms);

    long next = pl->index + (direction >= 0 ? 1 : -1);
    if (next < 0) next = (long)n - 1;
    if ((size_t)next >= n) next = 0;
    return hde_player_start(pl, next, now_ms);
}

int hde_player_advance(HdePlayer *pl, int64_t now_ms)
{
    size_t n = pl->list.n;
    if (n == 0 || pl->index < 0) return 0;
    if (pl->repeat == HDE_REPEAT_ONE) {
        pl->started_ms = now_ms;
        pl->paused = 0;
        pl->playing = 1;
        return 1;                                       /* the same track, from the beginning */
    }
    if (pl->shuffle && n > 1) {
        hde_player_start(pl, shuffle_pick(pl), now_ms);
        return 1;
    }
    if (pl->repeat == HDE_REPEAT_OFF && (size_t)pl->index + 1 >= n) {
        pl->playing = 0;                                /* the end of the last track: the playback stops here */
        return 0;
    }
    long next = (long)pl->index + 1;
    if ((size_t)next >= n) next = 0;                     /* repeat all: back to the first one */
    hde_player_start(pl, next, now_ms);
    return 1;
}

void hde_player_stop(HdePlayer *pl)
{
    pl->playing = 0;
    pl->paused = 0;
}

double hde_player_set_volume(HdePlayer *pl, double v)
{
    if (v < 0.0) v = 0.0;
    if (v > 1.0) v = 1.0;
    pl->volume = v;
    return pl->volume;
}

double hde_player_volume_step(HdePlayer *pl, int direction)
{
    double v = pl->volume + (direction >= 0 ? 0.05 : -0.05);
    /* 5 % at a time, and the ends are reached exactly (0 % and 100 % are where the user expects them) */
    long percent = (long)(v * 100.0 + (direction >= 0 ? 0.5 : -0.5));
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    pl->volume = (double)percent / 100.0;
    if (pl->volume > 0.0) pl->muted = 0;
    return pl->volume;
}
