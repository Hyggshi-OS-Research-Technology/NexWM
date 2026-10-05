/* hde-hotkeys — system-level X11 shortcuts for HDE.
 * PrtSc / Shift+PrtSc / Alt+PrtSc are handled here, so they work on a real X11 session.
 */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdio.h>
#include <string.h>

static volatile sig_atomic_t stop_flag = 0;
static void on_signal(int sig) { (void)sig; stop_flag = 1; }

static void spawn_cmd(const char *cmd)
{
    pid_t p = fork();
    if (p < 0) return;
    if (p == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
}

static int have_bin(const char *name)
{
    const char *path = getenv("PATH");
    if (!path) path = "/usr/local/bin:/usr/bin:/bin";
    char *copy = strdup(path);
    if (!copy) return 0;
    int found = 0;
    for (char *d = strtok(copy, ":"); d && !found; d = strtok(NULL, ":")) {
        char buf[4096];
        snprintf(buf, sizeof buf, "%s/%s", *d ? d : ".", name);
        if (access(buf, X_OK) == 0) found = 1;
    }
    free(copy);
    return found;
}

/* Mỗi công cụ: lệnh cho toàn màn hình / vùng chọn / cửa sổ đang hoạt động.
 * scrot lưu vào ~/Pictures (tạo nếu chưa có) thay vì thư mục hiện tại. */
static const struct { const char *bin, *full, *area, *window; } tools[] = {
    { "gnome-screenshot",     "gnome-screenshot",
                              "gnome-screenshot -a",
                              "gnome-screenshot -w" },
    { "xfce4-screenshooter",  "xfce4-screenshooter -f",
                              "xfce4-screenshooter -r",
                              "xfce4-screenshooter -w" },
    { "flameshot",            "flameshot full -p \"$HOME/Pictures\"",
                              "flameshot gui",
                              "flameshot gui" },
    { "scrot",                "mkdir -p \"$HOME/Pictures\" && scrot \"$HOME/Pictures/screenshot-%Y%m%d-%H%M%S.png\"",
                              "mkdir -p \"$HOME/Pictures\" && scrot -s \"$HOME/Pictures/screenshot-%Y%m%d-%H%M%S.png\"",
                              "mkdir -p \"$HOME/Pictures\" && scrot -u \"$HOME/Pictures/screenshot-%Y%m%d-%H%M%S.png\"" },
};

static void screenshot(unsigned int state)
{
    /* Shift+Print = vùng chọn, Alt+Print = cửa sổ đang hoạt động, Print = toàn màn hình. */
    for (unsigned i = 0; i < sizeof tools / sizeof tools[0]; ++i) {
        if (!have_bin(tools[i].bin)) continue;
        spawn_cmd((state & ShiftMask) ? tools[i].area
                  : (state & Mod1Mask) ? tools[i].window : tools[i].full);
        return;
    }
    fprintf(stderr, "hde-hotkeys: no screenshot tool found (gnome-screenshot, xfce4-screenshooter, flameshot, scrot)\n");
    if (have_bin("notify-send"))
        spawn_cmd("notify-send 'HDE' 'Chưa có công cụ chụp màn hình. Cài: sudo apt install scrot'");
    else if (have_bin("xmessage"))
        spawn_cmd("xmessage -timeout 8 'HDE: chua co cong cu chup man hinh. Cai: sudo apt install scrot'");
}

/* Nếu window manager (vd. Openbox) đã chiếm phím Print thì XGrabKey trả BadAccess.
 * Handler mặc định của Xlib sẽ thoát cả tiến trình -> bắt lỗi và chỉ cảnh báo. */
static int g_grab_failed = 0;
static int on_x_error(Display *d, XErrorEvent *e)
{
    (void)d;
    if (e->error_code == BadAccess) g_grab_failed = 1;
    else fprintf(stderr, "hde-hotkeys: X error code=%d request=%d\n", e->error_code, e->request_code);
    return 0;
}

int main(void)
{
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "hde-hotkeys: cannot open X display\n");
        return 1;
    }

    Window root = DefaultRootWindow(dpy);
    KeyCode print_key = XKeysymToKeycode(dpy, XK_Print);
    if (!print_key) {
        fprintf(stderr, "hde-hotkeys: Print/SysRq key is unavailable\n");
        XCloseDisplay(dpy);
        return 1;
    }

    XSetErrorHandler(on_x_error);
    int ignored_mods[] = { 0, LockMask, Mod2Mask, Mod5Mask,
                           LockMask|Mod2Mask, LockMask|Mod5Mask,
                           Mod2Mask|Mod5Mask, LockMask|Mod2Mask|Mod5Mask };
    for (unsigned i = 0; i < sizeof(ignored_mods)/sizeof(ignored_mods[0]); ++i) {
        XGrabKey(dpy, print_key, ignored_mods[i], root, True,
                 GrabModeAsync, GrabModeAsync);
        XGrabKey(dpy, print_key, ignored_mods[i] | ShiftMask, root, True,
                 GrabModeAsync, GrabModeAsync);
        XGrabKey(dpy, print_key, ignored_mods[i] | Mod1Mask, root, True,
                 GrabModeAsync, GrabModeAsync);
    }
    XSelectInput(dpy, root, KeyPressMask);
    XSync(dpy, False);
    if (g_grab_failed)
        fprintf(stderr, "hde-hotkeys: phím Print đã bị chương trình khác (window manager?) chiếm; "
                        "xóa keybind 'Print' trong ~/.config/openbox/rc.xml nếu muốn HDE tự xử lý.\n");

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    while (!stop_flag) {
        XEvent ev;
        if (XPending(dpy)) {
            XNextEvent(dpy, &ev);
            if (ev.type == KeyPress && ev.xkey.keycode == print_key)
                screenshot(ev.xkey.state);
        } else {
            usleep(20000);
        }
    }

    XUngrabKey(dpy, AnyKey, AnyModifier, root);
    XCloseDisplay(dpy);
    return 0;
}
