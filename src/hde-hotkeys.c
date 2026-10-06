/* hde-hotkeys — HDE system shortcuts (Xlib + XInput2), runs independently of GTK.
 *
 *  Super (tapped alone)          open / close the Start menu (command sent to hde-panel, see hde-ipc.h)
 *  F1 / F2 / F3                  mute / volume down / volume up   (settings.ini: fkeys_sound=true)
 *  F6 / F7                       screen brightness down / up      (settings.ini: fkeys_display=true)
 *  F8, Super+P, the display key  "Project": PC screen only / Duplicate / Extend / Second screen only, like
 *                                Windows + P (the hde-settings --project window; pressing it again moves on)
 *  Media keys                    Mute, Volume-/+, MicMute, Brightness-/+, Play/Pause/Next/Prev, Display
 *  PrtSc, Shift+PrtSc, Alt+PrtSc screenshot of the whole screen / a selected area / a window
 *                                (HDE's own hde-screenshot; settings.ini screenshot_tool= picks another tool)
 *  Ctrl + the same keys          the same, copied to the clipboard only (no file)
 *  Super+L  lock screen          Super+E  file manager            Super+D  show desktop
 *  Super+R, Alt+F2  Run dialog   Super+S  app search              Ctrl+Alt+T  terminal
 *  Ctrl+Alt+Delete  Session / Power dialog
 *
 * The Super key is read through XInput2 raw events (no grab), so the Super+<key> bindings of the WM and
 * of applications keep working; the menu only opens when Super is pressed and released with no other key/button.
 * Configuration (~/.config/hde/settings.ini, reloaded automatically when the file changes or on SIGHUP):
 *   super_menu=true  fkeys_sound=true  fkeys_display=true  media_keys=true  system_shortcuts=true
 *   screenshot_tool=builtin
 * Brightness needs no extra tool (src/hde-brightness.c): the laptop backlight (directly or through systemd-logind),
 * else software dimming of the screens (desktop monitors, virtual machines).
 *
 * hde-session starts hde-hotkeys BEFORE the window manager (and waits for HDE_READY_FD), so these keys belong
 * to HDE even when the WM's own config binds them too (e.g. Openbox rc.xml: Print -> scrot, W-e -> kfmclient,
 * which fail with "Failed to execute child process" when those programs are not installed).
 * Every Print combination is claimed, so no such WM binding is left over. If another program got a key first
 * anyway (typically: a session started by an older hde-session, which launched the WM first), hde-hotkeys says so
 * in a notification and takes the key over by itself as soon as that program lets go of it (checked every
 * 2 seconds, and every 10 ms for 2 seconds after the window manager exits or is replaced, so that a newly
 * started window manager cannot take the key again).
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
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "hde-ipc.h"
#include "hde-brightness.h"

static volatile sig_atomic_t stop_flag = 0;
static int debug_on;               /* HDE_DEBUG=1: diagnostic log */
static volatile sig_atomic_t reload_flag = 0;
static Display *dpy;
static Window root;

/* ---------------- configuration ---------------- */
typedef struct { int super_menu, fkeys, fkeys_display, media, shortcuts; char shot_tool[64]; } Config;
static Config cfg = { 1, 1, 1, 1, 1, "builtin" };
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

/* String value of key= (the last one wins, like cfg_bool); def if the key is missing or empty. */
static void cfg_str(const char *key, const char *def, char *out, size_t len)
{
    snprintf(out, len, "%s", def);
    FILE *f = fopen(cfg_path, "r");
    if (!f) return;
    char line[1024];
    size_t n = strlen(key);
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, key, n) != 0) continue;
        char *q = p + n;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != '=') continue;
        q++;
        while (*q == ' ' || *q == '\t') q++;
        size_t l = strcspn(q, "\r\n");
        while (l > 0 && (q[l - 1] == ' ' || q[l - 1] == '\t')) l--;
        if (l == 0) snprintf(out, len, "%s", def);
        else snprintf(out, len, "%.*s", (int)l, q);
    }
    fclose(f);
}

static void load_config(void)
{
    cfg_str("screenshot_tool", "builtin", cfg.shot_tool, sizeof cfg.shot_tool);
    cfg.super_menu = cfg_bool("super_menu", 1);
    cfg.fkeys = cfg_bool("fkeys_sound", 1);
    cfg.fkeys_display = cfg_bool("fkeys_display", 1);
    cfg.media = cfg_bool("media_keys", 1);
    cfg.shortcuts = cfg_bool("system_shortcuts", 1);
}

/* inode + size + mtime (ns): detects even two writes within the same second */
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

/* ---------------- child processes ---------------- */
static void on_signal(int sig) { (void)sig; stop_flag = 1; }
static void on_hup(int sig) { (void)sig; reload_flag = 1; }

static void on_sigchld(int sig)
{
    (void)sig;
    int saved = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0) {}
    errno = saved;
}

/* fork + reset signals so child programs do not inherit the daemon's handlers. */
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

/* Full path of program name in $PATH (written to out), or 0 if it is not installed. */
static int find_bin(const char *name, char *out, size_t len)
{
    const char *path = getenv("PATH");
    if (!path) path = "/usr/local/bin:/usr/bin:/bin";
    char *copy = strdup(path);
    if (!copy) return 0;
    int found = 0;
    for (char *d = strtok(copy, ":"); d && !found; d = strtok(NULL, ":")) {
        char buf[4096];
        snprintf(buf, sizeof buf, "%s/%s", *d ? d : ".", name);
        if (access(buf, X_OK) == 0) {
            found = 1;
            if (out) snprintf(out, len, "%s", buf);
        }
    }
    free(copy);
    return found;
}

static int have_bin(const char *name) { return find_bin(name, NULL, 0); }

/* GVariant text-format string literal: "..." with \ and " escaped. */
static void gv_quote(char *out, size_t len, const char *in)
{
    size_t o = 0;
    if (len < 3) { if (len) out[0] = '\0'; return; }
    out[o++] = '"';
    for (const char *p = in; *p && o + 3 < len; p++) {
        if (*p == '"' || *p == '\\') out[o++] = '\\';
        out[o++] = *p;
    }
    out[o++] = '"';
    out[o] = '\0';
}

static void notify(const char *summary, const char *body)
{
    if (have_bin("notify-send")) {
        pid_t p = fork_child();
        if (p == 0) {
            execlp("notify-send", "notify-send", "-a", "HDE", "-i", "dialog-information", summary, body, (char *)NULL);
            _exit(127);
        }
    } else if (have_bin("gdbus")) {                  /* libnotify-bin missing: talk to the notification daemon directly */
        char s[512], b[2048];
        gv_quote(s, sizeof s, summary);
        gv_quote(b, sizeof b, body);
        pid_t p = fork_child();
        if (p == 0) {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) dup2(devnull, STDOUT_FILENO);
            execlp("gdbus", "gdbus", "call", "--session", "--dest", "org.freedesktop.Notifications", "--object-path",
                   "/org/freedesktop/Notifications", "--method", "org.freedesktop.Notifications.Notify", "HDE", "0",
                   "dialog-information", s, b, "[]", "{}", "-1", (char *)NULL);
            _exit(127);
        }
    } else {
        fprintf(stderr, "hde-hotkeys: %s: %s\n", summary, body);
    }
}

/* Send a command to hde-panel from a child process (separate X connection). */
static void child_send_panel(long cmd, long arg)
{
    Display *d = XOpenDisplay(NULL);
    if (!d) return;
    hde_ipc_send(d, cmd, arg, CurrentTime);
    XCloseDisplay(d);
}

/* Run the volume/microphone command in a child process (serialized with flock), then ask the panel to show the OSD. */
static void run_with_osd(const char *script, long osd_cmd)
{
    pid_t p = fork_child();
    if (p != 0) return;
    char full[8192];
    snprintf(full, sizeof full,
             "exec 9>\"${XDG_RUNTIME_DIR:-/tmp}/hde-hotkeys-volume.lock\" && command -v flock >/dev/null 2>&1 && flock 9; %s",
             script);
    int st = system(full);
    (void)st;
    child_send_panel(osd_cmd, 0);
    _exit(0);
}

/* F6 / F7 and the brightness keys: in a child process (one at a time, the key repeats while held), then the OSD of
 * the panel. No external tool needed: backlight (sysfs / systemd-logind) or software dimming, see hde-brightness.h. */
static void brightness_key(int step)
{
    pid_t p = fork_child();
    if (p != 0) return;
    char lock[4096];
    const char *rt = getenv("XDG_RUNTIME_DIR");
    snprintf(lock, sizeof lock, "%s/hde-hotkeys-brightness.lock", rt && *rt ? rt : "/tmp");
    int lfd = open(lock, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lfd >= 0 && flock(lfd, LOCK_EX) != 0) { close(lfd); lfd = -1; }
    Display *d = XOpenDisplay(NULL);
    HdeBrightness b;
    int ok = hde_brightness_set(d, step, 1, &b);
    char desc[384];
    hde_brightness_describe(&b, desc, sizeof desc);
    if (debug_on || !ok || b.note[0])
        fprintf(stderr, "hde-hotkeys: brightness %s: %s%s%s%s\n", step > 0 ? "up" : "down", desc,
                b.via[0] ? " (via " : "", b.via, b.via[0] ? ")" : "");
    if (ok && d) hde_ipc_send(d, HDE_CMD_OSD_BRIGHTNESS, b.percent, CurrentTime);
    if (d) XCloseDisplay(d);
    if (!ok) {
        char body[400];
        snprintf(body, sizeof body, "The brightness of this screen cannot be changed: %s.", b.note);
        notify("Brightness", body);
    }
    if (lfd >= 0) close(lfd);
    _exit(0);
}

/* ---------------- actions ---------------- */
enum {
    A_SHOT_FULL, A_SHOT_AREA, A_SHOT_WIN, A_CLIP_FULL, A_CLIP_AREA, A_CLIP_WIN,
    A_VOL_MUTE, A_VOL_DOWN, A_VOL_UP, A_MIC_MUTE, A_BRIGHT_UP, A_BRIGHT_DOWN,
    A_PLAY, A_NEXT, A_PREV, A_STOP,
    A_LOCK, A_FILES, A_TERMINAL, A_DESKTOP, A_RUN, A_SEARCH, A_POWER, A_PROJECT
};
enum { G_ALWAYS, G_FKEYS, G_FKEYS_DISPLAY, G_MEDIA, G_SHORTCUTS };

typedef struct { KeySym sym; unsigned mods; int act; int group; const char *name; } Binding;

static const Binding bindings[] = {
    { XK_Print, 0, A_SHOT_FULL, G_ALWAYS, "Print" },
    { XK_Print, ShiftMask, A_SHOT_AREA, G_ALWAYS, "Shift+Print" },
    { XK_Print, Mod1Mask, A_SHOT_WIN, G_ALWAYS, "Alt+Print" },
    /* clipboard only, like GNOME; also leaves no Print combination to a WM binding (C-Print -> scrot -s, ...) */
    { XK_Print, ControlMask, A_CLIP_FULL, G_ALWAYS, "Ctrl+Print" },
    { XK_Print, ControlMask | ShiftMask, A_CLIP_AREA, G_ALWAYS, "Ctrl+Shift+Print" },
    { XK_Print, ControlMask | Mod1Mask, A_CLIP_WIN, G_ALWAYS, "Ctrl+Alt+Print" },
    { XK_F1, 0, A_VOL_MUTE, G_FKEYS, "F1" },
    { XK_F2, 0, A_VOL_DOWN, G_FKEYS, "F2" },
    { XK_F3, 0, A_VOL_UP, G_FKEYS, "F3" },
    { XK_F6, 0, A_BRIGHT_DOWN, G_FKEYS_DISPLAY, "F6" },
    { XK_F7, 0, A_BRIGHT_UP, G_FKEYS_DISPLAY, "F7" },
    { XK_F8, 0, A_PROJECT, G_FKEYS_DISPLAY, "F8" },
    { XF86XK_AudioMute, 0, A_VOL_MUTE, G_MEDIA, "XF86AudioMute" },
    { XF86XK_AudioLowerVolume, 0, A_VOL_DOWN, G_MEDIA, "XF86AudioLowerVolume" },
    { XF86XK_AudioRaiseVolume, 0, A_VOL_UP, G_MEDIA, "XF86AudioRaiseVolume" },
    { XF86XK_AudioMicMute, 0, A_MIC_MUTE, G_MEDIA, "XF86AudioMicMute" },
    { XF86XK_MonBrightnessUp, 0, A_BRIGHT_UP, G_MEDIA, "XF86MonBrightnessUp" },
    { XF86XK_MonBrightnessDown, 0, A_BRIGHT_DOWN, G_MEDIA, "XF86MonBrightnessDown" },
    { XF86XK_Display, 0, A_PROJECT, G_MEDIA, "XF86Display" },             /* Fn + the key with two screens */
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
    { XK_p, Mod4Mask, A_PROJECT, G_SHORTCUTS, "Super+P" },                /* also what many laptops' Fn key sends */
    { XK_F2, Mod1Mask, A_RUN, G_SHORTCUTS, "Alt+F2" },
    { XK_t, ControlMask | Mod1Mask, A_TERMINAL, G_SHORTCUTS, "Ctrl+Alt+T" },
    { XK_Delete, ControlMask | Mod1Mask, A_POWER, G_SHORTCUTS, "Ctrl+Alt+Delete" },
};
#define N_BINDINGS (sizeof bindings / sizeof bindings[0])
static KeyCode codes[N_BINDINGS];
static int grabbed[N_BINDINGS];
static int failed[N_BINDINGS];          /* another client holds (some variants of) this key: retried until ours */

/* External tools that Settings > Keyboard can choose instead of the built-in hde-screenshot. */
static const struct { const char *bin, *full, *area, *window; } shot_tools[] = {
    { "gnome-screenshot", "gnome-screenshot", "gnome-screenshot -a", "gnome-screenshot -w" },
    { "xfce4-screenshooter", "xfce4-screenshooter -f", "xfce4-screenshooter -r", "xfce4-screenshooter -w" },
    { "mate-screenshot", "mate-screenshot", "mate-screenshot -a", "mate-screenshot -w" },
    { "flameshot", "flameshot full -p \"$HOME/Pictures\"", "flameshot gui", "flameshot gui" },
    { "spectacle", "spectacle -f -b", "spectacle -r", "spectacle -a -b" },
    { "maim", "mkdir -p \"$HOME/Pictures/Screenshots\" && maim \"$HOME/Pictures/Screenshots/Screenshot_$(date +%Y-%m-%d_%H-%M-%S).png\"",
      "mkdir -p \"$HOME/Pictures/Screenshots\" && maim -s \"$HOME/Pictures/Screenshots/Screenshot_$(date +%Y-%m-%d_%H-%M-%S).png\"",
      "mkdir -p \"$HOME/Pictures/Screenshots\" && maim -i \"$(xdotool getactivewindow)\" \"$HOME/Pictures/Screenshots/Screenshot_$(date +%Y-%m-%d_%H-%M-%S).png\"" },
    { "scrot", "mkdir -p \"$HOME/Pictures/Screenshots\" && scrot \"$HOME/Pictures/Screenshots/Screenshot_%Y-%m-%d_%H-%M-%S.png\"",
      "mkdir -p \"$HOME/Pictures/Screenshots\" && scrot -s \"$HOME/Pictures/Screenshots/Screenshot_%Y-%m-%d_%H-%M-%S.png\"",
      "mkdir -p \"$HOME/Pictures/Screenshots\" && scrot -u \"$HOME/Pictures/Screenshots/Screenshot_%Y-%m-%d_%H-%M-%S.png\"" },
};

/* An HDE program next to this binary (a fresh ./build copy wins, like hde-session does), else in $PATH. */
static int find_hde_program(const char *name, char *out, size_t len)
{
    char self[4096];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n > 0) {
        self[n] = '\0';
        char *slash = strrchr(self, '/');
        if (slash) {
            *slash = '\0';
            int w = snprintf(out, len, "%s/%s", self, name);
            if (w > 0 && (size_t)w < len && access(out, X_OK) == 0) return 1;
        }
    }
    return find_bin(name, out, len);
}

static int find_builtin_shot(char *out, size_t len) { return find_hde_program("hde-screenshot", out, len); }

/* F8 / Super+P / the display key: the Project window of Hyggshi Settings. Pressed again while it is open, the
 * running window moves to the next layout (it owns _HDE_PROJECT_S<n>; the new process only tells it, then exits). */
static void project(Time t)
{
    char path[4096], targ[48];
    if (!find_hde_program("hde-settings", path, sizeof path)) {
        notify("Project", "hde-settings is missing. Reinstall HDE (make && sudo make install).");
        return;
    }
    snprintf(targ, sizeof targ, "--time=%lu", (unsigned long)t);
    if (debug_on) fprintf(stderr, "hde-hotkeys: project: %s --project %s\n", path, targ);
    pid_t p = fork_child();
    if (p == 0) {
        execl(path, "hde-settings", "--project", targ, (char *)NULL);
        _exit(127);
    }
}

/* Never ends in a "Failed to execute child process" error: a chosen tool that is not installed (or
 * screenshot_tool=builtin, the default) uses the built-in hde-screenshot; external tools are only a last resort. */
static void screenshot(int act)
{
    int clip = act == A_CLIP_FULL || act == A_CLIP_AREA || act == A_CLIP_WIN;
    if (clip) act = act == A_CLIP_AREA ? A_SHOT_AREA : act == A_CLIP_WIN ? A_SHOT_WIN : A_SHOT_FULL;
    const char *mode_arg = act == A_SHOT_AREA ? "--area" : act == A_SHOT_WIN ? "--window" : NULL;
    const unsigned n_tools = sizeof shot_tools / sizeof shot_tools[0];
    char path[4096];
    if (clip && find_builtin_shot(path, sizeof path)) {      /* Ctrl+Print & co.: always the built-in tool */
        if (debug_on) fprintf(stderr, "hde-hotkeys: screenshot: %s --clipboard %s\n", path, mode_arg ? mode_arg : "");
        pid_t p = fork_child();
        if (p == 0) {
            execl(path, "hde-screenshot", "--clipboard", mode_arg, (char *)NULL);
            _exit(127);
        }
        return;
    }
    if (strcmp(cfg.shot_tool, "builtin") != 0 && strcmp(cfg.shot_tool, "auto") != 0) {
        for (unsigned i = 0; i < n_tools; ++i) {
            if (strcmp(cfg.shot_tool, shot_tools[i].bin) != 0) continue;
            if (have_bin(shot_tools[i].bin)) {
                spawn_sh(act == A_SHOT_AREA ? shot_tools[i].area : act == A_SHOT_WIN ? shot_tools[i].window : shot_tools[i].full);
                return;
            }
            fprintf(stderr, "hde-hotkeys: screenshot_tool=%s is not installed; using hde-screenshot\n", cfg.shot_tool);
        }
    }
    if (find_builtin_shot(path, sizeof path)) {
        if (debug_on) fprintf(stderr, "hde-hotkeys: screenshot: %s %s\n", path, mode_arg ? mode_arg : "");
        pid_t p = fork_child();
        if (p == 0) {
            execl(path, "hde-screenshot", mode_arg, (char *)NULL);
            _exit(127);
        }
        return;
    }
    for (unsigned i = 0; i < n_tools; ++i) {
        if (!have_bin(shot_tools[i].bin)) continue;
        spawn_sh(act == A_SHOT_AREA ? shot_tools[i].area : act == A_SHOT_WIN ? shot_tools[i].window : shot_tools[i].full);
        return;
    }
    notify("Screenshot", "hde-screenshot is missing. Reinstall HDE (make && sudo make install).");
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
    int rc = hde_ipc_send(dpy, cmd, 0, t);
    if (rc != 0) fprintf(stderr, "hde-hotkeys: hde-panel is not running (command %ld ignored)\n", cmd);
    else if (debug_on) fprintf(stderr, "hde-hotkeys: sent command %ld to hde-panel (time %lu)\n", cmd, (unsigned long)t);
}

static void do_action(int act, Time t)
{
    switch (act) {
    case A_SHOT_FULL: case A_SHOT_AREA: case A_SHOT_WIN:
    case A_CLIP_FULL: case A_CLIP_AREA: case A_CLIP_WIN: screenshot(act); break;
    case A_VOL_MUTE:  run_with_osd(HDE_SH_VOLUME_MUTE, HDE_CMD_OSD_VOLUME); break;
    case A_VOL_DOWN:  run_with_osd(HDE_SH_VOLUME_DOWN, HDE_CMD_OSD_VOLUME); break;
    case A_VOL_UP:    run_with_osd(HDE_SH_VOLUME_UP, HDE_CMD_OSD_VOLUME); break;
    case A_MIC_MUTE:  run_with_osd(HDE_SH_MIC_MUTE, HDE_CMD_OSD_MIC); break;
    case A_BRIGHT_UP:   brightness_key(+HDE_BRIGHTNESS_STEP); break;
    case A_BRIGHT_DOWN: brightness_key(-HDE_BRIGHTNESS_STEP); break;
    case A_PROJECT: project(t); break;
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

/* ---------------- key grabs ---------------- */
/* If the WM already owns a key, XGrabKey fails with BadAccess; Xlib's default handler would kill the whole
 * process -> catch the error, warn and skip that key. */
static int g_grab_failed = 0;
static int g_quiet_errors = 0;          /* reading properties of / watching windows of other clients: may be gone */
static int on_x_error(Display *d, XErrorEvent *e)
{
    (void)d;
    if (e->error_code == BadAccess) g_grab_failed = 1;
    else if (!g_quiet_errors) fprintf(stderr, "hde-hotkeys: X error code=%d request=%d\n", e->error_code, e->request_code);
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
    return g == G_ALWAYS || (g == G_FKEYS && cfg.fkeys) || (g == G_FKEYS_DISPLAY && cfg.fkeys_display) ||
           (g == G_MEDIA && cfg.media) || (g == G_SHORTCUTS && cfg.shortcuts);
}

static const unsigned ignored_mods[] = { 0, LockMask, Mod2Mask, Mod5Mask, LockMask | Mod2Mask, LockMask | Mod5Mask,
                                         Mod2Mask | Mod5Mask, LockMask | Mod2Mask | Mod5Mask };

static void ungrab_binding(unsigned i)
{
    for (unsigned k = 0; k < sizeof ignored_mods / sizeof ignored_mods[0]; k++)
        XUngrabKey(dpy, codes[i], bindings[i].mods | ignored_mods[k], root);
}

/* Grab every lock-key variant of binding i; 1 = all of them are ours, 0 = another client holds some. */
static int grab_binding(unsigned i)
{
    g_grab_failed = 0;
    for (unsigned k = 0; k < sizeof ignored_mods / sizeof ignored_mods[0]; k++)
        XGrabKey(dpy, codes[i], bindings[i].mods | ignored_mods[k], root, False, GrabModeAsync, GrabModeAsync);
    XSync(dpy, False);
    return !g_grab_failed;
}

/* Name of the running window manager (EWMH _NET_SUPPORTING_WM_CHECK -> _NET_WM_NAME), 0 if unknown. */
static int wm_name(char *out, size_t len)
{
    int found = 0;
    Atom type;
    int fmt;
    unsigned long n, after;
    unsigned char *data = NULL;
    Window check = None;
    g_quiet_errors = 1;
    if (XGetWindowProperty(dpy, root, XInternAtom(dpy, "_NET_SUPPORTING_WM_CHECK", False), 0, 1, False, XA_WINDOW,
                           &type, &fmt, &n, &after, &data) == Success && data) {
        if (n == 1 && fmt == 32) check = *(Window *)data;
        XFree(data);
        data = NULL;
    }
    if (check != None &&
        XGetWindowProperty(dpy, check, XInternAtom(dpy, "_NET_WM_NAME", False), 0, 64, False,
                           XInternAtom(dpy, "UTF8_STRING", False), &type, &fmt, &n, &after, &data) == Success && data) {
        if (n > 0 && fmt == 8) {
            snprintf(out, len, "%.*s", (int)n, (char *)data);
            found = out[0] != '\0';
        }
        XFree(data);
    }
    XSync(dpy, False);
    g_quiet_errors = 0;
    return found;
}

/* Print belongs to another program: say so once (until HDE gets the key back), with what to do about it. */
static int shot_conflict_reported;
static void report_conflicts(void)
{
    char names[1024] = "";
    int shot = 0;
    for (unsigned i = 0; i < N_BINDINGS; i++) {
        if (!failed[i]) continue;
        if (bindings[i].group == G_ALWAYS) shot = 1;
        if (strlen(names) + strlen(bindings[i].name) + 3 < sizeof names) {
            strcat(names, " ");
            strcat(names, bindings[i].name);
        }
    }
    if (!names[0]) { shot_conflict_reported = 0; return; }
    char wm[128], body[512];
    int have_wm = wm_name(wm, sizeof wm);
    fprintf(stderr, "hde-hotkeys: already used by another program (%s%s):%s; HDE takes them over as soon as they are "
            "free (log out and back in to fix this now)\n", have_wm ? "window manager: " : "window manager?",
            have_wm ? wm : "", names);
    if (!shot || shot_conflict_reported) return;
    shot_conflict_reported = 1;
    if (have_wm)
        snprintf(body, sizeof body, "Probably the window manager (%s): pressing Print runs its command instead of "
                 "HDE's screenshot. Log out and log back in, then HDE claims Print first.", wm);
    else
        snprintf(body, sizeof body, "Pressing Print runs that program's command instead of HDE's screenshot. "
                 "Log out and log back in, then HDE claims Print first.");
    notify("Print key taken by another program", body);
}

/* (Re)grab the enabled bindings. A key that stays ours is never released, not even for a moment: after a
 * keyboard layout change (MappingNotify) the window manager re-grabs its own bindings at the same time and
 * would otherwise get PrtSc & co. Only disabled bindings and keys whose keycode changed are released. */
static void grab_all(void)
{
    for (unsigned i = 0; i < N_BINDINGS; i++) {
        KeyCode kc = XKeysymToKeycode(dpy, bindings[i].sym);
        int want = kc && group_enabled(bindings[i].group);
        if (grabbed[i] && (!want || kc != codes[i])) ungrab_binding(i);
        grabbed[i] = 0;
        failed[i] = 0;
        codes[i] = kc;
        if (!want) continue;
        failed[i] = !grab_binding(i);
        grabbed[i] = 1;                     /* some NumLock/CapsLock variants may still be ours */
    }
    XSync(dpy, False);
    report_conflicts();
    fprintf(stderr, "hde-hotkeys: super_menu=%d fkeys_sound=%d fkeys_display=%d media_keys=%d system_shortcuts=%d "
            "screenshot_tool=%s\n", cfg.super_menu, cfg.fkeys, cfg.fkeys_display, cfg.media, cfg.shortcuts, cfg.shot_tool);
}

static int any_failed(void)
{
    for (unsigned i = 0; i < N_BINDINGS; i++) if (failed[i]) return 1;
    return 0;
}

/* Try again to grab the keys another program had: as soon as it lets go (exits, is replaced, ...) they are HDE's. */
static void retry_failed(void)
{
    int left = 0;
    for (unsigned i = 0; i < N_BINDINGS; i++) {
        if (!failed[i] || !grabbed[i]) continue;
        if (grab_binding(i)) {
            failed[i] = 0;
            fprintf(stderr, "hde-hotkeys: %s is free again: handled by HDE now\n", bindings[i].name);
        } else {
            left = 1;
        }
    }
    if (!left) shot_conflict_reported = 0;  /* a new conflict later is reported again */
}

/* While keys are missing, watch the window manager's WM_S<n> selection window: when that WM exits or is replaced,
 * retry at once and then very often for a moment — before a newly started WM can grab the keys again. */
static Window wm_owner = None;

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static void watch_wm(void)
{
    char name[32];
    snprintf(name, sizeof name, "WM_S%d", DefaultScreen(dpy));
    Window o = XGetSelectionOwner(dpy, XInternAtom(dpy, name, False));
    if (o == wm_owner) return;
    wm_owner = o;
    if (o == None || o == root) return;      /* never replace our own KeyPress mask on the root window */
    g_quiet_errors = 1;
    XSelectInput(dpy, o, StructureNotifyMask);
    XSync(dpy, False);
    g_quiet_errors = 0;
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

/* ---------------- Super key (XInput2 raw events) ---------------- */
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
    if (debug_on && c->evtype != XI_RawButtonPress)
        fprintf(stderr, "hde-hotkeys: raw key %s code=%d super=%d down=%d other=%d dev=%d src=%d\n",
                c->evtype == XI_RawKeyPress ? "press" : "release", re->detail, is_super, super_down, super_other,
                re->deviceid, re->sourceid);
    switch (c->evtype) {
    case XI_RawKeyPress:
        if (is_super) {
            /* Every Super press starts a new "tap", INDEPENDENT of the previous state: while one of the
             * Super+<key> combos grabbed by hde-hotkeys itself is active, the X server filters out raw events for
             * the client holding the grab (FilterRawEvents), so the Super release can get lost (e.g. Super released before S).
             * Auto-repeat events are ignored so that holding Super down does not count as another tap. */
            if (!(re->flags & XIKeyRepeat)) {
                super_down = 1;
                super_other = 0;
                super_time = re->time;
            }
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

/* Allow only one hde-hotkeys per screen (two instances would open and immediately close the menu again). */
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
                   "Config: ~/.config/hde/settings.ini  super_menu fkeys_sound fkeys_display media_keys system_shortcuts\n"
                   "        screenshot_tool\n"
                   "Send SIGHUP to reload.\n");
            return 0;
        }
    }
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg) snprintf(cfg_path, sizeof cfg_path, "%s/hde/settings.ini", xdg);
    else snprintf(cfg_path, sizeof cfg_path, "%s/.config/hde/settings.ini", home ? home : "/tmp");

    debug_on = getenv("HDE_DEBUG") != NULL;
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
    /* Tell hde-session that our keys are grabbed: only now does it start the window manager. */
    const char *ready = getenv("HDE_READY_FD");
    if (ready && *ready) {
        int rfd = atoi(ready);
        if (rfd > 2) {
            ssize_t w = write(rfd, "1", 1);
            (void)w;
            close(rfd);
        }
        unsetenv("HDE_READY_FD");
    }

    int fd = ConnectionNumber(dpy);
    long long next_retry = now_ms() + 2000, burst_until = 0;
    if (any_failed()) watch_wm();
    while (!stop_flag) {
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            if (ev.type == KeyPress) {
                handle_key(&ev.xkey);
            } else if (ev.type == DestroyNotify) {
                /* StructureNotify is only selected on WM selection windows: a window manager exited / was replaced
                 * (its keys are released when its connection closes, which may come a moment later) */
                if (ev.xdestroywindow.window == wm_owner) wm_owner = None;
                if (any_failed()) {
                    retry_failed();
                    burst_until = now_ms() + 2000;
                    watch_wm();
                }
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
        if (burst_until) { tv.tv_sec = 0; tv.tv_usec = 10000; }
        if (select(fd + 1, &fds, NULL, NULL, &tv) < 0 && errno != EINTR) break;
        if (reload_flag || config_changed()) {
            reload_flag = 0;
            load_config();
            grab_all();
        }
        long long t = now_ms();
        if (any_failed() && (t >= next_retry || burst_until)) {
            next_retry = t + 2000;
            retry_failed();
            if (any_failed()) watch_wm();
        }
        if (burst_until && (t >= burst_until || !any_failed())) burst_until = 0;
    }

    XUngrabKey(dpy, AnyKey, AnyModifier, root);
    XCloseDisplay(dpy);
    return 0;
}
