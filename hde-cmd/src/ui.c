/* ui.c — HDE Cmd's GTK3 window, PTY session, keyboard, renderer and optional VTE adapter. */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "app.h"
#include "vt.h"

#include <gtk/gtk.h>
#include <glib-unix.h>
#include <gdk/gdkkeysyms.h>
#include <cairo.h>
#include <pango/pangocairo.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#ifdef HDE_CMD_HAVE_VTE
#include <vte/vte.h>
#endif

#define APP_ID "org.hyggshi.Cmd"
#define DEFAULT_FONT_SIZE 11
#define MIN_FONT_SIZE 7
#define MAX_FONT_SIZE 30
#define PAD_X 12
#define PAD_Y 9
#define DEFAULT_COLUMNS 80
#define DEFAULT_ROWS 24
#define PTY_BUFFER_SIZE 16384

typedef struct {
    GtkApplication *application;
    GtkWidget *window;
    GtkWidget *terminal_widget;
    GtkWidget *header;
    GtkWidget *context_menu;
    HdeCmdVt *vt;
    HdeCmdOptions options;
    const char *program_name;

    int pty_fd;
    GPid child_pid;
    guint pty_watch;
    guint child_watch;
    guint blink_watch;
    int child_exited;
    int focused;
    int cursor_phase;
    int fullscreen;

    int rows;
    int columns;
    int cell_width;
    int cell_height;
    int font_size;
    PangoFontDescription *font;
    char *title;

    int selecting;
    int selection_x1, selection_y1;
    int selection_x2, selection_y2;

#ifdef HDE_CMD_HAVE_VTE
    VteTerminal *vte;
    char **spawn_argv;
    char **spawn_environment;
#endif
} CmdWindow;

static const double DEFAULT_BG_R = 0.090;
static const double DEFAULT_BG_G = 0.102;
static const double DEFAULT_BG_B = 0.125;
static const double DEFAULT_FG_R = 0.855;
static const double DEFAULT_FG_G = 0.878;
static const double DEFAULT_FG_B = 0.914;

static void menu_new(GtkMenuItem *item, gpointer userdata);
static void menu_open_folder(GtkMenuItem *item, gpointer userdata);

static void queue_draw(CmdWindow *app)
{
    if (app->terminal_widget && !app->options.use_vte) gtk_widget_queue_draw(app->terminal_widget);
}

static void set_title(CmdWindow *app, const char *title)
{
    if (!app || !app->window || !title || !*title) return;
    g_free(app->title);
    app->title = g_strdup(title);
    gtk_window_set_title(GTK_WINDOW(app->window), title);
    if (app->header) gtk_header_bar_set_title(GTK_HEADER_BAR(app->header), title);
}

static void pty_write(CmdWindow *app, const char *bytes, size_t length)
{
    if (app->pty_fd < 0 || !bytes || !length) return;
    size_t at = 0;
    while (at < length) {
        ssize_t n = write(app->pty_fd, bytes + at, length - at);
        if (n > 0) at += (size_t)n;
        else if (n < 0 && errno == EINTR) continue;
        else break; /* PTY input is nonblocking; a full buffer is not a reason to freeze the GTK loop. */
    }
}

static void vt_reply(void *userdata, const char *bytes, size_t length)
{
    pty_write(userdata, bytes, length);
}

static void vt_title(void *userdata, const char *title)
{
    CmdWindow *app = userdata;
    set_title(app, title);
}

static void close_pty(CmdWindow *app, int remove_source)
{
    if (remove_source && app->pty_watch) g_source_remove(app->pty_watch);
    app->pty_watch = 0;
    if (app->pty_fd >= 0) close(app->pty_fd);
    app->pty_fd = -1;
}

static gboolean pty_ready(gint fd, GIOCondition condition, gpointer userdata)
{
    CmdWindow *app = userdata;
    char buffer[PTY_BUFFER_SIZE];
    int saw_eof = 0;
    for (;;) {
        ssize_t n = read(fd, buffer, sizeof buffer);
        if (n > 0) {
            hde_cmd_vt_feed(app->vt, buffer, (size_t)n, vt_reply, vt_title, app);
            continue;
        }
        if (n == 0 || (n < 0 && errno == EIO)) {
            saw_eof = 1;
            break;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        saw_eof = 1;
        break;
    }
    queue_draw(app);
    if (saw_eof || (condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL))) {
        app->pty_watch = 0;
        close_pty(app, 0);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void reset_child_signals(void)
{
    const int signals[] = { SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGPIPE, SIGCHLD, SIGTSTP, SIGTTIN, SIGTTOU, SIGWINCH };
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    for (size_t i = 0; i < sizeof signals / sizeof signals[0]; i++) sigaction(signals[i], &action, NULL);
    sigset_t empty;
    sigemptyset(&empty);
    sigprocmask(SIG_SETMASK, &empty, NULL);
}

static const char *default_shell(void)
{
    const char *shell = getenv("SHELL");
    if (shell && shell[0] == '/' && access(shell, X_OK) == 0) return shell;
    if (access("/bin/bash", X_OK) == 0) return "/bin/bash";
    if (access("/bin/sh", X_OK) == 0) return "/bin/sh";
    return "sh";
}

static void run_child(const HdeCmdOptions *options)
{
    const char *cwd = options->working_directory;
    if (cwd && *cwd && chdir(cwd) != 0)
        dprintf(STDERR_FILENO, "hde-cmd: cannot change directory to %s: %s\r\n", cwd, strerror(errno));
    setenv("TERM", "xterm-256color", 1);
    setenv("COLORTERM", "truecolor", 1);
    setenv("TERM_PROGRAM", "hde-cmd", 1);
    reset_child_signals();

    if (options->command && options->command[0]) {
        execvp(options->command[0], options->command);
        dprintf(STDERR_FILENO, "hde-cmd: cannot run %s: %s\r\n", options->command[0], strerror(errno));
        _exit(127);
    }

    const char *shell = default_shell();
    const char *base = strrchr(shell, '/');
    base = base ? base + 1 : shell;
    char *const argv[] = { (char *)base, (char *)"-l", NULL };
    execv(shell, argv);
    dprintf(STDERR_FILENO, "hde-cmd: cannot start shell %s: %s\r\n", shell, strerror(errno));
    _exit(127);
}

static void child_exited(GPid pid, gint status, gpointer userdata)
{
    CmdWindow *app = userdata;
    (void)status;
    app->child_watch = 0;
    app->child_exited = 1;
    if (app->child_pid == pid) app->child_pid = 0;
    g_spawn_close_pid(pid);
    if (!app->options.use_vte && app->window) {
        char *title = g_strdup_printf("%s — finished", app->title ? app->title : "HDE Cmd");
        set_title(app, title);
        g_free(title);
    }
}

static int start_internal_session(CmdWindow *app)
{
    struct winsize size;
    memset(&size, 0, sizeof size);
    size.ws_row = (unsigned short)app->rows;
    size.ws_col = (unsigned short)app->columns;
    int master = -1;
    pid_t pid = forkpty(&master, NULL, NULL, &size);
    if (pid < 0) {
        g_printerr("hde-cmd: cannot create a pseudo-terminal: %s\n", g_strerror(errno));
        return -1;
    }
    if (pid == 0) run_child(&app->options);

    app->pty_fd = master;
    app->child_pid = (GPid)pid;
    int flags = fcntl(master, F_GETFL, 0);
    if (flags >= 0) fcntl(master, F_SETFL, flags | O_NONBLOCK);
    app->pty_watch = g_unix_fd_add_full(G_PRIORITY_DEFAULT, master,
                                        G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
                                        pty_ready, app, NULL);
    app->child_watch = g_child_watch_add((GPid)pid, child_exited, app);
    return 0;
}

static void update_pty_size(CmdWindow *app)
{
    if (app->options.use_vte || !app->vt || !app->terminal_widget) return;
    GtkAllocation allocation;
    gtk_widget_get_allocation(app->terminal_widget, &allocation);
    int columns = (allocation.width - 2 * PAD_X) / (app->cell_width > 0 ? app->cell_width : 1);
    int rows = (allocation.height - 2 * PAD_Y) / (app->cell_height > 0 ? app->cell_height : 1);
    columns = CLAMP(columns, 2, 500);
    rows = CLAMP(rows, 2, 300);
    if (columns == app->columns && rows == app->rows) return;
    app->columns = columns;
    app->rows = rows;
    hde_cmd_vt_resize(app->vt, rows, columns);
    if (app->pty_fd >= 0) {
        struct winsize size;
        memset(&size, 0, sizeof size);
        size.ws_row = (unsigned short)rows;
        size.ws_col = (unsigned short)columns;
        ioctl(app->pty_fd, TIOCSWINSZ, &size); /* the kernel sends SIGWINCH to the foreground process group */
    }
    queue_draw(app);
}

static void on_size_allocate(GtkWidget *widget, GtkAllocation *allocation, gpointer userdata)
{
    (void)widget;
    (void)allocation;
    update_pty_size(userdata);
}

static void rgba_from_color(uint32_t color, double *red, double *green, double *blue)
{
    *red = (double)((color >> 16) & 0xff) / 255.0;
    *green = (double)((color >> 8) & 0xff) / 255.0;
    *blue = (double)(color & 0xff) / 255.0;
}

static int selection_contains(const CmdWindow *app, int row, int column)
{
    if (!app->selecting) return 0;
    int ay = app->selection_y1, ax = app->selection_x1;
    int by = app->selection_y2, bx = app->selection_x2;
    if (ay > by || (ay == by && ax > bx)) {
        int t = ay; ay = by; by = t;
        t = ax; ax = bx; bx = t;
    }
    if (row < ay || row > by) return 0;
    if (ay == by) return column >= ax && column <= bx;
    if (row == ay) return column >= ax;
    if (row == by) return column <= bx;
    return 1;
}

static void append_cell_text(GString *text, const HdeCmdVtCell *cell)
{
    uint32_t cp = cell->codepoint ? cell->codepoint : ' ';
    g_string_append_unichar(text, (gunichar)cp);
    for (int i = 0; i < cell->combining_count; i++)
        g_string_append_unichar(text, (gunichar)cell->combining[i]);
}

static void add_foreground_attribute(PangoAttrList *attributes, guint16 red, guint16 green, guint16 blue,
                                    guint start, guint end)
{
    if (end <= start) return;
    PangoAttribute *attribute = pango_attr_foreground_new(red, green, blue);
    attribute->start_index = start;
    attribute->end_index = end;
    pango_attr_list_insert(attributes, attribute);
}

static gboolean draw_terminal(GtkWidget *widget, cairo_t *cr, gpointer userdata)
{
    CmdWindow *app = userdata;
    if (!app->vt) return FALSE;
    update_pty_size(app);

    cairo_set_source_rgb(cr, DEFAULT_BG_R, DEFAULT_BG_G, DEFAULT_BG_B);
    cairo_paint(cr);

    int rows = hde_cmd_vt_rows(app->vt);
    int columns = hde_cmd_vt_columns(app->vt);
    for (int y = 0; y < rows; y++) {
        const HdeCmdVtCell *line = hde_cmd_vt_line(app->vt, y);
        for (int x = 0; x < columns; x++) {
            const HdeCmdVtCell *cell = &line[x];
            int inverse = (cell->attributes & HDE_CMD_VT_INVERSE) != 0;
            uint32_t bg = inverse ? cell->foreground : cell->background;
            double r = DEFAULT_BG_R, g = DEFAULT_BG_G, b = DEFAULT_BG_B;
            if (bg == HDE_CMD_VT_DEFAULT_COLOR && inverse) {
                r = DEFAULT_FG_R; g = DEFAULT_FG_G; b = DEFAULT_FG_B;
            } else if (bg != HDE_CMD_VT_DEFAULT_COLOR) {
                rgba_from_color(bg, &r, &g, &b);
            }
            if (bg != HDE_CMD_VT_DEFAULT_COLOR || inverse) {
                cairo_set_source_rgb(cr, r, g, b);
                cairo_rectangle(cr, PAD_X + x * app->cell_width, PAD_Y + y * app->cell_height,
                                app->cell_width + 1, app->cell_height + 1);
                cairo_fill(cr);
            }
            if (selection_contains(app, y, x)) {
                cairo_set_source_rgba(cr, 0.24, 0.47, 0.78, 0.75);
                cairo_rectangle(cr, PAD_X + x * app->cell_width, PAD_Y + y * app->cell_height,
                                app->cell_width + 1, app->cell_height + 1);
                cairo_fill(cr);
            }
        }
    }

    PangoLayout *layout = gtk_widget_create_pango_layout(widget, NULL);
    pango_layout_set_font_description(layout, app->font);
    pango_layout_set_single_paragraph_mode(layout, TRUE);
    pango_layout_set_width(layout, -1);
    GString *text = g_string_sized_new((gsize)columns * 2);
    guint *starts = g_new0(guint, (gsize)columns);
    guint *ends = g_new0(guint, (gsize)columns);
    for (int y = 0; y < rows; y++) {
        const HdeCmdVtCell *line = hde_cmd_vt_line(app->vt, y);
        g_string_truncate(text, 0);
        memset(starts, 0, (gsize)columns * sizeof *starts);
        memset(ends, 0, (gsize)columns * sizeof *ends);
        for (int x = 0; x < columns; x++) {
            const HdeCmdVtCell *cell = &line[x];
            if (cell->attributes & HDE_CMD_VT_WIDE_CONT) {
                starts[x] = ends[x] = (guint)text->len;
                continue;
            }
            starts[x] = (guint)text->len;
            append_cell_text(text, cell);
            ends[x] = (guint)text->len;
        }
        pango_layout_set_text(layout, text->str, (int)text->len);
        PangoAttrList *attributes = pango_attr_list_new();
        for (int x = 0; x < columns; x++) {
            const HdeCmdVtCell *cell = &line[x];
            if (cell->attributes & HDE_CMD_VT_WIDE_CONT) continue;
            int inverse = (cell->attributes & HDE_CMD_VT_INVERSE) != 0;
            uint32_t fg = inverse ? cell->background : cell->foreground;
            double r = DEFAULT_FG_R, g = DEFAULT_FG_G, b = DEFAULT_FG_B;
            if (fg == HDE_CMD_VT_DEFAULT_COLOR && inverse) {
                r = DEFAULT_BG_R; g = DEFAULT_BG_G; b = DEFAULT_BG_B;
            } else if (fg != HDE_CMD_VT_DEFAULT_COLOR) {
                rgba_from_color(fg, &r, &g, &b);
            }
            add_foreground_attribute(attributes, (guint16)(r * 65535.0), (guint16)(g * 65535.0),
                                     (guint16)(b * 65535.0), starts[x], ends[x]);
            if (cell->attributes & HDE_CMD_VT_BOLD) {
                PangoAttribute *a = pango_attr_weight_new(PANGO_WEIGHT_BOLD);
                a->start_index = starts[x]; a->end_index = ends[x]; pango_attr_list_insert(attributes, a);
            }
            if (cell->attributes & HDE_CMD_VT_ITALIC) {
                PangoAttribute *a = pango_attr_style_new(PANGO_STYLE_ITALIC);
                a->start_index = starts[x]; a->end_index = ends[x]; pango_attr_list_insert(attributes, a);
            }
            if (cell->attributes & HDE_CMD_VT_UNDERLINE) {
                PangoAttribute *a = pango_attr_underline_new(PANGO_UNDERLINE_SINGLE);
                a->start_index = starts[x]; a->end_index = ends[x]; pango_attr_list_insert(attributes, a);
            }
            if (cell->attributes & HDE_CMD_VT_STRIKE) {
                PangoAttribute *a = pango_attr_strikethrough_new(TRUE);
                a->start_index = starts[x]; a->end_index = ends[x]; pango_attr_list_insert(attributes, a);
            }
        }
        pango_layout_set_attributes(layout, attributes);
        cairo_set_source_rgb(cr, DEFAULT_FG_R, DEFAULT_FG_G, DEFAULT_FG_B);
        cairo_move_to(cr, PAD_X, PAD_Y + y * app->cell_height);
        pango_cairo_show_layout(cr, layout);
        pango_attr_list_unref(attributes);
    }
    g_free(ends);
    g_free(starts);
    g_string_free(text, TRUE);
    g_object_unref(layout);

    if (app->focused && app->cursor_phase && hde_cmd_vt_cursor_visible(app->vt) &&
        hde_cmd_vt_view_offset(app->vt) == 0) {
        int x = hde_cmd_vt_cursor_x(app->vt);
        int y = hde_cmd_vt_cursor_y(app->vt);
        cairo_set_source_rgba(cr, 0.47, 0.67, 0.98, 0.9);
        cairo_set_line_width(cr, 1.0);
        cairo_rectangle(cr, PAD_X + x * app->cell_width + 0.5, PAD_Y + y * app->cell_height + 0.5,
                        app->cell_width - 1, app->cell_height - 1);
        cairo_stroke(cr);
    }
    return FALSE;
}

static char *selection_text(CmdWindow *app)
{
    if (!app->vt || !app->selecting) return NULL;
    int ay = app->selection_y1, ax = app->selection_x1;
    int by = app->selection_y2, bx = app->selection_x2;
    if (ay > by || (ay == by && ax > bx)) {
        int t = ay; ay = by; by = t;
        t = ax; ax = bx; bx = t;
    }
    int rows = hde_cmd_vt_rows(app->vt), columns = hde_cmd_vt_columns(app->vt);
    ay = CLAMP(ay, 0, rows - 1); by = CLAMP(by, 0, rows - 1);
    ax = CLAMP(ax, 0, columns - 1); bx = CLAMP(bx, 0, columns - 1);
    GString *text = g_string_new(NULL);
    for (int y = ay; y <= by; y++) {
        const HdeCmdVtCell *line = hde_cmd_vt_line(app->vt, y);
        int first = y == ay ? ax : 0;
        int last = y == by ? bx : columns - 1;
        guint before = (guint)text->len;
        for (int x = first; x <= last; x++) {
            const HdeCmdVtCell *cell = &line[x];
            if (cell->attributes & HDE_CMD_VT_WIDE_CONT) continue;
            append_cell_text(text, cell);
        }
        while (text->len > before && text->str[text->len - 1] == ' ') g_string_truncate(text, text->len - 1);
        if (y != by) g_string_append_c(text, '\n');
    }
    return g_string_free(text, FALSE);
}

static void clipboard_copy(CmdWindow *app)
{
    char *text = selection_text(app);
    if (!text) return;
    gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), text, -1);
    g_free(text);
}

static void paste_text(GtkClipboard *clipboard, const gchar *text, gpointer userdata)
{
    CmdWindow *app = userdata;
    (void)clipboard;
    if (!text || app->options.use_vte) return;
    GString *paste = g_string_new(NULL);
    if (hde_cmd_vt_bracketed_paste(app->vt)) g_string_append(paste, "\033[200~");
    for (const char *p = text; *p; p++) {
        if (*p == '\r') {
            g_string_append_c(paste, '\r');
            if (p[1] == '\n') p++;
        } else if (*p == '\n') {
            g_string_append_c(paste, '\r');
        } else {
            g_string_append_c(paste, *p);
        }
    }
    if (hde_cmd_vt_bracketed_paste(app->vt)) g_string_append(paste, "\033[201~");
    pty_write(app, paste->str, paste->len);
    g_string_free(paste, TRUE);
}

static void clipboard_paste(CmdWindow *app)
{
#ifdef HDE_CMD_HAVE_VTE
    if (app->options.use_vte) {
        vte_terminal_paste_clipboard(app->vte);
        return;
    }
#endif
    gtk_clipboard_request_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), paste_text, app);
}

static void update_font(CmdWindow *app, int size)
{
    app->font_size = CLAMP(size, MIN_FONT_SIZE, MAX_FONT_SIZE);
    if (app->font) pango_font_description_free(app->font);
    app->font = pango_font_description_from_string("Monospace");
    pango_font_description_set_size(app->font, app->font_size * PANGO_SCALE);
    if (app->terminal_widget) {
#ifdef HDE_CMD_HAVE_VTE
        if (app->options.use_vte) {
            vte_terminal_set_font(app->vte, app->font);
            return;
        }
#endif
        PangoContext *context = gtk_widget_get_pango_context(app->terminal_widget);
        PangoFontMetrics *metrics = pango_context_get_metrics(context, app->font, pango_language_get_default());
        app->cell_width = MAX(1, PANGO_PIXELS_CEIL(pango_font_metrics_get_approximate_char_width(metrics)));
        app->cell_height = MAX(1, PANGO_PIXELS_CEIL(pango_font_metrics_get_ascent(metrics) +
                                                    pango_font_metrics_get_descent(metrics)) + 2);
        pango_font_metrics_unref(metrics);
        gtk_widget_queue_resize(app->terminal_widget);
        update_pty_size(app);
        queue_draw(app);
    }
}

static gboolean app_key_press(GtkWidget *widget, GdkEventKey *event, gpointer userdata)
{
    CmdWindow *app = userdata;
    guint key = event->keyval;
    GdkModifierType state = event->state & gtk_accelerator_get_default_mod_mask();
    gboolean ctrl = (state & GDK_CONTROL_MASK) != 0;
    gboolean shift = (state & GDK_SHIFT_MASK) != 0;
    gboolean alt = (state & GDK_MOD1_MASK) != 0;
    (void)widget;

    if (ctrl && shift) {
        switch (gdk_keyval_to_lower(key)) {
        case GDK_KEY_t: menu_new(NULL, app); return TRUE;
        case GDK_KEY_o: menu_open_folder(NULL, app); return TRUE;
        case GDK_KEY_w: gtk_widget_destroy(app->window); return TRUE;
        default: break;
        }
    }

#ifdef HDE_CMD_HAVE_VTE
    if (app->options.use_vte) {
        if (ctrl && shift && (key == GDK_KEY_c || key == GDK_KEY_C)) { vte_terminal_copy_clipboard(app->vte); return TRUE; }
        if (ctrl && shift && (key == GDK_KEY_v || key == GDK_KEY_V)) { clipboard_paste(app); return TRUE; }
        if (ctrl && (key == GDK_KEY_plus || key == GDK_KEY_equal || key == GDK_KEY_KP_Add)) {
            update_font(app, app->font_size + 1); return TRUE;
        }
        if (ctrl && (key == GDK_KEY_minus || key == GDK_KEY_KP_Subtract)) { update_font(app, app->font_size - 1); return TRUE; }
        if (ctrl && (key == GDK_KEY_0 || key == GDK_KEY_KP_0)) { update_font(app, DEFAULT_FONT_SIZE); return TRUE; }
        if (key == GDK_KEY_F11) {
            if (app->fullscreen) {
                gtk_window_unfullscreen(GTK_WINDOW(app->window));
                app->fullscreen = 0;
            } else {
                gtk_window_fullscreen(GTK_WINDOW(app->window));
                app->fullscreen = 1;
            }
            return TRUE;
        }
        return FALSE;
    }
#endif

    if (ctrl && shift && (key == GDK_KEY_c || key == GDK_KEY_C)) { clipboard_copy(app); return TRUE; }
    if (ctrl && shift && (key == GDK_KEY_v || key == GDK_KEY_V)) { clipboard_paste(app); return TRUE; }
    if (ctrl && (key == GDK_KEY_plus || key == GDK_KEY_equal || key == GDK_KEY_KP_Add)) {
        update_font(app, app->font_size + 1); return TRUE;
    }
    if (ctrl && (key == GDK_KEY_minus || key == GDK_KEY_KP_Subtract)) { update_font(app, app->font_size - 1); return TRUE; }
    if (ctrl && (key == GDK_KEY_0 || key == GDK_KEY_KP_0)) { update_font(app, DEFAULT_FONT_SIZE); return TRUE; }
    if (key == GDK_KEY_F11) {
        if (app->fullscreen) {
            gtk_window_unfullscreen(GTK_WINDOW(app->window));
            app->fullscreen = 0;
        } else {
            gtk_window_fullscreen(GTK_WINDOW(app->window));
            app->fullscreen = 1;
        }
        return TRUE;
    }
    if (shift && (key == GDK_KEY_Page_Up || key == GDK_KEY_Page_Down)) {
        int amount = MAX(1, hde_cmd_vt_rows(app->vt) - 2);
        hde_cmd_vt_scroll_view(app->vt, key == GDK_KEY_Page_Up ? amount : -amount);
        queue_draw(app);
        return TRUE;
    }

    const char *sequence = NULL;
    char local[32];
    switch (key) {
    case GDK_KEY_Return: case GDK_KEY_KP_Enter: sequence = "\r"; break;
    case GDK_KEY_BackSpace: sequence = "\177"; break;
    case GDK_KEY_Tab: sequence = "\t"; break;
    case GDK_KEY_ISO_Left_Tab: sequence = "\033[Z"; break;
    case GDK_KEY_Escape: sequence = "\033"; break;
    case GDK_KEY_Up: sequence = hde_cmd_vt_application_cursor(app->vt) ? "\033OA" : "\033[A"; break;
    case GDK_KEY_Down: sequence = hde_cmd_vt_application_cursor(app->vt) ? "\033OB" : "\033[B"; break;
    case GDK_KEY_Right: sequence = hde_cmd_vt_application_cursor(app->vt) ? "\033OC" : "\033[C"; break;
    case GDK_KEY_Left: sequence = hde_cmd_vt_application_cursor(app->vt) ? "\033OD" : "\033[D"; break;
    case GDK_KEY_Home: sequence = hde_cmd_vt_application_cursor(app->vt) ? "\033OH" : "\033[H"; break;
    case GDK_KEY_End: sequence = hde_cmd_vt_application_cursor(app->vt) ? "\033OF" : "\033[F"; break;
    case GDK_KEY_Insert: sequence = "\033[2~"; break;
    case GDK_KEY_Delete: sequence = "\033[3~"; break;
    case GDK_KEY_Page_Up: sequence = "\033[5~"; break;
    case GDK_KEY_Page_Down: sequence = "\033[6~"; break;
    case GDK_KEY_F1: sequence = "\033OP"; break;
    case GDK_KEY_F2: sequence = "\033OQ"; break;
    case GDK_KEY_F3: sequence = "\033OR"; break;
    case GDK_KEY_F4: sequence = "\033OS"; break;
    case GDK_KEY_F5: sequence = "\033[15~"; break;
    case GDK_KEY_F6: sequence = "\033[17~"; break;
    case GDK_KEY_F7: sequence = "\033[18~"; break;
    case GDK_KEY_F8: sequence = "\033[19~"; break;
    case GDK_KEY_F9: sequence = "\033[20~"; break;
    case GDK_KEY_F10: sequence = "\033[21~"; break;
    case GDK_KEY_F11: sequence = "\033[23~"; break;
    case GDK_KEY_F12: sequence = "\033[24~"; break;
    default: break;
    }
    if (sequence) {
        if (alt) { local[0] = '\033'; size_t n = strlen(sequence); if (n > sizeof local - 2) n = sizeof local - 2;
            memcpy(local + 1, sequence, n); pty_write(app, local, n + 1); }
        else pty_write(app, sequence, strlen(sequence));
        app->selecting = 0;
        return TRUE;
    }

    gunichar character = gdk_keyval_to_unicode(key);
    if (!character) return FALSE;
    char utf8[8];
    int length = 0;
    if (ctrl) {
        if (character >= 'a' && character <= 'z') local[0] = (char)(character - 'a' + 1);
        else if (character >= 'A' && character <= 'Z') local[0] = (char)(character - 'A' + 1);
        else if (character == ' ' || character == '@') local[0] = 0;
        else if (character == '[') local[0] = 0x1b;
        else if (character == '\\') local[0] = 0x1c;
        else if (character == ']') local[0] = 0x1d;
        else if (character == '^') local[0] = 0x1e;
        else if (character == '_') local[0] = 0x1f;
        else if (character == '?') local[0] = 0x7f;
        else return FALSE;
        pty_write(app, local, 1);
    } else {
        length = g_unichar_to_utf8(character, utf8);
        if (alt) { local[0] = '\033'; memcpy(local + 1, utf8, (size_t)length); pty_write(app, local, (size_t)length + 1); }
        else pty_write(app, utf8, (size_t)length);
    }
    app->selecting = 0;
    return TRUE;
}

static void mouse_send(CmdWindow *app, int button, int column, int row, int release)
{
    if (!app->vt || hde_cmd_vt_mouse_mode(app->vt) == 0) return;
    int x = CLAMP(column + 1, 1, hde_cmd_vt_columns(app->vt));
    int y = CLAMP(row + 1, 1, hde_cmd_vt_rows(app->vt));
    if (hde_cmd_vt_sgr_mouse(app->vt)) {
        char seq[64];
        g_snprintf(seq, sizeof seq, "\033[<%d;%d;%d%c", release ? button + 3 : button, x, y, release ? 'm' : 'M');
        pty_write(app, seq, strlen(seq));
    } else {
        unsigned char seq[6] = { 0x1b, '[', 'M', 0, 0, 0 };
        seq[3] = (unsigned char)CLAMP(32 + (release ? button + 3 : button), 32, 255);
        seq[4] = (unsigned char)CLAMP(32 + x, 32, 255);
        seq[5] = (unsigned char)CLAMP(32 + y, 32, 255);
        pty_write(app, (const char *)seq, sizeof seq);
    }
}

static void event_cell(CmdWindow *app, double x, double y, int *column, int *row)
{
    *column = CLAMP((int)((x - PAD_X) / MAX(app->cell_width, 1)), 0, MAX(app->columns - 1, 0));
    *row = CLAMP((int)((y - PAD_Y) / MAX(app->cell_height, 1)), 0, MAX(app->rows - 1, 0));
}

static gboolean button_press(GtkWidget *widget, GdkEventButton *event, gpointer userdata)
{
    CmdWindow *app = userdata;
    (void)widget;
    if (event->button == 3) {
        if (app->options.use_vte) {
            if (event->state & GDK_SHIFT_MASK) return FALSE;
        } else if (app->vt && hde_cmd_vt_mouse_mode(app->vt) && !(event->state & GDK_SHIFT_MASK)) {
            int column, row;
            event_cell(app, event->x, event->y, &column, &row);
            mouse_send(app, 2, column, row, 0);
            return TRUE;
        }
        gtk_widget_grab_focus(app->terminal_widget);
        if (app->context_menu) {
            gtk_menu_popup(GTK_MENU(app->context_menu), NULL, NULL, NULL, NULL, event->button, event->time);
            return TRUE;
        }
        return FALSE;
    }
    if (app->options.use_vte) return FALSE;
    if (event->button == 1) {
        int column, row;
        event_cell(app, event->x, event->y, &column, &row);
        if (hde_cmd_vt_mouse_mode(app->vt) && !(event->state & GDK_SHIFT_MASK)) {
            mouse_send(app, 0, column, row, 0);
            return TRUE;
        }
        gtk_widget_grab_focus(app->terminal_widget);
        app->selection_x1 = app->selection_x2 = column;
        app->selection_y1 = app->selection_y2 = row;
        app->selecting = 1;
        queue_draw(app);
        return TRUE;
    }
    if (event->button == 2) {
        gtk_clipboard_request_text(gtk_clipboard_get(GDK_SELECTION_PRIMARY), paste_text, app);
        return TRUE;
    }
    return FALSE;
}

static gboolean button_release(GtkWidget *widget, GdkEventButton *event, gpointer userdata)
{
    CmdWindow *app = userdata;
    (void)widget;
    if (event->button == 3 && !app->options.use_vte && app->vt &&
        hde_cmd_vt_mouse_mode(app->vt) && !(event->state & GDK_SHIFT_MASK)) {
        int column, row;
        event_cell(app, event->x, event->y, &column, &row);
        mouse_send(app, 2, column, row, 1);
        return TRUE;
    }
    if (app->options.use_vte) return FALSE;
    if (event->button == 1 && hde_cmd_vt_mouse_mode(app->vt) && !(event->state & GDK_SHIFT_MASK)) {
        int column, row;
        event_cell(app, event->x, event->y, &column, &row);
        mouse_send(app, 0, column, row, 1);
        return TRUE;
    }
    if (event->button == 1 && app->selecting) {
        char *text = selection_text(app);
        if (text) {
            gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_PRIMARY), text, -1);
            g_free(text);
        }
    }
    return FALSE;
}

static gboolean pointer_motion(GtkWidget *widget, GdkEventMotion *event, gpointer userdata)
{
    CmdWindow *app = userdata;
    (void)widget;
    if (!app->selecting) return FALSE;
    event_cell(app, event->x, event->y, &app->selection_x2, &app->selection_y2);
    queue_draw(app);
    return TRUE;
}

static gboolean scroll_event(GtkWidget *widget, GdkEventScroll *event, gpointer userdata)
{
    CmdWindow *app = userdata;
    (void)widget;
    int direction = event->direction == GDK_SCROLL_UP ? -1 : 1;
    if (event->direction == GDK_SCROLL_SMOOTH) {
        double dx = 0, dy = 0;
        gdk_event_get_scroll_deltas((GdkEvent *)event, &dx, &dy);
        direction = dy < 0 ? -1 : dy > 0 ? 1 : 0;
    }
    if (!direction) return FALSE;
    if (hde_cmd_vt_mouse_mode(app->vt) && !(event->state & GDK_SHIFT_MASK)) {
        int column, row;
        event_cell(app, event->x, event->y, &column, &row);
        mouse_send(app, direction < 0 ? 64 : 65, column, row, 0);
    } else {
        hde_cmd_vt_scroll_view(app->vt, direction * 3);
        queue_draw(app);
    }
    return TRUE;
}

static gboolean focus_in(GtkWidget *widget, GdkEventFocus *event, gpointer userdata)
{
    CmdWindow *app = userdata;
    (void)widget; (void)event;
    app->focused = 1;
    app->cursor_phase = 1;
    queue_draw(app);
    return FALSE;
}

static gboolean focus_out(GtkWidget *widget, GdkEventFocus *event, gpointer userdata)
{
    CmdWindow *app = userdata;
    (void)widget; (void)event;
    app->focused = 0;
    app->cursor_phase = 1;
    queue_draw(app);
    return FALSE;
}

static gboolean cursor_blink(gpointer userdata)
{
    CmdWindow *app = userdata;
    app->cursor_phase = !app->cursor_phase;
    queue_draw(app);
    return G_SOURCE_CONTINUE;
}

static void send_hup(CmdWindow *app)
{
    if (app->child_pid <= 0 || app->child_exited) return;
    if (kill(-(pid_t)app->child_pid, SIGHUP) != 0) kill((pid_t)app->child_pid, SIGHUP);
}

static gboolean window_delete(GtkWidget *widget, GdkEvent *event, gpointer userdata)
{
    CmdWindow *app = userdata;
    (void)widget; (void)event;
    send_hup(app);
    return FALSE;
}

static void window_destroy(GtkWidget *widget, gpointer userdata)
{
    CmdWindow *app = userdata;
    (void)widget;
    send_hup(app);
    if (app->blink_watch) { g_source_remove(app->blink_watch); app->blink_watch = 0; }
    if (!app->options.use_vte) close_pty(app, 1);
    app->window = NULL;
}

static void menu_copy(GtkMenuItem *item, gpointer userdata)
{
    CmdWindow *app = userdata;
    (void)item;
#ifdef HDE_CMD_HAVE_VTE
    if (app->options.use_vte) vte_terminal_copy_clipboard(app->vte);
    else
#endif
        clipboard_copy(app);
}

static void menu_paste(GtkMenuItem *item, gpointer userdata)
{
    (void)item;
    clipboard_paste(userdata);
}

static void menu_font_up(GtkMenuItem *item, gpointer userdata)
{
    CmdWindow *app = userdata;
    (void)item;
    update_font(app, app->font_size + 1);
}

static void menu_font_down(GtkMenuItem *item, gpointer userdata)
{
    CmdWindow *app = userdata;
    (void)item;
    update_font(app, app->font_size - 1);
}

static void menu_font_reset(GtkMenuItem *item, gpointer userdata)
{
    CmdWindow *app = userdata;
    (void)item;
    update_font(app, DEFAULT_FONT_SIZE);
}

static void open_new_terminal(CmdWindow *app, const char *directory)
{
    GPtrArray *args = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(args, g_strdup(app->program_name ? app->program_name : "hde-cmd"));
    if (app->options.use_vte) g_ptr_array_add(args, g_strdup("--vte"));
    const char *cwd = directory && *directory ? directory : app->options.working_directory;
    if (cwd && *cwd) {
        g_ptr_array_add(args, g_strdup("--directory"));
        g_ptr_array_add(args, g_strdup(cwd));
    }
    if (app->options.title && *app->options.title) {
        g_ptr_array_add(args, g_strdup("--title"));
        g_ptr_array_add(args, g_strdup(app->options.title));
    }
    g_ptr_array_add(args, NULL);
    GError *error = NULL;
    if (!g_spawn_async(NULL, (char **)args->pdata, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &error)) {
        g_printerr("hde-cmd: cannot open another terminal: %s\n", error ? error->message : "unknown error");
        g_clear_error(&error);
    }
    g_ptr_array_unref(args);
}

static void menu_new(GtkMenuItem *item, gpointer userdata)
{
    (void)item;
    open_new_terminal(userdata, NULL);
}

static void menu_open_folder(GtkMenuItem *item, gpointer userdata)
{
    CmdWindow *app = userdata;
    (void)item;
    GtkWidget *dialog = gtk_file_chooser_dialog_new("Open a Terminal in a Folder", GTK_WINDOW(app->window),
                                                     GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                                                     "_Cancel", GTK_RESPONSE_CANCEL,
                                                     "_Open Terminal", GTK_RESPONSE_ACCEPT, NULL);
    const char *start = app->options.working_directory ? app->options.working_directory : g_get_home_dir();
    if (start && *start) gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), start);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *directory = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (directory) {
            open_new_terminal(app, directory);
            g_free(directory);
        }
    }
    gtk_widget_destroy(dialog);
}

static GtkWidget *menu_item(GtkWidget *menu, const char *label, GCallback callback, CmdWindow *app)
{
    GtkWidget *item = gtk_menu_item_new_with_label(label);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    g_signal_connect(item, "activate", callback, app);
    return item;
}

static void menu_fullscreen(GtkMenuItem *item, gpointer userdata)
{
    CmdWindow *app = userdata;
    (void)item;
    if (app->fullscreen) {
        gtk_window_unfullscreen(GTK_WINDOW(app->window));
        app->fullscreen = 0;
    } else {
        gtk_window_fullscreen(GTK_WINDOW(app->window));
        app->fullscreen = 1;
    }
}

static void menu_close(GtkMenuItem *item, gpointer userdata)
{
    CmdWindow *app = userdata;
    (void)item;
    gtk_widget_destroy(app->window);
}

static GtkWidget *build_header(CmdWindow *app)
{
    GtkWidget *header = gtk_header_bar_new();
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header), TRUE);
    gtk_header_bar_set_title(GTK_HEADER_BAR(header), "HDE Cmd");
    gtk_header_bar_set_subtitle(GTK_HEADER_BAR(header), "Terminal");

    GtkWidget *new_button = gtk_button_new_from_icon_name("list-add-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(new_button, "Open a new terminal window (Ctrl+Shift+T)");
    g_signal_connect(new_button, "clicked", G_CALLBACK(menu_new), app);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), new_button);

    GtkWidget *button = gtk_menu_button_new();
    gtk_button_set_image(GTK_BUTTON(button), gtk_image_new_from_icon_name("open-menu-symbolic", GTK_ICON_SIZE_BUTTON));
    gtk_widget_set_tooltip_text(button, "Terminal actions and keyboard shortcuts");
    GtkWidget *menu = gtk_menu_new();
    menu_item(menu, "New Terminal Window  Ctrl+Shift+T", G_CALLBACK(menu_new), app);
    menu_item(menu, "Open Folder in New Terminal…  Ctrl+Shift+O", G_CALLBACK(menu_open_folder), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    menu_item(menu, "Copy  Ctrl+Shift+C", G_CALLBACK(menu_copy), app);
    menu_item(menu, "Paste  Ctrl+Shift+V", G_CALLBACK(menu_paste), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    menu_item(menu, "Zoom In  Ctrl++", G_CALLBACK(menu_font_up), app);
    menu_item(menu, "Zoom Out  Ctrl+-", G_CALLBACK(menu_font_down), app);
    menu_item(menu, "Reset Zoom  Ctrl+0", G_CALLBACK(menu_font_reset), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    menu_item(menu, "Toggle Full Screen  F11", G_CALLBACK(menu_fullscreen), app);
    menu_item(menu, "Close Window  Ctrl+Shift+W", G_CALLBACK(menu_close), app);
    gtk_menu_button_set_popup(GTK_MENU_BUTTON(button), menu);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), button);
    app->context_menu = menu;
    gtk_widget_show_all(header);
    app->header = header;
    return header;
}

#ifdef HDE_CMD_HAVE_VTE
static char **build_spawn_environment(void)
{
    char **environment = g_get_environ();
    environment = g_environ_setenv(environment, "TERM", "xterm-256color", TRUE);
    environment = g_environ_setenv(environment, "COLORTERM", "truecolor", TRUE);
    environment = g_environ_setenv(environment, "TERM_PROGRAM", "hde-cmd", TRUE);
    return environment;
}

static void vte_spawned(VteTerminal *terminal, GPid pid, GError *error, gpointer userdata)
{
    CmdWindow *app = userdata;
    (void)terminal;
    if (error) {
        g_printerr("hde-cmd: VTE could not start the shell: %s\n", error->message);
        set_title(app, "HDE Cmd — could not start");
        g_clear_error(&error);
    } else {
        app->child_pid = pid;
    }
    g_clear_pointer(&app->spawn_argv, g_strfreev);
    g_clear_pointer(&app->spawn_environment, g_strfreev);
}

static void vte_child_exited(VteTerminal *terminal, gint status, gpointer userdata)
{
    CmdWindow *app = userdata;
    (void)terminal;
    (void)status;
    app->child_exited = 1;
}

static void vte_title_changed(VteTerminal *terminal, gpointer userdata)
{
    CmdWindow *app = userdata;
    const char *title = vte_terminal_get_window_title(terminal);
    if (title && *title) set_title(app, title);
}

static void start_vte_session(CmdWindow *app)
{
    if (app->options.command && app->options.command[0]) {
        app->spawn_argv = g_strdupv(app->options.command);
    } else {
        const char *shell = default_shell();
        const char *base = strrchr(shell, '/');
        base = base ? base + 1 : shell;
        app->spawn_argv = g_new0(char *, 3);
        app->spawn_argv[0] = g_strdup(base);
        app->spawn_argv[1] = g_strdup("-l");
    }
    app->spawn_environment = build_spawn_environment();
    vte_terminal_spawn_async(app->vte, VTE_PTY_DEFAULT, app->options.working_directory,
                             app->spawn_argv, app->spawn_environment, G_SPAWN_SEARCH_PATH,
                             NULL, NULL, NULL, -1, NULL, vte_spawned, app);
}
#endif

static void activate(GtkApplication *application, gpointer userdata)
{
    CmdWindow *app = userdata;
    app->application = application;
    if (app->window) {
        gtk_window_present(GTK_WINDOW(app->window));
        return;
    }
    app->window = gtk_application_window_new(application);
    gtk_window_set_title(GTK_WINDOW(app->window), app->options.title ? app->options.title : "HDE Cmd");
    gtk_window_set_icon_name(GTK_WINDOW(app->window), "utilities-terminal");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 880, 560);
    gtk_window_set_position(GTK_WINDOW(app->window), GTK_WIN_POS_CENTER);
    gtk_window_set_decorated(GTK_WINDOW(app->window), TRUE);
    gtk_window_set_titlebar(GTK_WINDOW(app->window), build_header(app));
    g_signal_connect(app->window, "delete-event", G_CALLBACK(window_delete), app);
    g_signal_connect(app->window, "destroy", G_CALLBACK(window_destroy), app);

    if (app->options.use_vte) {
#ifdef HDE_CMD_HAVE_VTE
        app->vte = VTE_TERMINAL(vte_terminal_new());
        app->terminal_widget = GTK_WIDGET(app->vte);
        gtk_widget_set_hexpand(app->terminal_widget, TRUE);
        gtk_widget_set_vexpand(app->terminal_widget, TRUE);
        gtk_widget_set_margin_start(app->terminal_widget, 5);
        gtk_widget_set_margin_end(app->terminal_widget, 5);
        gtk_widget_set_margin_top(app->terminal_widget, 5);
        gtk_widget_set_margin_bottom(app->terminal_widget, 5);
        vte_terminal_set_scrollback_lines(app->vte, HDE_CMD_VT_SCROLLBACK);
        vte_terminal_set_mouse_autohide(app->vte, TRUE);
        g_signal_connect(app->vte, "child-exited", G_CALLBACK(vte_child_exited), app);
        g_signal_connect(app->vte, "window-title-changed", G_CALLBACK(vte_title_changed), app);
        g_signal_connect(app->vte, "key-press-event", G_CALLBACK(app_key_press), app);
        gtk_widget_add_events(app->terminal_widget, GDK_BUTTON_PRESS_MASK);
        g_signal_connect(app->vte, "button-press-event", G_CALLBACK(button_press), app);
        gtk_container_add(GTK_CONTAINER(app->window), app->terminal_widget);
        gtk_widget_show_all(app->window);
        update_font(app, DEFAULT_FONT_SIZE);
        set_title(app, app->options.title ? app->options.title : "HDE Cmd");
        start_vte_session(app);
        gtk_widget_grab_focus(app->terminal_widget);
        return;
#endif
    }

    app->vt = hde_cmd_vt_new(DEFAULT_ROWS, DEFAULT_COLUMNS);
    if (!app->vt) {
        g_printerr("hde-cmd: cannot allocate the terminal screen\n");
        g_application_quit(G_APPLICATION(application));
        return;
    }
    app->terminal_widget = gtk_drawing_area_new();
    gtk_widget_set_can_focus(app->terminal_widget, TRUE);
    gtk_widget_set_hexpand(app->terminal_widget, TRUE);
    gtk_widget_set_vexpand(app->terminal_widget, TRUE);
    gtk_widget_set_size_request(app->terminal_widget, 400, 240);
    gtk_widget_add_events(app->terminal_widget, GDK_KEY_PRESS_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                                                 GDK_POINTER_MOTION_MASK | GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK | GDK_FOCUS_CHANGE_MASK);
    g_signal_connect(app->terminal_widget, "draw", G_CALLBACK(draw_terminal), app);
    g_signal_connect(app->terminal_widget, "size-allocate", G_CALLBACK(on_size_allocate), app);
    g_signal_connect(app->terminal_widget, "key-press-event", G_CALLBACK(app_key_press), app);
    g_signal_connect(app->terminal_widget, "button-press-event", G_CALLBACK(button_press), app);
    g_signal_connect(app->terminal_widget, "button-release-event", G_CALLBACK(button_release), app);
    g_signal_connect(app->terminal_widget, "motion-notify-event", G_CALLBACK(pointer_motion), app);
    g_signal_connect(app->terminal_widget, "scroll-event", G_CALLBACK(scroll_event), app);
    g_signal_connect(app->terminal_widget, "focus-in-event", G_CALLBACK(focus_in), app);
    g_signal_connect(app->terminal_widget, "focus-out-event", G_CALLBACK(focus_out), app);
    gtk_container_add(GTK_CONTAINER(app->window), app->terminal_widget);
    app->rows = DEFAULT_ROWS;
    app->columns = DEFAULT_COLUMNS;
    app->cursor_phase = 1;
    update_font(app, DEFAULT_FONT_SIZE);
    gtk_widget_show_all(app->window);
    gtk_widget_grab_focus(app->terminal_widget);
    if (app->options.title) set_title(app, app->options.title);
    update_pty_size(app);
    if (start_internal_session(app) != 0) {
        set_title(app, "HDE Cmd — failed to start");
        return;
    }
    app->blink_watch = g_timeout_add(530, cursor_blink, app);
}

int hde_cmd_ui_run(const HdeCmdOptions *options, const char *program_name)
{
    setlocale(LC_CTYPE, "");
    CmdWindow app;
    memset(&app, 0, sizeof app);
    if (options) app.options = *options;
    app.program_name = program_name;
    app.pty_fd = -1;
    app.font_size = DEFAULT_FONT_SIZE;
    app.cursor_phase = 1;

    GtkApplication *application = gtk_application_new(APP_ID, G_APPLICATION_NON_UNIQUE);
    g_signal_connect(application, "activate", G_CALLBACK(activate), &app);
    char *run_argv[] = { (char *)(program_name ? program_name : "hde-cmd"), NULL };
    int status = g_application_run(G_APPLICATION(application), 1, run_argv);

    send_hup(&app);
    close_pty(&app, 1);
    if (app.child_watch) g_source_remove(app.child_watch);
    if (app.blink_watch) g_source_remove(app.blink_watch);
    if (app.vt) hde_cmd_vt_free(app.vt);
    if (app.font) pango_font_description_free(app.font);
#ifdef HDE_CMD_HAVE_VTE
    g_clear_pointer(&app.spawn_argv, g_strfreev);
    g_clear_pointer(&app.spawn_environment, g_strfreev);
#endif
    g_free(app.title);
    g_object_unref(application);
    return status;
}
