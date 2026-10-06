/* hde-brightness.c — backlight / software brightness, see hde-brightness.h */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "hde-brightness.h"
#include "hde-randr.h"
#include <X11/Xatom.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* ------------------------------------------------------------------ root window properties */
static long get_cardinal(Display *dpy, const char *name, long def)
{
    Atom a = XInternAtom(dpy, name, False), type;
    int fmt;
    unsigned long n, after;
    unsigned char *data = NULL;
    long v = def;
    if (XGetWindowProperty(dpy, DefaultRootWindow(dpy), a, 0, 1, False, XA_CARDINAL, &type, &fmt, &n, &after, &data) ==
            Success && data) {
        if (type == XA_CARDINAL && fmt == 32 && n == 1) v = ((long *)data)[0];
        XFree(data);
    }
    return v;
}

static void set_cardinal(Display *dpy, const char *name, long v)
{
    Atom a = XInternAtom(dpy, name, False);
    XChangeProperty(dpy, DefaultRootWindow(dpy), a, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&v, 1);
}

int hde_gamma_soft_percent(Display *dpy)
{
    long v = dpy ? get_cardinal(dpy, "_HDE_BRIGHTNESS", 100) : 100;
    return v < HDE_BRIGHTNESS_SOFT_MIN ? HDE_BRIGHTNESS_SOFT_MIN : v > 100 ? 100 : (int)v;
}

void hde_gamma_set_soft_percent(Display *dpy, int percent)
{
    if (!dpy) return;
    if (percent < HDE_BRIGHTNESS_SOFT_MIN) percent = HDE_BRIGHTNESS_SOFT_MIN;
    if (percent > 100) percent = 100;
    set_cardinal(dpy, "_HDE_BRIGHTNESS", percent);
    XFlush(dpy);
}

int hde_gamma_night_kelvin(Display *dpy)
{
    long v = dpy ? get_cardinal(dpy, "_HDE_NIGHT_LIGHT", 0) : 0;
    return v <= 0 || v >= 6500 ? 0 : (int)v;
}

void hde_gamma_set_night_kelvin(Display *dpy, int kelvin)
{
    if (!dpy) return;
    if (kelvin <= 0 || kelvin >= 6500) XDeleteProperty(dpy, DefaultRootWindow(dpy), XInternAtom(dpy, "_HDE_NIGHT_LIGHT", False));
    else set_cardinal(dpy, "_HDE_NIGHT_LIGHT", kelvin < 1000 ? 1000 : kelvin);
    XFlush(dpy);
}

int hde_gamma_apply(Display *dpy, int force)
{
    if (!dpy) return 0;
    int pct = hde_gamma_soft_percent(dpy), k = hde_gamma_night_kelvin(dpy);
    int neutral = pct >= 100 && k == 0;
    int marked = get_cardinal(dpy, "_HDE_GAMMA_SET", 0) != 0;
    if (neutral && !marked && !force) return 0;
    int n = hde_randr_set_gamma(dpy, pct, k);
    if (neutral) XDeleteProperty(dpy, DefaultRootWindow(dpy), XInternAtom(dpy, "_HDE_GAMMA_SET", False));
    else if (n > 0) set_cardinal(dpy, "_HDE_GAMMA_SET", 1);
    XFlush(dpy);
    return n;
}

/* ------------------------------------------------------------------ backlight */
static const char *backlight_dir(void)
{
    const char *d = getenv("HDE_BACKLIGHT_DIR");
    return d && *d ? d : "/sys/class/backlight";
}

static int read_int(const char *dir, const char *dev, const char *file, int *v)
{
    char p[1024];
    snprintf(p, sizeof p, "%s/%s/%s", dir, dev, file);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    int ok = fscanf(f, "%d", v) == 1;
    fclose(f);
    return ok;
}

static void read_word(const char *dir, const char *dev, const char *file, char *out, size_t len)
{
    char p[1024];
    out[0] = '\0';
    snprintf(p, sizeof p, "%s/%s/%s", dir, dev, file);
    FILE *f = fopen(p, "r");
    if (!f) return;
    if (fgets(out, (int)len, f)) out[strcspn(out, " \r\n")] = '\0';
    fclose(f);
}

static int find_backlight(HdeBrightness *b)
{
    const char *dir = backlight_dir();
    DIR *d = opendir(dir);
    if (!d) return 0;
    int best = -1;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        int max = 0, raw = 0;
        if (!read_int(dir, e->d_name, "max_brightness", &max) || max <= 0) continue;
        if (!read_int(dir, e->d_name, "brightness", &raw)) continue;
        char type[32];
        read_word(dir, e->d_name, "type", type, sizeof type);
        int score = !strcmp(type, "firmware") ? 3 : !strcmp(type, "platform") ? 2 : 1;
        if (score <= best) continue;
        best = score;
        snprintf(b->device, sizeof b->device, "%.*s", (int)sizeof b->device - 1, e->d_name);
        b->raw = raw;
        b->max = max;
    }
    closedir(d);
    return best >= 0;
}

static int in_path(const char *name)
{
    const char *path = getenv("PATH");
    if (!path || !*path) path = "/usr/local/bin:/usr/bin:/bin";
    char *copy = strdup(path);
    if (!copy) return 0;
    int found = 0;
    char *save = NULL;
    for (char *d = strtok_r(copy, ":", &save); d && !found; d = strtok_r(NULL, ":", &save)) {
        char p[4096];
        snprintf(p, sizeof p, "%s/%s", *d ? d : ".", name);
        found = access(p, X_OK) == 0;
    }
    free(copy);
    return found;
}

/* Run a program quietly, at most timeout_ms; 1 = it exited with status 0. */
static int run_quiet(char *const argv[], int timeout_ms)
{
    if (!in_path(argv[0])) return 0;
    pid_t p = fork();
    if (p < 0) return 0;
    if (p == 0) {
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) {
            dup2(dn, 0);
            dup2(dn, 1);
            dup2(dn, 2);
        }
        execvp(argv[0], argv);
        _exit(127);
    }
    int status = 0;
    for (int waited = 0;; waited += 10) {
        pid_t r = waitpid(p, &status, WNOHANG);
        if (r == p) break;
        if (r < 0) return errno == ECHILD;          /* reaped by a SIGCHLD handler: assume it worked, checked below */
        if (waited >= timeout_ms) {
            kill(p, SIGKILL);
            waitpid(p, &status, 0);
            return 0;
        }
        usleep(10 * 1000);
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int write_sysfs(const char *dev, int value)
{
    char p[1024], buf[32];
    snprintf(p, sizeof p, "%s/%s/brightness", backlight_dir(), dev);
    int fd = open(p, O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (fd < 0) return 0;
    int n = snprintf(buf, sizeof buf, "%d\n", value);
    int ok = write(fd, buf, (size_t)n) == n;
    close(fd);
    return ok;
}

/* systemd-logind: any user may set the backlight of the screens of their own active session. */
static int write_logind(const char *dev, int value)
{
    char v[32], qdev[80], gv[40], sdev[96], uv[40];
    snprintf(v, sizeof v, "%d", value);
    char *busctl[] = { "busctl", "call", "--system", "org.freedesktop.login1", "/org/freedesktop/login1/session/auto",
                       "org.freedesktop.login1.Session", "SetBrightness", "ssu", "backlight", (char *)dev, v, NULL };
    if (run_quiet(busctl, 3000)) return 1;
    snprintf(qdev, sizeof qdev, "'%s'", dev);
    snprintf(gv, sizeof gv, "uint32 %d", value);
    char *gdbus[] = { "gdbus", "call", "--system", "--dest", "org.freedesktop.login1", "--object-path",
                      "/org/freedesktop/login1/session/auto", "--method", "org.freedesktop.login1.Session.SetBrightness",
                      "'backlight'", qdev, gv, NULL };
    if (run_quiet(gdbus, 3000)) return 1;
    snprintf(sdev, sizeof sdev, "string:%s", dev);
    snprintf(uv, sizeof uv, "uint32:%d", value);
    char *dsend[] = { "dbus-send", "--system", "--print-reply", "--dest=org.freedesktop.login1",
                      "/org/freedesktop/login1/session/auto", "org.freedesktop.login1.Session.SetBrightness",
                      "string:backlight", sdev, uv, NULL };
    return run_quiet(dsend, 3000);
}

static int write_tools(const char *dev, int value, int percent, char *via, size_t vialen)
{
    char v[32], pc[32];
    snprintf(v, sizeof v, "%d", value);
    snprintf(pc, sizeof pc, "%d", percent);
    char *bctl[] = { "brightnessctl", "-q", "-d", (char *)dev, "set", v, NULL };
    if (run_quiet(bctl, 3000)) { snprintf(via, vialen, "brightnessctl"); return 1; }
    char *light[] = { "light", "-S", pc, NULL };
    if (run_quiet(light, 3000)) { snprintf(via, vialen, "light"); return 1; }
    char *xbl[] = { "xbacklight", "-set", pc, NULL };
    if (run_quiet(xbl, 3000)) { snprintf(via, vialen, "xbacklight"); return 1; }
    return 0;
}

static int pct_of(int raw, int max) { return max > 0 ? (int)lround(raw * 100.0 / max) : -1; }

HdeBrightnessMethod hde_brightness_get(Display *dpy, HdeBrightness *b)
{
    memset(b, 0, sizeof *b);
    b->percent = -1;
    if (find_backlight(b)) {
        b->method = HDE_BRIGHTNESS_BACKLIGHT;
        b->percent = pct_of(b->raw, b->max);
        return b->method;
    }
    if (dpy && hde_randr_gamma_screens(dpy) > 0) {
        b->method = HDE_BRIGHTNESS_SOFTWARE;
        snprintf(b->device, sizeof b->device, "gamma");
        b->percent = hde_gamma_soft_percent(dpy);
        snprintf(b->note, sizeof b->note, "no backlight control on this screen (desktop monitor or virtual machine)");
        return b->method;
    }
    b->method = HDE_BRIGHTNESS_NONE;
    if (!hde_randr_supported())
        snprintf(b->note, sizeof b->note, "no backlight control, and HDE was built without libxrandr-dev "
                 "(needed for software dimming): sudo apt install libxrandr-dev, then rebuild HDE");
    else if (!dpy || !hde_randr_available(dpy))
        snprintf(b->note, sizeof b->note, "no backlight control, and the X server has no RandR 1.2 for software dimming");
    else
        snprintf(b->note, sizeof b->note, "no backlight control, and this screen's driver offers no gamma ramps for "
                 "software dimming");
    return b->method;
}

static int set_software(Display *dpy, int value, int relative, HdeBrightness *b)
{
    int cur = hde_gamma_soft_percent(dpy);
    int target = relative ? cur + value : value;
    if (target < HDE_BRIGHTNESS_SOFT_MIN) target = HDE_BRIGHTNESS_SOFT_MIN;
    if (target > 100) target = 100;
    hde_gamma_set_soft_percent(dpy, target);
    int n = hde_gamma_apply(dpy, 1);
    if (n <= 0) {
        hde_gamma_set_soft_percent(dpy, 100);
        b->method = HDE_BRIGHTNESS_NONE;
        b->percent = -1;
        snprintf(b->note, sizeof b->note, "no backlight control, and this screen's driver offers no gamma ramps for "
                 "software dimming");
        return 0;
    }
    b->method = HDE_BRIGHTNESS_SOFTWARE;
    snprintf(b->device, sizeof b->device, "gamma");
    snprintf(b->via, sizeof b->via, "gamma");
    b->percent = target;
    return 1;
}

int hde_brightness_set(Display *dpy, int value, int relative, HdeBrightness *b)
{
    hde_brightness_get(dpy, b);
    if (b->method == HDE_BRIGHTNESS_BACKLIGHT) {
        int cur = b->raw, max = b->max;
        int want = relative ? b->percent + value : value;
        if (want < 0) want = 0;
        if (want > 100) want = 100;
        int target = (int)lround(want * max / 100.0);
        /* few levels (acpi_video: 8-15): a step always moves at least one level */
        if (relative && value > 0 && target <= cur) target = cur + 1;
        if (relative && value < 0 && target >= cur) target = cur - 1;
        int min_raw = max >= 100 ? max / 100 : 1;    /* never all the way off: the picture would be gone */
        if (target < min_raw) target = min_raw;
        if (target > max) target = max;
        if (target == cur) {
            snprintf(b->via, sizeof b->via, "unchanged");
            return 1;
        }
        int ok = 0;
        if (write_sysfs(b->device, target)) { ok = 1; snprintf(b->via, sizeof b->via, "sysfs"); }
        else if (write_logind(b->device, target)) { ok = 1; snprintf(b->via, sizeof b->via, "logind"); }
        else if (write_tools(b->device, target, pct_of(target, max), b->via, sizeof b->via)) ok = 1;
        int now = cur;
        read_int(backlight_dir(), b->device, "brightness", &now);
        if (ok && now == cur && target != cur) ok = 0;   /* a tool said yes but nothing changed */
        if (ok) {
            b->raw = now;
            b->percent = pct_of(now, max);
            return 1;
        }
        snprintf(b->note, sizeof b->note, "the backlight %s could not be changed (no permission, and neither "
                 "systemd-logind nor brightnessctl worked): using software dimming", b->device);
        char note[256];
        snprintf(note, sizeof note, "%s", b->note);
        if (dpy && set_software(dpy, value, relative, b)) {
            snprintf(b->note, sizeof b->note, "%s", note);
            return 1;
        }
        return 0;
    }
    if (b->method == HDE_BRIGHTNESS_SOFTWARE) return set_software(dpy, value, relative, b);
    return 0;
}

void hde_brightness_describe(const HdeBrightness *b, char *buf, size_t len)
{
    switch (b->method) {
    case HDE_BRIGHTNESS_BACKLIGHT:
        snprintf(buf, len, "backlight %s: %d%% (%d of %d)", b->device, b->percent, b->raw, b->max);
        break;
    case HDE_BRIGHTNESS_SOFTWARE:
        snprintf(buf, len, "software dimming: %d%%", b->percent);
        break;
    default:
        snprintf(buf, len, "none: %s", b->note);
        break;
    }
}
