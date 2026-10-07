/*
 * engine.c — see engine.h. The choice (which of mpv / ffplay / gst-launch-1.0 / paplay / aplay this machine has) and the
 * command line for one file. No process is started here: that is the window's business (player.c), because only it
 * cares about the exit of the process and about the order of the list.
 */
#define _POSIX_C_SOURCE 200809L    /* opendir/access/strndup/strcasecmp: HDE is built with -std=c11, POSIX is not implied */

#include "engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "media.h" /* hde_media_strdup, hde_media_basename */

/* ---------------------------------------------------------------- $PATH */

char *hde_media_find_program(const char *name)
{
    if (!name || !*name) return NULL;

    /* a name with a directory in it is not looked up */
    if (strchr(name, '/')) {
        struct stat st;
        return stat(name, &st) == 0 && !S_ISDIR(st.st_mode) && access(name, X_OK) == 0 ? hde_media_strdup(name) : NULL;
    }

    const char *path = getenv("PATH");
    if (!path || !*path) path = "/usr/local/bin:/usr/bin:/bin";

    const char *p = path;
    while (1) {
        const char *colon = strchr(p, ':');
        size_t len = colon ? (size_t)(colon - p) : strlen(p);
        char *dir = len ? strndup(p, len) : hde_media_strdup(".");
        if (dir) {
            char *full = malloc(strlen(dir) + strlen(name) + 2);
            if (full) {
                sprintf(full, "%s/%s", dir, name);
                struct stat st;
                if (stat(full, &st) == 0 && !S_ISDIR(st.st_mode) && access(full, X_OK) == 0) {
                    free(dir);
                    return full;
                }
                free(full);
            }
            free(dir);
        }
        if (!colon) break;
        p = colon + 1;
    }
    return NULL;
}

/* ---------------------------------------------------------------- the choice */

const char *hde_media_engine_kind_name(HdeMediaEngineKind kind)
{
    switch (kind) {
    case HDE_MEDIA_ENGINE_MPV:    return "mpv";
    case HDE_MEDIA_ENGINE_FFPLAY: return "ffplay";
    case HDE_MEDIA_ENGINE_GST:    return "gst-launch-1.0";
    case HDE_MEDIA_ENGINE_PAPLAY: return "paplay";
    case HDE_MEDIA_ENGINE_APLAY:  return "aplay";
    default:                      return "none";
    }
}

/* what each of them is called in $PATH; the file name of gst-launch-1.0 is not the name in the list */
static const char *const engine_files[] = {NULL, "mpv", "ffplay", "gst-launch-1.0",
                                           "paplay", "aplay"};

HdeMediaEngineKind hde_media_engine_find(void)
{
    static const HdeMediaEngineKind order[] = {HDE_MEDIA_ENGINE_MPV, HDE_MEDIA_ENGINE_FFPLAY, HDE_MEDIA_ENGINE_GST};
    for (size_t i = 0; i < sizeof order / sizeof order[0]; i++) {
        const char *prog = engine_files[order[i]];
        if (!prog) continue;
        if (hde_media_find_program(prog)) return order[i];
    }
    return HDE_MEDIA_ENGINE_NONE;
}

/* the uncompressed sound files the sound server itself plays: no decoder is needed, so nothing heavy should be */
static int is_light_sound(const char *path)
{
    const char *ext = strrchr(path ? path : "", '.');
    if (!ext) return 0;
    ext++;
    return !strcasecmp(ext, "wav") || !strcasecmp(ext, "au") || !strcasecmp(ext, "aif") || !strcasecmp(ext, "aiff");
}

HdeMediaEngineKind hde_media_engine_find_for(HdeMediaKind file_kind, const char *path)
{
    if (file_kind == HDE_MEDIA_KIND_AUDIO && is_light_sound(path)) {
        if (hde_media_find_program("paplay")) return HDE_MEDIA_ENGINE_PAPLAY;
        if (hde_media_find_program("aplay")) return HDE_MEDIA_ENGINE_APLAY;
    }
    return hde_media_engine_find();
}

const char *hde_media_engine_hint(void)
{
    return "nothing on this machine can play it: install mpv, ffmpeg (ffplay) or gstreamer1.0-tools (gst-launch-1.0) "
           "— Fedora: sudo dnf install mpv; Debian/Ubuntu: sudo apt install mpv";
}

/* ---------------------------------------------------------------- the command line */

/* a path that begins with a '-' would be read as an option: './' in front keeps it a name */
static char *sane_path(const char *path)
{
    if (path && path[0] == '-' && path[1] != '/') {
        char *p = malloc(strlen(path) + 3);
        if (p) sprintf(p, "./%s", path);
        return p;
    }
    return hde_media_strdup(path);
}

char *hde_media_file_uri(const char *path)
{
    if (!path) return NULL;

    /* file:// + the path, with everything that is not unreserved written as %XX */
    static const char hex[] = "0123456789ABCDEF";
    size_t len = strlen(path);
    char *uri = malloc(len * 3 + sizeof "file://");
    if (!uri) return NULL;
    char *o = uri;
    memcpy(o, "file://", 7);
    o += 7;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)path[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || strchr("-._~/", c)) *o++ = (char)c;
        else {
            *o++ = '%';
            *o++ = hex[c >> 4];
            *o++ = hex[c & 0xf];
        }
    }
    *o = '\0';
    return uri;
}

HdeMediaEngine *hde_media_engine_new(HdeMediaEngineKind kind, const char *path, HdeMediaKind file_kind,
                                     unsigned long wid, const char *ipc)
{
    if (kind == HDE_MEDIA_ENGINE_NONE || !path) return NULL;

    /* the sound server cannot show anything: asking it for a video is a mistake, not a fallback */
    if (file_kind != HDE_MEDIA_KIND_AUDIO && (kind == HDE_MEDIA_ENGINE_PAPLAY || kind == HDE_MEDIA_ENGINE_APLAY)) return NULL;

    const char *prog = engine_files[kind];
    char *program = hde_media_find_program(prog);
    if (!program) return NULL;

    char *file = sane_path(path);
    if (!file) {
        free(program);
        return NULL;
    }

    char *wid_opt = NULL, *ipc_opt = NULL, *uri_opt = NULL;
    if (kind == HDE_MEDIA_ENGINE_MPV) {
        if (ipc && *ipc) {
            ipc_opt = malloc(strlen(ipc) + sizeof "--input-ipc-server=");
            if (ipc_opt) sprintf(ipc_opt, "--input-ipc-server=%s", ipc);
        }
        if (wid && file_kind != HDE_MEDIA_KIND_AUDIO) {
            wid_opt = malloc(sizeof "--wid=18446744073709551615");
            if (wid_opt) sprintf(wid_opt, "--wid=%lu", wid);
        }
    } else if (kind == HDE_MEDIA_ENGINE_GST) {
        char *uri = hde_media_file_uri(path);
        if (!uri) {
            free(program);
            free(file);
            return NULL;
        }
        uri_opt = malloc(strlen(uri) + sizeof "uri=");
        if (uri_opt) sprintf(uri_opt, "uri=%s", uri);
        free(uri);
    }

    /* argv: the program, its options, the file. NULL terminated. */
    const char *args[7];
    int n = 0;
    args[n++] = program;
    switch (kind) {
    case HDE_MEDIA_ENGINE_MPV:
        args[n++] = "--really-quiet";
        args[n++] = "--no-config"; /* the user's mpv.conf may loop or open a playlist of its own */
        if (ipc_opt) args[n++] = ipc_opt;
        if (wid_opt) args[n++] = wid_opt;
        /* a video plays in its own window unless a wid was given; a sound must not open one at all */
        if (file_kind == HDE_MEDIA_KIND_AUDIO) args[n++] = "--no-video";
        break;
    case HDE_MEDIA_ENGINE_FFPLAY:
        if (file_kind == HDE_MEDIA_KIND_AUDIO) args[n++] = "-nodisp"; /* without it: a black window for a sound */
        args[n++] = "-autoexit";
        args[n++] = "-loglevel";
        args[n++] = "quiet";
        break;
    case HDE_MEDIA_ENGINE_GST:
        args[n++] = "-q";
        args[n++] = "playbin";
        args[n++] = uri_opt;
        break;
    case HDE_MEDIA_ENGINE_APLAY:
        args[n++] = "-q"; /* aplay is loud about every file it opens */
        break;
    default:
        break;
    }
    args[n++] = file;
    args[n] = NULL;

    HdeMediaEngine *e = calloc(1, sizeof *e);
    if (!e) {
        free(program);
        free(file);
        free(wid_opt);
        free(ipc_opt);
        free(uri_opt);
        return NULL;
    }
    e->argv = calloc((size_t)n + 1, sizeof *e->argv);
    if (!e->argv) {
        free(program);
        free(file);
        free(wid_opt);
        free(ipc_opt);
        free(uri_opt);
        free(e);
        return NULL;
    }
    /* the options may sit in the array (ipc_opt, wid_opt, uri_opt): the strings are moved into one buffer each argv
       entry owns, so that free()ing the array frees everything */
    for (int i = 0; i < n; i++) {
        e->argv[i] = hde_media_strdup(args[i]);
        if (!e->argv[i]) {
            for (int j = 0; j < i; j++) free(e->argv[j]);
            free(e->argv);
            free(program);
            free(file);
            free(wid_opt);
            free(ipc_opt);
            free(uri_opt);
            free(e);
            return NULL;
        }
    }
    free(file);
    free(wid_opt);
    free(ipc_opt);
    free(uri_opt);

    e->kind = kind;
    e->program = program;
    e->can_seek = kind == HDE_MEDIA_ENGINE_MPV && ipc && *ipc;
    e->sound_only = kind == HDE_MEDIA_ENGINE_PAPLAY || kind == HDE_MEDIA_ENGINE_APLAY;
    return e;
}

void hde_media_engine_free(HdeMediaEngine *engine)
{
    if (!engine) return;
    if (engine->argv) {
        for (char **a = engine->argv; *a; a++) free(*a);
        free(engine->argv);
    }
    free(engine->program);
    free(engine);
}

/* ---------------------------------------------------------------- for a human */

static int plain_word(const char *s)
{
    if (!s || !*s) return 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("._/:=+-@,", c)) continue;
        return 0;
    }
    return 1;
}

char *hde_media_engine_line(const HdeMediaEngine *engine)
{
    if (!engine || !engine->argv || !engine->argv[0]) return NULL;

    /* no GLib here: the line is built by hand */
    char *buf = NULL;
    size_t len = 0;

    for (char **a = engine->argv; *a; a++) {
        const char *s = *a;
        size_t add;
        if (plain_word(s)) {
            add = strlen(s) + 1;
            char *n = realloc(buf, len + add + 1);
            if (!n) {
                free(buf);
                return NULL;
            }
            buf = n;
            memcpy(buf + len, s, strlen(s));
            len += strlen(s);
            buf[len++] = ' ';
        } else {
            /* ' ... ' with every ' written as '\'' */
            size_t need = 3;
            for (const char *p = s; *p; p++) need += *p == '\'' ? 4 : 1;
            char *n = realloc(buf, len + need + 1);
            if (!n) {
                free(buf);
                return NULL;
            }
            buf = n;
            buf[len++] = '\'';
            for (const char *p = s; *p; p++) {
                if (*p == '\'') {
                    memcpy(buf + len, "'\\''", 4);
                    len += 4;
                } else {
                    buf[len++] = *p;
                }
            }
            buf[len++] = '\'';
            buf[len++] = ' ';
        }
    }
    if (len) len--; /* the last space */
    if (!buf) return hde_media_strdup("");
    buf[len] = '\0';
    return buf;
}

