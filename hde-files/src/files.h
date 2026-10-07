/* files.h — Hyggshi Files (hde-files), the file manager of HDE.
 *
 *   main.c       the application: one process, command line (hde-files [--select] PATH...), the
 *                org.freedesktop.FileManager1 D-Bus service ("Show in Folder" of browsers / the Screenshot tool)
 *   window.c     a window: tool bar (back / forward / up, path bar or location, search, view, menu), the places
 *                sidebar, tabs, the trash bar, the status bar (selection, free space, file operations, zoom),
 *                the actions and their keyboard shortcuts, the right-click menus
 *   pane.c       one tab: the folder (loaded asynchronously, watched for changes), icon and list views, sorting,
 *                hidden files, search in subfolders, Trash, Recent, thumbnails, drag and drop, type-ahead
 *   ops.c        file operations in a worker thread: copy, move, link, move to the trash, delete, restore, empty
 *                the trash; conflicts (replace / skip / keep both / merge), progress, cancel, undo (Ctrl+Z);
 *                compress / extract with the usual tools
 *   trash.c      the freedesktop.org trash (~/.local/share/Trash) without GVfs: list, restore, delete, empty
 *   thumbs.c     thumbnails (pictures, and whatever has a thumbnailer: videos, PDF...), ~/.cache/thumbnails
 *   clipboard.c  cut / copy / paste in the format of GNOME / Xfce / the HDE desktop (x-special/gnome-copied-files)
 *   dialogs.c    rename, new folder / document, properties (permissions, open with), open with, run or open
 *   util.c       settings (~/.config/hde/files.ini), sizes and dates, icons, menus, bookmarks, the terminal
 *
 * HDE_DEBUG=1: what happens is logged on stderr ("hde-files: ...") for the tests (tests/smoke.sh).
 */
#ifndef HDE_FILES_H
#define HDE_FILES_H

#include <gtk/gtk.h>
#include <cairo-gobject.h>

#define FILES_APP_ID "org.hyggshi.Files"
#define FILES_TITLE  "Hyggshi Files"
#define FILES_VERSION "1.0"

/* ------------------------------------------------------------------ util.c */
extern gboolean files_debug;
void        files_log(const char *fmt, ...) G_GNUC_PRINTF(1, 2);

typedef struct {
    gboolean show_hidden;
    gboolean list_view;
    int      icon_size;          /* 32 48 64 96 128 */
    int      sort_by;            /* SORT_* */
    gboolean sort_desc;
    gboolean folders_first;
    gboolean show_sidebar;
    int      sidebar_width;
    int      win_w, win_h;
    gboolean maximized;
    gboolean single_click;       /* open items with a single click */
    gboolean thumbnails;
} FilesPrefs;
extern FilesPrefs prefs;
enum { SORT_NAME, SORT_SIZE, SORT_TYPE, SORT_MTIME, SORT_N };
void        prefs_load(void);
void        prefs_save(void);
int         prefs_zoom_level(void);          /* index of prefs.icon_size in files_icon_sizes */
extern const int files_icon_sizes[5];

char       *files_format_time(gint64 t);     /* Today 14:05, Yesterday 09:12, 3 Oct 10:20, 3 Oct 2024 */
char       *files_format_size(goffset size, gboolean exact);
char       *files_display_path(GFile *f);    /* ~/Documents, /etc, Trash, sftp://host/... */
char       *files_location_title(GFile *f);  /* Documents, Home, Trash, Recent, File System */
const char *files_location_icon(GFile *f);   /* icon name for the tab / path bar */
gboolean    files_is_trash(GFile *f);        /* trash:/// (the view of ~/.local/share/Trash) */
gboolean    files_is_recent(GFile *f);       /* recent:/// (GtkRecentManager) */
gboolean    files_in_trash_dir(GFile *f);    /* a file inside ~/.local/share/Trash/files */
GFile      *files_parse_location(const char *text, GFile *relative_to);   /* ~/x, /x, file://, trash:, sftp://... */
char       *files_unique_name(GFile *dir, const char *name, gboolean copy);  /* name (2).txt / name (copy).txt */
void        files_split_ext(const char *name, char **base, const char **ext);
cairo_surface_t *files_icon_surface(GIcon *icon, int size, int scale);       /* cached; new reference */
void        files_icon_cache_clear(void);
void        files_error(GtkWindow *parent, const char *title, const char *detail);
gboolean    files_confirm(GtkWindow *parent, const char *title, const char *detail, const char *ok, gboolean danger);
GtkWidget  *files_menu_item(const char *icons, const char *label, const char *accel);   /* icons: "a|b" first found */
GtkWidget  *files_menu_check(const char *label, gboolean active, const char *accel);
GtkWidget  *files_menu_radio(GSList **group, const char *label, gboolean active);
void        files_menu_sep(GtkWidget *menu);
gboolean    files_open_terminal(GFile *dir, GtkWindow *parent);
gboolean    files_bookmark_add(GFile *f);    /* ~/.config/gtk-3.0/bookmarks (the sidebar shows it at once) */
gboolean    files_bookmark_has(GFile *f);
gboolean    files_can_thumbnail(const char *content_type);
GList      *files_list_copy(GList *files);   /* a list of GFile, each one referenced */
void        files_list_free(GList *files);
char       *files_names_text(GList *files, int max);
char       *files_type_description(const char *content_type);   /* "Folder", "PNG image" */   /* "a.txt, b.png and 3 more" */

/* ------------------------------------------------------------------ the model of a pane */
enum {
    COL_FILE,          /* GFile */
    COL_NAME,          /* display name */
    COL_KEY,           /* collation key (natural order: file2 < file10) */
    COL_GICON,         /* GIcon of the type */
    COL_SURFACE,       /* the icon or the thumbnail at the size of the icon view */
    COL_IS_DIR,
    COL_HIDDEN,        /* .name, name~ */
    COL_SIZE,          /* gint64, -1 for folders */
    COL_SIZE_TEXT,
    COL_MTIME,         /* modified (Trash: deleted) */
    COL_MTIME_TEXT,
    COL_TYPE,          /* content type */
    COL_TYPE_TEXT,     /* its description */
    COL_EXTRA,         /* Trash: original folder; search / Recent: the folder of the file */
    COL_CUT,           /* cut to the clipboard: drawn dimmed */
    COL_TOOLTIP,       /* markup */
    COL_THUMB,         /* 0 not tried, 1 queued, 2 done, 3 failed */
    COL_CAN_EXEC,
    COL_IS_LINK,
    COL_WEIGHT,        /* PANGO_WEIGHT_NORMAL */
    N_COLS
};

typedef struct _FilesWindow FilesWindow;
typedef struct _Pane Pane;

/* ------------------------------------------------------------------ pane.c */
struct _Pane {
    FilesWindow *win;
    GtkWidget *page, *overlay, *stack, *icon_scroll, *icon_view, *list_scroll, *list_view, *message, *spinner;
    GtkWidget *tab_box, *tab_icon, *tab_label;
    GtkTreeViewColumn *col_name, *col_size, *col_type, *col_mtime, *col_extra;
    GtkCellRenderer *icon_text_cell;
    GtkListStore *store;
    GtkTreeModel *filter, *sort;
    GHashTable *rows;            /* uri -> GtkTreeRowReference (store) */
    GFile *location;
    GFileMonitor *monitor;
    GCancellable *cancel;        /* loading, search, thumbnails of the location shown */
    GList *back, *forward;       /* GFile */
    char *search;                /* searching (in the location and its subfolders) */
    gboolean is_trash, is_recent, is_search, loading, mounting;
    int n_items, n_hidden;
    GPtrArray *pending_select;   /* uris to select once they are in the view */
    gboolean pending_scroll;
    guint geom_id, reload_id, sel_id, done_id;
    GString *typeahead;
    gint64 typeahead_time;
    char *error;                 /* the folder could not be read */
    gint64 load_start;
};

Pane       *pane_new(FilesWindow *w);
void        pane_free(Pane *p);
void        pane_go(Pane *p, GFile *location, gboolean add_history);
void        pane_reload(Pane *p);
void        pane_back(Pane *p);
void        pane_forward(Pane *p);
void        pane_up(Pane *p);
void        pane_set_search(Pane *p, const char *text);
void        pane_apply_view(Pane *p);            /* prefs.list_view */
void        pane_apply_prefs(Pane *p);           /* hidden files, sorting */
void        pane_apply_zoom(Pane *p);            /* prefs.icon_size */
void        pane_icons_changed(Pane *p);         /* icon theme changed: render the icons again */
GtkWidget  *pane_view(Pane *p);
void        pane_focus(Pane *p);
GList      *pane_selected_files(Pane *p);        /* GFile list, free with files_list_free */
int         pane_selected_count(Pane *p);
void        pane_selection_summary(Pane *p, int *n, int *dirs, goffset *bytes, char **first);
gboolean    pane_selected_is_dir(Pane *p);       /* exactly one item selected and it is a folder */
char       *pane_selected_type(Pane *p);         /* content type of the first selected item */
void        pane_select_all(Pane *p);
void        pane_unselect_all(Pane *p);
void        pane_invert_selection(Pane *p);
void        pane_select_files(Pane *p, GList *files);     /* now, or as soon as they appear */
void        pane_update_cut(Pane *p);            /* the clipboard changed: dim the cut items */
GFile      *pane_drop_dir(Pane *p);              /* where pasting / dropping goes (NULL: Trash, Recent, search) */
void        pane_item_geometry(Pane *p);         /* HDE_DEBUG: log where the items are on the screen */

/* ------------------------------------------------------------------ window.c */
struct _FilesWindow {
    GtkWidget *window;
    GtkWidget *sidebar, *paned, *notebook;
    GtkWidget *back_btn, *fwd_btn, *up_btn;
    GtkWidget *path_stack, *pathbar_scroll, *pathbar, *location_entry, *search_entry;
    GtkWidget *search_btn, *view_btn, *view_img, *menu_btn;
    GtkWidget *trash_bar, *trash_label, *trash_restore_btn, *trash_empty_btn;
    GtkWidget *status_label, *free_label, *zoom_scale;
    GtkWidget *job_box, *job_label, *job_bar, *job_cancel;
    Pane *pane;                  /* the tab shown */
    guint status_id;
    gboolean in_search_update, in_zoom_update;
    GCancellable *free_cancel;
};

FilesWindow *files_window_new(GtkApplication *app, GFile *location);
void        files_window_open_tab(FilesWindow *w, GFile *location, gboolean switch_to);
void        files_window_close_tab(FilesWindow *w, Pane *p);
void        files_window_update(FilesWindow *w);          /* title, buttons, path bar, sidebar (w->pane) */
void        files_window_update_status(FilesWindow *w);   /* soon */
void        files_window_update_tab(Pane *p);
void        files_window_select(FilesWindow *w, GFile *dir, GList *files);
void        files_window_popup(FilesWindow *w, const GdkEvent *ev, gboolean on_items);
void        files_window_activate_items(FilesWindow *w, GList *files, int how);   /* 0 open, 1 new tab, 2 new window */
void        files_window_start_location(FilesWindow *w, const char *text);        /* the location entry, with TEXT */
void        files_window_selection_changed(FilesWindow *w);
void        files_window_location_changed(Pane *p);   /* the pane went somewhere else */
void        files_window_set_accels(GtkApplication *app);
GList      *files_windows(void);
FilesWindow *files_window_for_widget(GtkWidget *w);
void        files_windows_jobs_changed(void);
void        files_windows_job_done(GFile *dest_dir, GList *created);
void        files_windows_icons_changed(void);
void        files_windows_cut_changed(void);

/* ------------------------------------------------------------------ ops.c */
typedef enum { JOB_COPY, JOB_MOVE, JOB_LINK, JOB_TRASH, JOB_DELETE, JOB_RESTORE, JOB_EMPTY_TRASH } JobKind;
void        files_job_start(GtkWindow *parent, JobKind kind, GList *sources, GFile *dest_dir);
gboolean    files_jobs_running(void);
char       *files_jobs_status(double *fraction);  /* NULL when nothing runs */
void        files_jobs_cancel(void);
char       *files_undo_label(void);               /* NULL: nothing to undo */
void        files_undo(GtkWindow *parent);
void        files_undo_push_rename(GFile *from, GFile *to);
void        files_undo_push_created(GFile *created);
void        files_compress(GtkWindow *parent, GList *files, GFile *dir, const char *format);
void        files_extract(GtkWindow *parent, GList *archives);
gboolean    files_is_archive(const char *name);
gboolean    files_have_zip(void);

/* ------------------------------------------------------------------ trash.c */
char       *trash_dir(void);                      /* ~/.local/share/Trash */
char       *trash_files_dir(void);
char       *trash_info_dir(void);
gboolean    trash_read_info(const char *name, char **orig_path, gint64 *deleted);
gboolean    trash_restore_to(GFile *trashed, GFile *dest, GCancellable *c, GError **error);  /* + its .trashinfo */
gboolean    trash_delete(GFile *trashed, GCancellable *c, GError **error);
gboolean    trash_empty(GCancellable *c, GError **error);
char       *trash_find_newest(const char *orig_path);   /* name in Trash/files of what was trashed last from there */
int         trash_count(void);
gboolean    files_delete_recursive(GFile *f, GCancellable *c, GError **error);

/* ------------------------------------------------------------------ thumbs.c */
/* Thumbnails of pictures (gdk-pixbuf) and of whatever has a thumbnailer installed (/usr/share/thumbnailers:
 * videos, PDF, fonts...), shared with the other programs through ~/.cache/thumbnails (freedesktop.org). DONE runs in
 * the main thread with the picture at SIZE x SIZE at most (NULL: none), unless CANCEL was cancelled. */
typedef void (*ThumbDone)(GFile *file, cairo_surface_t *surface, gpointer data);
void        thumbs_init(void);
gboolean    thumbs_possible(const char *content_type, goffset size);
void        thumbs_request(GFile *file, const char *content_type, gint64 mtime, int size, int scale,
                           GCancellable *cancel, ThumbDone done, gpointer data);

/* ------------------------------------------------------------------ clipboard.c */
void        files_clipboard_init(void);
void        files_clipboard_set(GList *files, gboolean cut);
void        files_clipboard_paste(FilesWindow *w, GFile *dest_dir);
gboolean    files_clipboard_can_paste(void);
gboolean    files_clipboard_is_cut(GFile *f);
void        files_clipboard_copy_text(const char *text);

/* ------------------------------------------------------------------ dialogs.c */
void        files_rename_dialog(FilesWindow *w, GFile *file);
void        files_new_folder_dialog(FilesWindow *w, GFile *dir);
void        files_new_document_dialog(FilesWindow *w, GFile *dir, GFile *template_file);
void        files_properties_dialog(FilesWindow *w, GList *files);
void        files_open_with_dialog(FilesWindow *w, GList *files);
void        files_launch(FilesWindow *w, GList *files, GAppInfo *app);
void        files_open_files(FilesWindow *w, GList *files);   /* default apps, executables, launchers */
int         files_conflict_dialog(GtkWindow *parent, GFile *src, GFile *dest, gboolean *apply_all, char **new_name);
void        files_about(GtkWindow *parent);
void        files_shortcuts(GtkWindow *parent);
GList      *files_templates(void);                /* files of ~/Templates */

enum { CONFLICT_CANCEL, CONFLICT_SKIP, CONFLICT_REPLACE, CONFLICT_KEEP_BOTH, CONFLICT_RENAME };

/* ------------------------------------------------------------------ main.c */
extern GtkApplication *files_app;

#endif
