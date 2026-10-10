/* disks.c — the drives in the sidebar of Hyggshi Files: the USB stick, the SD card and the disc that are plugged
 * in, where they are mounted, and the button to get them out again safely.
 *
 * HDE asks udisks2 itself (src/hde-udisks.c) instead of leaving it to GTK's places sidebar, which only shows a
 * drive when a GVfs volume monitor happens to be running — so a stick is shown, mounted and removed here on a
 * machine where GVfs is not installed at all. Which of the disks udisks2 knows about are worth showing, and
 * what they are called, is decided in src/hde-disks.c.
 *
 * Mounting a stick when it is plugged in is hde-automount's job, not this file's: this is the list in the
 * sidebar, and it mounts a drive only when the user clicks it.
 */
#include "files.h"

#include "hde-disks.h"
#include "hde-udisks.h"

/* One monitor for all of the open windows: one connection to udisks2, and every sidebar rebuilt when it says
 * something changed. */
static HdeDisks *disks;
static GList    *views;

/* ---------------------------------------------------------------- windows */

/* A window can be closed while udisks2 is still answering, and the list is rebuilt out from under the row that
 * was clicked (mounting changes the disk, and the monitor says so) — so a row's widget is never kept past the
 * call it came from, and a FilesWindow is looked up again before it is used. */
static gboolean window_alive(FilesWindow *w)
{
    return w && g_list_find(files_windows(), w) != NULL;
}

static FilesWindow *a_window(FilesWindow *w)
{
    if (window_alive(w)) return w;
    GList *all = files_windows();
    return all ? (FilesWindow *)all->data : NULL;
}

static void open_in(FilesWindow *w, const char *path)
{
    if (!w || !path || !*path) return;
    GFile *f = g_file_new_for_path(path);
    files_log("drives: opening %s", path);
    files_window_open_tab(w, f, TRUE);
    g_object_unref(f);
}

/* ---------------------------------------------------------------- clicking a drive */

static void on_drive_done(HdeDisks *d, const char *object_path, gboolean ok, const char *message,
                          const char *mount_point, gpointer data)
{
    (void)d; (void)object_path; (void)mount_point; (void)data;
    if (ok) return;
    files_log("drives: %s", message ? message : "that did not work");
}

static void on_mounted(HdeDisks *d, const char *object_path, gboolean ok, const char *message,
                       const char *mount_point, gpointer data)
{
    (void)d; (void)object_path;
    FilesWindow *w = a_window((FilesWindow *)data);
    if (!ok) {
        files_log("drives: %s", message ? message : "the drive could not be mounted");
        files_error(w ? GTK_WINDOW(w->window) : NULL, "The drive could not be mounted",
                    message ? message : "udisks2 would not mount it.");
        return;
    }
    open_in(w, mount_point);
}

static void on_open_clicked(GtkButton *b, gpointer data)
{
    (void)data;
    const char *object_path = g_object_get_data(G_OBJECT(b), "hde-drive-path");
    if (!object_path) return;
    HdeDisk *d = hde_disks_find(disks, object_path);
    if (!d) return;
    FilesWindow *w = files_window_for_widget(GTK_WIDGET(b));
    if (d->mounted) { open_in(w, d->mount_point); return; }
    /* Not mounted: mount it and go there. (hde-automount leaves alone what the user unmounted — clicking is
     * the user asking for it.) */
    hde_disks_mount_async(disks, object_path, on_mounted, w);
}

static void on_eject_clicked(GtkButton *b, gpointer data)
{
    (void)data;
    const char *object_path = g_object_get_data(G_OBJECT(b), "hde-drive-path");
    if (object_path) hde_disks_eject_async(disks, object_path, on_drive_done, NULL);
}

/* ---------------------------------------------------------------- the rows */

/* A row is a button that opens the drive, with the "safely remove" button beside it. The list box itself
 * activates nothing (gtk_list_box_row_set_activatable): two clickable things in one row, and the one that
 * answers is the one the mouse was over. */
static GtkWidget *row_new(const HdeDisk *d)
{
    char title[HDE_DISK_NAME_MAX * 2], subtitle[256];
    hde_disk_title(d, title, sizeof title);
    hde_disk_subtitle(d, subtitle, sizeof subtitle);

    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(hbox, 8);
    gtk_widget_set_margin_end(hbox, 8);
    gtk_widget_set_margin_top(hbox, 4);
    gtk_widget_set_margin_bottom(hbox, 4);

    GtkWidget *open = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(open), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(open, d->mounted ? d->mount_point
                                                 : "Not mounted: click to mount it and open it");
    GtkWidget *inner = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    GtkWidget *img = gtk_image_new_from_icon_name(hde_disk_icon(d), GTK_ICON_SIZE_MENU);
    gtk_box_pack_start(GTK_BOX(inner), img, FALSE, FALSE, 0);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *t = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(t), 0);
    gtk_label_set_ellipsize(GTK_LABEL(t), PANGO_ELLIPSIZE_END);
    GtkWidget *s = gtk_label_new(subtitle);
    gtk_label_set_xalign(GTK_LABEL(s), 0);
    gtk_label_set_ellipsize(GTK_LABEL(s), PANGO_ELLIPSIZE_END);
    gtk_style_context_add_class(gtk_widget_get_style_context(s), "dim-label");
    gtk_box_pack_start(GTK_BOX(vbox), t, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), s, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(inner), vbox, TRUE, TRUE, 0);

    gtk_container_add(GTK_CONTAINER(open), inner);
    g_object_set_data_full(G_OBJECT(open), "hde-drive-path", g_strdup(d->object_path), g_free);
    g_signal_connect(open, "clicked", G_CALLBACK(on_open_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(hbox), open, TRUE, TRUE, 0);

    if (hde_disk_can_eject(d)) {
        GtkWidget *btn = gtk_button_new_from_icon_name("media-eject-symbolic", GTK_ICON_SIZE_MENU);
        gtk_widget_set_tooltip_text(btn, d->optical ? "Eject the disc" : "Safely remove the drive");
        gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);
        g_object_set_data_full(G_OBJECT(btn), "hde-drive-path", g_strdup(d->object_path), g_free);
        g_signal_connect(btn, "clicked", G_CALLBACK(on_eject_clicked), NULL);
        gtk_box_pack_start(GTK_BOX(hbox), btn, FALSE, FALSE, 0);
    }

    gtk_container_add(GTK_CONTAINER(row), hbox);
    return row;
}

/* ---------------------------------------------------------------- the list */

static void rebuild(GtkWidget *view)
{
    GtkWidget *list = g_object_get_data(G_OBJECT(view), "hde-drives-list");
    if (!list) return;

    GList *old = gtk_container_get_children(GTK_CONTAINER(list));
    for (GList *l = old; l; l = l->next) gtk_container_remove(GTK_CONTAINER(list), GTK_WIDGET(l->data));
    g_list_free(old);

    GPtrArray *all = hde_disks_list(disks);
    int shown = 0;
    if (all) {
        for (guint i = 0; i < all->len; ++i) {
            HdeDisk *d = g_ptr_array_index(all, i);
            if (!hde_disk_should_show(d)) continue;
            gtk_container_add(GTK_CONTAINER(list), row_new(d));
            ++shown;
        }
    }
    gtk_widget_show_all(list);
    /* No drives, no "Drives": an empty heading over an empty list is worse than nothing. */
    gtk_widget_set_visible(view, shown > 0);
}

static void on_changed(HdeDisks *d, gpointer data)
{
    (void)d; (void)data;
    for (GList *l = views; l; l = l->next) rebuild(GTK_WIDGET(l->data));
}

static void on_view_destroy(GtkWidget *view, gpointer data)
{
    (void)data;
    views = g_list_remove(views, view);
}

GtkWidget *files_drives_new(void)
{
    GtkWidget *view = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *heading = gtk_label_new("Drives");
    gtk_label_set_xalign(GTK_LABEL(heading), 0);
    gtk_widget_set_margin_start(heading, 12);
    gtk_widget_set_margin_top(heading, 8);
    gtk_widget_set_margin_bottom(heading, 4);
    gtk_style_context_add_class(gtk_widget_get_style_context(heading), "dim-label");

    GtkWidget *list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);

    gtk_box_pack_start(GTK_BOX(view), heading, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(view), list, FALSE, FALSE, 0);
    g_object_set_data(G_OBJECT(view), "hde-drives-list", list);
    g_object_set_data(G_OBJECT(view), "hde-drives-heading", heading);
    gtk_widget_show_all(view);              /* (before no_show_all: show_all does nothing after it) */
    gtk_widget_hide(view);
    gtk_widget_set_no_show_all(view, TRUE);

    if (!disks) disks = hde_disks_new(on_changed, NULL);
    views = g_list_prepend(views, view);
    g_signal_connect(view, "destroy", G_CALLBACK(on_view_destroy), NULL);
    rebuild(view);              /* empty until udisks2 has answered; on_changed fills it in */
    return view;
}
