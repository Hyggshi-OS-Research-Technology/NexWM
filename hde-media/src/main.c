/* main.c — hde-media: the pictures, the music and the video of HDE (its own folder, hde-media/).
 *
 * This first part of the multimedia work is the picture viewer; the media player (music and video, with what this
 * program can do itself plus gstreamer/mpv/ffplay for the rest) and the screen recorder follow in this same folder.
 *
 *   hde-media PICTURE...          show the pictures given (the folder of the first one is the list the arrows walk)
 *   hde-media FOLDER              the pictures of a folder
 *   hde-media                     the Pictures folder of this user
 *   hde-media -s FOLDER           start with the slideshow running
 *   hde-media -i 2 FOLDER         seconds per picture in the slideshow (0.5 .. 600, default 5)
 *   hde-media --sort date FOLDER  the order: name (the natural one: img2 before img10), date or size
 *   hde-media -r FOLDER           also the pictures of the sub-folders (8 deep at most)
 *   hde-media -f PICTURE          start full screen
 *   hde-media --version, --help
 *
 * One process (GtkApplication org.hyggshi.Media): running it again shows the picture in the window that is already
 * open, which is what "Open With" of the file manager and of a browser expects. The theme follows HDE (Light / Dark,
 * the accent colour of Settings).
 *
 * Exit status: 0 shown; 2 nothing to show (the message says what to give it: tests/media-test.sh checks this); 1 a
 * usage mistake. What the window does (the keys, the zoom, the rotation, the slideshow) is checked by that same test,
 * which reads the "hde-media: " lines this program logs.
 */
#include "media.h"
#include "viewer.h"
#include "hde-theme.h"

#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hde-build.h"      /* HDE_RELEASE and HDE_VERSION: what --version prints ("unknown" outside the Makefile) */

#define APP_ID "org.hyggshi.Media"

typedef struct {
    char  *paths[256];
    int    n_paths;
    HdeMediaSort sort;
    int    recursive;
    int    slideshow;
    int    fullscreen;
    double interval;
} MediaOptions;

static HdeMediaViewer *g_win;
static int g_status;                 /* what this process answers with: 0 shown, 2 nothing to show */
static GtkCssProvider *css;

/* The look of the viewer on top of the theme HDE picked: the toolbar and the status line separate themselves from the
 * picture, and the space around a picture that is smaller than the window stays quiet (what the eye should follow is
 * the picture). The accent colour of Settings comes first, exactly as in hde-files. */
static void load_css(void)
{
    HdeThemeInfo ti;
    hde_theme_info_load(&ti);
    char *accent = hde_theme_accent_css(&ti);
    char *data = g_strconcat(
        ".hde-media-toolbar { padding: 4px 6px; border-bottom: 1px solid alpha(@theme_fg_color, 0.12); }"
        ".hde-media-toolbar button { padding: 2px 7px; min-height: 24px; }"
        ".hde-media-statusbar { padding: 2px 6px; min-height: 24px;"
        "  border-top: 1px solid alpha(@theme_fg_color, 0.12); }"
        ".hde-media-status { font-size: 0.93em; opacity: 0.88; }"
        ".hde-media-view { background-color: shade(@theme_bg_color, 0.90); }",
        accent, NULL);
    gtk_css_provider_load_from_data(css, data, -1, NULL);
    g_free(data);
    g_free(accent);
    hde_theme_info_clear(&ti);
}

static void on_theme_changed(gpointer d)
{
    (void)d;
    load_css();
}

static void on_startup(GApplication *app, gpointer d)
{
    (void)app; (void)d;
    gtk_window_set_default_icon_name("image-x-generic");
    hde_theme_apply_process();
    css = gtk_css_provider_new();
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    load_css();
    hde_theme_watch(on_theme_changed, NULL);
}

static void options_init(MediaOptions *o)
{
    memset(o, 0, sizeof *o);
    o->sort = HDE_MEDIA_SORT_NAME;
    o->interval = 5.0;
}

static void options_free(MediaOptions *o)
{
    for (int i = 0; i < o->n_paths; i++) free(o->paths[i]);
    o->n_paths = 0;
}

static void usage(FILE *out)
{
    fprintf(out,
            "Usage: hde-media [OPTION...] [PICTURE|FOLDER...]\n"
            "The pictures of HDE (Hyggshi Media). The media player and the recorder come next in this same folder.\n"
            "\n"
            "  -s, --slideshow        start with the slideshow running\n"
            "  -i, --interval SEC     seconds per picture in the slideshow (0.5 .. 600, default 5)\n"
            "      --sort ORDER       name (default), date or size\n"
            "  -r, --recursive        also the pictures of the sub-folders\n"
            "  -f, --fullscreen       start full screen\n"
            "  -v, --version          print the version\n"
            "  -h, --help             this help\n"
            "\n"
            "Keys: Left/Right previous and next picture, Home/End the first and the last, + and - zoom, 0 fit the\n"
            "window, 1 as it is on disk, r and Shift+R rotate, s and Space the slideshow, i more information, f and\n"
            "F11 full screen, Ctrl+O open a picture, Escape leaves full screen (again: closes), q closes the window.\n"
            "The mouse wheel zooms, a double click goes full screen, dragging moves a picture bigger than the window.\n"
            "\n"
            "A picture opens its folder (the list the arrow keys walk through); a folder opens with its first\n"
            "picture. The pictures of the last run are logged on stdout (the tests read those lines).\n");
}

/* the arguments of one invocation: the first one, or the one a second hde-media handed to the running window */
static int options_parse(MediaOptions *o, char **argv, int argc)
{
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) {
            for (i++; i < argc; i++)
                if (o->n_paths < (int)(sizeof o->paths / sizeof o->paths[0])) o->paths[o->n_paths++] = strdup(argv[i]);
            break;
        }
        if (!strcmp(a, "-s") || !strcmp(a, "--slideshow")) { o->slideshow = 1; continue; }
        if (!strcmp(a, "-f") || !strcmp(a, "--fullscreen")) { o->fullscreen = 1; continue; }
        if (!strcmp(a, "-r") || !strcmp(a, "--recursive")) { o->recursive = 1; continue; }
        if (!strcmp(a, "-i") || !strcmp(a, "--interval")) {
            if (i + 1 >= argc) { fprintf(stderr, "hde-media: --interval needs a number of seconds\n"); return -1; }
            double v = atof(argv[++i]);
            o->interval = v < 0.5 ? 0.5 : (v > 600.0 ? 600.0 : v);
            continue;
        }
        if (!strncmp(a, "--interval=", 11)) {
            double v = atof(a + 11);
            o->interval = v < 0.5 ? 0.5 : (v > 600.0 ? 600.0 : v);
            continue;
        }
        if (!strcmp(a, "--sort")) {
            if (i + 1 >= argc) { fprintf(stderr, "hde-media: --sort needs name, date or size\n"); return -1; }
            a = argv[++i];
        } else if (!strncmp(a, "--sort=", 7)) {
            a += 7;
        } else if (a[0] == '-' && a[1]) {
            fprintf(stderr, "hde-media: unknown option '%s' (see hde-media --help)\n", a);
            return -1;
        } else {
            if (o->n_paths < (int)(sizeof o->paths / sizeof o->paths[0])) o->paths[o->n_paths++] = strdup(a);
            continue;
        }
        if (!strcmp(a, "name")) o->sort = HDE_MEDIA_SORT_NAME;
        else if (!strcmp(a, "date")) o->sort = HDE_MEDIA_SORT_DATE;
        else if (!strcmp(a, "size")) o->sort = HDE_MEDIA_SORT_SIZE;
        else {
            fprintf(stderr, "hde-media: --sort %s is not an order (name, date or size)\n", a);
            return -1;
        }
    }
    return 0;
}

/* the window that shows `o` (a new one, or the one that is already open) */
static void show(GtkApplication *app, MediaOptions *o)
{
    if (g_win && !hde_media_viewer_alive(g_win)) {
        hde_media_viewer_free(g_win);            /* its window was closed: the state is gone with it */
        g_win = NULL;
    }
    if (g_win) {
        if (o->n_paths) hde_media_viewer_open(g_win, o->paths[0], o->paths, o->n_paths);
        hde_media_viewer_present(g_win);
        return;
    }
    g_win = hde_media_viewer_new(app, o->n_paths ? o->paths[0] : NULL, o->paths, o->n_paths, o->sort, o->recursive,
                                 o->interval, o->slideshow, o->fullscreen);
    if (!hde_media_viewer_has_pictures(g_win)) {
        fprintf(stderr, "hde-media: nothing to show in %s\n"
                        "           (give a picture or a folder: hde-media ~/Pictures, or see hde-media --help)\n",
                o->n_paths ? o->paths[0] : "the Pictures folder of this user");
        GtkWidget *widget = hde_media_viewer_widget(g_win);
        if (widget) gtk_widget_destroy(widget);
        g_status = 2;
    }
}

static void activate_cb(GtkApplication *app, gpointer data)
{
    show(app, data);
}

static int command_line_cb(GtkApplication *app, GApplicationCommandLine *cmd, gpointer data)
{
    MediaOptions *defaults = data;
    int argc = 0;
    char **argv = g_application_command_line_get_arguments(cmd, &argc);

    MediaOptions o;
    options_init(&o);
    o.sort = defaults->sort;
    o.interval = defaults->interval;
    o.recursive = defaults->recursive;
    o.slideshow = defaults->slideshow;
    o.fullscreen = defaults->fullscreen;

    g_status = 0;
    if (options_parse(&o, argv, argc) != 0) {
        g_strfreev(argv);
        options_free(&o);
        usage(stderr);
        return 1;
    }
    g_strfreev(argv);
    show(app, &o);
    options_free(&o);
    return g_status;
}

static void shutdown_cb(GtkApplication *app, gpointer data)
{
    (void)app;
    (void)data;
    if (g_win) {
        hde_media_viewer_free(g_win);
        g_win = NULL;
    }
    if (css) {
        g_object_unref(css);
        css = NULL;
    }
}

int main(int argc, char **argv)
{
    /* --version and --help need no window (and a second hde-media should not disturb the one that is running) */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--version")) {
            printf("hde-media (%s) %s %s\n", MEDIA_TITLE, HDE_RELEASE, HDE_VERSION);
            return 0;
        }
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(stdout);
            return 0;
        }
        if (!strcmp(argv[i], "--")) break;
    }

    MediaOptions defaults;
    options_init(&defaults);
    if (options_parse(&defaults, argv, argc) != 0) {
        usage(stderr);
        options_free(&defaults);
        return 1;
    }

    g_set_prgname("hde-media");
    g_set_application_name(MEDIA_TITLE);

    GtkApplication *app = gtk_application_new(APP_ID, G_APPLICATION_HANDLES_COMMAND_LINE);
    /* Light / Dark, the accent colour and the CSS of the viewer: as soon as GTK is up, and again on every change */
    g_signal_connect(app, "startup", G_CALLBACK(on_startup), NULL);
    g_signal_connect(app, "activate", G_CALLBACK(activate_cb), &defaults);
    g_signal_connect(app, "command-line", G_CALLBACK(command_line_cb), &defaults);
    g_signal_connect(app, "shutdown", G_CALLBACK(shutdown_cb), NULL);

    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    options_free(&defaults);
    return status;
}
