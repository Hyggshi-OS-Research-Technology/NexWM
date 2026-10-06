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
#include <time.h>
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

int hde_brightness_backlight_present(void)
{
    HdeBrightness b;
    memset(&b, 0, sizeof b);
    return find_backlight(&b);
}

/* Run a program, its standard output into buf (at most len - 1 bytes, always terminated), for at most timeout_ms.
 * 1 = it exited with status 0. */
static int run_capture(char *const argv[], int timeout_ms, char *buf, size_t len)
{
    buf[0] = '\0';
    if (!in_path(argv[0])) return 0;
    int fds[2];
    if (pipe(fds) != 0) return 0;
    pid_t p = fork();
    if (p < 0) {
        close(fds[0]);
        close(fds[1]);
        return 0;
    }
    if (p == 0) {
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) {
            dup2(dn, 0);
            dup2(dn, 2);
        }
        dup2(fds[1], 1);
        close(fds[0]);
        close(fds[1]);
        setenv("LC_ALL", "C", 1);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(fds[1]);
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    size_t used = 0;
    int waited = 0, killed = 0;
    for (;;) {
        char tmp[512];
        ssize_t n = read(fds[0], tmp, sizeof tmp);
        if (n > 0) {
            size_t room = len - 1 - used, c = (size_t)n < room ? (size_t)n : room;
            memcpy(buf + used, tmp, c);
            used += c;
            continue;
        }
        if (n == 0) break;
        if (errno != EAGAIN && errno != EINTR) break;
        if (waited >= timeout_ms) {
            kill(p, SIGKILL);
            killed = 1;
            break;
        }
        usleep(10 * 1000);
        waited += 10;
    }
    buf[used] = '\0';
    close(fds[0]);
    int status = 0;
    pid_t r = waitpid(p, &status, 0);
    if (killed) return 0;
    if (r < 0) return errno == ECHILD && used > 0;  /* reaped by a SIGCHLD handler (hde-hotkeys): judge by the output */
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* ------------------------------------------------------------------ DDC/CI (ddcutil) */
#define DDC_MAX 4
typedef struct {
    int bus;                            /* /dev/i2c-N */
    char name[48];                      /* model from the EDID */
} DdcMonitor;

static int ddc_enabled(void)
{
    const char *e = getenv("HDE_DDC");
    return !(e && !strcmp(e, "0")) && in_path("ddcutil");
}

static void ddc_cache_path(char *buf, size_t len)
{
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (rt && *rt) snprintf(buf, len, "%s/hde-ddc.cache", rt);
    else snprintf(buf, len, "/tmp/hde-ddc-%d.cache", (int)getuid());
}

/* which screens are connected now: what the cache is for ("" without X) */
static void ddc_key(Display *dpy, char *key, size_t len)
{
    key[0] = '\0';
    if (!dpy || !hde_randr_available(dpy)) return;
    HdeRandrState *s = calloc(1, sizeof *s);
    if (s && hde_randr_read(dpy, s, 0)) hde_randr_connected_names(s, key, len);
    free(s);
}

/* `ddcutil detect --terse`: "Display 1", "   I2C bus:  /dev/i2c-4", "   Monitor:  DEL:DELL U2415:CFV9N7..." per
 * monitor that answers ("Invalid display" blocks are monitors without DDC/CI). */
static int ddc_parse_detect(const char *out, DdcMonitor *m, int max)
{
    int n = 0, in_valid = 0;
    for (const char *line = out; line && *line && n <= max;) {
        const char *nl = strchr(line, '\n');
        size_t l = nl ? (size_t)(nl - line) : strlen(line);
        char buf[256];
        snprintf(buf, sizeof buf, "%.*s", (int)(l < sizeof buf ? l : sizeof buf - 1), line);
        const char *t = buf;
        while (*t == ' ' || *t == '\t') t++;
        if (!strncmp(t, "Display ", 8)) {
            in_valid = n < max;
            if (in_valid) {
                memset(&m[n], 0, sizeof m[n]);
                m[n].bus = -1;
                n++;
            }
        } else if (!strncmp(t, "Invalid display", 15) || !strncmp(t, "Phantom display", 15)) {
            in_valid = 0;
        } else if (in_valid && !strncmp(t, "I2C bus:", 8)) {
            const char *d = strstr(t, "/dev/i2c-");
            if (d) m[n - 1].bus = atoi(d + 9);
        } else if (in_valid && !strncmp(t, "Monitor:", 8)) {
            const char *v = t + 8;
            while (*v == ' ') v++;
            const char *a = strchr(v, ':');               /* MFG:MODEL:SERIAL */
            const char *b = a ? strchr(a + 1, ':') : NULL;
            if (a && b > a + 1) snprintf(m[n - 1].name, sizeof m[n - 1].name, "%.*s", (int)(b - a - 1), a + 1);
            else snprintf(m[n - 1].name, sizeof m[n - 1].name, "%s", v);
        }
        line = nl ? nl + 1 : NULL;
    }
    int k = 0;
    for (int i = 0; i < n; i++)
        if (m[i].bus >= 0) m[k++] = m[i];
    return k;
}

/* The monitors that answer DDC/CI, from the cache when it is for the same screens; 0 = none. */
static int ddc_find(Display *dpy, DdcMonitor *m, int max)
{
    char path[1024], key[512], line[300];
    ddc_cache_path(path, sizeof path);
    ddc_key(dpy, key, sizeof key);
    FILE *f = fopen(path, "r");
    if (f) {
        int n = 0, ok = 0;
        long when = 0;
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\n")] = '\0';
            if (!strncmp(line, "key=", 4)) ok = !strcmp(line + 4, key);
            else if (!strncmp(line, "time=", 5)) when = atol(line + 5);
            else if (!strncmp(line, "bus=", 4) && n < max) {
                char *sp = strchr(line, ' ');
                m[n].bus = atoi(line + 4);
                snprintf(m[n].name, sizeof m[n].name, "%s", sp && !strncmp(sp, " name=", 6) ? sp + 6 : "");
                n++;
            }
        }
        fclose(f);
        if (ok && !key[0] && time(NULL) - when > 600) ok = 0;   /* without X: found again after 10 minutes */
        if (ok) return n;
    }
    char out[4096];
    char *argv[] = { "ddcutil", "detect", "--terse", NULL };
    run_capture(argv, 20000, out, sizeof out);
    int n = ddc_parse_detect(out, m, max);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "key=%s\ntime=%ld\n", key, (long)time(NULL));
        for (int i = 0; i < n; i++) fprintf(f, "bus=%d name=%s\n", m[i].bus, m[i].name);
        fclose(f);
    }
    return n;
}

static void ddc_forget(void)
{
    char path[1024];
    ddc_cache_path(path, sizeof path);
    unlink(path);
}

/* `ddcutil --bus N getvcp 10 --terse` -> "VCP 10 C 60 100" */
static int ddc_get(int bus, int *cur, int *max)
{
    char b[16], out[512];
    snprintf(b, sizeof b, "%d", bus);
    char *argv[] = { "ddcutil", "--bus", b, "getvcp", "10", "--terse", NULL };
    run_capture(argv, 5000, out, sizeof out);
    for (const char *l = out; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL) {
        int c = -1, m = -1;
        if (sscanf(l, "VCP %*s C %d %d", &c, &m) == 2 && m > 0 && c >= 0) {
            *cur = c;
            *max = m;
            return 1;
        }
    }
    return 0;
}

static int ddc_set(int bus, int value)
{
    char b[16], v[16], out[256];
    snprintf(b, sizeof b, "%d", bus);
    snprintf(v, sizeof v, "%d", value);
    char *argv[] = { "ddcutil", "--bus", b, "setvcp", "10", v, "--noverify", NULL };
    return run_capture(argv, 5000, out, sizeof out);
}

static void ddc_describe_device(HdeBrightness *b, const DdcMonitor *m, int n)
{
    if (n > 1) snprintf(b->device, sizeof b->device, "%.40s +%d", m[0].name[0] ? m[0].name : "monitor", n - 1);
    else snprintf(b->device, sizeof b->device, "%.60s", m[0].name[0] ? m[0].name : "monitor");
}

/* the first monitor's level: b filled, 1 = DDC/CI works here */
static int ddc_probe(Display *dpy, HdeBrightness *b, DdcMonitor *m, int *n)
{
    if (!ddc_enabled()) return 0;
    *n = ddc_find(dpy, m, DDC_MAX);
    if (*n <= 0) return 0;
    int cur = 0, max = 0;
    if (!ddc_get(m[0].bus, &cur, &max)) {
        ddc_forget();                   /* a monitor that no longer answers: look again next time */
        snprintf(b->note, sizeof b->note, "the monitor did not answer over DDC/CI");
        return 0;
    }
    b->method = HDE_BRIGHTNESS_DDC;
    ddc_describe_device(b, m, *n);
    b->raw = cur;
    b->max = max;
    b->percent = pct_of(cur, max);
    return 1;
}

HdeBrightnessMethod hde_brightness_get(Display *dpy, HdeBrightness *b)
{
    memset(b, 0, sizeof *b);
    b->percent = -1;
    if (find_backlight(b)) {
        b->method = HDE_BRIGHTNESS_BACKLIGHT;
        b->percent = pct_of(b->raw, b->max);
        return b->method;
    }
    DdcMonitor m[DDC_MAX];
    int nm = 0;
    if (ddc_probe(dpy, b, m, &nm)) return b->method;
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
    if (b->method == HDE_BRIGHTNESS_DDC) {
        DdcMonitor m[DDC_MAX];
        int n = ddc_find(dpy, m, DDC_MAX);
        int want = relative ? b->percent + value : value;
        if (want < 0) want = 0;
        if (want > 100) want = 100;
        int set = 0;
        for (int i = 0; i < n; i++) {
            int cur = 0, max = b->max;
            if (i > 0 && !ddc_get(m[i].bus, &cur, &max)) continue;
            if (ddc_set(m[i].bus, (int)lround(want * max / 100.0))) set++;
        }
        if (set > 0) {
            b->percent = want;
            b->raw = (int)lround(want * b->max / 100.0);
            snprintf(b->via, sizeof b->via, "ddcutil");
            if (set < n) snprintf(b->note, sizeof b->note, "%d of the %d monitors took the new brightness", set, n);
            /* software dimming left from before (a monitor that had no DDC/CI then) would dim it twice */
            if (dpy && hde_gamma_soft_percent(dpy) < 100) {
                hde_gamma_set_soft_percent(dpy, 100);
                hde_gamma_apply(dpy, 0);
            }
            return 1;
        }
        ddc_forget();
        snprintf(b->note, sizeof b->note, "the monitor did not take the new brightness over DDC/CI: using software dimming");
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
    case HDE_BRIGHTNESS_DDC:
        snprintf(buf, len, "DDC/CI %s: %d%% (%d of %d)", b->device, b->percent, b->raw, b->max);
        break;
    default:
        snprintf(buf, len, "none: %s", b->note);
        break;
    }
}
