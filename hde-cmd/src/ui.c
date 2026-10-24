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
    const char *name;
    double bg_r, bg_g, bg_b;
    double fg_r, fg_g, fg_b;
    double cur_r, cur_g, cur_b;
    double sel_r, sel_g, sel_b;
} CmdTheme;

static const CmdTheme CMD_THEMES[] = {
    { "Hyggshi Dark",   0.090, 0.102, 0.125,  0.855, 0.878, 0.914,  0.470, 0.670, 0.980,  0.240, 0.470, 0.780 },
    { "Hyggshi Light",  0.973, 0.976, 0.984,  0.118, 0.133, 0.165,  0.110, 0.443, 0.847,  0.208, 0.518, 0.894 },
    { "Monokai Pro",    0.118, 0.122, 0.110,  0.973, 0.973, 0.949,  0.651, 0.886, 0.180,  0.340, 0.360, 0.280 },
    { "Solarized Dark", 0.000, 0.169, 0.212,  0.576, 0.631, 0.631,  0.149, 0.545, 0.824,  0.090, 0.340, 0.420 },
    { "Nord",           0.180, 0.204, 0.251,  0.847, 0.871, 0.914,  0.533, 0.753, 0.816,  0.298, 0.337, 0.416 },
    { "Matrix Green",   0.027, 0.071, 0.035,  0.200, 1.000, 0.400,  0.000, 0.902, 0.302,  0.078, 0.360, 0.141 },
};

typedef struct {
    GtkApplication *application;
    GtkWidget *window;
    GtkWidget *terminal_widget;
    GtkWidget *terminal_frame;
    GtkWidget *context_menu;
    GtkWidget *open_url_item;
    GtkWidget *copy_url_item;
    GtkWidget *find_bar;
    GtkWidget *find_entry;
    GtkWidget *find_status;
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
    int theme_index;
    int cursor_style;
    PangoFontDescription *font;
    char *title;
    char *hovered_url;

    int selecting;
    int selection_x1, selection_y1;
    int selection_x2, selection_y2;

    int find_match_abs_row;
    int find_match_col;
    int find_match_len;

#ifdef HDE_CMD_HAVE_VTE
    VteTerminal *vte;
    char **spawn_argv;
    char **spawn_environment;
#endif
} CmdWindow;

static void menu_new(GtkMenuItem *item, gpointer userdata);
static void menu_open_folder(GtkMenuItem *item, gpointer userdata);
static void toggle_find_bar(CmdWindow *app, gboolean show);

static const CmdTheme *current_theme(const CmdWindow *app)
{
    int idx = app ? CLAMP(app->theme_index, 0, (int)G_N_ELEMENTS(CMD_THEMES) - 1) : 0;
    return &CMD_THEMES[idx];
}

static void css_rgba(double r, double g, double b, double a, char *out, size_t out_size)
{
    /* GTK CSS needs a dot decimal separator even when the user's LC_NUMERIC uses a comma. */
    char alpha[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(alpha, sizeof(alpha), "%.2f", CLAMP(a, 0.0, 1.0));
    snprintf(out, out_size, "rgba(%d, %d, %d, %s)",
             (int)(CLAMP(r, 0.0, 1.0) * 255.0 + 0.5),
             (int)(CLAMP(g, 0.0, 1.0) * 255.0 + 0.5),
             (int)(CLAMP(b, 0.0, 1.0) * 255.0 + 0.5), alpha);
}

static void css_hex(double r, double g, double b, char *out, size_t out_size)
{
    snprintf(out, out_size, "#%02x%02x%02x",
             (int)(CLAMP(r, 0.0, 1.0) * 255.0 + 0.5),
             (int)(CLAMP(g, 0.0, 1.0) * 255.0 + 0.5),
             (int)(CLAMP(b, 0.0, 1.0) * 255.0 + 0.5));
}

/* Paint the whole window with the terminal theme: the window and the find bar follow the terminal's
 * dark/light palette instead of the GTK theme's colours. */
static void apply_cmd_css(CmdWindow *app)
{
    static GtkCssProvider *cmd_css = NULL;
    if (!cmd_css) {
        cmd_css = gtk_css_provider_new();
        gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(cmd_css),
                                                  GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    const CmdTheme *th = current_theme(app);
    char bg[8], hdr[8];
    char fg[64], fg_soft[64], border[64], hover[64], field[64], field_border[64], accent[64];
    double hr = th->bg_r + (th->fg_r - th->bg_r) * 0.045;
    double hg = th->bg_g + (th->fg_g - th->bg_g) * 0.045;
    double hb = th->bg_b + (th->fg_b - th->bg_b) * 0.045;
    css_hex(th->bg_r, th->bg_g, th->bg_b, bg, sizeof(bg));
    css_hex(hr, hg, hb, hdr, sizeof(hdr));
    css_rgba(th->fg_r, th->fg_g, th->fg_b, 0.96, fg, sizeof(fg));
    css_rgba(th->fg_r, th->fg_g, th->fg_b, 0.68, fg_soft, sizeof(fg_soft));
    css_rgba(th->fg_r, th->fg_g, th->fg_b, 0.13, border, sizeof(border));
    css_rgba(th->fg_r, th->fg_g, th->fg_b, 0.10, hover, sizeof(hover));
    css_rgba(th->fg_r, th->fg_g, th->fg_b, 0.07, field, sizeof(field));
    css_rgba(th->fg_r, th->fg_g, th->fg_b, 0.16, field_border, sizeof(field_border));
    css_rgba(th->cur_r, th->cur_g, th->cur_b, 0.75, accent, sizeof(accent));

    char *css = g_strdup_printf(
        ".cmd-window { background-color: %s; }"
        ".cmd-find { background-color: %s; border-bottom: 1px solid %s; padding: 3px 8px; }"
        ".cmd-find entry { background-color: %s; color: %s; border: 1px solid %s; border-radius: 8px; }"
        ".cmd-find entry image { color: %s; }"
        ".cmd-find entry:focus { border-color: %s; }"
        ".cmd-find button { color: %s; background: transparent; border: none; box-shadow: none; }"
        ".cmd-find button:hover { background-color: %s; }"
        ".cmd-terminal-frame { background-color: %s; }",
        bg, hdr, border, field, fg, field_border, fg_soft, accent, fg, hover, bg);
    gtk_css_provider_load_from_data(cmd_css, css, -1, NULL);
    g_free(css);
}

#ifdef HDE_CMD_HAVE_VTE
static void apply_vte_theme(CmdWindow *app)
{
    if (!app || !app->vte) return;
    const CmdTheme *th = current_theme(app);
    GdkRGBA fg = { th->fg_r, th->fg_g, th->fg_b, 1.0 };
    GdkRGBA bg = { th->bg_r, th->bg_g, th->bg_b, 1.0 };
    GdkRGBA cur = { th->cur_r, th->cur_g, th->cur_b, 1.0 };
    vte_terminal_set_colors(app->vte, &fg, &bg, NULL, 0);
    vte_terminal_set_color_cursor(app->vte, &cur);
}

static void cmd_vte_copy_clipboard(VteTerminal *vte)
{
#if VTE_CHECK_VERSION(0, 50, 0)
    vte_terminal_copy_clipboard_format(vte, VTE_FORMAT_TEXT);
#else
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    vte_terminal_copy_clipboard(vte);
    G_GNUC_END_IGNORE_DEPRECATIONS
#endif
}
#else
static void apply_vte_theme(CmdWindow *app) { (void)app; }
#endif

static char *cmd_config_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "hde", "cmd.ini", NULL);
}

static void load_preferences(CmdWindow *app)
{
    char *path = cmd_config_path();
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        if (g_key_file_has_key(kf, "cmd", "font_size", NULL))
            app->font_size = CLAMP(g_key_file_get_integer(kf, "cmd", "font_size", NULL), MIN_FONT_SIZE, MAX_FONT_SIZE);
        if (g_key_file_has_key(kf, "cmd", "theme_index", NULL))
            app->theme_index = CLAMP(g_key_file_get_integer(kf, "cmd", "theme_index", NULL), 0, (int)G_N_ELEMENTS(CMD_THEMES) - 1);
        if (g_key_file_has_key(kf, "cmd", "cursor_style", NULL))
            app->cursor_style = CLAMP(g_key_file_get_integer(kf, "cmd", "cursor_style", NULL), 0, 2);
    }
    g_key_file_free(kf);
    g_free(path);
}

static void save_preferences(const CmdWindow *app)
{
    char *path = cmd_config_path();
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    GKeyFile *kf = g_key_file_new();
    g_key_file_load_from_file(kf, path, G_KEY_FILE_KEEP_COMMENTS, NULL);
    g_key_file_set_integer(kf, "cmd", "font_size", app->font_size);
    g_key_file_set_integer(kf, "cmd", "theme_index", app->theme_index);
    g_key_file_set_integer(kf, "cmd", "cursor_style", app->cursor_style);
    g_key_file_save_to_file(kf, path, NULL);
    g_key_file_free(kf);
    g_free(dir);
    g_free(path);
}

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

    if (g_getenv("HDE_CMD_DEBUG"))
        g_printerr("hde-cmd: pty %dx%d, vt %dx%d, cursor %d,%d\n", app->rows, app->columns,
                   hde_cmd_vt_rows(app->vt), hde_cmd_vt_columns(app->vt), hde_cmd_vt_cursor_y(app->vt),
                   hde_cmd_vt_cursor_x(app->vt));

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
    /* the first allocation of the widget can be tiny (2x2): a terminal that resizes to that would throw
     * its text away, so the size is only taken over when the window really has a size */
    if (rows < 4 || columns < 12) return;
    if (columns == app->columns && rows == app->rows) return;
    app->columns = columns;
    app->rows = rows;
    if (g_getenv("HDE_CMD_DEBUG"))
        g_printerr("hde-cmd: resize -> %dx%d (was %dx%d)\n", rows, columns, hde_cmd_vt_rows(app->vt),
                   hde_cmd_vt_columns(app->vt));
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

    const CmdTheme *th = current_theme(app);
    cairo_set_source_rgb(cr, th->bg_r, th->bg_g, th->bg_b);
    cairo_paint(cr);

    int rows = hde_cmd_vt_rows(app->vt);
    int columns = hde_cmd_vt_columns(app->vt);
    int history = hde_cmd_vt_scrollback(app->vt);
    int view_offset = hde_cmd_vt_view_offset(app->vt);
    for (int y = 0; y < rows; y++) {
        const HdeCmdVtCell *line = hde_cmd_vt_line(app->vt, y);
        int abs_row = history - view_offset + y;
        for (int x = 0; x < columns; x++) {
            const HdeCmdVtCell *cell = &line[x];
            int inverse = (cell->attributes & HDE_CMD_VT_INVERSE) != 0;
            uint32_t bg = inverse ? cell->foreground : cell->background;
            double r = th->bg_r, g = th->bg_g, b = th->bg_b;
            if (bg == HDE_CMD_VT_DEFAULT_COLOR && inverse) {
                r = th->fg_r; g = th->fg_g; b = th->fg_b;
            } else if (bg != HDE_CMD_VT_DEFAULT_COLOR) {
                rgba_from_color(bg, &r, &g, &b);
            }
            if (bg != HDE_CMD_VT_DEFAULT_COLOR || inverse) {
                cairo_set_source_rgb(cr, r, g, b);
                cairo_rectangle(cr, PAD_X + x * app->cell_width, PAD_Y + y * app->cell_height,
                                app->cell_width + 1, app->cell_height + 1);
                cairo_fill(cr);
            }
            if (app->find_match_abs_row >= 0 && abs_row == app->find_match_abs_row &&
                x >= app->find_match_col && x < app->find_match_col + app->find_match_len) {
                cairo_set_source_rgba(cr, 0.96, 0.76, 0.07, 0.55);
                cairo_rectangle(cr, PAD_X + x * app->cell_width, PAD_Y + y * app->cell_height,
                                app->cell_width + 1, app->cell_height + 1);
                cairo_fill(cr);
            }
            if (selection_contains(app, y, x)) {
                cairo_set_source_rgba(cr, th->sel_r, th->sel_g, th->sel_b, 0.75);
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
            double r = th->fg_r, g = th->fg_g, b = th->fg_b;
            if (fg == HDE_CMD_VT_DEFAULT_COLOR && inverse) {
                r = th->bg_r; g = th->bg_g; b = th->bg_b;
            } else if (fg != HDE_CMD_VT_DEFAULT_COLOR) {
                rgba_from_color(fg, &r, &g, &b);
            }
            if (cell->attributes & HDE_CMD_VT_DIM) {
                r = r * 0.58 + th->bg_r * 0.42;
                g = g * 0.58 + th->bg_g * 0.42;
                b = b * 0.58 + th->bg_b * 0.42;
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
        cairo_set_source_rgb(cr, th->fg_r, th->fg_g, th->fg_b);
        cairo_move_to(cr, PAD_X, PAD_Y + y * app->cell_height);
        pango_cairo_show_layout(cr, layout);
        pango_attr_list_unref(attributes);
    }
    g_free(ends);
    g_free(starts);
    g_string_free(text, TRUE);
    g_object_unref(layout);

    if (app->focused && app->cursor_phase && hde_cmd_vt_cursor_visible(app->vt) &&
        view_offset == 0) {
        int x = hde_cmd_vt_cursor_x(app->vt);
        int y = hde_cmd_vt_cursor_y(app->vt);
        HdeCmdVtCursorShape shape = hde_cmd_vt_cursor_shape(app->vt);
        if (shape == HDE_CMD_VT_CURSOR_BLOCK && app->cursor_style == 1) shape = HDE_CMD_VT_CURSOR_BEAM;
        else if (shape == HDE_CMD_VT_CURSOR_BLOCK && app->cursor_style == 2) shape = HDE_CMD_VT_CURSOR_UNDERLINE;
        cairo_set_source_rgba(cr, th->cur_r, th->cur_g, th->cur_b, 0.92);
        double cx = PAD_X + x * app->cell_width;
        double cy = PAD_Y + y * app->cell_height;
        if (shape == HDE_CMD_VT_CURSOR_BEAM) {
            cairo_rectangle(cr, cx, cy, 2.0, app->cell_height);
            cairo_fill(cr);
        } else if (shape == HDE_CMD_VT_CURSOR_UNDERLINE) {
            cairo_rectangle(cr, cx, cy + app->cell_height - 2.0, app->cell_width, 2.0);
            cairo_fill(cr);
        } else {
            cairo_set_line_width(cr, 1.0);
            cairo_rectangle(cr, cx + 0.5, cy + 0.5, app->cell_width - 1, app->cell_height - 1);
            cairo_stroke(cr);
        }
    }

    if (history > 0) {
        GtkAllocation alloc;
        gtk_widget_get_allocation(widget, &alloc);
        double track_h = MAX(24.0, (double)alloc.height - 2 * PAD_Y);
        double total = (double)(history + rows);
        double thumb_h = CLAMP(track_h * ((double)rows / total), 18.0, track_h);
        double top_frac = (double)(history - view_offset) / MAX(1.0, (double)history);
        double thumb_y = PAD_Y + (track_h - thumb_h) * top_frac;
        cairo_set_source_rgba(cr, th->fg_r, th->fg_g, th->fg_b, view_offset > 0 ? 0.42 : 0.16);
        cairo_rectangle(cr, alloc.width - 5.0, thumb_y, 3.0, thumb_h);
        cairo_fill(cr);
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
    save_preferences(app);
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

static void select_all_cells(CmdWindow *app)
{
    if (!app->vt) return;
    app->selection_x1 = 0;
    app->selection_y1 = 0;
    app->selection_x2 = MAX(0, hde_cmd_vt_columns(app->vt) - 1);
    app->selection_y2 = MAX(0, hde_cmd_vt_rows(app->vt) - 1);
    app->selecting = 1;
    char *text = selection_text(app);
    if (text) {
        gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_PRIMARY), text, -1);
        g_free(text);
    }
    queue_draw(app);
}

static void save_output_dialog(CmdWindow *app)
{
    if (!app || !app->vt || !app->window) return;
    GtkWidget *dialog = gtk_file_chooser_dialog_new("Save Terminal Output", GTK_WINDOW(app->window),
                                                     GTK_FILE_CHOOSER_ACTION_SAVE,
                                                     "_Cancel", GTK_RESPONSE_CANCEL,
                                                     "_Save", GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), "terminal-output.txt");
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (filename) {
            int total = hde_cmd_vt_scrollback(app->vt) + hde_cmd_vt_rows(app->vt);
            int cols = hde_cmd_vt_columns(app->vt);
            GString *out = g_string_new(NULL);
            for (int r = 0; r < total; r++) {
                const HdeCmdVtCell *line = hde_cmd_vt_history_line(app->vt, r);
                if (!line) continue;
                guint before = (guint)out->len;
                for (int x = 0; x < cols; x++) {
                    if (line[x].attributes & HDE_CMD_VT_WIDE_CONT) continue;
                    append_cell_text(out, &line[x]);
                }
                while (out->len > before && out->str[out->len - 1] == ' ')
                    g_string_truncate(out, out->len - 1);
                g_string_append_c(out, '\n');
            }
            GError *err = NULL;
            if (!g_file_set_contents(filename, out->str, (gssize)out->len, &err)) {
                g_printerr("hde-cmd: cannot save output: %s\n", err ? err->message : "error");
                g_clear_error(&err);
            }
            g_string_free(out, TRUE);
            g_free(filename);
        }
    }
    gtk_widget_destroy(dialog);
}

static gboolean is_word_char(uint32_t cp)
{
    if (cp <= 0x20 || cp == 0x7f) return FALSE;
    return strchr(" \t\r\n\"'`<>()[]{}|,;!", (int)cp) == NULL;
}

static char *extract_word_at(const CmdWindow *app, int row, int col, int *out_x1, int *out_x2)
{
    if (!app->vt) return NULL;
    const HdeCmdVtCell *line = hde_cmd_vt_line(app->vt, row);
    int cols = hde_cmd_vt_columns(app->vt);
    if (!line || col < 0 || col >= cols) return NULL;
    uint32_t cp = line[col].codepoint ? line[col].codepoint : ' ';
    if (!is_word_char(cp)) return NULL;
    int x1 = col, x2 = col;
    while (x1 > 0 && is_word_char(line[x1 - 1].codepoint ? line[x1 - 1].codepoint : ' ')) x1--;
    while (x2 + 1 < cols && is_word_char(line[x2 + 1].codepoint ? line[x2 + 1].codepoint : ' ')) x2++;
    if (out_x1) *out_x1 = x1;
    if (out_x2) *out_x2 = x2;
    GString *s = g_string_new(NULL);
    for (int x = x1; x <= x2; x++) {
        if (line[x].attributes & HDE_CMD_VT_WIDE_CONT) continue;
        append_cell_text(s, &line[x]);
    }
    while (s->len > 0 && strchr(".,;:!?)>]}", s->str[s->len - 1]))
        g_string_truncate(s, s->len - 1);
    return g_string_free(s, FALSE);
}

static char *url_at_cell(const CmdWindow *app, int row, int col)
{
    char *w = extract_word_at(app, row, col, NULL, NULL);
    if (!w) return NULL;
    if (g_str_has_prefix(w, "http://") || g_str_has_prefix(w, "https://") || g_str_has_prefix(w, "file://"))
        return w;
    if (g_str_has_prefix(w, "www.")) {
        char *u = g_strdup_printf("https://%s", w);
        g_free(w);
        return u;
    }
    g_free(w);
    return NULL;
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
        case GDK_KEY_n: menu_new(NULL, app); return TRUE;
        case GDK_KEY_o: menu_open_folder(NULL, app); return TRUE;
        case GDK_KEY_w: gtk_widget_destroy(app->window); return TRUE;
        case GDK_KEY_f: toggle_find_bar(app, TRUE); return TRUE;
        case GDK_KEY_a: select_all_cells(app); return TRUE;
        case GDK_KEY_s: save_output_dialog(app); return TRUE;
        case GDK_KEY_k:
            if (app->vt) {
                hde_cmd_vt_clear_scrollback(app->vt);
                queue_draw(app);
            }
            return TRUE;
        default: break;
        }
    }

#ifdef HDE_CMD_HAVE_VTE
    if (app->options.use_vte) {
        if (ctrl && shift && (key == GDK_KEY_c || key == GDK_KEY_C)) { cmd_vte_copy_clipboard(app->vte); return TRUE; }
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
        if (!app->options.use_vte && app->vt) {
            int column, row;
            event_cell(app, event->x, event->y, &column, &row);
            g_free(app->hovered_url);
            app->hovered_url = url_at_cell(app, row, column);
            if (app->open_url_item) gtk_widget_set_visible(app->open_url_item, app->hovered_url != NULL);
            if (app->copy_url_item) gtk_widget_set_visible(app->copy_url_item, app->hovered_url != NULL);
        }
        gtk_widget_grab_focus(app->terminal_widget);
        if (app->context_menu) {
            gtk_menu_popup_at_pointer(GTK_MENU(app->context_menu), (GdkEvent *)event);
            return TRUE;
        }
        return FALSE;
    }
    if (app->options.use_vte) return FALSE;
    if (event->button == 1) {
        int column, row;
        event_cell(app, event->x, event->y, &column, &row);
        if ((event->state & GDK_CONTROL_MASK) && event->type == GDK_BUTTON_PRESS) {
            char *url = url_at_cell(app, row, column);
            if (url) {
                g_app_info_launch_default_for_uri(url, NULL, NULL);
                g_free(url);
                return TRUE;
            }
        }
        if (hde_cmd_vt_mouse_mode(app->vt) && !(event->state & GDK_SHIFT_MASK)) {
            mouse_send(app, 0, column, row, 0);
            return TRUE;
        }
        gtk_widget_grab_focus(app->terminal_widget);
        if (event->type == GDK_2BUTTON_PRESS) {
            int x1 = column, x2 = column;
            char *w = extract_word_at(app, row, column, &x1, &x2);
            g_free(w);
            app->selection_y1 = app->selection_y2 = row;
            app->selection_x1 = x1;
            app->selection_x2 = x2;
            app->selecting = 1;
            char *text = selection_text(app);
            if (text) {
                gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_PRIMARY), text, -1);
                g_free(text);
            }
            queue_draw(app);
            return TRUE;
        }
        if (event->type == GDK_3BUTTON_PRESS) {
            app->selection_y1 = app->selection_y2 = row;
            app->selection_x1 = 0;
            app->selection_x2 = MAX(0, app->columns - 1);
            app->selecting = 1;
            char *text = selection_text(app);
            if (text) {
                gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_PRIMARY), text, -1);
                g_free(text);
            }
            queue_draw(app);
            return TRUE;
        }
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
    if (app->options.use_vte) cmd_vte_copy_clipboard(app->vte);
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

static void run_find(CmdWindow *app, int direction)
{
    if (!app || !app->vt || !app->find_entry) return;
    const char *q = gtk_entry_get_text(GTK_ENTRY(app->find_entry));
    if (!q || !*q) {
        app->find_match_abs_row = -1;
        if (app->find_status) gtk_label_set_text(GTK_LABEL(app->find_status), "");
        queue_draw(app);
        return;
    }
    int total = hde_cmd_vt_scrollback(app->vt) + hde_cmd_vt_rows(app->vt);
    int start = app->find_match_abs_row >= 0 ? app->find_match_abs_row + direction
              : (direction < 0 ? total - 1 : 0);
    if (start < 0) start = total - 1;
    if (start >= total) start = 0;

    int col = 0, len = 0;
    int r = hde_cmd_vt_find(app->vt, q, start, direction, &col, &len);
    if (r >= 0) {
        app->find_match_abs_row = r;
        app->find_match_col = col;
        app->find_match_len = len;
        int history = hde_cmd_vt_scrollback(app->vt);
        int rows = hde_cmd_vt_rows(app->vt);
        int cur_off = hde_cmd_vt_view_offset(app->vt);
        int top_abs = history - cur_off;
        if (r < top_abs || r >= top_abs + rows) {
            int target_top = CLAMP(r - rows / 2, 0, history);
            int target_off = history - target_top;
            hde_cmd_vt_scroll_view(app->vt, target_off - cur_off);
        }
        char st[48];
        g_snprintf(st, sizeof st, "Row %d", r + 1);
        if (app->find_status) gtk_label_set_text(GTK_LABEL(app->find_status), st);
    } else {
        app->find_match_abs_row = -1;
        if (app->find_status) gtk_label_set_text(GTK_LABEL(app->find_status), "No match");
    }
    queue_draw(app);
}

static void on_find_changed(GtkSearchEntry *e, gpointer userdata)
{
    (void)e;
    CmdWindow *app = userdata;
    app->find_match_abs_row = -1;
    run_find(app, -1);
}

static void on_find_next(GtkWidget *w, gpointer userdata)
{
    (void)w;
    run_find(userdata, 1);
}

static void on_find_prev(GtkWidget *w, gpointer userdata)
{
    (void)w;
    run_find(userdata, -1);
}

static void on_find_close(GtkWidget *w, gpointer userdata)
{
    (void)w;
    toggle_find_bar(userdata, FALSE);
}

static gboolean on_find_key_press(GtkWidget *w, GdkEventKey *ev, gpointer userdata)
{
    (void)w;
    CmdWindow *app = userdata;
    if (ev->keyval == GDK_KEY_Escape) {
        toggle_find_bar(app, FALSE);
        return TRUE;
    }
    if (ev->keyval == GDK_KEY_Return || ev->keyval == GDK_KEY_KP_Enter) {
        run_find(app, (ev->state & GDK_SHIFT_MASK) ? -1 : 1);
        return TRUE;
    }
    return FALSE;
}

static void toggle_find_bar(CmdWindow *app, gboolean show)
{
    if (!app || !app->find_bar) return;
    if (show) {
        gtk_widget_show(app->find_bar);
        gtk_widget_show_all(app->find_bar);
        if (app->find_entry) gtk_widget_grab_focus(app->find_entry);
    } else {
        app->find_match_abs_row = -1;
        gtk_widget_hide(app->find_bar);
        if (app->terminal_widget) gtk_widget_grab_focus(app->terminal_widget);
        queue_draw(app);
    }
}



static GtkWidget *build_find_bar(CmdWindow *app)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_style_context_add_class(gtk_widget_get_style_context(bar), "cmd-find");
    gtk_container_set_border_width(GTK_CONTAINER(bar), 6);
    gtk_widget_set_visible(bar, FALSE);

    app->find_entry = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(app->find_entry), "Find in terminal…");
    gtk_widget_set_hexpand(app->find_entry, TRUE);
    g_signal_connect(app->find_entry, "search-changed", G_CALLBACK(on_find_changed), app);
    g_signal_connect(app->find_entry, "key-press-event", G_CALLBACK(on_find_key_press), app);
    gtk_box_pack_start(GTK_BOX(bar), app->find_entry, TRUE, TRUE, 0);

    app->find_status = gtk_label_new("");
    gtk_box_pack_start(GTK_BOX(bar), app->find_status, FALSE, FALSE, 4);

    GtkWidget *prev = gtk_button_new_from_icon_name("go-up-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(prev, "Previous match (Shift+Enter)");
    g_signal_connect(prev, "clicked", G_CALLBACK(on_find_prev), app);
    gtk_box_pack_start(GTK_BOX(bar), prev, FALSE, FALSE, 0);

    GtkWidget *next = gtk_button_new_from_icon_name("go-down-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(next, "Next match (Enter)");
    g_signal_connect(next, "clicked", G_CALLBACK(on_find_next), app);
    gtk_box_pack_start(GTK_BOX(bar), next, FALSE, FALSE, 0);

    GtkWidget *close = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(close, "Close search bar (Escape)");
    g_signal_connect(close, "clicked", G_CALLBACK(on_find_close), app);
    gtk_box_pack_start(GTK_BOX(bar), close, FALSE, FALSE, 0);

    app->find_bar = bar;
    return bar;
}

static void menu_find(GtkMenuItem *item, gpointer userdata)
{
    (void)item;
    toggle_find_bar(userdata, TRUE);
}

static void menu_select_all(GtkMenuItem *item, gpointer userdata)
{
    (void)item;
    select_all_cells(userdata);
}

static void menu_save_output(GtkMenuItem *item, gpointer userdata)
{
    (void)item;
    save_output_dialog(userdata);
}

static void menu_clear_scrollback(GtkMenuItem *item, gpointer userdata)
{
    CmdWindow *app = userdata;
    (void)item;
    if (app->vt) {
        hde_cmd_vt_clear_scrollback(app->vt);
        queue_draw(app);
    }
}

static void menu_reset_terminal(GtkMenuItem *item, gpointer userdata)
{
    CmdWindow *app = userdata;
    (void)item;
#ifdef HDE_CMD_HAVE_VTE
    if (app->options.use_vte && app->vte) {
        vte_terminal_reset(app->vte, TRUE, TRUE);
        return;
    }
#endif
    if (app->vt) {
        hde_cmd_vt_reset(app->vt);
        pty_write(app, "\014", 1);
        queue_draw(app);
    }
}

static void menu_open_url(GtkMenuItem *item, gpointer userdata)
{
    CmdWindow *app = userdata;
    (void)item;
    if (app->hovered_url) g_app_info_launch_default_for_uri(app->hovered_url, NULL, NULL);
}

static void menu_copy_url(GtkMenuItem *item, gpointer userdata)
{
    CmdWindow *app = userdata;
    (void)item;
    if (app->hovered_url)
        gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), app->hovered_url, -1);
}

static void menu_set_theme(GtkMenuItem *item, gpointer userdata)
{
    CmdWindow *app = userdata;
    int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), "hde-theme-idx"));
    app->theme_index = CLAMP(idx, 0, (int)G_N_ELEMENTS(CMD_THEMES) - 1);
    save_preferences(app);
    apply_cmd_css(app);
    apply_vte_theme(app);
    queue_draw(app);
}

static void menu_set_cursor(GtkMenuItem *item, gpointer userdata)
{
    CmdWindow *app = userdata;
    int style = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), "hde-cursor-style"));
    app->cursor_style = CLAMP(style, 0, 2);
    save_preferences(app);
    queue_draw(app);
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

#ifndef HDE_VERSION
#define HDE_VERSION "1.0.0"
#endif

static void menu_about(GtkMenuItem *mi, gpointer userdata)
{
    (void)mi;
    CmdWindow *app = userdata;
    const char *authors[] = { "Hyggshi OS Research Technology", NULL };
    gtk_show_about_dialog(app && app->window ? GTK_WINDOW(app->window) : NULL,
        "program-name", "HDE Cmd",
        "version", HDE_VERSION,
        "comments", "Fast, customizable VT100/xterm-256color terminal emulator with search, color themes, and URL detection for Hyggshi Desktop Environment.",
        "website", "https://github.com/Hyggshi-OS-Research-Technology/NexWM",
        "website-label", "NexWM / HDE Repository",
        "copyright", "Copyright (c) 2024-2026 Hyggshi OS Research Technology",
        "license-type", GTK_LICENSE_MIT_X11,
        "authors", authors,
        "logo-icon-name", "utilities-terminal",
        NULL);
}

/* The menu of the terminal (right click). HDE Cmd shows no header bar of its own: the window is a plain
 * terminal — the title bar of the window manager, the text and nothing else. */
static GtkWidget *build_context_menu(CmdWindow *app)
{
    GtkWidget *menu = gtk_menu_new();
    app->open_url_item = menu_item(menu, "Open Link", G_CALLBACK(menu_open_url), app);
    app->copy_url_item = menu_item(menu, "Copy Link Address", G_CALLBACK(menu_copy_url), app);
    gtk_widget_set_no_show_all(app->open_url_item, TRUE);
    gtk_widget_set_no_show_all(app->copy_url_item, TRUE);
    menu_item(menu, "New Terminal Window  Ctrl+Shift+T", G_CALLBACK(menu_new), app);
    menu_item(menu, "Open Folder in New Terminal…  Ctrl+Shift+O", G_CALLBACK(menu_open_folder), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    menu_item(menu, "Copy  Ctrl+Shift+C", G_CALLBACK(menu_copy), app);
    menu_item(menu, "Paste  Ctrl+Shift+V", G_CALLBACK(menu_paste), app);
    menu_item(menu, "Select All  Ctrl+Shift+A", G_CALLBACK(menu_select_all), app);
    menu_item(menu, "Find…  Ctrl+Shift+F", G_CALLBACK(menu_find), app);
    menu_item(menu, "Save Output to File…  Ctrl+Shift+S", G_CALLBACK(menu_save_output), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    GtkWidget *theme_mi = gtk_menu_item_new_with_label("Color Theme");
    GtkWidget *theme_sub = gtk_menu_new();
    for (guint i = 0; i < G_N_ELEMENTS(CMD_THEMES); i++) {
        GtkWidget *ti = gtk_menu_item_new_with_label(CMD_THEMES[i].name);
        g_object_set_data(G_OBJECT(ti), "hde-theme-idx", GINT_TO_POINTER((int)i));
        g_signal_connect(ti, "activate", G_CALLBACK(menu_set_theme), app);
        gtk_menu_shell_append(GTK_MENU_SHELL(theme_sub), ti);
    }
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(theme_mi), theme_sub);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), theme_mi);

    GtkWidget *cur_mi = gtk_menu_item_new_with_label("Cursor Style");
    GtkWidget *cur_sub = gtk_menu_new();
    static const char *const cur_names[] = { "Block (Default)", "Beam (│)", "Underline (_)" };
    for (int i = 0; i < 3; i++) {
        GtkWidget *ci = gtk_menu_item_new_with_label(cur_names[i]);
        g_object_set_data(G_OBJECT(ci), "hde-cursor-style", GINT_TO_POINTER(i));
        g_signal_connect(ci, "activate", G_CALLBACK(menu_set_cursor), app);
        gtk_menu_shell_append(GTK_MENU_SHELL(cur_sub), ci);
    }
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(cur_mi), cur_sub);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), cur_mi);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    menu_item(menu, "Zoom In  Ctrl++", G_CALLBACK(menu_font_up), app);
    menu_item(menu, "Zoom Out  Ctrl+-", G_CALLBACK(menu_font_down), app);
    menu_item(menu, "Reset Zoom  Ctrl+0", G_CALLBACK(menu_font_reset), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    menu_item(menu, "Clear Scrollback  Ctrl+Shift+K", G_CALLBACK(menu_clear_scrollback), app);
    menu_item(menu, "Reset Terminal", G_CALLBACK(menu_reset_terminal), app);
    menu_item(menu, "Toggle Full Screen  F11", G_CALLBACK(menu_fullscreen), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    menu_item(menu, "About HDE Cmd…", G_CALLBACK(menu_about), app);
    menu_item(menu, "Close Window  Ctrl+Shift+W", G_CALLBACK(menu_close), app);
    gtk_widget_show_all(menu);
    app->context_menu = menu;
    return menu;
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
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    const char *title = vte_terminal_get_window_title(terminal);
    G_GNUC_END_IGNORE_DEPRECATIONS
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
    gtk_style_context_add_class(gtk_widget_get_style_context(app->window), "cmd-window");
    gtk_window_set_title(GTK_WINDOW(app->window), app->options.title ? app->options.title : "HDE Cmd");
    gtk_window_set_icon_name(GTK_WINDOW(app->window), "utilities-terminal");
    gtk_window_set_position(GTK_WINDOW(app->window), GTK_WIN_POS_CENTER);
    gtk_window_set_decorated(GTK_WINDOW(app->window), TRUE);
    build_context_menu(app);
    apply_cmd_css(app);
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

        app->terminal_frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_style_context_add_class(gtk_widget_get_style_context(app->terminal_frame), "cmd-terminal-frame");
        gtk_box_pack_start(GTK_BOX(app->terminal_frame), app->terminal_widget, TRUE, TRUE, 0);

        GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_box_pack_start(GTK_BOX(vbox), app->terminal_frame, TRUE, TRUE, 0);
        gtk_container_add(GTK_CONTAINER(app->window), vbox);
        gtk_widget_show_all(app->window);
        apply_cmd_css(app);
        apply_vte_theme(app);
        update_font(app, DEFAULT_FONT_SIZE);
        set_title(app, app->options.title ? app->options.title : "HDE Cmd");
        start_vte_session(app);
        gtk_widget_grab_focus(app->terminal_widget);
        return;
#endif
    }

    int init_cols = app->options.initial_columns > 0 ? app->options.initial_columns : DEFAULT_COLUMNS;
    int init_rows = app->options.initial_rows > 0 ? app->options.initial_rows : DEFAULT_ROWS;
    app->vt = hde_cmd_vt_new(init_rows, init_cols);
    if (!app->vt) {
        g_printerr("hde-cmd: cannot allocate the terminal screen\n");
        g_application_quit(G_APPLICATION(application));
        return;
    }
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(vbox), build_find_bar(app), FALSE, FALSE, 0);
    app->terminal_frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(app->terminal_frame), "cmd-terminal-frame");
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
    gtk_box_pack_start(GTK_BOX(app->terminal_frame), app->terminal_widget, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), app->terminal_frame, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(app->window), vbox);
    app->rows = init_rows;
    app->columns = init_cols;
    app->cursor_phase = 1;
    update_font(app, app->font_size);
    /* the window is exactly the grid of the terminal (like every other terminal): 80x24 cells by default */
    if (app->cell_width > 0 && app->cell_height > 0) {
        gtk_window_resize(GTK_WINDOW(app->window),
                          2 * PAD_X + init_cols * app->cell_width,
                          2 * PAD_Y + init_rows * app->cell_height);
    }
    gtk_widget_show_all(app->window);
    if (app->find_bar)
        gtk_widget_hide(app->find_bar);
    apply_cmd_css(app);
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
    app.find_match_abs_row = -1;
    load_preferences(&app);

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
    g_free(app.hovered_url);
    g_free(app.title);
    g_object_unref(application);
    return status;
}
