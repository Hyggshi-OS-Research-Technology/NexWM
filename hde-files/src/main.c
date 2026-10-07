/* main.c — Hyggshi Files: the application.
 *
 *   hde-files                      a window on the home folder
 *   hde-files PATH|URI...          windows / tabs on these folders (a file: its folder, with the file selected)
 *   hde-files --select PATH...     the folders of these items, with the items selected
 *   hde-files --new-window, --quit, --version, --help
 *
 * One process (GtkApplication org.hyggshi.Files): running it again opens a window in the running one. While it runs it
 * also answers org.freedesktop.FileManager1 (ShowFolders / ShowItems / ShowItemProperties: "Show in Folder" of
 * browsers, download managers and the HDE Screenshot tool). The theme follows HDE (Light / Dark, accent colour).
 */
#include "files.h"
#include "hde-theme.h"
#include <stdio.h>
#include <string.h>

GtkApplication *files_app;
static GtkCssProvider *css;
static guint fm1_owner, fm1_object;

static const char fm1_xml[] =
    "<node><interface name='org.freedesktop.FileManager1'>"
    "<method name='ShowFolders'><arg type='as' name='URIs' direction='in'/><arg type='s' name='StartupId' direction='in'/></method>"
    "<method name='ShowItems'><arg type='as' name='URIs' direction='in'/><arg type='s' name='StartupId' direction='in'/></method>"
    "<method name='ShowItemProperties'><arg type='as' name='URIs' direction='in'/><arg type='s' name='StartupId' direction='in'/>"
    "</method></interface></node>";

static void load_css(void)
{
    HdeThemeInfo ti;
    hde_theme_info_load(&ti);
    char *accent = hde_theme_accent_css(&ti);
    char *data = g_strconcat(
        ".files-toolbar { padding: 5px 6px; border-bottom: 1px solid alpha(@theme_fg_color, 0.12); }"
        ".files-pathbox { border: 1px solid alpha(@theme_fg_color, 0.16); border-radius: 6px;"
        "  background-color: @theme_base_color; padding: 0 2px; }"
        ".files-pathbar button { padding: 1px 7px; min-height: 22px; border-radius: 4px; }"
        ".files-pathbar button.current label { font-weight: bold; }"
        ".files-pathbar .sep { opacity: 0.45; margin: 0 1px; }"
        ".files-status { padding: 2px 10px; min-height: 24px; border-top: 1px solid alpha(@theme_fg_color, 0.12); }"
        ".files-status label { font-size: 0.93em; }"
        ".files-message { font-size: 1.25em; opacity: 0.55; }"
        ".files-tab-close { padding: 0; min-width: 18px; min-height: 18px; }"
        ".files-trashbar { padding: 6px 10px; background-color: alpha(@theme_selected_bg_color, 0.10);"
        "  border-bottom: 1px solid alpha(@theme_fg_color, 0.10); }"
        "notebook.files-tabs > header { border-bottom: 1px solid alpha(@theme_fg_color, 0.12); }",
        accent, NULL);
    gtk_css_provider_load_from_data(css, data, -1, NULL);
    files_log("theme: accent %s %s", ti.accent_auto ? "automatic" : "chosen", ti.accent);
    g_free(data);
    g_free(accent);
    hde_theme_info_clear(&ti);
}

static void on_theme_changed(gpointer d)
{
    (void)d;
    load_css();
}

static void on_icons_changed(GtkIconTheme *t, gpointer d)
{
    (void)t; (void)d;
    files_windows_icons_changed();
}

/* ------------------------------------------------------------------ showing things */
static FilesWindow *window_on(GFile *dir)
{
    for (GList *l = files_windows(); l; l = l->next) {
        FilesWindow *w = l->data;
        if (w->pane && w->pane->location && g_file_equal(w->pane->location, dir) && !w->pane->is_search) return w;
    }
    return NULL;
}

static void startup_id(FilesWindow *w, const char *id)
{
    if (id && *id) gtk_window_set_startup_id(GTK_WINDOW(w->window), id);
    else gtk_window_present(GTK_WINDOW(w->window));
}

/* the items in their folders (one window per folder), selected; PROPS: and their Properties */
static void show_items(GList *files, const char *id, gboolean props)
{
    GPtrArray *dirs = g_ptr_array_new_with_free_func(g_object_unref);
    GPtrArray *lists = g_ptr_array_new();
    for (GList *l = files; l; l = l->next) {
        GFile *dir = g_file_get_parent(l->data);
        if (!dir) dir = g_object_ref(l->data);
        guint i;
        for (i = 0; i < dirs->len; i++)
            if (g_file_equal(dirs->pdata[i], dir)) break;
        if (i == dirs->len) {
            g_ptr_array_add(dirs, g_object_ref(dir));
            g_ptr_array_add(lists, NULL);
        }
        lists->pdata[i] = g_list_append(lists->pdata[i], l->data);
        g_object_unref(dir);
    }
    for (guint i = 0; i < dirs->len; i++) {
        GFile *dir = dirs->pdata[i];
        GList *list = lists->pdata[i];
        FilesWindow *w = window_on(dir);
        if (!w) w = files_window_new(files_app, dir);
        files_window_select(w, dir, list);
        startup_id(w, id);
        char *where = files_display_path(dir), *names = files_names_text(list, 5);
        files_log("show items: %s (%s)", where, names);
        g_free(names);
        g_free(where);
        if (props) files_properties_dialog(w, list);
        g_list_free(list);
    }
    g_ptr_array_unref(lists);
    g_ptr_array_unref(dirs);
}

/* folders: a window on the first one, tabs for the others; files among them: shown selected */
static void open_locations(GList *locations, const char *id, gboolean new_window)
{
    FilesWindow *w = NULL;
    GList *files = NULL;
    for (GList *l = locations; l; l = l->next) {
        GFileType t = g_file_query_file_type(l->data, G_FILE_QUERY_INFO_NONE, NULL);
        gboolean place = !g_file_is_native(l->data) || t == G_FILE_TYPE_DIRECTORY || t == G_FILE_TYPE_UNKNOWN;
        if (!place) {
            files = g_list_append(files, l->data);
            continue;
        }
        if (!w && !new_window) w = window_on(l->data);
        if (!w) w = files_window_new(files_app, l->data);
        else if (!window_on(l->data) || new_window) files_window_open_tab(w, l->data, TRUE);
        new_window = FALSE;
        startup_id(w, id);
    }
    if (files) show_items(files, id, FALSE);
    g_list_free(files);
}

/* ------------------------------------------------------------------ org.freedesktop.FileManager1 */
static void fm1_call(GDBusConnection *c, const char *sender, const char *path, const char *iface, const char *method,
                     GVariant *params, GDBusMethodInvocation *inv, gpointer d)
{
    (void)c; (void)path; (void)iface; (void)d;
    const char **uris = NULL;
    const char *id = NULL;
    g_variant_get(params, "(^a&s&s)", &uris, &id);
    GList *files = NULL;
    for (int i = 0; uris && uris[i]; i++)
        files = g_list_append(files, strstr(uris[i], "://") || g_str_has_prefix(uris[i], "file:") ?
                              g_file_new_for_uri(uris[i]) : g_file_new_for_path(uris[i]));
    files_log("FileManager1.%s from %s: %u item(s)", method, sender ? sender : "?", g_list_length(files));
    if (!strcmp(method, "ShowFolders")) open_locations(files, id, TRUE);
    else show_items(files, id, !strcmp(method, "ShowItemProperties"));
    files_list_free(files);
    g_free(uris);
    g_dbus_method_invocation_return_value(inv, NULL);
}

static const GDBusInterfaceVTable fm1_vtable = { fm1_call, NULL, NULL, { 0 } };

static void fm1_start(GApplication *app)
{
    GDBusConnection *bus = g_application_get_dbus_connection(app);
    if (!bus || g_getenv("HDE_FILES_NO_FILEMANAGER1")) return;
    GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(fm1_xml, NULL);
    if (!node) return;
    GError *e = NULL;
    fm1_object = g_dbus_connection_register_object(bus, "/org/freedesktop/FileManager1", node->interfaces[0], &fm1_vtable,
                                                   NULL, NULL, &e);
    if (!fm1_object) {
        g_printerr("hde-files: FileManager1: %s\n", e->message);
        g_clear_error(&e);
    } else fm1_owner = g_bus_own_name_on_connection(bus, "org.freedesktop.FileManager1", G_BUS_NAME_OWNER_FLAGS_NONE,
                                                    NULL, NULL, NULL, NULL);
    g_dbus_node_info_unref(node);
}

/* ------------------------------------------------------------------ the application */
static void on_quit(GSimpleAction *a, GVariant *v, gpointer d)
{
    (void)a; (void)v; (void)d;
    GList *ws = g_list_copy(files_windows());          /* (file operations still running finish first) */
    for (GList *l = ws; l; l = l->next) gtk_widget_destroy(((FilesWindow *)l->data)->window);
    g_list_free(ws);
}

static void on_startup(GApplication *app, gpointer d)
{
    (void)d;
    g_set_application_name(FILES_TITLE);
    gtk_window_set_default_icon_name("system-file-manager");
    prefs_load();
    hde_theme_apply_process();
    css = gtk_css_provider_new();
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    load_css();
    hde_theme_watch(on_theme_changed, NULL);
    g_signal_connect(gtk_icon_theme_get_default(), "changed", G_CALLBACK(on_icons_changed), NULL);
    files_clipboard_init();
    thumbs_init();
    GSimpleAction *q = g_simple_action_new("quit", NULL);
    g_signal_connect(q, "activate", G_CALLBACK(on_quit), app);
    g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(q));
    g_object_unref(q);
    files_window_set_accels(GTK_APPLICATION(app));
    fm1_start(app);
}

static void on_activate(GApplication *app, gpointer d)
{
    (void)d;
    files_window_new(GTK_APPLICATION(app), NULL);
}

static int on_command_line(GApplication *app, GApplicationCommandLine *cl, gpointer d)
{
    (void)d;
    int argc = 0;
    char **argv = g_application_command_line_get_arguments(cl, &argc);
    gboolean select = FALSE, new_window = FALSE, quit = FALSE, files_only = FALSE;
    GList *files = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!files_only && a[0] == '-' && a[1]) {
            if (!strcmp(a, "--")) files_only = TRUE;
            else if (!strcmp(a, "--select") || !strcmp(a, "-s")) select = TRUE;
            else if (!strcmp(a, "--new-window") || !strcmp(a, "-w")) new_window = TRUE;
            else if (!strcmp(a, "--quit") || !strcmp(a, "-q")) quit = TRUE;
            else g_application_command_line_printerr(cl, "hde-files: unknown option %s (see hde-files --help)\n", a);
            continue;
        }
        files = g_list_append(files, g_application_command_line_create_file_for_arg(cl, a));
    }
    GVariant *pd = g_application_command_line_get_platform_data(cl);
    const char *id = NULL;
    if (pd) g_variant_lookup(pd, "desktop-startup-id", "&s", &id);
    if (quit) {
        files_log("quit");
        g_action_group_activate_action(G_ACTION_GROUP(app), "quit", NULL);
    } else if (!files) {
        FilesWindow *w = files_window_new(GTK_APPLICATION(app), NULL);
        startup_id(w, id);
    } else if (select) show_items(files, id, FALSE);
    else open_locations(files, id, new_window);
    files_list_free(files);
    if (pd) g_variant_unref(pd);
    g_strfreev(argv);
    return 0;
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--version") || !strcmp(argv[i], "-v")) {
            printf("hde-files (%s) %s\n", FILES_TITLE, FILES_VERSION);
            return 0;
        }
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("Usage: hde-files [OPTION...] [FOLDER|FILE|URI...]\n"
                   "Hyggshi Files, the file manager of HDE.\n\n"
                   "  -s, --select       show the folders of the items given, with the items selected\n"
                   "  -w, --new-window   open a new window even if one shows the folder already\n"
                   "  -q, --quit         close the windows of the running Hyggshi Files\n"
                   "  -v, --version      print the version\n"
                   "  -h, --help         this help\n\n"
                   "A file given opens its folder with the file selected. Places: ~/Documents, /etc, trash:, recent:,\n"
                   "sftp://host/path, smb://server/share (the last ones with GVfs). HDE_DEBUG=1 logs on stderr.\n");
            return 0;
        }
        if (!strcmp(argv[i], "--")) break;
    }
    files_debug = g_getenv("HDE_DEBUG") != NULL;
    g_set_prgname("hde-files");
    files_app = gtk_application_new(FILES_APP_ID, G_APPLICATION_HANDLES_COMMAND_LINE);
    g_signal_connect(files_app, "startup", G_CALLBACK(on_startup), NULL);
    g_signal_connect(files_app, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(files_app, "command-line", G_CALLBACK(on_command_line), NULL);
    int r = g_application_run(G_APPLICATION(files_app), argc, argv);
    if (fm1_owner) g_bus_unown_name(fm1_owner);
    g_object_unref(files_app);
    return r;
}
