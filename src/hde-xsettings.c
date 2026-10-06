/* hde-xsettings — a minimal XSETTINGS manager for HDE.
 *
 * Owns the _XSETTINGS_S<n> selection and publishes the _XSETTINGS_SETTINGS property as specified by freedesktop
 * (https://specifications.freedesktop.org/xsettings-spec/). Every GTK2/3/4 application (and Qt using the
 * gtk platform theme) reads these values and SWITCHES IMMEDIATELY when they change — this is how Dark mode, theme,
 * icons and font chosen in Hyggshi Settings apply instantly, even to applications that are already open.
 *
 * Value sources (reloaded automatically when a file changes, or on SIGHUP):
 *   ~/.config/hde/settings.ini  [settings] theme_index, gtk_theme_effective, icon_theme_name, font,
 *                                          scale, decoration_layout
 *   ~/.config/gtk-3.0/settings.ini [Settings] gtk-theme-name, gtk-icon-theme-name, gtk-font-name,
 *                                          gtk-cursor-theme-name, gtk-cursor-theme-size  (fallback values)
 *
 * It is also HDE's input settings daemon (src/hde-input.c): touchpad scroll direction + tap to click, mouse wheel
 * direction, pointer speed — applied at login, whenever settings.ini changes, to every pointer device that is plugged
 * in or re-enabled later (USB/Bluetooth mice, a touchpad that comes back after suspend/resume), and again whenever
 * another program changes one of these values (a window manager with its own touchpad settings such as Mutter or
 * Muffin, an autostart script, xinput): HDE puts it back to what Settings says. SIGHUP re-applies everything at once.
 * The input part keeps running when another XSETTINGS manager owns the theme settings (HDE started inside another
 * desktop, or a program that took XSETTINGS over): only the theme part is left to that manager then.
 *
 * And HDE's display service (src/hde-randr.c, XRandR): when a screen is plugged in it opens the Project window of F8
 * (settings.ini display_connect=ask, or applies extend / duplicate / second / nothing); when the screen in use is
 * unplugged it turns the PC screen back on, and an unplugged screen never stays part of the desktop; at login it
 * restores the layout chosen with F8 for the same screens (display_mode / display_outputs); it puts software
 * brightness (F6/F7 without a backlight) and Night Light (night_light, night_light_temperature) back on every screen
 * after a change.
 *
 * Usage: hde-xsettings [--replace]   (started by hde-session; exits if another hde-xsettings is already running,
 *                                     unless --replace is given)
 *        hde-xsettings --status      touchpads and mice, their state and whether it matches Settings > Input;
 *                                    the screens and how their brightness can be changed
 *        hde-xsettings --version
 */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <glib.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "hde-input.h"
#include "hde-randr.h"
#include "hde-brightness.h"
#include "hde-build.h"

enum { XS_INT = 0, XS_STRING = 1 };

typedef struct {
    const char *name;
    int type;
    int ival;
    char *sval;
    guint32 last_serial;
} XSetting;

static Display *dpy;
static Window root, mgr_win;
static Atom sel_atom, settings_atom, manager_atom;
static guint32 serial;
static GPtrArray *current;     /* XSetting* */
static volatile sig_atomic_t stop_flag, reload_flag;

static void on_term(int s) { (void)s; stop_flag = 1; }
static void on_hup(int s) { (void)s; reload_flag = 1; }

static void setting_free(gpointer p)
{
    XSetting *s = p;
    g_free(s->sval);
    g_free(s);
}

static char *kf_string(GKeyFile *kf, const char *group, const char *key)
{
    char *v = kf ? g_key_file_get_string(kf, group, key, NULL) : NULL;
    if (v) {
        g_strstrip(v);
        if (!*v) { g_free(v); v = NULL; }
    }
    return v;
}

static GKeyFile *load_kf(const char *a, const char *b)
{
    char *path = g_build_filename(g_get_user_config_dir(), a, b, NULL);
    GKeyFile *kf = g_key_file_new();
    if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        g_key_file_free(kf);
        kf = NULL;
    }
    g_free(path);
    return kf;
}

static void add_string(GPtrArray *a, const char *name, char *value /* takes ownership */)
{
    if (!value) return;
    XSetting *s = g_new0(XSetting, 1);
    s->name = name;
    s->type = XS_STRING;
    s->sval = value;
    g_ptr_array_add(a, s);
}

static void add_int(GPtrArray *a, const char *name, int value)
{
    XSetting *s = g_new0(XSetting, 1);
    s->name = name;
    s->type = XS_INT;
    s->ival = value;
    g_ptr_array_add(a, s);
}

static GPtrArray *read_settings(void)
{
    GPtrArray *a = g_ptr_array_new_with_free_func(setting_free);
    GKeyFile *hde = load_kf("hde", "settings.ini");
    GKeyFile *gtk = load_kf("gtk-3.0", "settings.ini");

    int style = hde ? g_key_file_get_integer(hde, "settings", "theme_index", NULL) : 0;
    char *theme = (style == 1 || style == 2) ? kf_string(hde, "settings", "gtk_theme_effective") : NULL;
    if (!theme) theme = kf_string(gtk, "Settings", "gtk-theme-name");
    if (!theme) theme = g_strdup("Adwaita");
    add_string(a, "Net/ThemeName", theme);

    char *icons = kf_string(hde, "settings", "icon_theme_name");
    if (!icons) icons = kf_string(gtk, "Settings", "gtk-icon-theme-name");
    add_string(a, "Net/IconThemeName", icons);

    char *font = kf_string(hde, "settings", "font");
    if (!font) font = kf_string(gtk, "Settings", "gtk-font-name");
    add_string(a, "Gtk/FontName", font);

    add_string(a, "Gtk/CursorThemeName", kf_string(gtk, "Settings", "gtk-cursor-theme-name"));
    if (gtk && g_key_file_has_key(gtk, "Settings", "gtk-cursor-theme-size", NULL))
        add_int(a, "Gtk/CursorThemeSize", g_key_file_get_integer(gtk, "Settings", "gtk-cursor-theme-size", NULL));

    char *layout = kf_string(hde, "settings", "decoration_layout");
    add_string(a, "Gtk/DecorationLayout", layout ? layout : g_strdup("menu:minimize,maximize,close"));
    add_int(a, "Gtk/EnableAnimations", 1);

    /* Settings > Display > Scale: 0=100% (not set, keep the user's Xft.dpi), 1=125%, 2=150%, 3=200% */
    int scale = hde ? g_key_file_get_integer(hde, "settings", "scale", NULL) : 0;
    static const int dpi[] = { 0, 120, 144, 192 };
    if (scale > 0 && scale < 4) add_int(a, "Xft/DPI", dpi[scale] * 1024);

    if (hde) g_key_file_free(hde);
    if (gtk) g_key_file_free(gtk);
    return a;
}

static XSetting *find_setting(GPtrArray *a, const char *name)
{
    for (guint i = 0; a && i < a->len; i++) {
        XSetting *s = a->pdata[i];
        if (!strcmp(s->name, name)) return s;
    }
    return NULL;
}

static void put_card16(GByteArray *b, guint16 v) { g_byte_array_append(b, (guint8 *)&v, 2); }
static void put_card32(GByteArray *b, guint32 v) { g_byte_array_append(b, (guint8 *)&v, 4); }
static void put_pad(GByteArray *b, gsize n)
{
    static const guint8 zero[4] = { 0, 0, 0, 0 };
    if (n % 4) g_byte_array_append(b, zero, 4 - n % 4);
}

static void publish(GPtrArray *next)
{
    serial++;
    for (guint i = 0; i < next->len; i++) {           /* keep last-change-serial for unchanged values */
        XSetting *n = next->pdata[i];
        XSetting *o = find_setting(current, n->name);
        gboolean same = o && o->type == n->type &&
                        (n->type == XS_INT ? o->ival == n->ival : g_strcmp0(o->sval, n->sval) == 0);
        n->last_serial = same ? o->last_serial : serial;
    }
    GByteArray *b = g_byte_array_new();
    union { guint32 i; guint8 c[4]; } probe = { 1 };
    guint8 order = probe.c[0] == 1 ? LSBFirst : MSBFirst;     /* values are written in the machine's byte order */
    guint8 hdr[4] = { order, 0, 0, 0 };
    g_byte_array_append(b, hdr, 4);
    put_card32(b, serial);
    put_card32(b, next->len);
    for (guint i = 0; i < next->len; i++) {
        XSetting *s = next->pdata[i];
        gsize nl = strlen(s->name);
        guint8 t[2] = { (guint8)s->type, 0 };
        g_byte_array_append(b, t, 2);
        put_card16(b, (guint16)nl);
        g_byte_array_append(b, (const guint8 *)s->name, nl);
        put_pad(b, nl);
        put_card32(b, s->last_serial);
        if (s->type == XS_INT) {
            put_card32(b, (guint32)s->ival);
        } else {
            gsize vl = strlen(s->sval);
            put_card32(b, (guint32)vl);
            g_byte_array_append(b, (const guint8 *)s->sval, vl);
            put_pad(b, vl);
        }
    }
    XChangeProperty(dpy, mgr_win, settings_atom, settings_atom, 8, PropModeReplace, b->data, (int)b->len);
    XFlush(dpy);
    g_byte_array_free(b, TRUE);
    if (current) g_ptr_array_unref(current);
    current = next;

    XSetting *th = find_setting(current, "Net/ThemeName");
    fprintf(stderr, "hde-xsettings: published serial %u (theme=%s)\n", serial, th ? th->sval : "?");
}

static Time server_time(void)
{
    Atom a = XInternAtom(dpy, "_HDE_XSETTINGS_TIMESTAMP", False);
    XChangeProperty(dpy, mgr_win, a, XA_STRING, 8, PropModeAppend, (const unsigned char *)"", 0);
    XEvent ev;
    XWindowEvent(dpy, mgr_win, PropertyChangeMask, &ev);
    return ev.xproperty.time;
}

/* File "signature": inode + size + mtime in nanoseconds (catches 2 changes within the same second;
 * g_file_set_contents writes via rename, so the inode changes too). */
static guint64 file_sig(const char *a, const char *b)
{
    char *path = g_build_filename(g_get_user_config_dir(), a, b, NULL);
    struct stat st;
    guint64 sig = 0;
    if (stat(path, &st) == 0)
        sig = ((guint64)st.st_ino * 1000003u) ^ ((guint64)st.st_size << 32) ^
              ((guint64)st.st_mtim.tv_sec * 1000000000u + (guint64)st.st_mtim.tv_nsec);
    g_free(path);
    return sig;
}

/* One line per touchpad / mouse at startup: shows in the session log what HDE found and their state. */
static void log_input_devices(const HdeInputPrefs *p)
{
    HdeInputDevice devs[24];
    int n = hde_input_list(dpy, devs, (int)G_N_ELEMENTS(devs));
    for (int i = 0; i < n; i++) {
        const HdeInputDevice *d = &devs[i];
        fprintf(stderr, "hde-xsettings: input device %d: %s (%s, %s): natural scrolling %s%s\n", d->id, d->name,
                hde_input_kind_label(d, p), d->driver,
                d->natural < 0 ? "?" : d->natural ? "on" : "off",
                d->kind != HDE_INPUT_TOUCHPAD ? "" : d->tapping < 0 ? ", tap to click ?" :
                d->tapping ? ", tap to click on" : ", tap to click off");
    }
    if (n == 0)
        fprintf(stderr, "hde-xsettings: input: no touchpad or mouse with the libinput, synaptics or evdev X driver\n");
}

/* Is this window the manager window of an hde-xsettings (WM_NAME "hde-xsettings")? */
static int name_trap;
static int name_trap_handler(Display *d, XErrorEvent *e) { (void)d; name_trap = e->error_code; return 0; }
static gboolean window_is_hde_xsettings(Window w)
{
    char *name = NULL;
    name_trap = 0;
    int (*old)(Display *, XErrorEvent *) = XSetErrorHandler(name_trap_handler);
    Status ok = XFetchName(dpy, w, &name);
    XSync(dpy, False);
    XSetErrorHandler(old);
    gboolean is = ok && !name_trap && name && !strcmp(name, "hde-xsettings");
    if (name) XFree(name);
    return is;
}

static const char *on_off(int v) { return v < 0 ? "?" : v ? "on" : "off"; }

/* ===================== display service ===================== */
static char *settings_string(const char *key, const char *def)
{
    GKeyFile *kf = load_kf("hde", "settings.ini");
    char *v = kf_string(kf, "settings", key);
    if (kf) g_key_file_free(kf);
    return v ? v : g_strdup(def);
}

/* An HDE program next to this one (a fresh ./build copy wins), else in $PATH. */
static char *hde_program(const char *name)
{
    char self[4096];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n > 0) {
        self[n] = '\0';
        char *dir = g_path_get_dirname(self);
        char *p = g_build_filename(dir, name, NULL);
        g_free(dir);
        if (access(p, X_OK) == 0) return p;
        g_free(p);
    }
    return g_find_program_in_path(name);
}

static void spawn_detached(char *const argv[])
{
    pid_t p = fork();
    if (p == 0) {
        if (fork() == 0) {
            setsid();
            close(ConnectionNumber(dpy));
            execv(argv[0], argv);
            _exit(127);
        }
        _exit(0);
    }
    if (p > 0) waitpid(p, NULL, 0);
}

static int apply_layout(HdeRandrState *st, HdeProjectMode mode, const char *why)
{
    HdeRandrPlan *p = g_new0(HdeRandrPlan, 1);
    char err[256] = "";
    int ok = hde_randr_plan(st, mode, p) && hde_randr_apply(dpy, st, p, err, sizeof err);
    if (ok) fprintf(stderr, "hde-xsettings: displays: %s (%s applied)\n", why, hde_project_label(mode));
    else fprintf(stderr, "hde-xsettings: displays: %s: %s failed: %s\n", why, hde_project_label(mode), p->error[0] ? p->error : err);
    g_free(p);
    return ok;
}

static void log_displays(const char *what)
{
    HdeRandrState *st = g_new0(HdeRandrState, 1);
    if (hde_randr_read(dpy, st, 0)) {
        char desc[768];
        hde_randr_describe(st, desc, sizeof desc);
        fprintf(stderr, "hde-xsettings: displays%s: %s (layout: %s)\n", what, desc, hde_project_label(hde_randr_mode_of(st)));
    }
    g_free(st);
}

/* Night Light from settings.ini -> _HDE_NIGHT_LIGHT, then the gamma ramps of every screen. */
static void apply_night_light(void)
{
    GKeyFile *kf = load_kf("hde", "settings.ini");
    gboolean on = kf && g_key_file_get_boolean(kf, "settings", "night_light", NULL);
    int k = kf && g_key_file_has_key(kf, "settings", "night_light_temperature", NULL)
                ? g_key_file_get_integer(kf, "settings", "night_light_temperature", NULL) : 4000;
    if (kf) g_key_file_free(kf);
    int want = on ? CLAMP(k, 1500, 6000) : 0;
    if (want == hde_gamma_night_kelvin(dpy)) return;
    hde_gamma_set_night_kelvin(dpy, want);
    int n = hde_gamma_apply(dpy, 0);
    if (want) fprintf(stderr, "hde-xsettings: night light on (%d K) on %d screen(s)\n", want, n);
    else fprintf(stderr, "hde-xsettings: night light off\n");
}

static char display_names[512];         /* connected screens at the last look */
static gint64 fix_window_start;
static int fix_count;

/* At login: the layout chosen with F8 last time, if exactly the same screens are connected. */
static void display_start(void)
{
    HdeRandrState *st = g_new0(HdeRandrState, 1);
    if (!hde_randr_read(dpy, st, 0)) { g_free(st); return; }
    hde_randr_connected_names(st, display_names, sizeof display_names);
    char desc[768];
    hde_randr_describe(st, desc, sizeof desc);
    fprintf(stderr, "hde-xsettings: displays: %s (layout: %s)\n", desc, hde_project_label(hde_randr_mode_of(st)));
    char *mode = settings_string("display_mode", ""), *outs = settings_string("display_outputs", "");
    HdeProjectMode m = hde_project_from_id(mode);
    if (m != HDE_PROJECT_OTHER && hde_randr_n_connected(st) >= 2 && !strcmp(outs, display_names) &&
        hde_randr_mode_of(st) != m) {
        char why[600];
        g_snprintf(why, sizeof why, "the layout chosen earlier for %s", display_names);
        if (apply_layout(st, m, why)) log_displays(" now");
    }
    g_free(mode);
    g_free(outs);
    g_free(st);
}

/* After RandR events (debounced): fix a dark or too large desktop, react to a screen plugged in, put software
 * brightness / Night Light back on the screens. */
static void display_check(void)
{
    HdeRandrState *st = g_new0(HdeRandrState, 1);
    if (!hde_randr_read(dpy, st, 0)) { g_free(st); return; }
    HdeProjectMode fix;
    char why[300];
    if (hde_randr_needs_fix(st, display_names, &fix, why, sizeof why)) {
        gint64 now = g_get_monotonic_time();
        if (now - fix_window_start > 10 * G_USEC_PER_SEC) { fix_window_start = now; fix_count = 0; }
        if (++fix_count <= 3 && apply_layout(st, fix, why)) hde_randr_read(dpy, st, 0);
        else if (fix_count == 4) fprintf(stderr, "hde-xsettings: displays: %s: given up after 3 tries\n", why);
    }
    char names[512];
    hde_randr_connected_names(st, names, sizeof names);
    if (strcmp(names, display_names)) {
        char **was = g_strsplit(display_names, ",", -1), **is = g_strsplit(names, ",", -1);
        GString *added = g_string_new(NULL), *removed = g_string_new(NULL);
        for (int i = 0; is[i]; i++)
            if (*is[i] && !g_strv_contains((const char *const *)was, is[i])) g_string_append_printf(added, "%s%s", added->len ? "," : "", is[i]);
        for (int i = 0; was[i]; i++)
            if (*was[i] && !g_strv_contains((const char *const *)is, was[i])) g_string_append_printf(removed, "%s%s", removed->len ? "," : "", was[i]);
        if (removed->len) fprintf(stderr, "hde-xsettings: displays: unplugged: %s\n", removed->str);
        if (added->len) {
            char *act = settings_string("display_connect", "ask");
            fprintf(stderr, "hde-xsettings: displays: connected: %s (display_connect=%s)\n", added->str, act);
            HdeProjectMode m = hde_project_from_id(act);
            if (hde_randr_n_connected(st) < 2) {
                /* the only screen: nothing to choose */
            } else if (!strcmp(act, "ask")) {
                char *prog = hde_program("hde-settings");
                if (prog) {
                    char *arg = g_strdup_printf("--connected=%s", added->str);
                    char *argv[] = { prog, (char *)"--project", arg, NULL };
                    spawn_detached(argv);
                    g_free(arg);
                    g_free(prog);
                }
            } else if (m != HDE_PROJECT_OTHER) {
                apply_layout(st, m, "screen connected, display_connect");
            }
            g_free(act);
        }
        g_string_free(added, TRUE);
        g_string_free(removed, TRUE);
        g_strfreev(was);
        g_strfreev(is);
        g_strlcpy(display_names, names, sizeof display_names);
    }
    hde_gamma_apply(dpy, 0);
    g_free(st);
}

/* hde-xsettings --status: what HDE sees and whether the devices match Settings > Input. 0 = all match. */
static int print_input_status(void)
{
    HdeInputPrefs p;
    hde_input_prefs_load(&p);
    printf("HDE build %s\n", HDE_VERSION);
    printf("Settings > Input: touchpad scrolling %s, tap to click %s, mouse wheel %s\n",
           p.touchpad_natural ? "like a phone (natural scrolling on)" : "like a mouse wheel (natural scrolling off)",
           p.tap_to_click ? "on" : "off",
           !p.has_mouse_natural ? "left alone" : p.mouse_natural ? "reversed (natural)" : "classic");
    Window xs = XGetSelectionOwner(dpy, sel_atom);
    gboolean service = hde_input_service_running(dpy);
    printf("HDE input service (hde-xsettings): %s\n",
           service ? "running" : "NOT running: the settings are applied only at login and while Settings > Input is "
                                 "open; log out and log in again");
    printf("XSETTINGS (theme) manager: %s\n", xs == None ? "none" : window_is_hde_xsettings(xs) ? "hde-xsettings"
                                                                                                : "another program");
    if (!hde_input_supported()) {
        printf("This hde-xsettings was built without libxi-dev: touchpad and mouse settings cannot be applied.\n"
               "Install it (sudo apt install libxi-dev), then: make && sudo make install\n");
        return 1;
    }
    if (!hde_input_init(dpy)) {
        printf("The X server has no XInput 2: touchpad and mouse settings cannot be applied.\n");
        return 1;
    }
    HdeInputDevice devs[32];
    int n = hde_input_list_all(dpy, devs, (int)G_N_ELEMENTS(devs)), bad = 0, managed = 0;
    printf("Pointer devices:\n");
    for (int i = 0; i < n; i++) {
        const HdeInputDevice *d = &devs[i];
        if (!d->configurable) {
            printf("  [%d] %s: not configurable by HDE (%s driver, no libinput, synaptics or evdev settings)\n", d->id,
                   d->name, d->driver);
            continue;
        }
        managed++;
        int wn, wt;
        hde_input_wanted(d, &p, &wn, &wt);
        gboolean differs = (wn >= 0 && d->natural >= 0 && wn != d->natural) ||
                           (wt >= 0 && d->tapping >= 0 && wt != d->tapping);
        if (differs) bad++;
        printf("  [%d] %s: %s, %s driver: natural scrolling %s", d->id, d->name, hde_input_kind_label(d, &p), d->driver,
               on_off(d->natural));
        if (d->kind == HDE_INPUT_TOUCHPAD) printf(", tap to click %s", on_off(d->tapping));
        if (differs)
            printf("  <- DIFFERS from Settings (natural scrolling %s, tap to click %s)", on_off(wn), on_off(wt));
        else if (wn < 0)
            printf("  (wheel direction left alone)");
        else
            printf("  OK");
        printf("\n");
    }
    if (managed == 0)
        printf("  No touchpad or mouse uses the libinput, synaptics or evdev X driver "
               "(package xserver-xorg-input-libinput).\n");
    if (p.n_as_touchpad) {
        printf("Used as the touchpad (treat_as_touchpad):");
        for (int i = 0; i < p.n_as_touchpad; i++) printf("%s \"%s\"", i ? "," : "", p.as_touchpad[i]);
        printf("\n");
    }
    if (bad) printf("Result: %d device(s) differ from Settings.%s\n", bad,
                    service ? "" : " Start the service: hde-xsettings & (or log out and in)");
    else printf("Result: every device matches Settings.\n");
    return bad ? 1 : 0;
}

static int print_status(void)
{
    int rc = print_input_status();
    HdeRandrState *st = g_new0(HdeRandrState, 1);
    if (hde_randr_read(dpy, st, 0)) {
        char desc[768];
        hde_randr_describe(st, desc, sizeof desc);
        printf("Displays: %s (layout: %s)\n", desc, hde_project_label(hde_randr_mode_of(st)));
    } else {
        printf("Displays: %s\n", hde_randr_supported() ? "the X server has no RandR 1.2"
                                                       : "unknown (built without libxrandr-dev: no F8 layouts)");
    }
    g_free(st);
    HdeBrightness b;
    hde_brightness_get(dpy, &b);
    char bd[384];
    hde_brightness_describe(&b, bd, sizeof bd);
    printf("Brightness (F6/F7): %s\n", bd);
    return rc;
}

int main(int argc, char **argv)
{
    gboolean replace = FALSE, status = FALSE;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--replace")) replace = TRUE;
        else if (!strcmp(argv[i], "--status")) status = TRUE;
        else if (!strcmp(argv[i], "--version")) {
            printf("hde-xsettings (HDE) %s\n", HDE_VERSION);
            return 0;
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("Usage: hde-xsettings [--replace]   XSETTINGS + touchpad/mouse + display service (started by hde-session)\n"
                   "       hde-xsettings --status      touchpads and mice, and whether they match Settings > Input;\n"
                   "                                   the screens and how their brightness can be changed\n"
                   "       hde-xsettings --version\n");
            return 0;
        }
    }
    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "hde-xsettings: cannot open X display\n");
        return status ? 2 : 1;
    }
    int screen = DefaultScreen(dpy);
    root = RootWindow(dpy, screen);
    char selname[32], inputsel[32];
    g_snprintf(selname, sizeof selname, "_XSETTINGS_S%d", screen);
    g_snprintf(inputsel, sizeof inputsel, HDE_INPUT_SELECTION, screen);
    sel_atom = XInternAtom(dpy, selname, False);
    settings_atom = XInternAtom(dpy, "_XSETTINGS_SETTINGS", False);
    manager_atom = XInternAtom(dpy, "MANAGER", False);
    Atom input_atom = XInternAtom(dpy, inputsel, False);

    if (status) {
        int rc = print_status();
        XCloseDisplay(dpy);
        return rc;
    }
    fprintf(stderr, "hde-xsettings: HDE build %s\n", HDE_VERSION);

    /* XSETTINGS: ours, unless another manager runs. Another hde-xsettings already does everything: leave it be. */
    gboolean xs_owner = TRUE;
    Window other = XGetSelectionOwner(dpy, sel_atom);
    if (other != None && !replace) {
        if (window_is_hde_xsettings(other)) {
            fprintf(stderr, "hde-xsettings: already running (use --replace to restart it)\n");
            XCloseDisplay(dpy);
            return 0;
        }
        fprintf(stderr, "hde-xsettings: another XSETTINGS manager is running: it keeps the theme settings; HDE still "
                        "applies the touchpad and mouse settings (--replace takes the theme settings over too)\n");
        xs_owner = FALSE;
    }

    XSetWindowAttributes attrs;
    attrs.override_redirect = True;
    attrs.event_mask = PropertyChangeMask | StructureNotifyMask;
    mgr_win = XCreateWindow(dpy, root, -100, -100, 1, 1, 0, CopyFromParent, InputOnly, CopyFromParent,
                            CWOverrideRedirect | CWEventMask, &attrs);
    XStoreName(dpy, mgr_win, "hde-xsettings");
    Time t = server_time();
    if (xs_owner) {
        XSetSelectionOwner(dpy, sel_atom, mgr_win, t);
        if (XGetSelectionOwner(dpy, sel_atom) != mgr_win) {
            fprintf(stderr, "hde-xsettings: failed to acquire %s; only the touchpad and mouse settings are applied\n",
                    selname);
            xs_owner = FALSE;
        }
    }
    if (xs_owner) {
        publish(read_settings());
        XClientMessageEvent xev;
        memset(&xev, 0, sizeof xev);
        xev.type = ClientMessage;
        xev.window = root;
        xev.message_type = manager_atom;
        xev.format = 32;
        xev.data.l[0] = (long)t;
        xev.data.l[1] = (long)sel_atom;
        xev.data.l[2] = (long)mgr_win;
        XSendEvent(dpy, root, False, StructureNotifyMask, (XEvent *)&xev);
        XFlush(dpy);
    }

    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    signal(SIGHUP, on_hup);

    /* touchpad / mouse settings: one hde-xsettings does them (the newest one takes the selection over) */
    static const char *const ITAG = "hde-xsettings: input";
    static const char *const CTAG = "hde-xsettings: input (changed by another program, set back)";
    HdeInputPrefs iprefs;
    hde_input_prefs_load(&iprefs);
    XSetSelectionOwner(dpy, input_atom, mgr_win, t);
    gboolean input_owner = XGetSelectionOwner(dpy, input_atom) == mgr_win;
    gboolean xi = input_owner && hde_input_watch(dpy);
    time_t recheck_at = 0;
    gint64 changed_at = 0;          /* monotonic time at which to look at devices changed by other programs */
    if (xi) {
        hde_input_apply(dpy, -1, &iprefs, ITAG);
        log_input_devices(&iprefs);
        recheck_at = time(NULL) + 4;
    } else if (input_owner) {
        fprintf(stderr, "hde-xsettings: %s: touchpad and mouse settings are not applied\n",
                hde_input_supported() ? "no XInput 2 on this X server" : "built without libxi-dev");
    }
    /* display service: one per X screen too (the input selection owner), see display_check() */
    int rr_base = -1;
    gboolean rr = input_owner && hde_randr_watch(dpy, &rr_base);
    gint64 display_at = 0;
    if (rr) {
        display_start();
        apply_night_light();
        hde_gamma_apply(dpy, 0);
    } else if (input_owner) {
        fprintf(stderr, "hde-xsettings: displays: %s\n", hde_randr_supported() ? "no RandR 1.2 on this X server"
                                                         : "built without libxrandr-dev (no screen hotplug handling)");
    }
    if (!xs_owner && !xi && !rr) {
        fprintf(stderr, "hde-xsettings: nothing to do; exiting\n");
        XDestroyWindow(dpy, mgr_win);
        XCloseDisplay(dpy);
        return 0;
    }

    guint64 m1 = file_sig("hde", "settings.ini"), m2 = file_sig("gtk-3.0", "settings.ini");
    int fd = ConnectionNumber(dpy);
    while (!stop_flag) {
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            if (rr && hde_randr_is_event(dpy, &ev, rr_base)) {
                if (!display_at) display_at = g_get_monotonic_time() + 500 * 1000;
                continue;
            }
            int r = xi ? hde_input_handle_event(dpy, &ev, &iprefs, ITAG) : -1;
            if (r >= 0) {
                if (r & HDE_INPUT_EV_ADDED) recheck_at = time(NULL) + 2;
                if ((r & HDE_INPUT_EV_CHANGED) && !changed_at) changed_at = g_get_monotonic_time() + 300 * 1000;
                continue;
            }
            if (ev.type != SelectionClear) continue;
            if (ev.xselectionclear.selection == sel_atom && xs_owner) {
                fprintf(stderr, "hde-xsettings: another XSETTINGS manager took the theme settings over%s\n",
                        xi ? "; HDE keeps applying the touchpad and mouse settings" : "");
                xs_owner = FALSE;
            } else if (ev.xselectionclear.selection == input_atom && xi) {
                fprintf(stderr, "hde-xsettings: another hde-xsettings applies the touchpad and mouse settings now\n");
                xi = FALSE;
                rr = FALSE;
                changed_at = 0;
                recheck_at = 0;
                display_at = 0;
            }
            if (!xs_owner && !xi && !rr) {
                fprintf(stderr, "hde-xsettings: nothing left to do; exiting\n");
                stop_flag = 1;
            }
        }
        if (stop_flag) break;
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        struct timeval tv = { 1, 0 };
        gint64 next_at = changed_at && (!display_at || changed_at < display_at) ? changed_at : display_at;
        if (next_at) {
            gint64 wait = next_at - g_get_monotonic_time();
            if (wait < 0) wait = 0;
            if (wait < G_USEC_PER_SEC) {
                tv.tv_sec = 0;
                tv.tv_usec = (suseconds_t)wait;
            }
        }
        if (select(fd + 1, &fds, NULL, NULL, &tv) < 0 && errno != EINTR) break;
        /* settings.ini first: a program that changed a device right after writing it (Settings) is not undone */
        guint64 n1 = file_sig("hde", "settings.ini"), n2 = file_sig("gtk-3.0", "settings.ini");
        gboolean forced = reload_flag != 0;
        if (reload_flag || n1 != m1 || n2 != m2) {
            reload_flag = 0;
            m1 = n1;
            m2 = n2;
            if (xs_owner) publish(read_settings());
            HdeInputPrefs np;
            hde_input_prefs_load(&np);
            gboolean changed = !hde_input_prefs_equal(&np, &iprefs);
            iprefs = np;
            if (xi && (changed || forced)) hde_input_apply(dpy, -1, &iprefs, ITAG);
            if (rr) apply_night_light();
        }
        if (rr && display_at && g_get_monotonic_time() >= display_at) {
            display_at = 0;
            display_check();
        }
        if (xi && changed_at && g_get_monotonic_time() >= changed_at) {
            changed_at = 0;
            hde_input_apply_changed(dpy, &iprefs, CTAG);
        }
        if (xi && recheck_at && time(NULL) >= recheck_at) {
            recheck_at = 0;
            hde_input_apply(dpy, -1, &iprefs, "hde-xsettings: input re-check");
        }
    }
    XDestroyWindow(dpy, mgr_win);
    XCloseDisplay(dpy);
    return 0;
}
