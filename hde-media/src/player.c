/* player.c — the window that plays the music and the video (the second part of hde-media; viewer.c is the pictures).
 *
 * It shows: what plays now at the top, the seek bar, the transport, the list that was given to it, and a status line
 * with the engine and the repeat / shuffle state. It *starts* the sound; it does not decode it — engine.c chose which
 * of mpv / ffplay / gst-launch-1.0 / paplay / aplay is installed and what the command line is, playlist.c knows the
 * list, the tags and the state of the playback, and this file is the thread between them.
 *
 *   Space (or Play)      play / pause        (only mpv can pause: without it, "next" or "stop")
 *   Left / Right, b / n  previous / next track (the list wraps around)
 *   s (or Stop)          stop
 *   Ctrl+Left / Ctrl+Right  back / forward 5 s inside the track (mpv only, like the seek bar)
 *   Up / Down, + / -     the volume (5 % at a time; 0 % and 100 % are reachable exactly)
 *   m                    mute / unmute
 *   z                    shuffle on / off    r   repeat: off → all → one
 *   q or Escape          close the window (the sound stops with it)
 *
 * mpv is the only engine that can be talked to while it plays: it is started with --input-ipc-server, and this file
 * speaks its JSON protocol over that socket (pause, seek, volume, mute, and where we are in the track, twice a second).
 * The others (ffplay, gst-launch-1.0, paplay, aplay) are a process: this window can start them and end them, nothing
 * more — the buttons that cannot work are greyed out rather than pretending.
 *
 * The program logs what it does on stdout with the "hde-media: player: " prefix (engine, the command line it runs,
 * which track of how many, the end of a track, the end of the list): tests/player-window-test.sh reads those.
 */
#include "player.h"     /* the part of this file main.c sees (and the opaque type) */
#include "playlist.h"   /* HdePlayer: the list, the tags, repeat / shuffle / volume */
#include "engine.h"     /* which program plays it, and with what arguments */
#include "media.h"      /* MEDIA_TITLE */

#include <gtk/gtk.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define PLAYER_TICK_MS 500        /* the seek bar and the elapsed time: twice a second is smooth enough */

struct _HdeMediaPlayer {
    GtkApplication *app;
    GtkWidget *window;
    /* what is shown */
    GtkWidget *heading, *heading_sub, *hint, *elapsed, *total, *seek;
    GtkWidget *prev, *play, *stop, *next, *shuffle, *repeat, *volume, *mute_button, *list, *status;
    /* what plays */
    HdePlayer player;             /* the list lives in here too (player.list): one state, not two */
    HdeMediaEngineKind engine;
    char *engine_program;         /* the path of the engine found in $PATH, or NULL */
    GSubprocess *proc;
    unsigned generation;          /* which start the wait in flight belongs to */
    int can_seek;                 /* the mpv socket is up: pause, seek and the volume work */
    /* the mpv socket */
    char *ipc_path;
    GIOChannel *ipc;
    guint ipc_watch, ipc_connect_id;
    int ipc_tries;
    GString *ipc_in;
    int ipc_want_duration;        /* the next number mpv sends is the length, not the position */
    /* the clock */
    guint tick_id;
    double position, duration;
    /* guards: a value set by this program must not come back as a user action */
    int setting_seek, setting_volume;
    int alive, quitting;
    /* one wait on a process may still be in flight when the window closes and this state is freed: the shared flag
     * (refcounted) is what tells it so, because the state itself cannot be looked at any more by then */
    struct PlayerGuard *guard;
};

typedef struct PlayerGuard {
    int refs, alive;
} PlayerGuard;

typedef struct {                  /* the wait on one process, so an old exit is never taken for the current track */
    HdeMediaPlayer *p;
    PlayerGuard *guard;
    unsigned gen;
} PlayerWait;

static void guard_unref(PlayerGuard *g)
{
    if (g && --g->refs == 0) g_free(g);
}

static void player_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *m = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    printf("hde-media: player: %s\n", m);
    fflush(stdout);
    g_free(m);
}

static int64_t now_ms(void) { return g_get_monotonic_time() / 1000; }

/* ---------------------------------------------------------------- what is on the screen */

static void player_status_update(HdeMediaPlayer *p)
{
    const char *rep = p->player.repeat == HDE_REPEAT_ALL ? "repeat all"
                    : (p->player.repeat == HDE_REPEAT_ONE ? "repeat one" : "repeat off");
    char *s = g_strdup_printf("%s · %d in the list · %s%s%s", hde_media_engine_kind_name(p->engine),
                              (int)p->player.list.n, rep, p->player.shuffle ? " · shuffle" : "",
                              p->player.muted ? " · muted" : "");
    gtk_label_set_text(GTK_LABEL(p->status), s);
    g_free(s);
}

static void player_heading_update(HdeMediaPlayer *p)
{
    long i = p->player.index;
    if (i < 0 || (size_t)i >= p->player.list.n) {
        gtk_window_set_title(GTK_WINDOW(p->window), MEDIA_TITLE);
        gtk_label_set_text(GTK_LABEL(p->heading), "Nothing playing");
        gtk_label_set_text(GTK_LABEL(p->heading_sub),
                           p->engine == HDE_MEDIA_ENGINE_NONE ? hde_media_engine_hint()
                                                              : "click a track below, or Play");
        return;
    }
    const HdeTrack *t = &p->player.list.items[i];
    char *title = g_strdup_printf("%s%s", t->title,
                                  p->player.paused ? "  (paused)" : (p->player.playing ? "" : "  (stopped)"));
    gtk_label_set_text(GTK_LABEL(p->heading), title);
    g_free(title);
    char *wintitle = g_strdup_printf("%s — %s", t->title, MEDIA_TITLE);
    gtk_window_set_title(GTK_WINDOW(p->window), wintitle);
    g_free(wintitle);

    /* the second line: the tags when there are any, the folder when there are none (the file name is the title) */
    char *sub;
    if (t->artist[0] || t->album[0])
        sub = g_strdup_printf("%s%s%s", t->artist[0] ? t->artist : "",
                              (t->artist[0] && t->album[0]) ? " · " : "", t->album[0] ? t->album : "");
    else
        sub = g_path_get_dirname(t->path);
    gtk_label_set_text(GTK_LABEL(p->heading_sub), sub);
    g_free(sub);
}

static void player_seek_update(HdeMediaPlayer *p)
{
    char buf[32];
    hde_media_format_time(buf, sizeof buf, p->position);
    gtk_label_set_text(GTK_LABEL(p->elapsed), buf);
    hde_media_format_time(buf, sizeof buf, p->duration > 0 ? p->duration : -1);
    gtk_label_set_text(GTK_LABEL(p->total), buf);

    if (p->setting_seek || !p->seek) return;
    p->setting_seek = 1;
    gtk_range_set_range(GTK_RANGE(p->seek), 0, p->duration > 0 ? p->duration : 1);
    gtk_range_set_value(GTK_RANGE(p->seek), p->position);
    p->setting_seek = 0;
}

static void player_buttons_update(HdeMediaPlayer *p)
{
    int has = p->player.list.n > 0 && p->engine != HDE_MEDIA_ENGINE_NONE;
    gtk_widget_set_sensitive(p->play, has);
    gtk_widget_set_sensitive(p->prev, has);
    gtk_widget_set_sensitive(p->next, has);
    gtk_widget_set_sensitive(p->stop, has && p->player.playing);
    gtk_widget_set_sensitive(p->shuffle, has);
    gtk_widget_set_sensitive(p->repeat, has);
    gtk_widget_set_sensitive(p->volume, has);
    gtk_widget_set_sensitive(p->mute_button, has);
    gtk_button_set_label(GTK_BUTTON(p->play), p->player.playing && !p->player.paused ? "Pause" : "Play");

    int seekable = p->can_seek && p->duration > 0;
    gtk_widget_set_sensitive(p->seek, seekable);
    gtk_widget_set_tooltip_text(p->seek, seekable ? "Drag to move inside the track"
                                                  : (p->engine == HDE_MEDIA_ENGINE_MPV
                                                         ? "mpv will say how long the track is in a moment"
                                                         : "only mpv can be moved inside a track"));
    gtk_widget_set_tooltip_text(p->play,
                                p->can_seek ? "Play / pause (Space)"
                                            : (has ? "Play (mpv is not installed: it cannot pause)" : "Play (Space)"));
}

static void player_select_current(HdeMediaPlayer *p)
{
    if (!p->list) return;
    if (p->player.index < 0) { gtk_list_box_unselect_all(GTK_LIST_BOX(p->list)); return; }
    GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(p->list), (int)p->player.index);
    if (row) gtk_list_box_select_row(GTK_LIST_BOX(p->list), row);
}

static void player_ui_update(HdeMediaPlayer *p)
{
    if (!p->window) return;
    player_heading_update(p);
    player_status_update(p);
    player_seek_update(p);
    player_buttons_update(p);
    player_select_current(p);
}

/* the rows of the list: one label for "Artist — Title" and the length on the right */
static GtkWidget *player_row(HdeMediaPlayer *p, long i)
{
    const HdeTrack *t = &p->player.list.items[i];
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(box, 4);
    gtk_widget_set_margin_end(box, 6);
    gtk_widget_set_margin_top(box, 2);
    gtk_widget_set_margin_bottom(box, 2);

    char *label = t->artist[0] ? g_strdup_printf("%s — %s", t->artist, t->title) : g_strdup(t->title);
    GtkWidget *title = gtk_label_new(label);
    g_free(label);
    gtk_label_set_xalign(GTK_LABEL(title), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(title, TRUE);

    char buf[32];
    hde_media_format_time(buf, sizeof buf, t->duration > 0 ? t->duration : -1);
    GtkWidget *len = gtk_label_new(buf);
    gtk_style_context_add_class(gtk_widget_get_style_context(len), "hde-media-time");

    gtk_box_pack_start(GTK_BOX(box), title, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), len, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(row), box);
    return row;
}

static void player_rows_fill(HdeMediaPlayer *p)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(p->list));
    for (GList *l = children; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(children);
    for (size_t i = 0; i < p->player.list.n; i++)
        gtk_list_box_insert(GTK_LIST_BOX(p->list), player_row(p, (long)i), -1);
    gtk_widget_show_all(p->list);
}

/* ---------------------------------------------------------------- the mpv socket */

static void mpv_teardown(HdeMediaPlayer *p, int from_callback)
{
    if (p->ipc_watch && !from_callback) g_source_remove(p->ipc_watch);
    p->ipc_watch = 0;
    if (p->ipc) {
        g_io_channel_shutdown(p->ipc, TRUE, NULL);   /* closes the socket too */
        g_io_channel_unref(p->ipc);
        p->ipc = NULL;
    }
    if (p->ipc_connect_id) {
        g_source_remove(p->ipc_connect_id);
        p->ipc_connect_id = 0;
    }
    if (p->ipc_in) g_string_truncate(p->ipc_in, 0);
    p->ipc_want_duration = 0;
    p->can_seek = 0;
}

static void mpv_send(HdeMediaPlayer *p, const char *json)
{
    if (!p->ipc) return;
    gsize written = 0;
    if (g_io_channel_write_chars(p->ipc, json, -1, &written, NULL) != G_IO_STATUS_NORMAL) return;
    g_io_channel_write_chars(p->ipc, "\n", 1, &written, NULL);
    g_io_channel_flush(p->ipc, NULL);
}

static int mpv_number(const char *line, double *out)
{
    const char *d = strstr(line, "\"data\":");
    if (!d) return 0;                                       /* a reply to something we do not read (set_property) */
    d += 7;
    while (*d == ' ') d++;
    if (*d == 'n') return 0;                                /* null: mpv does not know it (yet) */
    char *end = NULL;
    double v = strtod(d, &end);
    if (end == d) return 0;
    *out = v;
    return 1;
}

static gboolean player_ipc_data(GIOChannel *ch, GIOCondition cond, gpointer d)
{
    HdeMediaPlayer *p = d;
    (void)ch;
    if (cond & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) {
        mpv_teardown(p, 1);
        player_buttons_update(p);
        return G_SOURCE_REMOVE;
    }

    char buf[1024];
    gsize got = 0;
    GIOStatus st = g_io_channel_read_chars(p->ipc, buf, sizeof buf - 1, &got, NULL);
    if (st == G_IO_STATUS_AGAIN) return G_SOURCE_CONTINUE;
    if (st != G_IO_STATUS_NORMAL) {
        mpv_teardown(p, 1);
        player_buttons_update(p);
        return G_SOURCE_REMOVE;
    }
    g_string_append_len(p->ipc_in, buf, (gssize)got);

    /* one reply per line */
    char *nl;
    while ((nl = strchr(p->ipc_in->str, '\n')) != NULL) {
        char *line = g_strndup(p->ipc_in->str, (gsize)(nl - p->ipc_in->str));
        g_string_erase(p->ipc_in, 0, (gssize)(nl - p->ipc_in->str + 1));
        double v = 0;
        if (mpv_number(line, &v)) {
            if (p->ipc_want_duration) {
                p->ipc_want_duration = 0;
                if (v > 0) p->duration = v;
            } else if (v >= 0) {
                p->position = v;
            }
        }
        g_free(line);
    }
    player_seek_update(p);
    return G_SOURCE_CONTINUE;
}

static gboolean player_ipc_connect(gpointer d)
{
    HdeMediaPlayer *p = d;
    if (!p->alive || !p->ipc_path) { p->ipc_connect_id = 0; return G_SOURCE_REMOVE; }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { p->ipc_connect_id = 0; return G_SOURCE_REMOVE; }

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    int ok = -1;
    if (strlen(p->ipc_path) < sizeof sa.sun_path) {
        snprintf(sa.sun_path, sizeof sa.sun_path, "%s", p->ipc_path);
        ok = connect(fd, (struct sockaddr *)&sa, sizeof sa);
    }
    if (ok != 0) {
        close(fd);
        /* mpv needs a moment to create the socket: 100 ms at a time, 2.5 s in all */
        if (++p->ipc_tries > 25) {
            p->ipc_connect_id = 0;
            player_log("mpv is not answering on %s: play and stop only", p->ipc_path);
            return G_SOURCE_REMOVE;
        }
        return G_SOURCE_CONTINUE;
    }

    p->ipc = g_io_channel_unix_new(fd);            /* the channel owns the fd from here on */
    g_io_channel_set_encoding(p->ipc, NULL, NULL); /* bytes, not characters: mpv speaks UTF-8 JSON */
    g_io_channel_set_buffered(p->ipc, FALSE);
    g_io_channel_set_flags(p->ipc, G_IO_FLAG_NONBLOCK, NULL);
    p->ipc_watch = g_io_add_watch(p->ipc, G_IO_IN | G_IO_HUP | G_IO_ERR, player_ipc_data, p);
    p->ipc_connect_id = 0;
    p->can_seek = 1;
    player_log("mpv is listening: pause, seek and the volume are on");
    /* the length first, then the position: mpv answers in the order it is asked */
    p->ipc_want_duration = 1;
    mpv_send(p, "{\"command\":[\"get_property\",\"duration\"]}");
    mpv_send(p, "{\"command\":[\"get_property\",\"time-pos\"]}");
    player_ui_update(p);
    return G_SOURCE_REMOVE;
}

/* ---------------------------------------------------------------- playing */

static void player_kill(HdeMediaPlayer *p)
{
    mpv_teardown(p, 0);
    if (p->proc) {
        g_subprocess_force_exit(p->proc);
        g_object_unref(p->proc);
        p->proc = NULL;
    }
    p->generation++;                 /* whatever was waiting on that process is not this track any more */
    p->position = 0;
    if (p->tick_id) {
        g_source_remove(p->tick_id);
        p->tick_id = 0;
    }
}

static gboolean player_on_tick(gpointer d)
{
    HdeMediaPlayer *p = d;
    if (!p->alive || !p->proc) { p->tick_id = 0; return G_SOURCE_REMOVE; }
    if (p->can_seek) mpv_send(p, "{\"command\":[\"get_property\",\"time-pos\"]}");
    player_seek_update(p);
    return G_SOURCE_CONTINUE;
}

static void player_play_index(HdeMediaPlayer *p, long index);

static void player_on_proc_exit(GObject *src, GAsyncResult *res, gpointer data)
{
    PlayerWait *w = data;
    PlayerGuard *guard = w->guard;
    HdeMediaPlayer *p = w->p;
    unsigned gen = w->gen;
    g_free(w);

    GError *err = NULL;
    g_subprocess_wait_finish(G_SUBPROCESS(src), res, &err);
    if (err) g_clear_error(&err);

    /* the window was closed and the state freed while this wait was in flight */
    if (!guard->alive) {
        guard_unref(guard);
        return;
    }
    guard_unref(guard);
    if (!p->alive || p->quitting) return;
    if (gen != p->generation) return;                  /* an old process: the current track is another one */
    if (p->proc) {
        g_object_unref(p->proc);
        p->proc = NULL;
    }
    mpv_teardown(p, 0);
    if (p->tick_id) {
        g_source_remove(p->tick_id);
        p->tick_id = 0;
    }
    p->position = 0;

    long i = p->player.index;
    const char *title = (i >= 0 && (size_t)i < p->player.list.n) ? p->player.list.items[i].title : "the track";
    player_log("%s finished", title);
    if (hde_player_advance(&p->player, now_ms())) {
        player_play_index(p, p->player.index);
    } else {
        player_log("end of the list, stopping");
        player_ui_update(p);
    }
}

/* start the engine on one track: the only place that runs a process */
static void player_play_index(HdeMediaPlayer *p, long index)
{
    const HdePlaylist *list = &p->player.list;
    if (index < 0 || (size_t)index >= list->n) return;
    if (p->engine == HDE_MEDIA_ENGINE_NONE) {
        player_log("nothing on this machine plays it: %s", hde_media_engine_hint());
        return;
    }

    player_kill(p);                                    /* the old track's exit must not be taken for this one's end */

    const HdeTrack *t = &list->items[index];
    HdeMediaEngine *cmd = hde_media_engine_new(p->engine, t->path, t->kind, 0,
                                               p->engine == HDE_MEDIA_ENGINE_MPV ? p->ipc_path : NULL);
    if (!cmd) {
        player_log("cannot build the command line for %s", t->path);
        return;
    }
    char *line = hde_media_engine_line(cmd);
    GError *err = NULL;
    GSubprocess *proc = g_subprocess_newv((const gchar * const *)cmd->argv,
                                          G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, &err);
    int can_seek = cmd->can_seek;
    hde_media_engine_free(cmd);
    if (!proc) {
        player_log("cannot start %s: %s", hde_media_engine_kind_name(p->engine), err ? err->message : "?");
        g_clear_error(&err);
        g_free(line);
        return;
    }

    hde_player_start(&p->player, index, now_ms());
    p->proc = proc;
    p->position = 0;
    p->duration = t->duration;
    p->generation++;
    PlayerWait *w = g_new0(PlayerWait, 1);
    w->p = p;
    w->gen = p->generation;
    w->guard = p->guard;
    p->guard->refs++;
    g_subprocess_wait_async(proc, NULL, player_on_proc_exit, w);
    p->tick_id = g_timeout_add(PLAYER_TICK_MS, player_on_tick, p);

    player_log("playing %ld/%d: %s", index + 1, (int)list->n, t->title);
    player_log("run: %s", line ? line : hde_media_engine_kind_name(p->engine));
    g_free(line);

    if (can_seek) {                                    /* mpv: wait for the socket it was told to open */
        p->ipc_tries = 0;
        if (!p->ipc_connect_id) p->ipc_connect_id = g_timeout_add(100, player_ipc_connect, p);
    }
    player_ui_update(p);
}

static void player_skip(HdeMediaPlayer *p, int direction)
{
    if (p->engine == HDE_MEDIA_ENGINE_NONE || p->player.list.n == 0) return;
    long i = hde_player_next(&p->player, direction, now_ms());
    if (i < 0) return;
    player_log("%s: %s", direction < 0 ? "previous" : "next", p->player.list.items[i].title);
    player_play_index(p, i);
}

static void player_stop_here(HdeMediaPlayer *p)
{
    player_kill(p);
    hde_player_stop(&p->player);
    player_log("stopped");
    player_ui_update(p);
}

/* Ctrl+Left / Ctrl+Right: five seconds back or forward, the keyboard of a seek bar */
static void player_seek_by(HdeMediaPlayer *p, double delta)
{
    if (!p->player.playing) return;
    if (!p->can_seek) {
        player_log("%s cannot be moved inside a track (only mpv can)", hde_media_engine_kind_name(p->engine));
        return;
    }
    double to = p->position + delta;
    if (to < 0) to = 0;
    if (p->duration > 0 && to > p->duration) to = p->duration;
    p->position = to;
    char *cmd = g_strdup_printf("{\"command\":[\"seek\",%.3f,\"absolute\"]}", to);
    mpv_send(p, cmd);
    g_free(cmd);
    player_log("seek %.1f s", to);
    player_seek_update(p);
}

static void player_volume_step(HdeMediaPlayer *p, int direction)
{
    double v = hde_player_volume_step(&p->player, direction);
    p->setting_volume = 1;
    gtk_range_set_value(GTK_RANGE(p->volume), v * 100.0);
    p->setting_volume = 0;
    if (p->can_seek) {
        char *cmd = g_strdup_printf("{\"command\":[\"set_property\",\"volume\",%.1f]}", v * 100.0);
        mpv_send(p, cmd);
        g_free(cmd);
    }
    player_log("volume %d %%", (int)(v * 100.0 + 0.5));
    player_status_update(p);
}

/* ---------------------------------------------------------------- the buttons and the keys */

static void player_on_play(GtkWidget *b, gpointer d)
{
    (void)b;
    HdeMediaPlayer *p = d;
    if (!p->player.playing || !p->proc) {              /* nothing plays: the current one, or the first one */
        player_play_index(p, p->player.index >= 0 ? p->player.index : 0);
        return;
    }
    if (!p->can_seek) {
        player_log("%s cannot pause a track (only mpv can): stopping instead", hde_media_engine_kind_name(p->engine));
        player_stop_here(p);
        return;
    }
    p->player.paused = !p->player.paused;
    mpv_send(p, p->player.paused ? "{\"command\":[\"set_property\",\"pause\",true]}"
                                 : "{\"command\":[\"set_property\",\"pause\",false]}");
    player_log(p->player.paused ? "paused" : "playing on");
    player_ui_update(p);
}

static void player_on_stop(GtkWidget *b, gpointer d) { (void)b; player_stop_here(d); }
static void player_on_prev(GtkWidget *b, gpointer d) { (void)b; player_skip(d, -1); }
static void player_on_next(GtkWidget *b, gpointer d) { (void)b; player_skip(d, 1); }

static void player_on_seek(GtkRange *r, gpointer d)
{
    HdeMediaPlayer *p = d;
    if (p->setting_seek) return;
    p->position = gtk_range_get_value(r);
    if (p->can_seek) {
        char *cmd = g_strdup_printf("{\"command\":[\"seek\",%.3f,\"absolute\"]}", p->position);
        mpv_send(p, cmd);
        g_free(cmd);
        player_log("seek %.1f s", p->position);
    }
    player_seek_update(p);
}

static void player_on_volume(GtkRange *r, gpointer d)
{
    HdeMediaPlayer *p = d;
    if (p->setting_volume) return;
    hde_player_set_volume(&p->player, gtk_range_get_value(r) / 100.0);
    if (p->can_seek) {
        char *cmd = g_strdup_printf("{\"command\":[\"set_property\",\"volume\",%.1f]}", p->player.volume * 100.0);
        mpv_send(p, cmd);
        g_free(cmd);
    }
    player_status_update(p);
}

static void player_on_mute(GtkToggleButton *b, gpointer d)
{
    HdeMediaPlayer *p = d;
    p->player.muted = gtk_toggle_button_get_active(b);
    if (p->can_seek)
        mpv_send(p, p->player.muted ? "{\"command\":[\"set_property\",\"mute\",true]}"
                                    : "{\"command\":[\"set_property\",\"mute\",false]}");
    player_log(p->player.muted ? "muted" : "sound on");
    player_status_update(p);
}

static void player_on_shuffle(GtkToggleButton *b, gpointer d)
{
    HdeMediaPlayer *p = d;
    p->player.shuffle = gtk_toggle_button_get_active(b);
    player_log(p->player.shuffle ? "shuffle on" : "shuffle off");
    player_status_update(p);
}

static void player_repeat_next(HdeMediaPlayer *p)
{
    p->player.repeat = p->player.repeat == HDE_REPEAT_OFF ? HDE_REPEAT_ALL
                     : (p->player.repeat == HDE_REPEAT_ALL ? HDE_REPEAT_ONE : HDE_REPEAT_OFF);
    const char *what = p->player.repeat == HDE_REPEAT_ALL ? "all" : (p->player.repeat == HDE_REPEAT_ONE ? "one" : "off");
    char *label = g_strdup_printf("Repeat: %s", what);
    gtk_button_set_label(GTK_BUTTON(p->repeat), label);
    g_free(label);
    player_log("repeat %s", what);
    player_status_update(p);
}

static void player_on_repeat(GtkWidget *b, gpointer d) { (void)b; player_repeat_next(d); }

static void player_on_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer d)
{
    (void)box;
    HdeMediaPlayer *p = d;
    int i = gtk_list_box_row_get_index(row);
    if (i >= 0) player_play_index(p, i);
}

static gboolean player_on_key(GtkWidget *w, GdkEventKey *ev, gpointer d)
{
    (void)w;
    HdeMediaPlayer *p = d;
    switch (ev->keyval) {
    case GDK_KEY_space:
    case GDK_KEY_p:          player_on_play(NULL, p); return TRUE;
    case GDK_KEY_Right:
    case GDK_KEY_n:
        if (ev->state & GDK_CONTROL_MASK) { player_seek_by(p, 5.0); return TRUE; }
        player_skip(p, 1); return TRUE;
    case GDK_KEY_Left:
    case GDK_KEY_b:
        if (ev->state & GDK_CONTROL_MASK) { player_seek_by(p, -5.0); return TRUE; }
        player_skip(p, -1); return TRUE;
    case GDK_KEY_s:          player_stop_here(p); return TRUE;
    case GDK_KEY_m:          gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(p->mute_button), !p->player.muted); return TRUE;
    case GDK_KEY_plus:
    case GDK_KEY_equal:
    case GDK_KEY_KP_Add:     player_volume_step(p, 1); return TRUE;
    case GDK_KEY_minus:
    case GDK_KEY_KP_Subtract: player_volume_step(p, -1); return TRUE;
    case GDK_KEY_z:          gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(p->shuffle), !p->player.shuffle); return TRUE;
    case GDK_KEY_r:          player_repeat_next(p); return TRUE;
    case GDK_KEY_q:
    case GDK_KEY_Escape:     gtk_widget_destroy(p->window); return TRUE;
    default:                 return FALSE;
    }
}

static gboolean player_on_delete(GtkWidget *w, GdkEvent *ev, gpointer d)
{
    (void)w; (void)ev;
    HdeMediaPlayer *p = d;
    p->quitting = 1;
    player_kill(p);                                     /* closing the window stops the sound */
    return FALSE;                                       /* ... and the window is destroyed */
}

static void player_on_destroy(GtkWidget *w, gpointer d)
{
    (void)w;
    HdeMediaPlayer *p = d;
    p->window = NULL;
    p->alive = 0;
    p->quitting = 1;
    player_kill(p);
    player_log("closed");
}

/* ---------------------------------------------------------------- the window */

static GtkWidget *player_button(const char *label, const char *tip, GCallback cb, HdeMediaPlayer *p, GtkWidget *bar)
{
    GtkWidget *b = gtk_button_new_with_label(label);
    gtk_widget_set_tooltip_text(b, tip);
    g_signal_connect(b, "clicked", cb, p);
    gtk_box_pack_start(GTK_BOX(bar), b, FALSE, FALSE, 0);
    return b;
}

static void player_build_window(HdeMediaPlayer *p)
{
    p->alive = 1;
    p->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(p->window), 860, 560);
    gtk_window_set_title(GTK_WINDOW(p->window), MEDIA_TITLE);
    gtk_window_set_icon_name(GTK_WINDOW(p->window), "audio-x-generic");
    gtk_application_add_window(p->app, GTK_WINDOW(p->window));

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(p->window), box);

    /* what plays now */
    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(head), "hde-media-player-head");
    p->heading = gtk_label_new("Nothing playing");
    gtk_style_context_add_class(gtk_widget_get_style_context(p->heading), "hde-media-heading");
    gtk_label_set_xalign(GTK_LABEL(p->heading), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(p->heading), PANGO_ELLIPSIZE_END);
    gtk_widget_set_margin_start(p->heading, 10);
    gtk_widget_set_margin_end(p->heading, 10);
    gtk_widget_set_margin_top(p->heading, 8);
    p->heading_sub = gtk_label_new("");
    gtk_style_context_add_class(gtk_widget_get_style_context(p->heading_sub), "hde-media-sub");
    gtk_label_set_xalign(GTK_LABEL(p->heading_sub), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(p->heading_sub), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_margin_start(p->heading_sub, 10);
    gtk_widget_set_margin_end(p->heading_sub, 10);
    gtk_widget_set_margin_bottom(p->heading_sub, 8);
    gtk_box_pack_start(GTK_BOX(head), p->heading, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(head), p->heading_sub, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), head, FALSE, FALSE, 0);

    /* nothing installed: what to install (the one line of this window that is not about the files given) */
    p->hint = gtk_label_new(hde_media_engine_hint());
    gtk_style_context_add_class(gtk_widget_get_style_context(p->hint), "hde-media-hint");
    gtk_label_set_xalign(GTK_LABEL(p->hint), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(p->hint), TRUE);
    gtk_widget_set_margin_start(p->hint, 10);
    gtk_widget_set_margin_end(p->hint, 10);
    gtk_widget_set_margin_bottom(p->hint, 8);
    gtk_widget_set_visible(p->hint, p->engine == HDE_MEDIA_ENGINE_NONE);
    gtk_box_pack_start(GTK_BOX(box), p->hint, FALSE, FALSE, 0);

    /* where we are in the track */
    GtkWidget *seekrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(seekrow, 10);
    gtk_widget_set_margin_end(seekrow, 10);
    p->elapsed = gtk_label_new("-:--");
    gtk_style_context_add_class(gtk_widget_get_style_context(p->elapsed), "hde-media-time");
    p->seek = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 1, 0.5);
    gtk_scale_set_draw_value(GTK_SCALE(p->seek), FALSE);
    gtk_widget_set_hexpand(p->seek, TRUE);
    p->total = gtk_label_new("-:--");
    gtk_style_context_add_class(gtk_widget_get_style_context(p->total), "hde-media-time");
    gtk_box_pack_start(GTK_BOX(seekrow), p->elapsed, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(seekrow), p->seek, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(seekrow), p->total, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), seekrow, FALSE, FALSE, 0);
    g_signal_connect(p->seek, "value-changed", G_CALLBACK(player_on_seek), p);

    /* the transport */
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_style_context_add_class(gtk_widget_get_style_context(bar), GTK_STYLE_CLASS_TOOLBAR);
    gtk_style_context_add_class(gtk_widget_get_style_context(bar), "hde-media-toolbar");
    p->prev = player_button("Previous", "Previous track (Left)", G_CALLBACK(player_on_prev), p, bar);
    p->play = player_button("Play", "Play / pause (Space)", G_CALLBACK(player_on_play), p, bar);
    p->stop = player_button("Stop", "Stop (s)", G_CALLBACK(player_on_stop), p, bar);
    p->next = player_button("Next", "Next track (Right)", G_CALLBACK(player_on_next), p, bar);

    p->shuffle = gtk_toggle_button_new_with_label("Shuffle");
    gtk_widget_set_tooltip_text(p->shuffle, "Shuffle the list (z)");
    g_signal_connect(p->shuffle, "toggled", G_CALLBACK(player_on_shuffle), p);
    gtk_box_pack_start(GTK_BOX(bar), p->shuffle, FALSE, FALSE, 0);

    p->repeat = gtk_button_new_with_label("Repeat: off");
    gtk_widget_set_tooltip_text(p->repeat, "Repeat: off → all → one (r)");
    g_signal_connect(p->repeat, "clicked", G_CALLBACK(player_on_repeat), p);
    gtk_box_pack_start(GTK_BOX(bar), p->repeat, FALSE, FALSE, 0);

    /* the volume, to the right */
    GtkWidget *space = gtk_label_new("");
    gtk_widget_set_hexpand(space, TRUE);
    gtk_box_pack_start(GTK_BOX(bar), space, TRUE, TRUE, 0);
    p->mute_button = gtk_toggle_button_new_with_label("Mute");
    gtk_widget_set_tooltip_text(p->mute_button, "Mute / unmute (m)");
    g_signal_connect(p->mute_button, "toggled", G_CALLBACK(player_on_mute), p);
    gtk_box_pack_start(GTK_BOX(bar), p->mute_button, FALSE, FALSE, 0);
    p->volume = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 5);
    gtk_scale_set_draw_value(GTK_SCALE(p->volume), FALSE);
    gtk_widget_set_size_request(p->volume, 110, -1);
    gtk_widget_set_tooltip_text(p->volume, "The volume (Up / Down)");
    g_signal_connect(p->volume, "value-changed", G_CALLBACK(player_on_volume), p);
    gtk_box_pack_start(GTK_BOX(bar), p->volume, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), bar, FALSE, FALSE, 0);

    /* the list */
    p->list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(p->list), GTK_SELECTION_SINGLE);
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(p->list), TRUE);   /* one click plays */
    g_signal_connect(p->list, "row-activated", G_CALLBACK(player_on_row_activated), p);
    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_hexpand(scrolled, TRUE);
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_container_add(GTK_CONTAINER(scrolled), p->list);
    gtk_box_pack_start(GTK_BOX(box), scrolled, TRUE, TRUE, 0);

    /* the status line */
    p->status = gtk_label_new("");
    gtk_style_context_add_class(gtk_widget_get_style_context(p->status), "hde-media-status");
    gtk_label_set_xalign(GTK_LABEL(p->status), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(p->status), PANGO_ELLIPSIZE_END);
    gtk_widget_set_margin_start(p->status, 8);
    gtk_widget_set_margin_end(p->status, 8);
    gtk_widget_set_margin_top(p->status, 3);
    gtk_widget_set_margin_bottom(p->status, 3);
    GtkWidget *statusbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(statusbar), "hde-media-statusbar");
    gtk_box_pack_start(GTK_BOX(statusbar), p->status, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), statusbar, FALSE, FALSE, 0);

    g_signal_connect(p->window, "destroy", G_CALLBACK(player_on_destroy), p);
    g_signal_connect(p->window, "delete-event", G_CALLBACK(player_on_delete), p);
    g_signal_connect(p->window, "key-press-event", G_CALLBACK(player_on_key), p);

    /* the volume the state starts with (80 %: hde_player_init) */
    p->setting_volume = 1;
    gtk_range_set_value(GTK_RANGE(p->volume), p->player.volume * 100.0);
    p->setting_volume = 0;
}

/* ---------------------------------------------------------------- the API of player.h */

/* what the list is made of: the files given, the music and video of a folder, or a playlist file */
static void player_collect(HdeMediaPlayer *p, char *const *paths, int n_paths, int recursive)
{
    for (int i = 0; i < n_paths; i++) {
        const char *path = paths[i];
        struct stat st;
        if (stat(path, &st) != 0) {
            player_log("%s cannot be read", path);
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            long n = hde_playlist_scan(&p->player.list, path, recursive);
            if (n <= 0) player_log("no music or video in %s", path);
        } else if (hde_media_is_playlist(path)) {
            long n = hde_playlist_read(&p->player.list, path);
            if (n < 0) player_log("%s is not a playlist this can read", path);
        } else if (hde_media_is_media(path)) {
            hde_playlist_add(&p->player.list, path);
        } else {
            player_log("%s is neither music nor video", path);
        }
    }
}

HdeMediaPlayer *hde_media_player_new(GtkApplication *app, char *const *paths, int n_paths, int recursive)
{
    HdeMediaPlayer *p = g_new0(HdeMediaPlayer, 1);
    p->app = app;
    p->ipc_in = g_string_new(NULL);
    p->guard = g_new0(PlayerGuard, 1);
    p->guard->refs = 1;                              /* this state owns one reference; every wait takes one more */
    p->guard->alive = 1;
    p->engine = hde_media_engine_find();
    if (p->engine != HDE_MEDIA_ENGINE_NONE)
        p->engine_program = hde_media_find_program(hde_media_engine_kind_name(p->engine));
    hde_player_init(&p->player);

    /* mpv is talked to over a socket; it has to be a file name no other run of hde-media is using */
    if (p->engine == HDE_MEDIA_ENGINE_MPV) {
        const char *run = g_get_user_runtime_dir();
        p->ipc_path = g_strdup_printf("%s/hde-media-%d.sock", run && *run ? run : "/tmp", (int)getpid());
        unlink(p->ipc_path);
    }

    player_collect(p, paths, n_paths, recursive);
    player_build_window(p);
    player_rows_fill(p);
    player_ui_update(p);

    if (p->engine == HDE_MEDIA_ENGINE_NONE)
        player_log("engine: none — %s", hde_media_engine_hint());
    else
        player_log("engine: %s (%s)", hde_media_engine_kind_name(p->engine), p->engine_program ? p->engine_program : "?");
    player_log("%d in the list", (int)p->player.list.n);

    gtk_widget_show_all(p->window);
    gtk_widget_set_visible(p->hint, p->engine == HDE_MEDIA_ENGINE_NONE);
    if (p->player.list.n > 0 && p->engine != HDE_MEDIA_ENGINE_NONE) player_play_index(p, 0);
    return p;
}

int hde_media_player_has_tracks(HdeMediaPlayer *p) { return p && p->player.list.n > 0; }
int hde_media_player_alive(HdeMediaPlayer *p) { return p && p->alive; }
GtkWidget *hde_media_player_widget(HdeMediaPlayer *p) { return p ? p->window : NULL; }

void hde_media_player_present(HdeMediaPlayer *p)
{
    if (p && p->window) gtk_window_present(GTK_WINDOW(p->window));
}

void hde_media_player_open(HdeMediaPlayer *p, char *const *paths, int n_paths, int recursive)
{
    if (!p) return;
    long before = (long)p->player.list.n;
    player_collect(p, paths, n_paths, recursive);
    player_rows_fill(p);
    if ((long)p->player.list.n > before) {
        player_log("%d added to the list", (int)(p->player.list.n - (size_t)before));
        player_play_index(p, before);                  /* what was handed over is what plays */
    } else {
        player_log("nothing new to play in what was handed over");
    }
    player_ui_update(p);
    hde_media_player_present(p);
}

void hde_media_player_free(HdeMediaPlayer *p)
{
    if (!p) return;
    player_kill(p);
    if (p->guard) {
        p->guard->alive = 0;                         /* a wait still in flight must not look at `p` any more */
        guard_unref(p->guard);
        p->guard = NULL;
    }
    if (p->window) {
        gtk_widget_destroy(p->window);
        p->window = NULL;
    }
    if (p->ipc_path) {
        unlink(p->ipc_path);
        g_free(p->ipc_path);
    }
    if (p->ipc_in) g_string_free(p->ipc_in, TRUE);
    hde_player_free(&p->player);
    g_free(p->engine_program);
    g_free(p);
}
