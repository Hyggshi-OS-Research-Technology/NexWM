/* clipboard.c — Hyggshi Files: Cut / Copy / Paste of files.
 *
 * The clipboard carries the format of GNOME / Xfce / MATE / the HDE desktop (x-special/gnome-copied-files:
 * "copy" or "cut", then one URI per line) plus text/uri-list and plain text (the paths), so files copied here paste
 * into the desktop, Thunar, Nautilus..., and the other way round. Nautilus 3.30+ also puts the GNOME format as
 * text ("x-special/nautilus-clipboard" first): that is understood too.
 */
#include "files.h"
#include <string.h>

enum { T_GNOME, T_URIS, T_TEXT, T_KDE_CUT };

static GList *clip_files;          /* what this process put on the clipboard */
static gboolean clip_cut;
static gboolean can_paste;
static GHashTable *cut_uris;

static GtkClipboard *clipboard(void)
{
    return gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
}

static void get_func(GtkClipboard *cb, GtkSelectionData *sd, guint info, gpointer data)
{
    (void)cb; (void)data;
    GString *s = g_string_new(NULL);
    switch (info) {
    case T_GNOME:
        g_string_append(s, clip_cut ? "cut" : "copy");
        for (GList *l = clip_files; l; l = l->next) {
            char *u = g_file_get_uri(l->data);
            g_string_append_printf(s, "\n%s", u);
            g_free(u);
        }
        gtk_selection_data_set(sd, gtk_selection_data_get_target(sd), 8, (const guchar *)s->str, (int)s->len);
        break;
    case T_URIS: {
        char **uris = g_new0(char *, g_list_length(clip_files) + 1);
        int i = 0;
        for (GList *l = clip_files; l; l = l->next) uris[i++] = g_file_get_uri(l->data);
        gtk_selection_data_set_uris(sd, uris);
        g_strfreev(uris);
        break;
    }
    case T_KDE_CUT:
        gtk_selection_data_set(sd, gtk_selection_data_get_target(sd), 8, (const guchar *)(clip_cut ? "1" : "0"), 1);
        break;
    default:
        for (GList *l = clip_files; l; l = l->next) {
            char *p = g_file_get_parse_name(l->data);
            g_string_append_printf(s, "%s%s", s->len ? "\n" : "", p);
            g_free(p);
        }
        gtk_selection_data_set_text(sd, s->str, (int)s->len);
        break;
    }
    g_string_free(s, TRUE);
}

static void clear_func(GtkClipboard *cb, gpointer data)
{
    (void)cb; (void)data;
    files_list_free(clip_files);
    clip_files = NULL;
    gboolean had_cut = cut_uris && g_hash_table_size(cut_uris) > 0;
    if (cut_uris) g_hash_table_remove_all(cut_uris);
    if (had_cut) files_windows_cut_changed();
}

void files_clipboard_set(GList *files, gboolean cut)
{
    if (!files) return;
    GtkTargetList *tl = gtk_target_list_new(NULL, 0);
    gtk_target_list_add(tl, gdk_atom_intern_static_string("x-special/gnome-copied-files"), 0, T_GNOME);
    gtk_target_list_add_uri_targets(tl, T_URIS);
    gtk_target_list_add(tl, gdk_atom_intern_static_string("application/x-kde-cutselection"), 0, T_KDE_CUT);
    gtk_target_list_add_text_targets(tl, T_TEXT);
    int n = 0;
    GtkTargetEntry *targets = gtk_target_table_new_from_list(tl, &n);
    if (gtk_clipboard_set_with_data(clipboard(), targets, (guint)n, get_func, clear_func, NULL)) {
        /* (the old contents were cleared by clear_func just now) */
        clip_files = files_list_copy(files);
        clip_cut = cut;
        if (!cut_uris) cut_uris = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        if (cut)
            for (GList *l = files; l; l = l->next) g_hash_table_add(cut_uris, g_file_get_uri(l->data));
        gtk_clipboard_set_can_store(clipboard(), NULL, 0);
        can_paste = TRUE;
        files_windows_cut_changed();
        char *names = files_names_text(files, 3);
        files_log("clipboard: %s %u item(s): %s", cut ? "cut" : "copy", g_list_length(files), names);
        g_free(names);
    }
    gtk_target_table_free(targets, n);
    gtk_target_list_unref(tl);
}

void files_clipboard_copy_text(const char *text)
{
    gtk_clipboard_set_text(clipboard(), text, -1);
    files_log("clipboard: text %s", text);
}

gboolean files_clipboard_is_cut(GFile *f)
{
    if (!cut_uris || !g_hash_table_size(cut_uris)) return FALSE;
    char *u = g_file_get_uri(f);
    gboolean r = g_hash_table_contains(cut_uris, u);
    g_free(u);
    return r;
}

gboolean files_clipboard_can_paste(void)
{
    return can_paste;
}

static void on_targets(GtkClipboard *cb, GdkAtom *atoms, int n, gpointer d)
{
    (void)cb; (void)d;
    gboolean ok = FALSE;
    for (int i = 0; i < n && !ok; i++) {
        char *name = gdk_atom_name(atoms[i]);
        ok = name && (!strcmp(name, "x-special/gnome-copied-files") || !strcmp(name, "text/uri-list"));
        g_free(name);
    }
    if (!ok && n > 0 && clip_files) ok = TRUE;
    can_paste = ok;
}

static void on_owner_change(GtkClipboard *cb, GdkEvent *ev, gpointer d)
{
    (void)ev; (void)d;
    gtk_clipboard_request_targets(cb, on_targets, NULL);
}

void files_clipboard_init(void)
{
    g_signal_connect(clipboard(), "owner-change", G_CALLBACK(on_owner_change), NULL);
    gtk_clipboard_request_targets(clipboard(), on_targets, NULL);
}

/* ------------------------------------------------------------------ paste */
typedef struct {
    FilesWindow *win;
    GFile *dest;
    int step;            /* 0: gnome format, 1: uri list, 2: text */
} PasteReq;

static void paste_files(PasteReq *r, GList *files, gboolean cut)
{
    if (!g_list_find(files_windows(), r->win)) return;          /* the window was closed meanwhile */
    if (!files) {
        files_log("paste: the clipboard has no files");
        return;
    }
    char *dest = files_display_path(r->dest);
    files_log("paste: %u item(s) into %s (%s)", g_list_length(files), dest, cut ? "move" : "copy");
    g_free(dest);
    files_job_start(GTK_WINDOW(r->win->window), cut ? JOB_MOVE : JOB_COPY, files, r->dest);
}

/* "copy\nuri\nuri" (optionally after a line "x-special/nautilus-clipboard") -> files, cut */
static GList *parse_gnome(const char *text, gboolean *cut)
{
    GList *files = NULL;
    char **lines = g_strsplit(text, "\n", -1);
    int i = 0;
    if (lines[0] && !strcmp(g_strstrip(lines[0]), "x-special/nautilus-clipboard")) i = 1;
    if (!lines[i] || (strcmp(g_strstrip(lines[i]), "copy") && strcmp(lines[i], "cut"))) {
        g_strfreev(lines);
        return NULL;
    }
    *cut = !strcmp(lines[i], "cut");
    for (i++; lines[i]; i++) {
        char *u = g_strstrip(lines[i]);
        if (*u) files = g_list_append(files, g_file_new_for_uri(u));
    }
    g_strfreev(lines);
    return files;
}

static void request_step(PasteReq *r);

static void on_contents(GtkClipboard *cb, GtkSelectionData *sd, gpointer d)
{
    (void)cb;
    PasteReq *r = d;
    GList *files = NULL;
    gboolean cut = FALSE;
    if (sd && gtk_selection_data_get_length(sd) > 0) {
        if (r->step == 0) {
            char *text = g_strndup((const char *)gtk_selection_data_get_data(sd), (gsize)gtk_selection_data_get_length(sd));
            files = parse_gnome(text, &cut);
            g_free(text);
        } else if (r->step == 1) {
            char **uris = gtk_selection_data_get_uris(sd);
            for (int i = 0; uris && uris[i]; i++) files = g_list_append(files, g_file_new_for_uri(uris[i]));
            g_strfreev(uris);
        } else {
            char *text = (char *)gtk_selection_data_get_text(sd);
            if (text) files = parse_gnome(text, &cut);
            g_free(text);
        }
    }
    if (!files && r->step < 2) {
        r->step++;
        request_step(r);
        return;
    }
    paste_files(r, files, cut);
    files_list_free(files);
    g_object_unref(r->dest);
    g_free(r);
}

static void request_step(PasteReq *r)
{
    static const char *const targets[] = { "x-special/gnome-copied-files", "text/uri-list", "UTF8_STRING" };
    gtk_clipboard_request_contents(clipboard(), gdk_atom_intern_static_string(targets[r->step]), on_contents, r);
}

void files_clipboard_paste(FilesWindow *w, GFile *dest_dir)
{
    if (!dest_dir) return;
    PasteReq *r = g_new0(PasteReq, 1);
    r->win = w;
    r->dest = g_object_ref(dest_dir);
    if (clip_files) {                                   /* our own: no need to ask the X server */
        GList *files = files_list_copy(clip_files);
        gboolean cut = clip_cut;
        paste_files(r, files, cut);
        if (cut) gtk_clipboard_clear(clipboard());       /* the files are moved: nothing left to paste */
        files_list_free(files);
        g_object_unref(r->dest);
        g_free(r);
        return;
    }
    request_step(r);
}
