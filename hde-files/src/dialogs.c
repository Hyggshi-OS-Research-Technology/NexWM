/* dialogs.c — Hyggshi Files: rename, new folder / document, properties, open with, run or display, conflicts,
 * opening files with their application, About, keyboard shortcuts. */
#include "files.h"
#include <gio/gdesktopappinfo.h>
#include <grp.h>
#include <pwd.h>
#include <string.h>
#include <unistd.h>

/* ------------------------------------------------------------------ a name: rename / new folder / new document */
typedef struct {
    FilesWindow *w;
    int mode;                 /* 0 rename, 1 new folder, 2 new document */
    GFile *file, *dir, *template_file;
    char *old;
    GtkWidget *dialog, *entry, *note, *ok;
} NameDlg;

static void name_free(gpointer d)
{
    NameDlg *n = d;
    if (n->file) g_object_unref(n->file);
    if (n->dir) g_object_unref(n->dir);
    if (n->template_file) g_object_unref(n->template_file);
    g_free(n->old);
    g_free(n);
}

static void name_note(NameDlg *n, const char *text, gboolean error)
{
    gtk_label_set_text(GTK_LABEL(n->note), text ? text : "");
    GtkStyleContext *sc = gtk_widget_get_style_context(n->note);
    if (error) gtk_style_context_add_class(sc, "error");
    else gtk_style_context_remove_class(sc, "error");
    gtk_style_context_add_class(sc, "dim-label");
    if (error) gtk_style_context_remove_class(sc, "dim-label");
}

static void on_name_changed(GtkEditable *e, gpointer d)
{
    (void)e;
    NameDlg *n = d;
    const char *t = gtk_entry_get_text(GTK_ENTRY(n->entry));
    gboolean ok = *t != '\0';
    const char *msg = NULL;
    gboolean err = TRUE;
    char *buf = NULL;
    if (strchr(t, '/')) { msg = "The name cannot contain “/”."; ok = FALSE; }
    else if (!strcmp(t, ".") || !strcmp(t, "..")) { msg = "“.” and “..” cannot be used as names."; ok = FALSE; }
    else if (strlen(t) > 255) { msg = "The name is too long."; ok = FALSE; }
    else if (n->mode == 0 && !g_strcmp0(t, n->old)) ok = FALSE;
    else if (*t) {
        GFile *c = g_file_get_child(n->dir, t);
        if (g_file_query_file_type(c, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL) != G_FILE_TYPE_UNKNOWN) {
            msg = buf = g_strdup_printf("A file or folder named “%s” already exists here.", t);
            ok = FALSE;
        } else if (t[0] == '.') {
            msg = "Names starting with a dot are hidden (Ctrl+H shows them).";
            err = FALSE;
        }
        g_object_unref(c);
    }
    name_note(n, msg, err);
    gtk_widget_set_sensitive(n->ok, ok);
    g_free(buf);
}

static void on_name_response(GtkDialog *d, int r, gpointer data)
{
    NameDlg *n = data;
    if (r != GTK_RESPONSE_ACCEPT) {
        gtk_widget_destroy(GTK_WIDGET(d));
        return;
    }
    if (!gtk_widget_get_sensitive(n->ok)) return;
    const char *t = gtk_entry_get_text(GTK_ENTRY(n->entry));
    GError *e = NULL;
    GFile *result = NULL;
    if (n->mode == 0) {
        result = g_file_set_display_name(n->file, t, NULL, &e);
        if (result) {
            files_undo_push_rename(n->file, result);
            files_log("renamed: %s -> %s", n->old, t);
        }
    } else {
        GFile *c = g_file_get_child(n->dir, t);
        gboolean ok;
        if (n->mode == 1) ok = g_file_make_directory(c, NULL, &e);
        else if (n->template_file) ok = g_file_copy(n->template_file, c, G_FILE_COPY_NONE, NULL, NULL, NULL, &e);
        else {
            GFileOutputStream *os = g_file_create(c, G_FILE_CREATE_NONE, NULL, &e);
            ok = os != NULL;
            if (os) {
                g_output_stream_close(G_OUTPUT_STREAM(os), NULL, NULL);
                g_object_unref(os);
            }
        }
        if (ok) {
            result = g_object_ref(c);
            files_undo_push_created(c);
            char *p = g_file_get_parse_name(c);
            files_log("%s: %s", n->mode == 1 ? "new folder" : "new document", p);
            g_free(p);
        }
        g_object_unref(c);
    }
    if (!result) {
        name_note(n, e ? e->message : "Failed", TRUE);
        g_clear_error(&e);
        return;
    }
    GList *one = g_list_append(NULL, result);
    files_windows_job_done(n->dir, one);
    g_list_free(one);
    g_object_unref(result);
    gtk_widget_destroy(GTK_WIDGET(d));
}

static void name_dialog(FilesWindow *w, int mode, GFile *file, GFile *dir, GFile *template_file)
{
    NameDlg *n = g_new0(NameDlg, 1);
    n->w = w;
    n->mode = mode;
    n->file = file ? g_object_ref(file) : NULL;
    n->dir = dir ? g_object_ref(dir) : g_file_get_parent(file);
    n->template_file = template_file ? g_object_ref(template_file) : NULL;
    gboolean is_dir = FALSE;
    char *initial;
    if (mode == 0) {
        GFileInfo *i = g_file_query_info(file, "standard::display-name,standard::type", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                         NULL, NULL);
        n->old = i ? g_strdup(g_file_info_get_display_name(i)) : g_file_get_basename(file);
        is_dir = i && g_file_info_get_file_type(i) == G_FILE_TYPE_DIRECTORY;
        if (i) g_object_unref(i);
        initial = g_strdup(n->old);
    } else if (mode == 1) initial = files_unique_name(n->dir, "New Folder", FALSE);
    else {
        char *b = template_file ? g_file_get_basename(template_file) : g_strdup("New Document.txt");
        initial = files_unique_name(n->dir, b, FALSE);
        g_free(b);
    }
    const char *title = mode == 0 ? (is_dir ? "Rename Folder" : "Rename File") : mode == 1 ? "New Folder" : "New Document";
    n->dialog = gtk_dialog_new_with_buttons(title, GTK_WINDOW(w->window), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                            "_Cancel", GTK_RESPONSE_CANCEL, NULL);
    n->ok = gtk_dialog_add_button(GTK_DIALOG(n->dialog), mode == 0 ? "_Rename" : "C_reate", GTK_RESPONSE_ACCEPT);
    gtk_style_context_add_class(gtk_widget_get_style_context(n->ok), "suggested-action");
    gtk_dialog_set_default_response(GTK_DIALOG(n->dialog), GTK_RESPONSE_ACCEPT);
    gtk_window_set_resizable(GTK_WINDOW(n->dialog), FALSE);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 14);
    GtkWidget *label = gtk_label_new(mode == 0 ? (is_dir ? "Folder name" : "File name") : mode == 1 ? "Folder name"
                                                                                                  : "Document name");
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
    n->entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(n->entry), initial);
    gtk_entry_set_activates_default(GTK_ENTRY(n->entry), TRUE);
    gtk_entry_set_width_chars(GTK_ENTRY(n->entry), 36);
    gtk_box_pack_start(GTK_BOX(box), n->entry, FALSE, FALSE, 0);
    n->note = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(n->note), 0);
    gtk_label_set_line_wrap(GTK_LABEL(n->note), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(n->note), 44);
    gtk_box_pack_start(GTK_BOX(box), n->note, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(n->dialog))), box, TRUE, TRUE, 0);
    g_signal_connect(n->entry, "changed", G_CALLBACK(on_name_changed), n);
    g_signal_connect(n->dialog, "response", G_CALLBACK(on_name_response), n);
    g_object_set_data_full(G_OBJECT(n->dialog), "name-dlg", n, name_free);
    on_name_changed(NULL, n);
    if (mode != 0) gtk_widget_set_sensitive(n->ok, TRUE);
    gtk_widget_show_all(n->dialog);
    gtk_widget_grab_focus(n->entry);
    char *base;                                   /* select the name without its extension */
    const char *ext;
    files_split_ext(initial, &base, &ext);
    gtk_editable_select_region(GTK_EDITABLE(n->entry), 0, is_dir || mode == 1 ? -1 : (int)g_utf8_strlen(base, -1));
    files_log("dialog: %s (%s)", title, initial);
    g_free(base);
    g_free(initial);
}

void files_rename_dialog(FilesWindow *w, GFile *file)
{
    name_dialog(w, 0, file, NULL, NULL);
}

void files_new_folder_dialog(FilesWindow *w, GFile *dir)
{
    name_dialog(w, 1, NULL, dir, NULL);
}

void files_new_document_dialog(FilesWindow *w, GFile *dir, GFile *template_file)
{
    name_dialog(w, 2, NULL, dir, template_file);
}

static int by_basename(gconstpointer a, gconstpointer b)
{
    char *x = g_file_get_basename((GFile *)a), *y = g_file_get_basename((GFile *)b);
    int r = g_utf8_collate(x, y);
    g_free(x);
    g_free(y);
    return r;
}

GList *files_templates(void)
{
    const char *dir = g_get_user_special_dir(G_USER_DIRECTORY_TEMPLATES);
    GList *list = NULL;
    if (!dir || !strcmp(dir, g_get_home_dir())) return NULL;
    GDir *d = g_dir_open(dir, 0, NULL);
    const char *n;
    while (d && (n = g_dir_read_name(d))) {
        if (n[0] == '.') continue;
        char *p = g_build_filename(dir, n, NULL);
        if (g_file_test(p, G_FILE_TEST_IS_REGULAR)) list = g_list_prepend(list, g_file_new_for_path(p));
        g_free(p);
    }
    if (d) g_dir_close(d);
    return g_list_sort(list, by_basename);
}

/* ------------------------------------------------------------------ properties */
typedef struct {
    GList *files;
    GtkWidget *dialog, *size_value;
    GCancellable *cancel;
    goffset bytes;
    guint64 n_dirs, n_files;
    int pending;
    GFile *file;               /* one item */
    guint32 mode, uid;
    gboolean is_dir, can_change;
    GtkWidget *perm[3], *exec_check;
    gboolean updating;
} Props;

static void props_free(gpointer d)
{
    Props *p = d;
    g_cancellable_cancel(p->cancel);
    g_object_unref(p->cancel);
    files_list_free(p->files);
    g_free(p);
}

static GtkWidget *grid_row(GtkWidget *grid, int *row, const char *key, const char *value)
{
    GtkWidget *k = gtk_label_new(key);
    gtk_label_set_xalign(GTK_LABEL(k), 1);
    gtk_label_set_yalign(GTK_LABEL(k), 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(k), "dim-label");
    GtkWidget *v = gtk_label_new(value ? value : "");
    gtk_label_set_xalign(GTK_LABEL(v), 0);
    gtk_label_set_selectable(GTK_LABEL(v), TRUE);
    gtk_label_set_line_wrap(GTK_LABEL(v), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(v), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(v), 46);
    gtk_widget_set_hexpand(v, TRUE);
    gtk_grid_attach(GTK_GRID(grid), k, 0, *row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), v, 1, *row, 1, 1);
    (*row)++;
    return v;
}

static char *full_time(GFileInfo *info, const char *attr)
{
    if (!g_file_info_has_attribute(info, attr)) return NULL;
    guint64 t = g_file_info_get_attribute_uint64(info, attr);
    if (!t) return NULL;
    GDateTime *dt = g_date_time_new_from_unix_local((gint64)t);
    char *s = dt ? g_date_time_format(dt, "%A %-d %B %Y, %H:%M:%S") : NULL;
    if (dt) g_date_time_unref(dt);
    return s;
}

static void props_show_size(Props *p)
{
    char *size = files_format_size(p->bytes, TRUE);
    char *s;
    guint64 dirs = p->n_dirs;
    if (dirs || p->n_files > 1 || p->is_dir)
        s = g_strdup_printf("%s%s, %" G_GUINT64_FORMAT " file%s and %" G_GUINT64_FORMAT " folder%s", size,
                            p->pending ? " so far" : "", p->n_files, p->n_files == 1 ? "" : "s", dirs,
                            dirs == 1 ? "" : "s");
    else s = g_strdup(size);
    gtk_label_set_text(GTK_LABEL(p->size_value), s);
    if (!p->pending) files_log("properties: size %s", s);
    g_free(s);
    g_free(size);
}

/* the size of what a folder holds: the files in it and in its subfolders (a worker thread) */
typedef struct {
    Props *p;                /* valid while CANCEL is not cancelled (the dialog closing cancels it) */
    GCancellable *cancel;
    GFile *file;
    goffset bytes;
    guint64 files, dirs;
} Count;

static void count_tree(Count *c, GFile *dir, int depth)
{
    if (g_cancellable_is_cancelled(c->cancel) || depth > 64) return;
    GFileEnumerator *en = g_file_enumerate_children(dir, "standard::name,standard::type,standard::size",
                                                    G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, c->cancel, NULL);
    if (!en) return;
    GFileInfo *i;
    while ((i = g_file_enumerator_next_file(en, c->cancel, NULL))) {
        GFileType t = g_file_info_get_file_type(i);
        if (t == G_FILE_TYPE_DIRECTORY) {
            c->dirs++;
            GFile *child = g_file_get_child(dir, g_file_info_get_name(i));
            count_tree(c, child, depth + 1);
            g_object_unref(child);
        } else {
            c->files++;
            if (t == G_FILE_TYPE_REGULAR) c->bytes += g_file_info_get_size(i);
        }
        g_object_unref(i);
    }
    g_object_unref(en);
}

static void count_thread(GTask *t, gpointer so, gpointer data, GCancellable *cc)
{
    (void)so; (void)cc;
    Count *c = data;
    count_tree(c, c->file, 0);
    g_task_return_boolean(t, TRUE);
}

static void count_free(gpointer d)
{
    Count *c = d;
    g_object_unref(c->cancel);
    g_object_unref(c->file);
    g_free(c);
}

static void count_done(GObject *so, GAsyncResult *res, gpointer d)
{
    (void)so;
    Count *c = g_task_get_task_data(G_TASK(res));
    (void)d;
    if (g_cancellable_is_cancelled(c->cancel)) return;
    Props *p = c->p;
    p->bytes += c->bytes;
    p->n_files += c->files;
    p->n_dirs += c->dirs;
    p->pending--;
    props_show_size(p);
}

/* SELF: the folder itself counts as one (several items selected) */
static void count_folder(Props *p, GFile *dir, gboolean self)
{
    Count *c = g_new0(Count, 1);
    c->p = p;
    c->cancel = g_object_ref(p->cancel);
    c->file = g_object_ref(dir);
    c->dirs = self ? 1 : 0;
    p->pending++;
    GTask *t = g_task_new(NULL, NULL, count_done, NULL);
    g_task_set_task_data(t, c, count_free);
    g_task_run_in_thread(t, count_thread);
    g_object_unref(t);
}

static const char *const file_access[] = { "None", "Read-only", "Read and write" };
static const char *const dir_access[] = { "None", "List files only", "Access files", "Create and delete files" };

static int access_level(guint32 mode, int shift, gboolean dir)
{
    guint32 b = (mode >> shift) & 7;
    if (dir) return (b & 2) && (b & 1) ? 3 : (b & 1) && (b & 4) ? 2 : (b & 4) ? 1 : 0;
    return (b & 2) ? 2 : (b & 4) ? 1 : 0;
}

static guint32 level_bits(int level, gboolean dir, guint32 old)
{
    if (dir) return level == 3 ? 7 : level == 2 ? 5 : level == 1 ? 4 : 0;
    return (level == 2 ? 6 : level == 1 ? 4 : 0) | (old & 1);
}

static void apply_mode(Props *p, guint32 mode)
{
    GError *e = NULL;
    if (!g_file_set_attribute_uint32(p->file, G_FILE_ATTRIBUTE_UNIX_MODE, mode, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, &e)) {
        files_error(GTK_WINDOW(p->dialog), "The permissions cannot be changed", e->message);
        g_clear_error(&e);
        return;
    }
    p->mode = mode;
    files_log("permissions: %04o", mode & 07777);
}

static void on_perm_changed(GtkComboBox *c, gpointer d)
{
    Props *p = d;
    if (p->updating) return;
    guint32 mode = p->mode;
    static const int shifts[3] = { 6, 3, 0 };
    for (int i = 0; i < 3; i++) {
        if (GTK_WIDGET(c) != p->perm[i]) continue;
        int level = gtk_combo_box_get_active(c);
        guint32 old = (mode >> shifts[i]) & 7;
        mode = (mode & ~(7u << shifts[i])) | (level_bits(level, p->is_dir, old) << shifts[i]);
    }
    apply_mode(p, mode);
}

static void on_exec_toggled(GtkToggleButton *t, gpointer d)
{
    Props *p = d;
    if (p->updating) return;
    guint32 mode = p->mode;
    if (gtk_toggle_button_get_active(t)) mode |= (mode & 0444) >> 2;   /* x where r */
    else mode &= ~0111u;
    apply_mode(p, mode);
}

static void on_app_changed(GtkAppChooserButton *b, gpointer d)
{
    (void)d;
    GAppInfo *app = gtk_app_chooser_get_app_info(GTK_APP_CHOOSER(b));
    char *ct = gtk_app_chooser_get_content_type(GTK_APP_CHOOSER(b));
    if (app && ct) {
        GError *e = NULL;
        if (g_app_info_set_as_default_for_type(app, ct, &e)) files_log("default for %s: %s", ct, g_app_info_get_id(app));
        else {
            g_printerr("hde-files: %s\n", e->message);
            g_clear_error(&e);
        }
    }
    if (app) g_object_unref(app);
    g_free(ct);
}

static GtkWidget *permissions_page(Props *p, GFileInfo *info)
{
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 16);
    int row = 0;
    guint32 gid = g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_GID);
    struct passwd *pw = getpwuid(p->uid);
    struct group *gr = getgrgid(gid);
    char *owner = g_strdup_printf("%s%s", pw ? pw->pw_name : "?", p->uid == getuid() ? " (you)" : "");
    grid_row(grid, &row, "Owner", owner);
    g_free(owner);
    grid_row(grid, &row, "Group", gr ? gr->gr_name : "?");
    static const char *const who[3] = { "Owner access", "Group access", "Others access" };
    static const int shifts[3] = { 6, 3, 0 };
    p->updating = TRUE;
    for (int i = 0; i < 3; i++) {
        GtkWidget *k = gtk_label_new(who[i]);
        gtk_label_set_xalign(GTK_LABEL(k), 1);
        gtk_style_context_add_class(gtk_widget_get_style_context(k), "dim-label");
        p->perm[i] = gtk_combo_box_text_new();
        const char *const *names = p->is_dir ? dir_access : file_access;
        int n = p->is_dir ? 4 : 3;
        for (int j = 0; j < n; j++) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(p->perm[i]), names[j]);
        gtk_combo_box_set_active(GTK_COMBO_BOX(p->perm[i]), access_level(p->mode, shifts[i], p->is_dir));
        gtk_widget_set_sensitive(p->perm[i], p->can_change);
        g_signal_connect(p->perm[i], "changed", G_CALLBACK(on_perm_changed), p);
        gtk_grid_attach(GTK_GRID(grid), k, 0, row, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), p->perm[i], 1, row, 1, 1);
        row++;
    }
    if (!p->is_dir) {
        p->exec_check = gtk_check_button_new_with_mnemonic("Allow _executing file as a program");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(p->exec_check), (p->mode & 0100) != 0);
        gtk_widget_set_sensitive(p->exec_check, p->can_change);
        g_signal_connect(p->exec_check, "toggled", G_CALLBACK(on_exec_toggled), p);
        gtk_grid_attach(GTK_GRID(grid), p->exec_check, 1, row++, 1, 1);
    }
    p->updating = FALSE;
    if (!p->can_change) {
        GtkWidget *l = gtk_label_new("You are not the owner, so you cannot change these permissions.");
        gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_style_context_add_class(gtk_widget_get_style_context(l), "dim-label");
        gtk_grid_attach(GTK_GRID(grid), l, 0, row++, 2, 1);
    }
    return grid;
}

static void on_props_response(GtkDialog *d, int r, gpointer u)
{
    (void)r; (void)u;
    gtk_widget_destroy(GTK_WIDGET(d));
}

void files_properties_dialog(FilesWindow *w, GList *files)
{
    if (!files) return;
    Props *p = g_new0(Props, 1);
    p->files = files_list_copy(files);
    p->cancel = g_cancellable_new();
    guint n = g_list_length(files);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 16);
    int row = 0;
    char *title;
    GtkWidget *perm_page = NULL;
    const char *attrs = "standard::*,time::*,unix::*,access::*,owner::*";
    GtkWidget *icon;
    if (n == 1) {
        p->file = files->data;
        GError *e = NULL;
        GFileInfo *info = g_file_query_info(p->file, attrs, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, &e);
        if (!info) {
            files_error(GTK_WINDOW(w->window), "The properties cannot be read", e ? e->message : NULL);
            g_clear_error(&e);
            props_free(p);
            gtk_widget_destroy(grid);
            return;
        }
        const char *name = g_file_info_get_display_name(info);
        const char *ct = g_file_info_get_content_type(info);
        p->is_dir = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
        title = g_strdup_printf("%s Properties", name);
        GIcon *gi = g_file_info_get_icon(info);
        icon = gi ? gtk_image_new_from_gicon(gi, GTK_ICON_SIZE_DIALOG) : gtk_image_new_from_icon_name("text-x-generic",
                                                                                                     GTK_ICON_SIZE_DIALOG);
        gtk_image_set_pixel_size(GTK_IMAGE(icon), 64);
        GtkWidget *nl = gtk_label_new(name);
        gtk_label_set_selectable(GTK_LABEL(nl), TRUE);
        gtk_label_set_xalign(GTK_LABEL(nl), 0);
        gtk_label_set_line_wrap(GTK_LABEL(nl), TRUE);
        gtk_label_set_line_wrap_mode(GTK_LABEL(nl), PANGO_WRAP_WORD_CHAR);
        PangoAttrList *al = pango_attr_list_new();
        pango_attr_list_insert(al, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
        pango_attr_list_insert(al, pango_attr_scale_new(1.2));
        gtk_label_set_attributes(GTK_LABEL(nl), al);
        pango_attr_list_unref(al);
        gtk_grid_attach(GTK_GRID(grid), icon, 0, row, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), nl, 1, row++, 1, 1);
        char *type = files_type_description(ct);
        char *type_full = ct ? g_strdup_printf("%s (%s)", type, ct) : g_strdup(type);
        grid_row(grid, &row, "Type", type_full);
        GFile *parent = g_file_get_parent(p->file);
        char *loc = files_in_trash_dir(p->file) ? g_strdup("Trash") : parent ? g_file_get_parse_name(parent) : g_strdup("/");
        grid_row(grid, &row, "Location", loc);
        if (files_in_trash_dir(p->file)) {
            char *orig = NULL;
            gint64 deleted = 0;
            char *bn = g_file_get_basename(p->file);
            if (trash_read_info(bn, &orig, &deleted)) {
                grid_row(grid, &row, "Original location", orig);
                char *dt = files_format_time(deleted);
                grid_row(grid, &row, "Deleted", dt);
                g_free(dt);
            }
            g_free(orig);
            g_free(bn);
        }
        if (g_file_info_get_is_symlink(info)) grid_row(grid, &row, "Link to", g_file_info_get_symlink_target(info));
        p->size_value = grid_row(grid, &row, p->is_dir ? "Contents" : "Size", "");
        if (p->is_dir) {
            gtk_label_set_text(GTK_LABEL(p->size_value), "Counting…");
            count_folder(p, p->file, FALSE);
        } else {
            p->bytes = g_file_info_get_size(info);
            p->n_files = 1;
            props_show_size(p);
        }
        if (ct && g_content_type_is_a(ct, "image/*") && !p->is_dir) {
            char *path = g_file_get_path(p->file);
            int iw = 0, ih = 0;
            if (path && gdk_pixbuf_get_file_info(path, &iw, &ih)) {
                char *dim = g_strdup_printf("%d × %d pixels", iw, ih);
                grid_row(grid, &row, "Dimensions", dim);
                g_free(dim);
            }
            g_free(path);
        }
        char *t;
        if ((t = full_time(info, G_FILE_ATTRIBUTE_TIME_MODIFIED))) { grid_row(grid, &row, "Modified", t); g_free(t); }
        if ((t = full_time(info, G_FILE_ATTRIBUTE_TIME_ACCESS))) { grid_row(grid, &row, "Accessed", t); g_free(t); }
        if ((t = full_time(info, G_FILE_ATTRIBUTE_TIME_CREATED))) { grid_row(grid, &row, "Created", t); g_free(t); }
        if (p->is_dir) {
            GFileInfo *fs = g_file_query_filesystem_info(p->file, "filesystem::*", NULL, NULL);
            if (fs && g_file_info_has_attribute(fs, G_FILE_ATTRIBUTE_FILESYSTEM_FREE)) {
                char *fr = g_format_size(g_file_info_get_attribute_uint64(fs, G_FILE_ATTRIBUTE_FILESYSTEM_FREE));
                char *sz = g_format_size(g_file_info_get_attribute_uint64(fs, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE));
                char *s = g_strdup_printf("%s free of %s", fr, sz);
                grid_row(grid, &row, "Free space", s);
                g_free(s);
                g_free(sz);
                g_free(fr);
            }
            if (fs) g_object_unref(fs);
        } else if (ct && !files_in_trash_dir(p->file)) {
            GtkWidget *k = gtk_label_new("Open with");
            gtk_label_set_xalign(GTK_LABEL(k), 1);
            gtk_style_context_add_class(gtk_widget_get_style_context(k), "dim-label");
            GtkWidget *chooser = gtk_app_chooser_button_new(ct);
            gtk_app_chooser_button_set_show_default_item(GTK_APP_CHOOSER_BUTTON(chooser), TRUE);
            gtk_widget_set_halign(chooser, GTK_ALIGN_START);
            g_signal_connect(chooser, "changed", G_CALLBACK(on_app_changed), NULL);
            gtk_grid_attach(GTK_GRID(grid), k, 0, row, 1, 1);
            gtk_grid_attach(GTK_GRID(grid), chooser, 1, row++, 1, 1);
        }
        if (g_file_is_native(p->file) && g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_UNIX_MODE)) {
            p->mode = g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_MODE);
            p->uid = g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_UID);
            p->can_change = p->uid == getuid() || getuid() == 0;
            perm_page = permissions_page(p, info);
        }
        char *sz = p->is_dir ? g_strdup("folder") : files_format_size(p->bytes, FALSE);
        files_log("properties: %s: %s, %s", name, type, sz);
        g_free(sz);
        g_free(loc);
        if (parent) g_object_unref(parent);
        g_free(type_full);
        g_free(type);
        g_object_unref(info);
    } else {
        title = g_strdup_printf("%u Items Properties", n);
        icon = gtk_image_new_from_icon_name("edit-copy", GTK_ICON_SIZE_DIALOG);
        gtk_image_set_pixel_size(GTK_IMAGE(icon), 64);
        char *names = files_names_text(files, 4);
        GtkWidget *nl = gtk_label_new(names);
        gtk_label_set_xalign(GTK_LABEL(nl), 0);
        gtk_label_set_line_wrap(GTK_LABEL(nl), TRUE);
        gtk_grid_attach(GTK_GRID(grid), icon, 0, row, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), nl, 1, row++, 1, 1);
        int dirs = 0;
        for (GList *l = files; l; l = l->next) {
            GFileInfo *i = g_file_query_info(l->data, "standard::type,standard::size", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                             NULL, NULL);
            if (!i) continue;
            if (g_file_info_get_file_type(i) == G_FILE_TYPE_DIRECTORY) {
                dirs++;
                count_folder(p, l->data, TRUE);
            } else {
                p->bytes += g_file_info_get_size(i);
                p->n_files++;
            }
            g_object_unref(i);
        }
        char *kinds = g_strdup_printf("%d folder%s, %u file%s", dirs, dirs == 1 ? "" : "s", n - (guint)dirs,
                                      n - (guint)dirs == 1 ? "" : "s");
        grid_row(grid, &row, "Selected", kinds);
        GFile *parent = g_file_get_parent(files->data);
        char *loc = parent ? g_file_get_parse_name(parent) : g_strdup("/");
        grid_row(grid, &row, "Location", loc);
        p->size_value = grid_row(grid, &row, "Total size", "");
        props_show_size(p);
        files_log("properties: %u items (%s)", n, names);
        g_free(loc);
        if (parent) g_object_unref(parent);
        g_free(kinds);
        g_free(names);
    }
    p->dialog = gtk_dialog_new_with_buttons(title, GTK_WINDOW(w->window), GTK_DIALOG_DESTROY_WITH_PARENT,
                                            "_Close", GTK_RESPONSE_CLOSE, NULL);
    gtk_window_set_default_size(GTK_WINDOW(p->dialog), 460, -1);
    g_object_set_data_full(G_OBJECT(p->dialog), "props", p, props_free);
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(p->dialog));
    if (perm_page) {
        GtkWidget *nb = gtk_notebook_new();
        gtk_notebook_append_page(GTK_NOTEBOOK(nb), grid, gtk_label_new("General"));
        gtk_notebook_append_page(GTK_NOTEBOOK(nb), perm_page, gtk_label_new("Permissions"));
        gtk_container_set_border_width(GTK_CONTAINER(nb), 6);
        gtk_box_pack_start(GTK_BOX(content), nb, TRUE, TRUE, 0);
    } else gtk_box_pack_start(GTK_BOX(content), grid, TRUE, TRUE, 0);
    g_signal_connect(p->dialog, "response", G_CALLBACK(on_props_response), NULL);
    gtk_widget_show_all(p->dialog);
    g_free(title);
}

/* ------------------------------------------------------------------ opening files */
void files_launch(FilesWindow *w, GList *files, GAppInfo *app)
{
    GdkAppLaunchContext *ctx = gdk_display_get_app_launch_context(gtk_widget_get_display(w->window));
    gdk_app_launch_context_set_timestamp(ctx, gtk_get_current_event_time());
    GError *e = NULL;
    gboolean ok;
    if (g_app_info_supports_uris(app) && files && !g_file_is_native(files->data)) {
        GList *uris = NULL;
        for (GList *l = files; l; l = l->next) uris = g_list_append(uris, g_file_get_uri(l->data));
        ok = g_app_info_launch_uris(app, uris, G_APP_LAUNCH_CONTEXT(ctx), &e);
        g_list_free_full(uris, g_free);
    } else ok = g_app_info_launch(app, files, G_APP_LAUNCH_CONTEXT(ctx), &e);
    char *names = files_names_text(files, 3);
    if (ok) files_log("open: %s with %s", names, g_app_info_get_id(app) ? g_app_info_get_id(app) : g_app_info_get_name(app));
    else {
        char *t = g_strdup_printf("“%s” cannot open %s", g_app_info_get_name(app), names);
        files_error(GTK_WINDOW(w->window), t, e ? e->message : NULL);
        g_free(t);
        g_clear_error(&e);
    }
    g_free(names);
    g_object_unref(ctx);
}

typedef struct {
    FilesWindow *w;
    GList *files;
    GtkWidget *check;
} OpenWith;

static void on_open_with_response(GtkDialog *d, int r, gpointer data)
{
    OpenWith *o = data;
    if (r == GTK_RESPONSE_OK && g_list_find(files_windows(), o->w)) {
        GAppInfo *app = gtk_app_chooser_get_app_info(GTK_APP_CHOOSER(d));
        if (app) {
            if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(o->check))) {
                char *ct = gtk_app_chooser_get_content_type(GTK_APP_CHOOSER(d));
                if (ct && g_app_info_set_as_default_for_type(app, ct, NULL))
                    files_log("default for %s: %s", ct, g_app_info_get_id(app));
                g_free(ct);
            }
            files_launch(o->w, o->files, app);
            g_object_unref(app);
        }
    }
    files_list_free(o->files);
    g_free(o);
    gtk_widget_destroy(GTK_WIDGET(d));
}

void files_open_with_dialog(FilesWindow *w, GList *files)
{
    if (!files) return;
    GtkWidget *d = gtk_app_chooser_dialog_new(GTK_WINDOW(w->window), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                              files->data);
    GtkWidget *widget = gtk_app_chooser_dialog_get_widget(GTK_APP_CHOOSER_DIALOG(d));
    gtk_app_chooser_widget_set_show_fallback(GTK_APP_CHOOSER_WIDGET(widget), TRUE);
    gtk_app_chooser_widget_set_show_other(GTK_APP_CHOOSER_WIDGET(widget), TRUE);
    OpenWith *o = g_new0(OpenWith, 1);
    o->w = w;
    o->files = files_list_copy(files);
    o->check = gtk_check_button_new_with_mnemonic("_Always use it for this type of file");
    gtk_widget_set_margin_start(o->check, 12);
    gtk_widget_set_margin_bottom(o->check, 6);
    gtk_box_pack_end(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))), o->check, FALSE, FALSE, 0);
    gtk_widget_show(o->check);
    g_signal_connect(d, "response", G_CALLBACK(on_open_with_response), o);
    gtk_widget_show(d);
    files_log("dialog: Open With");
}

static const char *find_terminal(const char **exec_flag)
{
    static const struct { const char *prog, *flag; } terms[] = {
        { "hde-cmd", "-e" }, { "x-terminal-emulator", "-e" }, { "gnome-terminal", "--" }, { "xfce4-terminal", "-x" }, { "mate-terminal", "-x" },
        { "tilix", "-e" }, { "konsole", "-e" }, { "lxterminal", "-e" }, { "qterminal", "-e" }, { "terminator", "-x" },
        { "alacritty", "-e" }, { "kitty", NULL }, { "foot", NULL }, { "xterm", "-e" } };
    for (guint i = 0; i < G_N_ELEMENTS(terms); i++) {
        char *p = g_find_program_in_path(terms[i].prog);
        if (p) {
            g_free(p);
            *exec_flag = terms[i].flag;
            return terms[i].prog;
        }
    }
    return NULL;
}

static void run_program(FilesWindow *w, GFile *file, gboolean terminal)
{
    char *path = g_file_get_path(file);
    GFile *parent = g_file_get_parent(file);
    char *cwd = parent ? g_file_get_path(parent) : NULL;
    const char *flag = NULL, *term = terminal ? find_terminal(&flag) : NULL;
    const char *argv[5] = { NULL };
    int i = 0;
    if (terminal && !term) {
        files_error(GTK_WINDOW(w->window), "No terminal found", "Install a terminal, for example xfce4-terminal.");
        goto out;
    }
    if (term) {
        argv[i++] = term;
        if (flag) argv[i++] = flag;
    }
    argv[i++] = path;
    GError *e = NULL;
    if (g_spawn_async(cwd, (char **)argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &e))
        files_log("run: %s%s", path, terminal ? " (in a terminal)" : "");
    else {
        files_error(GTK_WINDOW(w->window), "The program cannot be started", e->message);
        g_clear_error(&e);
    }
out:
    g_free(cwd);
    if (parent) g_object_unref(parent);
    g_free(path);
}

enum { RUN_CANCEL, RUN_RUN, RUN_TERMINAL, RUN_DISPLAY };

static int ask_run(FilesWindow *w, GFile *file, gboolean script)
{
    char *b = g_file_get_basename(file);
    char *d = g_filename_display_name(b);
    GtkWidget *m = gtk_message_dialog_new(GTK_WINDOW(w->window), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                          GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
                                          script ? "Run “%s”, or display its contents?" : "Run “%s”?", d);
    if (script) gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(m), "“%s” is an executable text file.", d);
    else gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(m), "%s",
                                                  "It is a program. Run it only if you trust where it comes from.");
    gtk_dialog_add_button(GTK_DIALOG(m), "_Cancel", RUN_CANCEL);
    gtk_dialog_add_button(GTK_DIALOG(m), "Run in _Terminal", RUN_TERMINAL);
    if (script) gtk_dialog_add_button(GTK_DIALOG(m), "_Display", RUN_DISPLAY);
    gtk_dialog_add_button(GTK_DIALOG(m), "_Run", RUN_RUN);
    gtk_dialog_set_default_response(GTK_DIALOG(m), script ? RUN_DISPLAY : RUN_RUN);
    files_log("question: run %s", d);
    int r = gtk_dialog_run(GTK_DIALOG(m));
    gtk_widget_destroy(m);
    g_free(d);
    g_free(b);
    return r < 0 ? RUN_CANCEL : r;
}

/* a .desktop launcher: run what it says (if it is trusted: executable, or installed), or open its URL */
static gboolean open_launcher(FilesWindow *w, GFile *file, gboolean trusted)
{
    char *path = g_file_get_path(file);
    if (!path) return FALSE;
    GKeyFile *kf = g_key_file_new();
    gboolean done = FALSE;
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        char *type = g_key_file_get_string(kf, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_TYPE, NULL);
        char *name = g_key_file_get_locale_string(kf, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_NAME, NULL, NULL);
        if (!trusted) {
            char *t = g_strdup_printf("Launch “%s”?", name ? name : path);
            trusted = files_confirm(GTK_WINDOW(w->window), t, "This launcher is not marked as trusted (executable). "
                                    "Launch it only if you trust where it comes from; it is then marked as trusted.",
                                    "_Trust and Launch", FALSE);
            g_free(t);
            if (trusted) {
                GFileInfo *i = g_file_query_info(file, G_FILE_ATTRIBUTE_UNIX_MODE, G_FILE_QUERY_INFO_NONE, NULL, NULL);
                if (i) {
                    guint32 mode = g_file_info_get_attribute_uint32(i, G_FILE_ATTRIBUTE_UNIX_MODE);
                    g_file_set_attribute_uint32(file, G_FILE_ATTRIBUTE_UNIX_MODE, mode | 0100, G_FILE_QUERY_INFO_NONE,
                                                NULL, NULL);
                    g_object_unref(i);
                }
            } else done = TRUE;                   /* (cancelled) */
        }
        if (trusted && !g_strcmp0(type, "Link")) {
            char *url = g_key_file_get_string(kf, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_URL, NULL);
            if (url && g_app_info_launch_default_for_uri(url, NULL, NULL)) files_log("open: %s", url);
            g_free(url);
            done = TRUE;
        } else if (trusted) {
            GDesktopAppInfo *app = g_desktop_app_info_new_from_keyfile(kf);
            if (app) {
                GdkAppLaunchContext *ctx = gdk_display_get_app_launch_context(gtk_widget_get_display(w->window));
                GError *e = NULL;
                if (g_app_info_launch(G_APP_INFO(app), NULL, G_APP_LAUNCH_CONTEXT(ctx), &e))
                    files_log("launch: %s", name ? name : path);
                else {
                    files_error(GTK_WINDOW(w->window), "The launcher cannot be started", e ? e->message : NULL);
                    g_clear_error(&e);
                }
                g_object_unref(ctx);
                g_object_unref(app);
                done = TRUE;
            }
        }
        g_free(name);
        g_free(type);
    }
    g_key_file_free(kf);
    g_free(path);
    return done;
}

/* opens F (or asks): returns the application to open it with (new reference), NULL when it is done already */
static GAppInfo *open_one(FilesWindow *w, GFile *f)
{
    GFileInfo *info = g_file_query_info(f, "standard::content-type,standard::type,access::can-execute",
                                        G_FILE_QUERY_INFO_NONE, NULL, NULL);
    if (!info) {
        char *n = g_file_get_parse_name(f);
        char *t = g_strdup_printf("“%s” cannot be opened", n);
        files_error(GTK_WINDOW(w->window), t, "It does not exist anymore, or it cannot be read.");
        g_free(t);
        g_free(n);
        return NULL;
    }
    char *ct = g_strdup(g_file_info_get_content_type(info));
    GFileType type = g_file_info_get_file_type(info);
    gboolean exec = g_file_info_get_attribute_boolean(info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE) &&
                    type == G_FILE_TYPE_REGULAR && g_file_is_native(f);
    g_object_unref(info);
    GAppInfo *app = NULL;
    if (type == G_FILE_TYPE_DIRECTORY) {
        GList *one = g_list_append(NULL, f);
        files_window_activate_items(w, one, 1);
        g_list_free(one);
        goto done;
    }
    if (ct && g_content_type_is_a(ct, "application/x-desktop") && g_file_is_native(f)) {
        char *path = g_file_get_path(f);
        gboolean installed = path && strstr(path, "/applications/") != NULL;
        g_free(path);
        if (open_launcher(w, f, exec || installed)) goto done;
    }
    gboolean binary = ct && (g_content_type_is_a(ct, "application/x-executable") ||
                             g_content_type_is_a(ct, "application/x-sharedlib") ||
                             g_content_type_is_a(ct, "application/x-pie-executable") ||
                             g_content_type_is_a(ct, "application/vnd.appimage"));
    if (exec && (binary || (ct && g_content_type_can_be_executable(ct)))) {
        int r = ask_run(w, f, !binary);
        if (r == RUN_RUN || r == RUN_TERMINAL) run_program(w, f, r == RUN_TERMINAL);
        if (r != RUN_DISPLAY) goto done;
    }
    app = ct ? g_app_info_get_default_for_type(ct, !g_file_is_native(f)) : NULL;
    if (!app && ct && g_content_type_is_a(ct, "text/plain")) app = g_app_info_get_default_for_type("text/plain", FALSE);
    if (!app) {
        GList *one = g_list_append(NULL, f);
        files_open_with_dialog(w, one);
        g_list_free(one);
    }
done:
    g_free(ct);
    return app;
}

void files_open_files(FilesWindow *w, GList *files)
{
    GPtrArray *apps = g_ptr_array_new_with_free_func(g_object_unref);
    GPtrArray *groups = g_ptr_array_new();
    for (GList *l = files; l; l = l->next) {
        GAppInfo *app = open_one(w, l->data);
        if (!app) continue;
        guint i;
        for (i = 0; i < apps->len; i++)
            if (g_app_info_equal(apps->pdata[i], app)) break;
        if (i == apps->len) {
            g_ptr_array_add(apps, app);
            g_ptr_array_add(groups, NULL);
        } else g_object_unref(app);
        groups->pdata[i] = g_list_append(groups->pdata[i], l->data);
    }
    for (guint i = 0; i < apps->len; i++) {
        files_launch(w, groups->pdata[i], apps->pdata[i]);
        g_list_free(groups->pdata[i]);
    }
    g_ptr_array_unref(groups);
    g_ptr_array_unref(apps);
}

/* ------------------------------------------------------------------ conflicts */
static GtkWidget *file_card(GFile *f, const char *heading)
{
    GFileInfo *i = g_file_query_info(f, "standard::icon,standard::size,standard::type,time::modified",
                                     G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GIcon *gi = i ? g_file_info_get_icon(i) : NULL;
    GtkWidget *img = gi ? gtk_image_new_from_gicon(gi, GTK_ICON_SIZE_DIALOG) : gtk_image_new_from_icon_name("text-x-generic",
                                                                                                           GTK_ICON_SIZE_DIALOG);
    gtk_image_set_pixel_size(GTK_IMAGE(img), 48);
    gtk_box_pack_start(GTK_BOX(box), img, FALSE, FALSE, 0);
    char *size = i && g_file_info_get_file_type(i) != G_FILE_TYPE_DIRECTORY ? files_format_size(g_file_info_get_size(i), FALSE)
                                                                             : g_strdup("Folder");
    gint64 mt = 0;
    GDateTime *dt = i ? g_file_info_get_modification_date_time(i) : NULL;
    if (dt) {
        mt = g_date_time_to_unix(dt);
        g_date_time_unref(dt);
    }
    char *when = files_format_time(mt);
    char *markup = g_markup_printf_escaped("<b>%s</b>\n%s\nModified: %s", heading, size, when);
    GtkWidget *l = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(l), markup);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_pack_start(GTK_BOX(box), l, TRUE, TRUE, 0);
    g_free(markup);
    g_free(when);
    g_free(size);
    if (i) g_object_unref(i);
    return box;
}

static void on_rename_clicked(GtkWidget *b, gpointer d)
{
    (void)b;
    gtk_dialog_response(GTK_DIALOG(d), 100 + CONFLICT_RENAME);
}

int files_conflict_dialog(GtkWindow *parent, GFile *src, GFile *dest, gboolean *apply_all, char **new_name)
{
    *apply_all = FALSE;
    *new_name = NULL;
    GFileType st = g_file_query_file_type(src, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL);
    GFileType dt = g_file_query_file_type(dest, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL);
    gboolean merge = st == G_FILE_TYPE_DIRECTORY && dt == G_FILE_TYPE_DIRECTORY;
    char *b = g_file_get_basename(dest);
    char *name = g_filename_display_name(b);
    GFile *dir = g_file_get_parent(dest);
    char *dir_name = dir ? files_location_title(dir) : g_strdup("/");
    GtkWidget *m = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_QUESTION,
                                          GTK_BUTTONS_NONE, merge ? "Merge folder “%s”?" : "Replace “%s”?", name);
    if (merge)
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(m), "A folder with the same name already exists in "
                                                 "“%s”. Merging puts the files of both together (and asks before "
                                                 "replacing any file).", dir_name);
    else gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(m), "An item with the same name already exists in “%s”.",
                                                  dir_name);
    gtk_window_set_title(GTK_WINDOW(m), merge ? "Merge Folders" : "Replace File");
    GtkWidget *area = gtk_message_dialog_get_message_area(GTK_MESSAGE_DIALOG(m));
    gtk_box_pack_start(GTK_BOX(area), file_card(dest, merge ? "Folder already there" : "Already there"), FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(area), file_card(src, merge ? "Merge with" : "Replace with"), FALSE, FALSE, 4);
    GtkWidget *exp = gtk_expander_new_with_mnemonic("Choose another _name");
    GtkWidget *rbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *entry = gtk_entry_new();
    char *suggest = dir ? files_unique_name(dir, name, FALSE) : g_strdup(name);
    gtk_entry_set_text(GTK_ENTRY(entry), suggest);
    gtk_widget_set_hexpand(entry, TRUE);
    gtk_box_pack_start(GTK_BOX(rbox), entry, TRUE, TRUE, 0);
    GtkWidget *rename_btn = gtk_button_new_with_mnemonic("Re_name");
    gtk_box_pack_start(GTK_BOX(rbox), rename_btn, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(exp), rbox);
    gtk_box_pack_start(GTK_BOX(area), exp, FALSE, FALSE, 4);
    GtkWidget *all = gtk_check_button_new_with_mnemonic("_Apply this action to all items");
    gtk_box_pack_start(GTK_BOX(area), all, FALSE, FALSE, 4);
    gtk_dialog_add_button(GTK_DIALOG(m), "_Cancel", 100 + CONFLICT_CANCEL);
    gtk_dialog_add_button(GTK_DIALOG(m), "_Skip", 100 + CONFLICT_SKIP);
    gtk_dialog_add_button(GTK_DIALOG(m), "_Keep Both", 100 + CONFLICT_KEEP_BOTH);
    GtkWidget *rep = gtk_dialog_add_button(GTK_DIALOG(m), merge ? "_Merge" : "_Replace", 100 + CONFLICT_REPLACE);
    gtk_style_context_add_class(gtk_widget_get_style_context(rep), merge ? "suggested-action" : "destructive-action");
    g_signal_connect(rename_btn, "clicked", G_CALLBACK(on_rename_clicked), m);
    g_signal_connect(entry, "activate", G_CALLBACK(on_rename_clicked), m);
    gtk_widget_show_all(m);
    files_log("question: conflict %s in %s", name, dir_name);
    int r;
    for (;;) {
        r = gtk_dialog_run(GTK_DIALOG(m));
        if (r != 100 + CONFLICT_RENAME) break;
        const char *t = gtk_entry_get_text(GTK_ENTRY(entry));
        if (*t && !strchr(t, '/') && strcmp(t, name)) {
            *new_name = g_strdup(t);
            break;
        }
    }
    int answer = r >= 100 && r <= 100 + CONFLICT_RENAME ? r - 100 : CONFLICT_CANCEL;
    *apply_all = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(all)) && answer != CONFLICT_RENAME;
    static const char *const words[] = { "cancel", "skip", "replace", "keep both", "rename" };
    files_log("conflict: %s -> %s%s", name, words[answer], *apply_all ? " (all)" : "");
    gtk_widget_destroy(m);
    g_free(suggest);
    g_free(dir_name);
    if (dir) g_object_unref(dir);
    g_free(name);
    g_free(b);
    return answer;
}

/* ------------------------------------------------------------------ About, shortcuts */
void files_about(GtkWindow *parent)
{
    static const char *const authors[] = { "The HDE / NexWM contributors", NULL };
    gtk_show_about_dialog(parent, "program-name", FILES_TITLE, "version", FILES_VERSION, "logo-icon-name",
                          "system-file-manager", "comments",
                          "The file manager of the Hyggshi Desktop Environment (HDE): folders, tabs, search, the trash, "
                          "thumbnails, drag and drop.",
                          "website", "https://github.com/Hyggshi-OS-Research-Technology/NexWM", "website-label",
                          "Hyggshi OS on GitHub", "license-type", GTK_LICENSE_MIT_X11, "authors", authors, NULL);
    files_log("dialog: About");
}

void files_shortcuts(GtkWindow *parent)
{
    static const char *const keys[][2] = {
        { "Ctrl+N", "New window" }, { "Ctrl+T", "New tab" }, { "Ctrl+W", "Close the tab" },
        { "Ctrl+Page Up / Down", "Previous / next tab" }, { "Alt+Left / Alt+Right", "Back / forward" },
        { "Alt+Up, Backspace", "Up one folder / back" }, { "Alt+Home", "Home folder" }, { "Ctrl+L", "Type a location" },
        { "Ctrl+F", "Search" }, { "F5, Ctrl+R", "Reload" }, { "Ctrl+H", "Show hidden files" },
        { "Ctrl+1 / Ctrl+2", "Icons / list" }, { "Ctrl++ / Ctrl+- / Ctrl+0", "Zoom in / out / normal" },
        { "F9", "Show the sidebar" }, { "Ctrl+Shift+N", "New folder" }, { "F2", "Rename (F1-F3 not sound keys)" },
        { "Ctrl+X / C / V", "Cut / copy / paste" }, { "Ctrl+A", "Select all" }, { "Ctrl+Shift+I", "Invert the selection" },
        { "Delete", "Move to the trash" }, { "Shift+Delete", "Delete permanently" }, { "Ctrl+Z", "Undo" },
        { "Alt+Enter, Ctrl+I", "Properties" }, { "Ctrl+Enter", "Open in a new tab" }, { "Ctrl+D", "Bookmark the folder" },
        { "Menu, Shift+F10", "The menu of the selection" }, { "F10", "The main menu" }, { "Ctrl+Q", "Quit" } };
    GtkWidget *d = gtk_dialog_new_with_buttons("Keyboard Shortcuts", parent, GTK_DIALOG_DESTROY_WITH_PARENT, "_Close",
                                               GTK_RESPONSE_CLOSE, NULL);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 24);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 16);
    int half = (int)(G_N_ELEMENTS(keys) + 1) / 2;
    for (int i = 0; i < (int)G_N_ELEMENTS(keys); i++) {
        GtkWidget *k = gtk_label_new(NULL);
        char *mk = g_markup_printf_escaped("<b>%s</b>", keys[i][0]);
        gtk_label_set_markup(GTK_LABEL(k), mk);
        g_free(mk);
        gtk_label_set_xalign(GTK_LABEL(k), 1);
        GtkWidget *v = gtk_label_new(keys[i][1]);
        gtk_label_set_xalign(GTK_LABEL(v), 0);
        int col = i < half ? 0 : 2, row = i < half ? i : i - half;
        gtk_grid_attach(GTK_GRID(grid), k, col, row, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), v, col + 1, row, 1, 1);
    }
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))), grid, TRUE, TRUE, 0);
    g_signal_connect(d, "response", G_CALLBACK(gtk_widget_destroy), NULL);
    gtk_widget_show_all(d);
    files_log("dialog: Keyboard Shortcuts");
}
