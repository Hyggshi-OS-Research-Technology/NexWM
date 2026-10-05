/* hde-hotkeys — phím tắt hệ thống của HDE (Xlib + XInput2), chạy độc lập với GTK.
 *
 *  Super (nhấn rồi thả riêng)   mở / đóng Start menu (gửi lệnh tới hde-panel, xem hde-ipc.h)
 *  F1 / F2 / F3                  tắt tiếng / giảm / tăng âm lượng   (settings.ini: fkeys_sound=true)
 *  Phím media                    Mute, Volume-/+, MicMute, Brightness-/+, Play/Pause/Next/Prev
 *  PrtSc, Shift+PrtSc, Alt+PrtSc chụp toàn màn hình / vùng chọn / cửa sổ
 *  Super+L  khoá màn hình        Super+E  trình quản lý file      Super+D  hiện desktop
 *  Super+R, Alt+F2  hộp thoại Run Super+S  tìm ứng dụng           Ctrl+Alt+T  terminal
 *  Ctrl+Alt+Delete  hộp thoại Session / Power
 *
 * Phím Super được nhận qua XInput2 raw events (không grab) nên Super+<phím> của WM và ứng dụng vẫn
 * hoạt động bình thường; menu chỉ mở khi Super được nhấn rồi thả mà không kèm phím/chuột nào khác.
 * Cấu hình (~/.config/hde/settings.ini, tự nạp lại khi file đổi hoặc khi nhận SIGHUP):
 *   super_menu=true  fkeys_sound=true  media_keys=true  system_shortcuts=true
 */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/XF86keysym.h>
#ifdef HAVE_XI2
#include <X11/extensions/XInput2.h>
#endif
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include "hde-ipc.h"

static volatile sig_atomic_t stop_flag = 0;
static volatile sig_atomic_t reload_flag = 0;
static Display *dpy;
static Window root;

/* ---------------- cấu hình ---------------- */
typedef struct { int super_menu, fkeys, media, shortcuts; } Config;
static Config cfg = { 1, 1, 1, 1 };
static char cfg_path[4096];
static unsigned long long cfg_sig = ~0ULL;

static int cfg_bool(const char *key, int def)
{
    FILE *f = fopen(cfg_path, "r");
    if (!f) return def;
    char line[1024];
    size_t n = strlen(key);
    int v = def;
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, key, n) != 0) continue;
        char *q = p + n;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != '=') continue;
        q++;
        while (*q == ' ' || *q == '\t') q++;
        v = !(strncasecmp(q, "false", 5) == 0 || strncasecmp(q, "no", 2) == 0 ||
              strncasecmp(q, "off", 3) == 0 || *q == '0');
    }
    fclose(f);
    return v;
}

static void load_config(void)
{
    cfg.super_menu = cfg_bool("super_menu", 1);
    cfg.fkeys = cfg_bool("fkeys_sound", 1);
    cfg.media = cfg_bool("media_keys", 1);
    cfg.shortcuts = cfg_bool("system_shortcuts", 1);
}

/* inode + kích thước + mtime (ns): nhận ra cả hai lần ghi trong cùng một giây */
static int config_changed(void)
{
    struct stat st;
    unsigned long long sig = 0;
    if (stat(cfg_path, &st) == 0)
        sig = ((unsigned long long)st.st_ino * 1000003ULL) ^ ((unsigned long long)st.st_size << 32) ^
              ((unsigned long long)st.st_mtim.tv_sec * 1000000000ULL + (unsigned long long)st.st_mtim.tv_nsec);
    if (sig == cfg_sig) return 0;
    cfg_sig = sig;
    return 1;
}

/* ---------------- tiến trình con ---------------- */
static void on_signal(int sig) { (void)sig; stop_flag = 1; }
static void on_hup(int sig) { (void)sig; reload_flag = 1; }

static void on_sigchld(int sig)
{
    (void)sig;
    int saved = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0) {}
    errno = saved;
}

/* fork + reset tín hiệu để chương trình con không thừa hưởng handler của daemon. */
static pid_t fork_child(void)
{
    pid_t p = fork();
    if (p == 0) {
        setsid();
        signal(SIGCHLD, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        if (dpy) close(ConnectionNumber(dpy));
    }
    return p;
}

static void spawn_sh(const char *cmd)
{
    pid_t p = fork_child();
    if (p == 0) {
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

static void notify(const char *summary, const char *body)
{
    if (have_bin("notify-send")) {
        pid_t p = fork_child();
        if (p == 0) {
            execlp("notify-send", "notify-send", "-a", "HDE", "-i", "dialog-information", summary, body, (char *)NULL);
            _exit(127);
        }
    } else {
        fprintf(stderr, "hde-hotkeys: %s: %s\n", summary, body);
    }
}

/* Gửi lệnh tới hde-panel từ tiến trình con (kết nối X riêng). */
static void child_send_panel(long cmd, long arg)
{
    Display *d = XOpenDisplay(NULL);
    if (!d) return;
    hde_ipc_send(d, cmd, arg, CurrentTime);
    XCloseDisplay(d);
}

/* Chạy lệnh đổi âm lượng/độ sáng trong tiến trình con (tuần tự hoá bằng flock), rồi báo panel hiện OSD. */
static void run_with_osd(const char *script, long osd_cmd, int read_percent)
{
    pid_t p = fork_child();
    if (p != 0) return;
    char full[8192];
    snprintf(full, sizeof full,
             "exec 9>\"${XDG_RUNTIME_DIR:-/tmp}/hde-hotkeys-%s.lock\" && command -v flock >/dev/null 2>&1 && flock 9; %s",
             read_percent ? "brightness" : "volume", script);
    long arg = 0;
    if (read_percent) {
        char buf[64] = "";
        FILE *f = popen(full, "r");
        int st = -1;
        if (f) {
            if (!fgets(buf, sizeof buf, f)) buf[0] = '\0';
            st = pclose(f);
        }
        if (st != -1 && WIFEXITED(st) && WEXITSTATUS(st) == 3) {
            notify("Brightness", "No brightness tool found. Install brightnessctl (sudo apt install brightnessctl).");
            _exit(0);
        }
        arg = buf[0] ? atol(buf) : -1;
    } else {
        int st = system(full);
        (void)st;
    }
    child_send_panel(osd_cmd, arg);
    _exit(0);
}

/* ---------------- hành động ---------------- */
enum {
    A_SHOT_FULL, A_SHOT_AREA, A_SHOT_WIN,
    A_VOL_MUTE, A_VOL_DOWN, A_VOL_UP, A_MIC_MUTE, A_BRIGHT_UP, A_BRIGHT_DOWN,
    A_PLAY, A_NEXT, A_PREV, A_STOP,
    A_LOCK, A_FILES, A_TERMINAL, A_DESKTOP, A_RUN, A_SEARCH, A_POWER
};
enum { G_ALWAYS, G_FKEYS, G_MEDIA, G_SHORTCUTS };

typedef struct { KeySym sym; unsigned mods; int act; int group; const char *name; } Binding;

static const Binding bindings[] = {
    { XK_Print, 0, A_SHOT_FULL, G_ALWAYS, "Print" },
    { XK_Print, ShiftMask, A_SHOT_AREA, G_ALWAYS, "Shift+Print" },
    { XK_Print, Mod1Mask, A_SHOT_WIN, G_ALWAYS, "Alt+Print" },
    { XK_F1, 0, A_VOL_MUTE, G_FKEYS, "F1" },
    { XK_F2, 0, A_VOL_DOWN, G_FKEYS, "F2" },
    { XK_F3, 0, A_VOL_UP, G_FKEYS, "F3" },
    { XF86XK_AudioMute, 0, A_VOL_MUTE, G_MEDIA, "XF86AudioMute" },
    { XF86XK_AudioLowerVolume, 0, A_VOL_DOWN, G_MEDIA, "XF86AudioLowerVolume" },
    { XF86XK_AudioRaiseVolume, 0, A_VOL_UP, G_MEDIA, "XF86AudioRaiseVolume" },
    { XF86XK_AudioMicMute, 0, A_MIC_MUTE, G_MEDIA, "XF86AudioMicMute" },
    { XF86XK_MonBrightnessUp, 0, A_BRIGHT_UP, G_MEDIA, "XF86MonBrightnessUp" },
    { XF86XK_MonBrightnessDown, 0, A_BRIGHT_DOWN, G_MEDIA, "XF86MonBrightnessDown" },
    { XF86XK_AudioPlay, 0, A_PLAY, G_MEDIA, "XF86AudioPlay" },
    { XF86XK_AudioPause, 0, A_PLAY, G_MEDIA, "XF86AudioPause" },
    { XF86XK_AudioNext, 0, A_NEXT, G_MEDIA, "XF86AudioNext" },
    { XF86XK_AudioPrev, 0, A_PREV, G_MEDIA, "XF86AudioPrev" },
    { XF86XK_AudioStop, 0, A_STOP, G_MEDIA, "XF86AudioStop" },
    { XF86XK_ScreenSaver, 0, A_LOCK, G_MEDIA, "XF86ScreenSaver" },
    { XF86XK_Search, 0, A_SEARCH, G_MEDIA, "XF86Search" },
    { XF86XK_Explorer, 0, A_FILES, G_MEDIA, "XF86Explorer" },
    { XK_l, Mod4Mask, A_LOCK, G_SHORTCUTS, "Super+L" },
    { XK_e, Mod4Mask, A_FILES, G_SHORTCUTS, "Super+E" },
    { XK_d, Mod4Mask, A_DESKTOP, G_SHORTCUTS, "Super+D" },
    { XK_r, Mod4Mask, A_RUN, G_SHORTCUTS, "Super+R" },
    { XK_s, Mod4Mask, A_SEARCH, G_SHORTCUTS, "Super+S" },
    { XK_F2, Mod1Mask, A_RUN, G_SHORTCUTS, "Alt+F2" },
    { XK_t, ControlMask | Mod1Mask, A_TERMINAL, G_SHORTCUTS, "Ctrl+Alt+T" },
    { XK_Delete, ControlMask | Mod1Mask, A_POWER, G_SHORTCUTS, "Ctrl+Alt+Delete" },
};
#define N_BINDINGS (sizeof bindings / sizeof bindings[0])
static KeyCode codes[N_BINDINGS];
static int grabbed[N_BINDINGS];

static const struct { const char *bin, *full, *area, *window; } shot_tools[] = {
    { "gnome-screenshot", "gnome-screenshot", "gnome-screenshot -a", "gnome-screenshot -w" },
    { "xfce4-screenshooter", "xfce4-screenshooter -f", "xfce4-screenshooter -r", "xfce4-screenshooter -w" },
    { "mate-screenshot", "mate-screenshot", "mate-screenshot -a", "mate-screenshot -w" },
    { "flameshot", "flameshot full -p \"$HOME/Pictures\"", "flameshot gui", "flameshot gui" },
    { "spectacle", "spectacle -f -b", "spectacle -r", "spectacle -a -b" },
    { "scrot", "mkdir -p \"$HOME/Pictures\" && scrot \"$HOME/Pictures/screenshot-%Y%m%d-%H%M%S.png\"",
      "mkdir -p \"$HOME/Pictures\" && scrot -s \"$HOME/Pictures/screenshot-%Y%m%d-%H%M%S.png\"",
      "mkdir -p \"$HOME/Pictures\" && scrot -u \"$HOME/Pictures/screenshot-%Y%m%d-%H%M%S.png\"" },
    { "import", "mkdir -p \"$HOME/Pictures\" && import -window root \"$HOME/Pictures/screenshot-$(date +%Y%m%d-%H%M%S).png\"",
      "mkdir -p \"$HOME/Pictures\" && import \"$HOME/Pictures/screenshot-$(date +%Y%m%d-%H%M%S).png\"",
      "mkdir -p \"$HOME/Pictures\" && import \"$HOME/Pictures/screenshot-$(date +%Y%m%d-%H%M%S).png\"" },
};

static void screenshot(int act)
{
    for (unsigned i = 0; i < sizeof shot_tools / sizeof shot_tools[0]; ++i) {
        if (!have_bin(shot_tools[i].bin)) continue;
        spawn_sh(act == A_SHOT_AREA ? shot_tools[i].area : act == A_SHOT_WIN ? shot_tools[i].window : shot_tools[i].full);
        return;
    }
    notify("Screenshot", "No screenshot tool found. Install one: sudo apt install gnome-screenshot (or scrot)");
}

static void media(const char *what)
{
    if (have_bin("playerctl")) {
        char cmd[128];
        snprintf(cmd, sizeof cmd, "playerctl %s", what);
        spawn_sh(cmd);
    } else {
        notify("Media keys", "Install playerctl to control music players (sudo apt install playerctl).");
    }
}

static void toggle_show_desktop(void)
{
    Atom a = XInternAtom(dpy, "_NET_SHOWING_DESKTOP", False);
    Atom type;
    int fmt;
    unsigned long n, after;
    unsigned char *data = NULL;
    long cur = 0;
    if (XGetWindowProperty(dpy, root, a, 0, 1, False, XA_CARDINAL, &type, &fmt, &n, &after, &data) == Success && data) {
        if (n == 1) cur = *(long *)data;
        XFree(data);
    }
    XEvent e;
    memset(&e, 0, sizeof e);
    e.xclient.type = ClientMessage;
    e.xclient.window = root;
    e.xclient.message_type = a;
    e.xclient.format = 32;
    e.xclient.data.l[0] = !cur;
    XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
    XFlush(dpy);
}

static void panel_cmd(long cmd, Time t)
{
    if (hde_ipc_send(dpy, cmd, 0, t) != 0)
        fprintf(stderr, "hde-hotkeys: hde-panel is not running (command %ld ignored)\n", cmd);
}

static void do_action(int act, Time t)
{
    switch (act) {
    case A_SHOT_FULL: case A_SHOT_AREA: case A_SHOT_WIN: screenshot(act); break;
    case A_VOL_MUTE:  run_with_osd(HDE_SH_VOLUME_MUTE, HDE_CMD_OSD_VOLUME, 0); break;
    case A_VOL_DOWN:  run_with_osd(HDE_SH_VOLUME_DOWN, HDE_CMD_OSD_VOLUME, 0); break;
    case A_VOL_UP:    run_with_osd(HDE_SH_VOLUME_UP, HDE_CMD_OSD_VOLUME, 0); break;
    case A_MIC_MUTE:  run_with_osd(HDE_SH_MIC_MUTE, HDE_CMD_OSD_MIC, 0); break;
    case A_BRIGHT_UP:   run_with_osd(HDE_SH_BRIGHTNESS_UP, HDE_CMD_OSD_BRIGHTNESS, 1); break;
    case A_BRIGHT_DOWN: run_with_osd(HDE_SH_BRIGHTNESS_DOWN, HDE_CMD_OSD_BRIGHTNESS, 1); break;
    case A_PLAY: media("play-pause"); break;
    case A_NEXT: media("next"); break;
    case A_PREV: media("previous"); break;
    case A_STOP: media("stop"); break;
    case A_LOCK: spawn_sh(HDE_SH_LOCK); break;
    case A_FILES: spawn_sh(HDE_SH_FILES); break;
    case A_TERMINAL: spawn_sh(HDE_SH_TERMINAL); break;
    case A_DESKTOP: toggle_show_desktop(); break;
    case A_RUN: panel_cmd(HDE_CMD_RUN, t); break;
    case A_SEARCH: panel_cmd(HDE_CMD_SEARCH, t); break;
    case A_POWER: panel_cmd(HDE_CMD_POWER, t); break;
    default: break;
    }
}

/* ---------------- grab phím ---------------- */
/* Nếu WM đã chiếm một phím thì XGrabKey trả BadAccess; handler mặc định của Xlib sẽ thoát cả
 * tiến trình -> bắt lỗi, cảnh báo rồi bỏ qua phím đó. */
static int g_grab_failed = 0;
static int on_x_error(Display *d, XErrorEvent *e)
{
    (void)d;
    if (e->error_code == BadAccess) g_grab_failed = 1;
    else fprintf(stderr, "hde-hotkeys: X error code=%d request=%d\n", e->error_code, e->request_code);
    return 0;
}

static int on_x_io_error(Display *d)
{
    (void)d;
    fprintf(stderr, "hde-hotkeys: lost connection to the X server\n");
    _exit(0);
    return 0;
}

static int group_enabled(int g)
{
    return g == G_ALWAYS || (g == G_FKEYS && cfg.fkeys) || (g == G_MEDIA && cfg.media) ||
           (g == G_SHORTCUTS && cfg.shortcuts);
}

static const unsigned ignored_mods[] = { 0, LockMask, Mod2Mask, Mod5Mask, LockMask | Mod2Mask, LockMask | Mod5Mask,
                                         Mod2Mask | Mod5Mask, LockMask | Mod2Mask | Mod5Mask };

static void grab_all(void)
{
    XUngrabKey(dpy, AnyKey, AnyModifier, root);
    XSync(dpy, False);
    int failed = 0;
    char failed_names[1024] = "";
    for (unsigned i = 0; i < N_BINDINGS; i++) {
        codes[i] = XKeysymToKeycode(dpy, bindings[i].sym);
        grabbed[i] = 0;
        if (!codes[i] || !group_enabled(bindings[i].group)) continue;
        g_grab_failed = 0;
        for (unsigned k = 0; k < sizeof ignored_mods / sizeof ignored_mods[0]; k++)
            XGrabKey(dpy, codes[i], bindings[i].mods | ignored_mods[k], root, False, GrabModeAsync, GrabModeAsync);
        XSync(dpy, False);
        if (g_grab_failed) {
            failed++;
            if (strlen(failed_names) + strlen(bindings[i].name) + 3 < sizeof failed_names) {
                strcat(failed_names, " ");
                strcat(failed_names, bindings[i].name);
            }
        }
        grabbed[i] = 1;
    }
    if (failed)
        fprintf(stderr, "hde-hotkeys: already used by another program (window manager?):%s\n", failed_names);
    fprintf(stderr, "hde-hotkeys: super_menu=%d fkeys_sound=%d media_keys=%d system_shortcuts=%d\n",
            cfg.super_menu, cfg.fkeys, cfg.media, cfg.shortcuts);
}

static void handle_key(XKeyEvent *k)
{
    unsigned state = k->state & (ShiftMask | ControlMask | Mod1Mask | Mod4Mask);
    for (unsigned i = 0; i < N_BINDINGS; i++) {
        if (grabbed[i] && codes[i] == k->keycode && bindings[i].mods == state) {
            do_action(bindings[i].act, k->time);
            return;
        }
    }
}

/* ---------------- phím Super (XInput2 raw events) ---------------- */
#ifdef HAVE_XI2
static int xi_opcode = -1;
static KeyCode super_l, super_r;
static int super_down, super_other;
static Time super_time;

static void xi2_init(void)
{
    int ev, err, major = 2, minor = 2;
    if (!XQueryExtension(dpy, "XInputExtension", &xi_opcode, &ev, &err) ||
        XIQueryVersion(dpy, &major, &minor) != Success || major < 2) {
        fprintf(stderr, "hde-hotkeys: XInput2 unavailable; Super key will not open the menu\n");
        xi_opcode = -1;
        return;
    }
    unsigned char mask[XIMaskLen(XI_LASTEVENT)];
    memset(mask, 0, sizeof mask);
    XIEventMask em;
    em.deviceid = XIAllMasterDevices;
    em.mask_len = sizeof mask;
    em.mask = mask;
    XISetMask(mask, XI_RawKeyPress);
    XISetMask(mask, XI_RawKeyRelease);
    XISetMask(mask, XI_RawButtonPress);
    XISelectEvents(dpy, root, &em, 1);
    super_l = XKeysymToKeycode(dpy, XK_Super_L);
    super_r = XKeysymToKeycode(dpy, XK_Super_R);
}

static void xi2_event(XGenericEventCookie *c)
{
    XIRawEvent *re = c->data;
    int is_super = re->detail != 0 && ((KeyCode)re->detail == super_l || (KeyCode)re->detail == super_r);
    switch (c->evtype) {
    case XI_RawKeyPress:
        if (is_super) {
            if (!super_down) { super_down = 1; super_other = 0; super_time = re->time; }
        } else if (super_down) {
            super_other = 1;
        }
        break;
    case XI_RawKeyRelease:
        if (is_super && super_down) {
            super_down = 0;
            if (!super_other && cfg.super_menu && re->time - super_time < 3000)
                panel_cmd(HDE_CMD_MENU, re->time);
        }
        break;
    case XI_RawButtonPress:
        if (super_down) super_other = 1;
        break;
    default:
        break;
    }
}
#endif

/* Chỉ cho phép một hde-hotkeys mỗi màn hình (hai bản sẽ mở rồi đóng menu ngay lập tức). */
static int acquire_instance_lock(void)
{
    char name[64];
    snprintf(name, sizeof name, "_HDE_HOTKEYS_S%d", DefaultScreen(dpy));
    Atom sel = XInternAtom(dpy, name, False);
    if (XGetSelectionOwner(dpy, sel) != None) return 0;
    Window w = XCreateSimpleWindow(dpy, root, -10, -10, 1, 1, 0, 0, 0);
    XSetSelectionOwner(dpy, sel, w, CurrentTime);
    return XGetSelectionOwner(dpy, sel) == w;
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("hde-hotkeys: HDE system shortcuts daemon (see the comment at the top of hde-hotkeys.c)\n"
                   "Config: ~/.config/hde/settings.ini  super_menu fkeys_sound media_keys system_shortcuts\n"
                   "Send SIGHUP to reload.\n");
            return 0;
        }
    }
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg) snprintf(cfg_path, sizeof cfg_path, "%s/hde/settings.ini", xdg);
    else snprintf(cfg_path, sizeof cfg_path, "%s/.config/hde/settings.ini", home ? home : "/tmp");

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "hde-hotkeys: cannot open X display\n");
        return 1;
    }
    root = DefaultRootWindow(dpy);
    XSetErrorHandler(on_x_error);
    XSetIOErrorHandler(on_x_io_error);
    if (!acquire_instance_lock()) {
        fprintf(stderr, "hde-hotkeys: another hde-hotkeys is already running\n");
        XCloseDisplay(dpy);
        return 0;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigchld;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_hup);

    config_changed();
    load_config();
    grab_all();
#ifdef HAVE_XI2
    xi2_init();
#else
    fprintf(stderr, "hde-hotkeys: built without XInput2 (libxi-dev); Super key will not open the menu\n");
#endif
    XSelectInput(dpy, root, KeyPressMask);
    XSync(dpy, False);

    int fd = ConnectionNumber(dpy);
    while (!stop_flag) {
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            if (ev.type == KeyPress) {
                handle_key(&ev.xkey);
            } else if (ev.type == MappingNotify) {
                XRefreshKeyboardMapping(&ev.xmapping);
                if (ev.xmapping.request == MappingKeyboard || ev.xmapping.request == MappingModifier) {
                    grab_all();
#ifdef HAVE_XI2
                    super_l = XKeysymToKeycode(dpy, XK_Super_L);
                    super_r = XKeysymToKeycode(dpy, XK_Super_R);
#endif
                }
            }
#ifdef HAVE_XI2
            else if (ev.type == GenericEvent && ev.xcookie.extension == xi_opcode &&
                     XGetEventData(dpy, &ev.xcookie)) {
                xi2_event(&ev.xcookie);
                XFreeEventData(dpy, &ev.xcookie);
            }
#endif
        }
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        struct timeval tv = { 1, 0 };
        if (select(fd + 1, &fds, NULL, NULL, &tv) < 0 && errno != EINTR) break;
        if (reload_flag || config_changed()) {
            reload_flag = 0;
            load_config();
            grab_all();
        }
    }

    XUngrabKey(dpy, AnyKey, AnyModifier, root);
    XCloseDisplay(dpy);
    return 0;
}
