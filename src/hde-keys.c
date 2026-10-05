/* hde-keys: phím tắt toàn cục cho panel — PrtSc / SysRq dùng công cụ chụp màn hình có sẵn.
 *   Print        -> chụp toàn màn hình
 *   Alt+Print    -> chụp cửa sổ đang focus   (Alt+PrtSc = SysRq trên bàn phím vật lý)
 *   Shift+Print  -> chụp vùng chọn
 * Công cụ được thử lần lượt: xfce4-screenshooter, gnome-screenshot, spectacle,
 * flameshot, maim, scrot. Ảnh (maim/scrot) lưu vào ~/Pictures/Screenshot_*.png.
 */
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <string.h>
#include "hde-keys.h"

typedef struct {
    const char *bin;
    const char *full, *win, *area;   /* %f = đường dẫn file (đã quote), %d = thư mục ảnh */
} ShotTool;

static const ShotTool tools[] = {
    { "xfce4-screenshooter", "xfce4-screenshooter -f", "xfce4-screenshooter -w", "xfce4-screenshooter -r" },
    { "gnome-screenshot",    "gnome-screenshot",       "gnome-screenshot -w",    "gnome-screenshot -a" },
    { "spectacle",           "spectacle -b -f",        "spectacle -b -a",        "spectacle -b -r" },
    { "flameshot",           "flameshot full -p %d",   "flameshot gui -p %d",    "flameshot gui -p %d" },
    { "maim",  "maim %f", "maim -i \"$(xdotool getactivewindow)\" %f", "maim -s %f" },
    { "scrot", "scrot %f", "scrot -u %f", "scrot -s %f" },
};

typedef enum { SHOT_FULL, SHOT_WINDOW, SHOT_AREA } ShotMode;

static char *subst(const char *tpl, const char *token, const char *value)
{
    gchar **parts = g_strsplit(tpl, token, -1);
    char *out = g_strjoinv(value, parts);
    g_strfreev(parts);
    return out;
}

static void notify(const char *msg)
{
    if (g_find_program_in_path("notify-send")) {
        const char *argv[] = { "notify-send", "-i", "camera-photo", "Screenshot", msg, NULL };
        g_spawn_async(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
    } else {
        g_print("hde-panel: %s\n", msg);
    }
}

static void take_screenshot(ShotMode mode)
{
    const char *dir = g_get_user_special_dir(G_USER_DIRECTORY_PICTURES);
    char *pdir = dir ? g_strdup(dir) : g_build_filename(g_get_home_dir(), "Pictures", NULL);
    g_mkdir_with_parents(pdir, 0755);

    GDateTime *now = g_date_time_new_now_local();
    char *stamp = g_date_time_format(now, "%Y%m%d_%H%M%S");
    char *file = g_strdup_printf("%s/Screenshot_%s.png", pdir, stamp);
    g_date_time_unref(now);

    char *qfile = g_shell_quote(file), *qdir = g_shell_quote(pdir);
    gboolean done = FALSE;

    for (guint i = 0; i < G_N_ELEMENTS(tools) && !done; i++) {
        if (!g_find_program_in_path(tools[i].bin)) continue;
        const char *tpl = mode == SHOT_FULL ? tools[i].full : mode == SHOT_WINDOW ? tools[i].win : tools[i].area;
        char *c1 = subst(tpl, "%f", qfile);
        char *cmd = subst(c1, "%d", qdir);
        const char *argv[] = { "sh", "-c", cmd, NULL };
        GError *err = NULL;
        if (g_spawn_async(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &err)) {
            done = TRUE;
        } else {
            g_printerr("hde-panel: %s: %s\n", tools[i].bin, err->message);
            g_clear_error(&err);
        }
        g_free(c1); g_free(cmd);
    }
    if (!done)
        notify("No screenshot tool found. Install one of: xfce4-screenshooter, gnome-screenshot, spectacle, flameshot, maim, scrot.");

    g_free(qfile); g_free(qdir); g_free(file); g_free(stamp); g_free(pdir);
}

/* ---------- X11 key grab ---------- */
static KeyCode print_key;

static GdkFilterReturn keys_filter(GdkXEvent *xev, GdkEvent *ev, gpointer data)
{
    (void)ev; (void)data;
    XEvent *e = xev;
    if (e->type != KeyPress || e->xkey.keycode != print_key) return GDK_FILTER_CONTINUE;

    unsigned st = e->xkey.state & (ShiftMask | ControlMask | Mod1Mask | Mod4Mask);
    if (st == 0)             take_screenshot(SHOT_FULL);
    else if (st == Mod1Mask) take_screenshot(SHOT_WINDOW);
    else if (st == ShiftMask) take_screenshot(SHOT_AREA);
    else return GDK_FILTER_CONTINUE;
    return GDK_FILTER_REMOVE;
}

void hde_keys_init(void)
{
    GdkDisplay *gd = gdk_display_get_default();
    if (!GDK_IS_X11_DISPLAY(gd)) return;
    Display *dpy = GDK_DISPLAY_XDISPLAY(gd);
    GdkWindow *groot = gdk_screen_get_root_window(gdk_screen_get_default());
    Window root = GDK_WINDOW_XID(groot);

    print_key = XKeysymToKeycode(dpy, XK_Print);
    if (!print_key) { g_printerr("hde-panel: no Print key on this keyboard\n"); return; }

    /* Grab cho mọi tổ hợp NumLock (Mod2) / CapsLock (Lock) để phím vẫn chạy khi đang bật chúng. */
    const unsigned locks[] = { 0, LockMask, Mod2Mask, LockMask | Mod2Mask };
    const unsigned mods[]  = { 0, Mod1Mask, ShiftMask };
    gboolean failed = FALSE;

    gdk_x11_display_error_trap_push(gd);
    for (guint m = 0; m < G_N_ELEMENTS(mods); m++)
        for (guint l = 0; l < G_N_ELEMENTS(locks); l++)
            XGrabKey(dpy, print_key, mods[m] | locks[l], root, False, GrabModeAsync, GrabModeAsync);
    XSync(dpy, False);
    if (gdk_x11_display_error_trap_pop(gd)) failed = TRUE;

    if (failed)
        g_printerr("hde-panel: Print key already grabbed by another program (xfce4-settings / WM keybinding?)\n");
    gdk_window_add_filter(groot, keys_filter, NULL);
}
