/* util.c — Hyggshi Files: settings, sizes and dates, locations, icons, menus, bookmarks, the terminal. */
#include "files.h"
#include <glib/gstdio.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

gboolean files_debug;
FilesPrefs prefs;
const int files_icon_sizes[5] = { 32, 48, 64, 96, 128 };

void files_log(const char *fmt, ...)
{
    if (!files_debug) return;
    va_list ap;
    va_start(ap, fmt);
    char *s = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    fprintf(stderr, "hde-files: %s\n", s);
    fflush(stderr);
    g_free(s);
}

/* ------------------------------------------------------------------ settings: ~/.config/hde/files.ini [files] */
static char *prefs_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "hde", "files.ini", NULL);
}

static const char *const sort_names[SORT_N] = { "name", "size", "type", "modified" };

void prefs_load(void)
{
    prefs = (FilesPrefs){ FALSE, FALSE, 64, SORT_NAME, FALSE, TRUE, TRUE, 200, 980, 620, FALSE, FALSE, TRUE };
    GKeyFile *kf = g_key_file_new();
    char *path = prefs_path();
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        GError *e = NULL;
#define GETB(key, field) do { gboolean v = g_key_file_get_boolean(kf, "files", key, &e); \
                              if (e) g_clear_error(&e); else prefs.field = v; } while (0)
#define GETI(key, field) do { int v = g_key_file_get_integer(kf, "files", key, &e); \
                              if (e) g_clear_error(&e); else prefs.field = v; } while (0)
        GETB("show_hidden", show_hidden);
        GETB("sort_desc", sort_desc);
        GETB("folders_first", folders_first);
        GETB("sidebar", show_sidebar);
        GETB("maximized", maximized);
        GETB("single_click", single_click);
        GETB("thumbnails", thumbnails);
        GETI("icon_size", icon_size);
        GETI("sidebar_width", sidebar_width);
        GETI("width", win_w);
        GETI("height", win_h);
#undef GETB
#undef GETI
        char *v = g_key_file_get_string(kf, "files", "view", NULL);
        if (v) prefs.list_view = !g_strcmp0(g_strstrip(v), "list");
        g_free(v);
        v = g_key_file_get_string(kf, "files", "sort", NULL);
        for (int i = 0; v && i < SORT_N; i++)
            if (!g_strcmp0(g_strstrip(v), sort_names[i])) prefs.sort_by = i;
        g_free(v);
    }
    int best = 2;
    for (int i = 0; i < 5; i++)
        if (ABS(files_icon_sizes[i] - prefs.icon_size) < ABS(files_icon_sizes[best] - prefs.icon_size)) best = i;
    prefs.icon_size = files_icon_sizes[best];
    prefs.sidebar_width = CLAMP(prefs.sidebar_width, 120, 480);
    prefs.win_w = CLAMP(prefs.win_w, 480, 4000);
    prefs.win_h = CLAMP(prefs.win_h, 360, 3000);
    if (prefs.sort_by < 0 || prefs.sort_by >= SORT_N) prefs.sort_by = SORT_NAME;
    g_key_file_free(kf);
    g_free(path);
}

void prefs_save(void)
{
    GKeyFile *kf = g_key_file_new();
    char *path = prefs_path();
    g_key_file_load_from_file(kf, path, G_KEY_FILE_KEEP_COMMENTS, NULL);
    g_key_file_set_boolean(kf, "files", "show_hidden", prefs.show_hidden);
    g_key_file_set_string(kf, "files", "view", prefs.list_view ? "list" : "icons");
    g_key_file_set_integer(kf, "files", "icon_size", prefs.icon_size);
    g_key_file_set_string(kf, "files", "sort", sort_names[prefs.sort_by]);
    g_key_file_set_boolean(kf, "files", "sort_desc", prefs.sort_desc);
    g_key_file_set_boolean(kf, "files", "folders_first", prefs.folders_first);
    g_key_file_set_boolean(kf, "files", "sidebar", prefs.show_sidebar);
    g_key_file_set_integer(kf, "files", "sidebar_width", prefs.sidebar_width);
    g_key_file_set_integer(kf, "files", "width", prefs.win_w);
    g_key_file_set_integer(kf, "files", "height", prefs.win_h);
    g_key_file_set_boolean(kf, "files", "maximized", prefs.maximized);
    g_key_file_set_boolean(kf, "files", "single_click", prefs.single_click);
    g_key_file_set_boolean(kf, "files", "thumbnails", prefs.thumbnails);
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    GError *e = NULL;
    if (!g_key_file_save_to_file(kf, path, &e)) {
        g_printerr("hde-files: cannot save %s: %s\n", path, e->message);
        g_clear_error(&e);
    }
    g_free(dir);
    g_free(path);
    g_key_file_free(kf);
}

int prefs_zoom_level(void)
{
    for (int i = 0; i < 5; i++)
        if (files_icon_sizes[i] == prefs.icon_size) return i;
    return 2;
}

/* ------------------------------------------------------------------ text */
char *files_format_time(gint64 t)
{
    if (t <= 0) return g_strdup("");
    GDateTime *dt = g_date_time_new_from_unix_local(t);
    if (!dt) return g_strdup("");
    GDateTime *now = g_date_time_new_now_local();
    GDateTime *today = g_date_time_new_local(g_date_time_get_year(now), g_date_time_get_month(now),
                                             g_date_time_get_day_of_month(now), 0, 0, 0);
    GTimeSpan diff = g_date_time_difference(dt, today);
    char *s;
    if (diff >= 0 && diff < G_TIME_SPAN_DAY) s = g_date_time_format(dt, "Today %H:%M");
    else if (diff < 0 && diff >= -G_TIME_SPAN_DAY) s = g_date_time_format(dt, "Yesterday %H:%M");
    else if (g_date_time_get_year(dt) == g_date_time_get_year(now)) s = g_date_time_format(dt, "%-d %b %H:%M");
    else s = g_date_time_format(dt, "%-d %b %Y");
    g_date_time_unref(today);
    g_date_time_unref(now);
    g_date_time_unref(dt);
    return s ? s : g_strdup("");
}

char *files_format_size(goffset size, gboolean exact)
{
    if (size < 0) return g_strdup("");
    if (!exact || size < 1000) return g_format_size(size);
    return g_format_size_full(size, G_FORMAT_SIZE_LONG_FORMAT);
}

/* "folder" -> "Folder", "plain text document" -> "Plain text document" (shared-mime-info's English is lower case) */
char *files_type_description(const char *content_type)
{
    char *d = content_type ? g_content_type_get_description(content_type) : NULL;
    if (!d || !*d) {
        g_free(d);
        return g_strdup("Unknown");
    }
    gunichar c = g_utf8_get_char(d), t = g_unichar_totitle(c);
    if (t == c) return d;
    char first[8] = { 0 };
    g_unichar_to_utf8(t, first);
    char *r = g_strconcat(first, g_utf8_next_char(d), NULL);
    g_free(d);
    return r;
}

char *files_names_text(GList *files, int max)
{
    guint n = g_list_length(files);
    GString *s = g_string_new(NULL);
    int shown = 0;
    for (GList *l = files; l && shown < max; l = l->next, shown++) {
        char *b = g_file_get_basename(l->data);
        char *d = b ? g_filename_display_name(b) : g_strdup("?");
        if (shown) g_string_append(s, (guint)shown + 1 == n ? " and " : ", ");
        g_string_append(s, d);
        g_free(d);
        g_free(b);
    }
    if (n > (guint)shown) g_string_append_printf(s, " and %u more", n - (guint)shown);
    return g_string_free(s, FALSE);
}

/* ------------------------------------------------------------------ locations */
gboolean files_is_trash(GFile *f)
{
    return f && g_file_has_uri_scheme(f, "trash");
}

gboolean files_is_recent(GFile *f)
{
    return f && g_file_has_uri_scheme(f, "recent");
}

gboolean files_in_trash_dir(GFile *f)
{
    if (!f || !g_file_is_native(f)) return FALSE;
    char *path = g_file_get_path(f);
    char *td = trash_files_dir();
    gboolean in = path && g_str_has_prefix(path, td) && path[strlen(td)] == '/';
    g_free(td);
    g_free(path);
    return in;
}

static gboolean is_home(GFile *f)
{
    GFile *h = g_file_new_for_path(g_get_home_dir());
    gboolean r = g_file_equal(f, h);
    g_object_unref(h);
    return r;
}

char *files_display_path(GFile *f)
{
    if (!f) return g_strdup("");
    if (files_is_trash(f)) return g_strdup("Trash");
    if (files_is_recent(f)) return g_strdup("Recent");
    if (g_file_is_native(f)) {
        char *path = g_file_get_path(f);
        const char *home = g_get_home_dir();
        size_t hl = strlen(home);
        char *r;
        if (path && !strcmp(path, home)) r = g_strdup("~");
        else if (path && hl > 1 && !strncmp(path, home, hl) && path[hl] == '/') r = g_strconcat("~", path + hl, NULL);
        else r = g_strdup(path ? path : "");
        g_free(path);
        char *d = g_filename_display_name(r);
        g_free(r);
        return d;
    }
    return g_file_get_parse_name(f);
}

char *files_location_title(GFile *f)
{
    if (!f) return g_strdup("");
    if (files_is_trash(f)) return g_strdup("Trash");
    if (files_is_recent(f)) return g_strdup("Recent");
    if (g_file_is_native(f)) {
        if (is_home(f)) return g_strdup("Home");
        char *path = g_file_get_path(f);
        char *r = path && strcmp(path, "/") ? g_filename_display_basename(path) : g_strdup("File System");
        g_free(path);
        return r;
    }
    char *b = g_file_get_basename(f);
    if (!b || !strcmp(b, "/")) {
        g_free(b);
        char *uri = g_file_get_uri(f);
        char *host = NULL;
        GUri *u = g_uri_parse(uri, G_URI_FLAGS_NONE, NULL);
        if (u && g_uri_get_host(u)) host = g_strdup_printf("%s on %s", g_uri_get_scheme(u), g_uri_get_host(u));
        if (u) g_uri_unref(u);
        if (!host) host = g_strdup(uri);
        g_free(uri);
        return host;
    }
    return b;
}

const char *files_location_icon(GFile *f)
{
    if (!f) return "folder";
    if (files_is_trash(f)) return trash_count() > 0 ? "user-trash-full" : "user-trash";
    if (files_is_recent(f)) return "document-open-recent";
    if (!g_file_is_native(f)) return "folder-remote";
    if (is_home(f)) return "user-home";
    char *path = g_file_get_path(f);
    const char *icon = "folder";
    static const struct { GUserDirectory dir; const char *icon; } special[] = {
        { G_USER_DIRECTORY_DESKTOP, "user-desktop" }, { G_USER_DIRECTORY_DOCUMENTS, "folder-documents" },
        { G_USER_DIRECTORY_DOWNLOAD, "folder-download" }, { G_USER_DIRECTORY_MUSIC, "folder-music" },
        { G_USER_DIRECTORY_PICTURES, "folder-pictures" }, { G_USER_DIRECTORY_VIDEOS, "folder-videos" },
        { G_USER_DIRECTORY_TEMPLATES, "folder-templates" }, { G_USER_DIRECTORY_PUBLIC_SHARE, "folder-publicshare" },
    };
    if (path && !strcmp(path, "/")) icon = "drive-harddisk";
    for (guint i = 0; path && i < G_N_ELEMENTS(special); i++) {
        const char *d = g_get_user_special_dir(special[i].dir);
        if (d && !strcmp(d, path) && strcmp(d, g_get_home_dir())) icon = special[i].icon;
    }
    g_free(path);
    return icon;
}

GFile *files_parse_location(const char *text, GFile *relative_to)
{
    if (!text) return NULL;
    char *t = g_strstrip(g_strdup(text));
    GFile *f = NULL;
    if (!*t) f = NULL;
    else if (!strcmp(t, "trash:") || g_str_has_prefix(t, "trash:")) f = g_file_new_for_uri("trash:///");
    else if (!strcmp(t, "recent:") || g_str_has_prefix(t, "recent:")) f = g_file_new_for_uri("recent:///");
    else if (t[0] == '~' && (t[1] == '\0' || t[1] == '/')) {
        char *p = g_build_filename(g_get_home_dir(), t + 1, NULL);
        f = g_file_new_for_path(p);
        g_free(p);
    } else if (t[0] == '/') f = g_file_new_for_path(t);
    else if (strstr(t, "://") || g_str_has_prefix(t, "file:")) f = g_file_new_for_uri(t);
    else if (relative_to && g_file_is_native(relative_to)) f = g_file_resolve_relative_path(relative_to, t);
    else f = g_file_parse_name(t);
    g_free(t);
    return f;
}

/* name.ext -> "name" + ".ext"; archives keep their double extension (.tar.gz); .bashrc has none */
void files_split_ext(const char *name, char **base, const char **ext)
{
    static const char *const doubles[] = { ".tar.gz", ".tar.bz2", ".tar.xz", ".tar.zst", ".tar.lz", ".tar.Z", NULL };
    size_t n = strlen(name);
    for (int i = 0; doubles[i]; i++) {
        size_t l = strlen(doubles[i]);
        if (n > l && !g_ascii_strcasecmp(name + n - l, doubles[i])) {
            *base = g_strndup(name, n - l);
            *ext = name + n - l;
            return;
        }
    }
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name || !dot[1] || strlen(dot) > 8) {
        *base = g_strdup(name);
        *ext = name + n;
        return;
    }
    *base = g_strndup(name, (gsize)(dot - name));
    *ext = dot;
}

static gboolean name_taken(GFile *dir, const char *name)
{
    GFile *c = g_file_get_child(dir, name);
    gboolean taken = g_file_query_file_type(c, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL) != G_FILE_TYPE_UNKNOWN;
    g_object_unref(c);
    return taken;
}

char *files_unique_name(GFile *dir, const char *name, gboolean copy)
{
    if (!name_taken(dir, name)) return g_strdup(name);
    char *base;
    const char *ext;
    files_split_ext(name, &base, &ext);
    int start = 2;
    /* "photo (2).jpg" -> "photo (3).jpg", not "photo (2) (2).jpg" */
    size_t bl = strlen(base);
    if (!copy && bl > 4 && base[bl - 1] == ')') {
        char *open = strrchr(base, '(');
        if (open && open > base && open[-1] == ' ') {
            char *endp = NULL;
            long v = strtol(open + 1, &endp, 10);
            if (endp && *endp == ')' && v >= 1 && v < 100000) {
                start = (int)v + 1;
                open[-1] = '\0';
            }
        }
    }
    char *r = NULL;
    for (int i = copy ? 1 : start; i < 100000; i++) {
        g_free(r);
        if (copy) r = i == 1 ? g_strdup_printf("%s (copy)%s", base, ext) : g_strdup_printf("%s (copy %d)%s", base, i, ext);
        else r = g_strdup_printf("%s (%d)%s", base, i, ext);
        if (!name_taken(dir, r)) break;
    }
    g_free(base);
    return r;
}

/* ------------------------------------------------------------------ icons */
static GHashTable *icon_cache;

void files_icon_cache_clear(void)
{
    if (icon_cache) g_hash_table_remove_all(icon_cache);
}

cairo_surface_t *files_icon_surface(GIcon *icon, int size, int scale)
{
    if (!icon_cache)
        icon_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)cairo_surface_destroy);
    char *s = icon ? g_icon_to_string(icon) : NULL;
    char *key = g_strdup_printf("%d@%d %s", size, scale, s ? s : "-");
    g_free(s);
    cairo_surface_t *surf = g_hash_table_lookup(icon_cache, key);
    if (surf) {
        g_free(key);
        return cairo_surface_reference(surf);
    }
    GtkIconTheme *theme = gtk_icon_theme_get_default();
    GdkPixbuf *pb = NULL;
    if (icon) {
        GtkIconInfo *ii = gtk_icon_theme_lookup_by_gicon_for_scale(theme, icon, size, scale, GTK_ICON_LOOKUP_FORCE_SIZE);
        if (ii) {
            pb = gtk_icon_info_load_icon(ii, NULL);
            g_object_unref(ii);
        }
    }
    if (!pb) pb = gtk_icon_theme_load_icon_for_scale(theme, "text-x-generic", size, scale, GTK_ICON_LOOKUP_FORCE_SIZE, NULL);
    if (!pb) pb = gtk_icon_theme_load_icon_for_scale(theme, "image-missing", size, scale, GTK_ICON_LOOKUP_FORCE_SIZE, NULL);
    if (pb) surf = gdk_cairo_surface_create_from_pixbuf(pb, scale, NULL);
    else {
        surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size * scale, size * scale);
        cairo_surface_set_device_scale(surf, scale, scale);
    }
    if (pb) g_object_unref(pb);
    g_hash_table_insert(icon_cache, key, cairo_surface_reference(surf));
    return surf;
}

/* gdk-pixbuf can read it (thumbnails of pictures without any thumbnailer program) */
gboolean files_can_thumbnail(const char *content_type)
{
    static GHashTable *types;
    if (!content_type) return FALSE;
    if (!types) {
        types = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        GSList *formats = gdk_pixbuf_get_formats();
        for (GSList *l = formats; l; l = l->next) {
            if (gdk_pixbuf_format_is_disabled(l->data)) continue;
            char **mimes = gdk_pixbuf_format_get_mime_types(l->data);
            for (int i = 0; mimes && mimes[i]; i++) g_hash_table_add(types, g_strdup(mimes[i]));
            g_strfreev(mimes);
        }
        g_slist_free(formats);
    }
    char *mime = g_content_type_get_mime_type(content_type);
    gboolean ok = mime && g_hash_table_contains(types, mime);
    g_free(mime);
    return ok;
}

/* ------------------------------------------------------------------ dialogs */
static void on_error_response(GtkDialog *d, int r, gpointer u)
{
    (void)r; (void)u;
    gtk_widget_destroy(GTK_WIDGET(d));
}

void files_error(GtkWindow *parent, const char *title, const char *detail)
{
    files_log("error: %s%s%s", title, detail ? ": " : "", detail ? detail : "");
    GtkWidget *m = gtk_message_dialog_new(parent, GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
                                          "%s", title);
    if (detail && *detail) gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(m), "%s", detail);
    gtk_window_set_title(GTK_WINDOW(m), FILES_TITLE);
    g_signal_connect(m, "response", G_CALLBACK(on_error_response), NULL);
    gtk_widget_show(m);
}

gboolean files_confirm(GtkWindow *parent, const char *title, const char *detail, const char *ok, gboolean danger)
{
    GtkWidget *m = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_QUESTION,
                                          GTK_BUTTONS_NONE, "%s", title);
    if (detail && *detail) gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(m), "%s", detail);
    gtk_window_set_title(GTK_WINDOW(m), FILES_TITLE);
    gtk_dialog_add_button(GTK_DIALOG(m), "_Cancel", GTK_RESPONSE_CANCEL);
    GtkWidget *b = gtk_dialog_add_button(GTK_DIALOG(m), ok, GTK_RESPONSE_OK);
    gtk_style_context_add_class(gtk_widget_get_style_context(b), danger ? "destructive-action" : "suggested-action");
    gtk_dialog_set_default_response(GTK_DIALOG(m), danger ? GTK_RESPONSE_CANCEL : GTK_RESPONSE_OK);
    files_log("question: %s", title);
    int r = gtk_dialog_run(GTK_DIALOG(m));
    gtk_widget_destroy(m);
    return r == GTK_RESPONSE_OK;
}

/* ------------------------------------------------------------------ menus with icons (like every menu of HDE) */
static const char *pick_icon(const char *spec, char *buf, size_t len)
{
    char **names = g_strsplit(spec, "|", -1);
    GtkIconTheme *t = gtk_icon_theme_get_default();
    const char *pick = names[0];
    for (int i = 0; names[i]; i++)
        if (gtk_icon_theme_has_icon(t, names[i])) { pick = names[i]; break; }
    g_strlcpy(buf, pick ? pick : "", len);
    g_strfreev(names);
    return buf;
}

static GtkWidget *item_box(GtkWidget *img, const char *label, const char *accel)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    if (img) {
        gtk_image_set_pixel_size(GTK_IMAGE(img), 16);
        gtk_widget_set_size_request(img, 16, 16);
        gtk_box_pack_start(GTK_BOX(box), img, FALSE, FALSE, 0);
    }
    GtkWidget *l = gtk_label_new_with_mnemonic(label);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_pack_start(GTK_BOX(box), l, TRUE, TRUE, 0);
    if (accel && *accel) {
        GtkWidget *a = gtk_label_new(accel);
        gtk_style_context_add_class(gtk_widget_get_style_context(a), "accel");
        gtk_style_context_add_class(gtk_widget_get_style_context(a), "dim-label");
        gtk_widget_set_margin_start(a, 18);
        gtk_box_pack_end(GTK_BOX(box), a, FALSE, FALSE, 0);
    }
    return box;
}

GtkWidget *files_menu_item(const char *icons, const char *label, const char *accel)
{
    GtkWidget *it = gtk_menu_item_new();
    GtkWidget *img;
    if (icons) {
        char buf[128];
        img = gtk_image_new_from_icon_name(pick_icon(icons, buf, sizeof buf), GTK_ICON_SIZE_MENU);
    } else img = gtk_image_new();
    gtk_container_add(GTK_CONTAINER(it), item_box(img, label, accel));
    gtk_widget_show_all(it);
    return it;
}

GtkWidget *files_menu_check(const char *label, gboolean active, const char *accel)
{
    GtkWidget *it = gtk_check_menu_item_new();
    gtk_container_add(GTK_CONTAINER(it), item_box(NULL, label, accel));
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(it), active);
    gtk_widget_show_all(it);
    return it;
}

GtkWidget *files_menu_radio(GSList **group, const char *label, gboolean active)
{
    GtkWidget *it = gtk_radio_menu_item_new(*group);
    *group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(it));
    gtk_container_add(GTK_CONTAINER(it), item_box(NULL, label, NULL));
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(it), active);
    gtk_widget_show_all(it);
    return it;
}

void files_menu_sep(GtkWidget *menu)
{
    GtkWidget *s = gtk_separator_menu_item_new();
    gtk_widget_show(s);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), s);
}

/* ------------------------------------------------------------------ the terminal, bookmarks */
gboolean files_open_terminal(GFile *dir, GtkWindow *parent)
{
    char *path = dir ? g_file_get_path(dir) : NULL;
    if (!path) {
        files_error(parent, "Cannot open a terminal here", "This place is not a folder of this computer.");
        return FALSE;
    }
    /* Keep the folder as cwd while letting HDE ask which terminal to use when there is more than one. */
    char *chooser = g_find_program_in_path("hde-choose");
    gboolean ok = FALSE;
    if (chooser) {
        char *argv[] = { chooser, (char *)"terminal", NULL };
        GError *e = NULL;
        ok = g_spawn_async(path, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &e);
        if (ok) files_log("terminal chooser opened in %s", path);
        else {
            g_printerr("hde-files: cannot start hde-choose: %s\n", e->message);
            g_clear_error(&e);
        }
        g_free(chooser);
    }
    static const char *const terms[] = { "hde-cmd", "x-terminal-emulator", "gnome-terminal", "xfce4-terminal", "mate-terminal",
        "tilix", "konsole", "lxterminal", "qterminal", "terminator", "alacritty", "kitty", "foot", "xterm", NULL };
    for (int i = 0; terms[i] && !ok; i++) {
        char *prog = g_find_program_in_path(terms[i]);
        if (!prog) continue;
        char *argv[] = { prog, NULL };
        GError *e = NULL;
        ok = g_spawn_async(path, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, &e);
        if (ok) files_log("terminal %s opened in %s", terms[i], path);
        else {
            g_printerr("hde-files: cannot start %s: %s\n", prog, e->message);
            g_clear_error(&e);
        }
        g_free(prog);
    }
    if (!ok) files_error(parent, "No terminal found", "Install a terminal, for example xfce4-terminal or xterm.");
    g_free(path);
    return ok;
}

static char *bookmarks_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "gtk-3.0", "bookmarks", NULL);
}

gboolean files_bookmark_has(GFile *f)
{
    char *path = bookmarks_path(), *data = NULL, *uri = g_file_get_uri(f);
    gboolean has = FALSE;
    if (g_file_get_contents(path, &data, NULL, NULL)) {
        char **lines = g_strsplit(data, "\n", -1);
        for (int i = 0; lines[i] && !has; i++) {
            char *sp = strchr(lines[i], ' ');
            if (sp) *sp = '\0';
            has = !strcmp(lines[i], uri);
        }
        g_strfreev(lines);
    }
    g_free(data);
    g_free(uri);
    g_free(path);
    return has;
}

gboolean files_bookmark_add(GFile *f)
{
    if (files_bookmark_has(f)) return FALSE;
    char *path = bookmarks_path(), *data = NULL, *uri = g_file_get_uri(f);
    g_file_get_contents(path, &data, NULL, NULL);
    GString *s = g_string_new(data ? data : "");
    if (s->len && s->str[s->len - 1] != '\n') g_string_append_c(s, '\n');
    g_string_append_printf(s, "%s\n", uri);
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    gboolean ok = g_file_set_contents(path, s->str, -1, NULL);
    files_log("bookmark added: %s", uri);
    g_string_free(s, TRUE);
    g_free(dir);
    g_free(data);
    g_free(uri);
    g_free(path);
    return ok;
}

/* ------------------------------------------------------------------ lists of GFile */
GList *files_list_copy(GList *files)
{
    return g_list_copy_deep(files, (GCopyFunc)(void (*)(void))g_object_ref, NULL);
}

void files_list_free(GList *files)
{
    g_list_free_full(files, g_object_unref);
}
