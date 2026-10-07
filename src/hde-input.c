/* hde-input.c — touchpad and mouse settings for X11 pointer devices. See hde-input.h.
 *
 * Device properties used (XInput 2):
 *   libinput driver   "libinput Natural Scrolling Enabled", "libinput Tapping Enabled" (only touchpads have it),
 *                     "libinput Accel Speed" (FLOAT -1..1), "libinput Accel Profile Enabled" (adaptive, flat[, custom])
 *   synaptics driver  "Synaptics Scrolling Distance" (negative = natural scrolling),
 *                     "Synaptics Tap Action" (RT RB LT LB F1 F2 F3: 1/2/3-finger taps -> buttons 1/3/2)
 *   evdev driver      "Evdev Scrolling Distance" (vertical, horizontal, dial; negative = reversed): only the scroll
 *                     direction, e.g. a touchpad in PS/2 mouse mode whose firmware turns swipes into wheel turns
 * A touchpad is recognised the way GTK does it: a "libinput Tapping Enabled" or "Synaptics Off" property (or
 * two-finger / edge scrolling). Some touchpads reach X as a plain mouse — the touchpad of the computer inside a
 * virtual machine (the host sends wheel turns), a touchpad in PS/2 or HID mouse mode: the user marks those in
 * Settings ("It is my touchpad", treat_as_touchpad) and they then follow the touchpad direction.
 * hde-xsettings also watches these properties (XI_PropertyEvent): when another program — a window manager with its
 * own touchpad settings (Mutter, Muffin), an autostart script, xinput — changes one of them, HDE puts it back to what
 * Settings says, so the direction the user picked stays the direction the touchpad scrolls.
 */
#include "hde-input.h"
#include <X11/Xatom.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_XI2
#include <X11/extensions/XInput2.h>
#endif

static gboolean kf_bool(GKeyFile *kf, const char *key, gboolean *out)
{
    GError *e = NULL;
    gboolean v = g_key_file_get_boolean(kf, "settings", key, &e);
    if (e) {
        g_error_free(e);
        return FALSE;
    }
    *out = v ? TRUE : FALSE;
    return TRUE;
}

void hde_input_prefs_load(HdeInputPrefs *p)
{
    memset(p, 0, sizeof *p);
    p->touchpad_natural = TRUE;
    p->tap_to_click = TRUE;
    p->speed = 0.5;
    p->acceleration = TRUE;
    char *path = g_build_filename(g_get_user_config_dir(), "hde", "settings.ini", NULL);
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        kf_bool(kf, "natural_scroll", &p->touchpad_natural);
        kf_bool(kf, "tap_to_click", &p->tap_to_click);
        p->has_mouse_natural = kf_bool(kf, "mouse_natural_scroll", &p->mouse_natural);
        p->has_acceleration = kf_bool(kf, "pointer_acceleration", &p->acceleration);
        gsize n = 0;
        char **names = g_key_file_get_string_list(kf, "settings", HDE_INPUT_AS_TOUCHPAD_KEY, &n, NULL);
        for (gsize i = 0; names && i < n && p->n_as_touchpad < HDE_INPUT_MAX_AS_TOUCHPAD; i++) {
            if (!names[i][0] || hde_input_prefs_as_touchpad(p, names[i])) continue;
            g_strlcpy(p->as_touchpad[p->n_as_touchpad++], names[i], sizeof p->as_touchpad[0]);
        }
        g_strfreev(names);
        GError *e = NULL;
        double d = g_key_file_get_double(kf, "settings", "pointer_speed", &e);
        if (e) g_error_free(e);
        else {
            p->has_speed = TRUE;
            p->speed = CLAMP(d, 0.0, 1.0);
        }
    }
    g_key_file_free(kf);
    g_free(path);
}

gboolean hde_input_prefs_equal(const HdeInputPrefs *a, const HdeInputPrefs *b)
{
    double ds = a->speed - b->speed;
    if (a->n_as_touchpad != b->n_as_touchpad) return FALSE;
    for (int i = 0; i < a->n_as_touchpad; i++)
        if (strcmp(a->as_touchpad[i], b->as_touchpad[i])) return FALSE;
    return a->touchpad_natural == b->touchpad_natural && a->tap_to_click == b->tap_to_click &&
           a->has_mouse_natural == b->has_mouse_natural && a->mouse_natural == b->mouse_natural &&
           a->has_speed == b->has_speed && ds < 1e-6 && ds > -1e-6 &&
           a->has_acceleration == b->has_acceleration && a->acceleration == b->acceleration;
}

gboolean hde_input_prefs_as_touchpad(const HdeInputPrefs *p, const char *name)
{
    for (int i = 0; p && name && i < p->n_as_touchpad; i++)
        if (!strcmp(p->as_touchpad[i], name)) return TRUE;
    return FALSE;
}

gboolean hde_input_follows_touchpad(const HdeInputDevice *d, const HdeInputPrefs *p)
{
    return d && (d->kind == HDE_INPUT_TOUCHPAD || (d->configurable && hde_input_prefs_as_touchpad(p, d->name)));
}

const char *hde_input_kind_label(const HdeInputDevice *d, const HdeInputPrefs *p)
{
    if (!d) return "?";
    if (d->kind == HDE_INPUT_TOUCHPAD) return "touchpad";
    if (hde_input_follows_touchpad(d, p)) return "mouse used as the touchpad";
    return d->kind == HDE_INPUT_MOUSE ? "mouse" : "other";
}

#ifdef HAVE_XI2

gboolean hde_input_supported(void) { return TRUE; }

enum { A_TAP, A_NATURAL, A_SPEED, A_PROFILE, A_PROFILES_AVAILABLE, A_SCROLL_METHODS,
       A_SYN_OFF, A_SYN_SCROLL_DIST, A_SYN_TAP_ACTION, A_EVDEV_AXIS_INV, A_EVDEV_SCROLL_DIST, A_FLOAT, N_ATOMS };
static const char *const atom_names[N_ATOMS] = {
    "libinput Tapping Enabled", "libinput Natural Scrolling Enabled", "libinput Accel Speed",
    "libinput Accel Profile Enabled", "libinput Accel Profiles Available", "libinput Scroll Methods Available",
    "Synaptics Off", "Synaptics Scrolling Distance", "Synaptics Tap Action",
    "Evdev Axis Inversion", "Evdev Scrolling Distance", "FLOAT",
};

static int watch_opcode = -1;
static Atom watch_atoms[N_ATOMS];        /* interned by hde_input_watch() */

/* ---- X errors: a device can disappear (unplugged, suspend) or be disabled between two requests ---- */
static int trap_error;
static int (*trap_old)(Display *, XErrorEvent *);
static int trap_handler(Display *d, XErrorEvent *e) { (void)d; trap_error = e->error_code; return 0; }
static void trap_push(void) { trap_error = 0; trap_old = XSetErrorHandler(trap_handler); }
static int trap_pop(Display *dpy)
{
    XSync(dpy, False);
    XSetErrorHandler(trap_old);
    return trap_error;
}

static gboolean debug_on(void) { return getenv("HDE_DEBUG") != NULL; }

gboolean hde_input_init(Display *dpy)
{
    int op, ev, err;
    if (!dpy || !XQueryExtension(dpy, "XInputExtension", &op, &ev, &err)) return FALSE;
    int major = 2, minor = 0;
    trap_push();
    Status s = XIQueryVersion(dpy, &major, &minor);
    if (trap_pop(dpy) || s != Success) return FALSE;
    return TRUE;
}

Display *hde_input_open(void)
{
    /* the Wayland session: labwc applies these settings (hde-settings --wayland-config); the X server there is
     * Xwayland, whose devices are only stand-ins */
    const char *st = g_getenv("XDG_SESSION_TYPE");
    if (g_getenv("WAYLAND_DISPLAY") && st && !strcmp(st, "wayland")) return NULL;
    Display *dpy = XOpenDisplay(NULL);
    if (dpy && !hde_input_init(dpy)) {
        XCloseDisplay(dpy);
        dpy = NULL;
    }
    return dpy;
}

/* ---- properties (XInput 2 returns raw items: 1, 2 or 4 bytes each) ---- */
typedef struct { Atom type; int format; unsigned long n; unsigned char *data; } Prop;

static gboolean prop_read(Display *dpy, int dev, Atom atom, Prop *p)
{
    unsigned long after = 0;
    memset(p, 0, sizeof *p);
    if (atom == None) return FALSE;
    if (XIGetProperty(dpy, dev, atom, 0, 64, False, AnyPropertyType, &p->type, &p->format, &p->n, &after,
                      &p->data) != Success)
        p->data = NULL;
    if (!p->data || p->type == None || p->n == 0) {
        if (p->data) XFree(p->data);
        memset(p, 0, sizeof *p);
        return FALSE;
    }
    return TRUE;
}

static void prop_free(Prop *p)
{
    if (p->data) XFree(p->data);
    p->data = NULL;
}

static long prop_int(const Prop *p, unsigned long i)
{
    if (i >= p->n) return 0;
    if (p->format == 8) return p->data[i];
    if (p->format == 16) { int16_t v; memcpy(&v, p->data + 2 * i, 2); return v; }
    int32_t v;
    memcpy(&v, p->data + 4 * i, 4);
    return v;
}

static void note(GString *log, const char *what, const char *value)
{
    g_string_append_printf(log, "%s%s %s", log->len ? ", " : "", what, value);
}

typedef struct {
    int id;
    const char *name;
    HdeInputKind kind;
    const char *driver;
    gboolean has[N_ATOMS];
} Dev;

/* What kind of device is this, and which of our properties does it have? FALSE: nothing HDE can configure. */
static gboolean dev_probe(Display *dpy, const Atom *atoms, int id, const char *name, Dev *d)
{
    memset(d, 0, sizeof *d);
    d->id = id;
    d->name = name ? name : "?";
    int n = 0;
    Atom *list = XIListProperties(dpy, id, &n);
    for (int i = 0; list && i < n; i++)
        for (int a = 0; a < A_FLOAT; a++)
            if (list[i] == atoms[a]) d->has[a] = TRUE;
    if (list) XFree(list);

    if (d->has[A_SYN_OFF] || d->has[A_SYN_SCROLL_DIST] || d->has[A_SYN_TAP_ACTION]) {
        d->kind = HDE_INPUT_TOUCHPAD;
        d->driver = "synaptics";
        return TRUE;
    }
    if (!d->has[A_TAP] && !d->has[A_NATURAL] && !d->has[A_SPEED]) {
        if (!d->has[A_EVDEV_SCROLL_DIST]) return FALSE;
        d->kind = HDE_INPUT_MOUSE;           /* evdev: no touchpad gestures, only wheel (or firmware) scrolling */
        d->driver = "evdev";
        return TRUE;
    }
    d->driver = "libinput";
    d->kind = d->has[A_TAP] ? HDE_INPUT_TOUCHPAD : HDE_INPUT_MOUSE;
    if (d->kind == HDE_INPUT_MOUSE && d->has[A_SCROLL_METHODS]) {
        /* a touchpad without tapping still offers two-finger or edge scrolling */
        Prop p;
        if (prop_read(dpy, id, atoms[A_SCROLL_METHODS], &p)) {
            if (p.format == 8 && (prop_int(&p, 0) || prop_int(&p, 1))) d->kind = HDE_INPUT_TOUCHPAD;
            prop_free(&p);
        }
    }
    return TRUE;
}

static void set_bool8(Display *dpy, const Dev *d, Atom atom, gboolean want, const char *what, GString *log)
{
    Prop p;
    if (!prop_read(dpy, d->id, atom, &p)) return;
    gboolean ok = p.format == 8 && p.n == 1;
    gboolean cur = ok && prop_int(&p, 0) != 0;
    prop_free(&p);
    if (!ok || cur == want) return;
    unsigned char v = want ? 1 : 0;
    XIChangeProperty(dpy, d->id, atom, XA_INTEGER, 8, PropModeReplace, &v, 1);
    note(log, what, want ? "on" : "off");
}

/* synaptics: natural scrolling = negative scrolling distances */
static void syn_natural(Display *dpy, const Dev *d, Atom atom, gboolean natural, GString *log)
{
    Prop p;
    if (!prop_read(dpy, d->id, atom, &p)) return;
    if (p.format == 32 && p.type == XA_INTEGER && p.n == 2) {
        int32_t cur[2] = { (int32_t)prop_int(&p, 0), (int32_t)prop_int(&p, 1) }, want[2];
        for (int i = 0; i < 2; i++) {
            int32_t a = cur[i] < 0 ? -cur[i] : cur[i];
            want[i] = natural ? -a : a;
        }
        if (cur[0] && cur[1] && (want[0] != cur[0] || want[1] != cur[1])) {
            XIChangeProperty(dpy, d->id, atom, XA_INTEGER, 32, PropModeReplace, (unsigned char *)want, 2);
            note(log, "natural scrolling", natural ? "on" : "off");
        }
    }
    prop_free(&p);
}

/* evdev: natural scrolling = negative vertical and horizontal scrolling distances (the dial is left alone) */
static void evdev_natural(Display *dpy, const Dev *d, Atom atom, gboolean natural, GString *log)
{
    Prop p;
    if (!prop_read(dpy, d->id, atom, &p)) return;
    if (p.format == 32 && p.type == XA_INTEGER && p.n == 3) {
        int32_t cur[3], want[3];
        for (int i = 0; i < 3; i++) cur[i] = want[i] = (int32_t)prop_int(&p, (unsigned long)i);
        for (int i = 0; i < 2; i++) {
            int32_t a = cur[i] < 0 ? -cur[i] : cur[i];
            want[i] = natural ? -a : a;
        }
        if (memcmp(cur, want, sizeof cur) != 0) {
            XIChangeProperty(dpy, d->id, atom, XA_INTEGER, 32, PropModeReplace, (unsigned char *)want, 3);
            note(log, "natural scrolling", natural ? "on" : "off");
        }
    }
    prop_free(&p);
}

/* synaptics: one-, two-, three-finger taps -> left, right, middle button (or nothing) */
static void syn_tap(Display *dpy, const Dev *d, Atom atom, gboolean tap, GString *log)
{
    Prop p;
    if (!prop_read(dpy, d->id, atom, &p)) return;
    if (p.format == 8 && p.n >= 7 && p.n <= 16) {
        unsigned char v[16];
        const unsigned char want[3] = { tap ? 1 : 0, tap ? 3 : 0, tap ? 2 : 0 };
        memcpy(v, p.data, p.n);
        if (memcmp(v + 4, want, 3) != 0) {
            memcpy(v + 4, want, 3);
            XIChangeProperty(dpy, d->id, atom, XA_INTEGER, 8, PropModeReplace, v, (int)p.n);
            note(log, "tap to click", tap ? "on" : "off");
        }
    }
    prop_free(&p);
}

static void set_speed(Display *dpy, const Dev *d, const Atom *atoms, double speed01, GString *log)
{
    Prop p;
    if (!prop_read(dpy, d->id, atoms[A_SPEED], &p)) return;
    if (p.format == 32 && p.n == 1 && p.type == atoms[A_FLOAT]) {
        float cur, want = (float)CLAMP(speed01 * 2.0 - 1.0, -1.0, 1.0), diff;
        memcpy(&cur, p.data, 4);
        diff = cur - want;
        if (diff > 0.005f || diff < -0.005f) {
            XIChangeProperty(dpy, d->id, atoms[A_SPEED], atoms[A_FLOAT], 32, PropModeReplace, (unsigned char *)&want, 1);
            char buf[32];
            g_snprintf(buf, sizeof buf, "%.2f", (double)want);
            note(log, "pointer speed", buf);
        }
    }
    prop_free(&p);
}

static void set_profile(Display *dpy, const Dev *d, const Atom *atoms, gboolean adaptive, GString *log)
{
    Prop cur, avail;
    int idx = adaptive ? 0 : 1;
    if (!prop_read(dpy, d->id, atoms[A_PROFILE], &cur)) return;
    if (cur.format == 8 && cur.n >= 2 && cur.n <= 8) {
        gboolean usable = TRUE;
        if (prop_read(dpy, d->id, atoms[A_PROFILES_AVAILABLE], &avail)) {
            usable = avail.format == 8 && prop_int(&avail, (unsigned long)idx) != 0;
            prop_free(&avail);
        }
        unsigned char want[8] = { 0 };
        want[idx] = 1;
        if (usable && memcmp(cur.data, want, cur.n) != 0) {
            XIChangeProperty(dpy, d->id, atoms[A_PROFILE], XA_INTEGER, 8, PropModeReplace, want, (int)cur.n);
            note(log, "acceleration", adaptive ? "adaptive" : "flat");
        }
    }
    prop_free(&cur);
}

static const char *kind_name(HdeInputKind k)
{
    return k == HDE_INPUT_TOUCHPAD ? "touchpad" : k == HDE_INPUT_MOUSE ? "mouse" : "other";
}

static int apply_dev(Display *dpy, const Atom *atoms, const Dev *d, const HdeInputPrefs *p, const char *tag)
{
    GString *log = g_string_new(NULL);
    gboolean libinput = !strcmp(d->driver, "libinput"), synaptics = !strcmp(d->driver, "synaptics");
    /* a touchpad that X sees as a mouse (virtual machine, mouse mode) follows the touchpad direction */
    gboolean as_touchpad = d->kind != HDE_INPUT_TOUCHPAD && hde_input_prefs_as_touchpad(p, d->name);
    int natural = d->kind == HDE_INPUT_TOUCHPAD || as_touchpad ? p->touchpad_natural
                : p->has_mouse_natural ? p->mouse_natural : -1;            /* -1: the mouse wheel is left alone */
    trap_push();
    if (synaptics) {
        syn_natural(dpy, d, atoms[A_SYN_SCROLL_DIST], p->touchpad_natural, log);
        syn_tap(dpy, d, atoms[A_SYN_TAP_ACTION], p->tap_to_click, log);
    } else if (libinput) {
        if (natural >= 0) set_bool8(dpy, d, atoms[A_NATURAL], natural, "natural scrolling", log);
        if (d->kind == HDE_INPUT_TOUCHPAD) set_bool8(dpy, d, atoms[A_TAP], p->tap_to_click, "tap to click", log);
    } else if (natural >= 0) {                                               /* evdev */
        evdev_natural(dpy, d, atoms[A_EVDEV_SCROLL_DIST], natural, log);
    }
    if (libinput && p->has_speed && d->has[A_SPEED]) set_speed(dpy, d, atoms, p->speed, log);
    if (libinput && p->has_acceleration && d->has[A_PROFILE]) set_profile(dpy, d, atoms, p->acceleration, log);
    int err = trap_pop(dpy);
    int changed = 0;
    const char *kind = as_touchpad ? "mouse used as the touchpad" : kind_name(d->kind);
    if (err) {
        if (tag && (log->len || debug_on()))
            fprintf(stderr, "%s: %s: not changed now (X error %d: device disabled or unplugged)\n", tag, d->name, err);
    } else if (log->len) {
        changed = 1;
        if (tag) fprintf(stderr, "%s: %s (%s, %s): %s\n", tag, d->name, kind, d->driver, log->str);
    } else if (tag && debug_on()) {
        fprintf(stderr, "%s: %s (%s, %s): already up to date\n", tag, d->name, kind, d->driver);
    }
    g_string_free(log, TRUE);
    return changed;
}

/* Calls fn for every enabled slave pointer HDE can configure (or only for deviceid). all: also for the pointers HDE
 * cannot configure (kind HDE_INPUT_OTHER, driver "evdev" or "unknown", d->has[] still filled in). */
typedef void (*DevFn)(Display *dpy, const Atom *atoms, const Dev *d, gboolean configurable, void *data);

static void for_each_device(Display *dpy, int deviceid, gboolean all, DevFn fn, void *data)
{
    Atom atoms[N_ATOMS];
    if (!XInternAtoms(dpy, (char **)atom_names, N_ATOMS, False, atoms)) return;
    int n = 0;
    trap_push();
    XIDeviceInfo *info = XIQueryDevice(dpy, deviceid >= 0 ? deviceid : XIAllDevices, &n);
    trap_pop(dpy);
    for (int i = 0; info && i < n; i++) {
        if (info[i].use != XISlavePointer || !info[i].enabled) continue;
        Dev d;
        trap_push();
        gboolean ok = dev_probe(dpy, atoms, info[i].deviceid, info[i].name, &d);
        if (trap_pop(dpy)) continue;
        if (ok) fn(dpy, atoms, &d, TRUE, data);
        else if (all) {
            d.kind = HDE_INPUT_OTHER;
            d.driver = d.has[A_EVDEV_AXIS_INV] || d.has[A_EVDEV_SCROLL_DIST] ? "evdev" : "unknown";
            fn(dpy, atoms, &d, FALSE, data);
        }
    }
    if (info) XIFreeDeviceInfo(info);
}

typedef struct { const HdeInputPrefs *p; const char *tag; int changed; } ApplyCtx;

static void apply_cb(Display *dpy, const Atom *atoms, const Dev *d, gboolean configurable, void *data)
{
    ApplyCtx *c = data;
    if (configurable) c->changed += apply_dev(dpy, atoms, d, c->p, c->tag);
}

int hde_input_apply(Display *dpy, int deviceid, const HdeInputPrefs *p, const char *tag)
{
    ApplyCtx c = { p, tag, 0 };
    if (dpy && p) for_each_device(dpy, deviceid, FALSE, apply_cb, &c);
    return c.changed;
}

typedef struct { HdeInputDevice *out; int max, n; } ListCtx;

static int read_flag(Display *dpy, int id, Atom atom)
{
    Prop p;
    int v = -1;
    if (prop_read(dpy, id, atom, &p)) {
        if (p.format == 8 && p.n == 1) v = prop_int(&p, 0) != 0;
        prop_free(&p);
    }
    return v;
}

static void list_cb(Display *dpy, const Atom *atoms, const Dev *d, gboolean configurable, void *data)
{
    ListCtx *c = data;
    if (c->n >= c->max) return;
    HdeInputDevice *o = &c->out[c->n];
    memset(o, 0, sizeof *o);
    o->id = d->id;
    g_strlcpy(o->name, d->name, sizeof o->name);
    o->kind = d->kind;
    o->driver = d->driver;
    o->configurable = configurable;
    o->natural = o->tapping = -1;
    if (!configurable) {
        c->n++;
        return;
    }
    trap_push();
    if (!strcmp(d->driver, "libinput")) {
        o->natural = read_flag(dpy, d->id, atoms[A_NATURAL]);
        if (d->kind == HDE_INPUT_TOUCHPAD) o->tapping = read_flag(dpy, d->id, atoms[A_TAP]);
    } else if (!strcmp(d->driver, "evdev")) {
        Prop p;
        if (prop_read(dpy, d->id, atoms[A_EVDEV_SCROLL_DIST], &p)) {
            if (p.format == 32 && p.n == 3 && prop_int(&p, 0) != 0) o->natural = prop_int(&p, 0) < 0;
            prop_free(&p);
        }
    } else {
        Prop p;
        if (prop_read(dpy, d->id, atoms[A_SYN_SCROLL_DIST], &p)) {
            if (p.format == 32 && p.n >= 1) o->natural = prop_int(&p, 0) < 0;
            prop_free(&p);
        }
        if (prop_read(dpy, d->id, atoms[A_SYN_TAP_ACTION], &p)) {
            if (p.format == 8 && p.n >= 7) o->tapping = prop_int(&p, 4) != 0;
            prop_free(&p);
        }
    }
    if (!trap_pop(dpy)) c->n++;
}

int hde_input_list(Display *dpy, HdeInputDevice *out, int max)
{
    ListCtx c = { out, max, 0 };
    if (dpy && out && max > 0) for_each_device(dpy, -1, FALSE, list_cb, &c);
    return c.n;
}

int hde_input_list_all(Display *dpy, HdeInputDevice *out, int max)
{
    ListCtx c = { out, max, 0 };
    if (dpy && out && max > 0) for_each_device(dpy, -1, TRUE, list_cb, &c);
    return c.n;
}

gboolean hde_input_lookup(Display *dpy, int deviceid, HdeInputDevice *out)
{
    ListCtx c = { out, 1, 0 };
    if (dpy && out && deviceid > 0) for_each_device(dpy, deviceid, TRUE, list_cb, &c);
    return c.n == 1;
}

int hde_input_device_natural(Display *dpy, int deviceid)
{
    HdeInputDevice d;
    return hde_input_lookup(dpy, deviceid, &d) && d.configurable ? d.natural : -1;
}

gboolean hde_input_watch(Display *dpy)
{
    int op, ev, err;
    if (!hde_input_init(dpy) || !XQueryExtension(dpy, "XInputExtension", &op, &ev, &err)) return FALSE;
    if (!XInternAtoms(dpy, (char **)atom_names, N_ATOMS, False, watch_atoms)) return FALSE;
    unsigned char bits[XIMaskLen(XI_LASTEVENT)];
    memset(bits, 0, sizeof bits);
    XISetMask(bits, XI_HierarchyChanged);
    XISetMask(bits, XI_PropertyEvent);
    XIEventMask m;
    m.deviceid = XIAllDevices;
    m.mask_len = sizeof bits;
    m.mask = bits;
    trap_push();
    XISelectEvents(dpy, DefaultRootWindow(dpy), &m, 1);
    if (trap_pop(dpy)) return FALSE;
    watch_opcode = op;
    return TRUE;
}

/* ---- changes made by other programs ---- */
static int changed_ids[32], n_changed;

typedef struct { int id; gint64 since, quiet_until; int count; } Guard;
static Guard guards[16];

static gboolean watched_property(Atom a)
{
    static const int which[] = { A_TAP, A_NATURAL, A_SPEED, A_PROFILE, A_SYN_SCROLL_DIST, A_SYN_TAP_ACTION,
                                 A_EVDEV_SCROLL_DIST };
    for (unsigned i = 0; i < G_N_ELEMENTS(which); i++)
        if (a != None && a == watch_atoms[which[i]]) return TRUE;
    return FALSE;
}

static void mark_changed(int id)
{
    for (int i = 0; i < n_changed; i++)
        if (changed_ids[i] == id) return;
    if (n_changed < (int)G_N_ELEMENTS(changed_ids)) changed_ids[n_changed++] = id;
}

static Guard *guard_for(int id)
{
    Guard *oldest = &guards[0];
    for (unsigned i = 0; i < G_N_ELEMENTS(guards); i++) {
        if (guards[i].id == id) return &guards[i];
        if (guards[i].since < oldest->since) oldest = &guards[i];
    }
    memset(oldest, 0, sizeof *oldest);
    oldest->id = id;
    return oldest;
}

int hde_input_handle_event(Display *dpy, XEvent *ev, const HdeInputPrefs *p, const char *tag)
{
    if (watch_opcode < 0 || ev->type != GenericEvent || ev->xcookie.extension != watch_opcode) return -1;
    if (!XGetEventData(dpy, &ev->xcookie)) return 0;
    int ids[64], n = 0, result = 0;
    if (ev->xcookie.evtype == XI_HierarchyChanged) {
        const XIHierarchyEvent *he = ev->xcookie.data;
        for (int i = 0; i < he->num_info && n < (int)G_N_ELEMENTS(ids); i++)
            if (he->info[i].flags & (XISlaveAdded | XIDeviceEnabled | XISlaveAttached)) ids[n++] = he->info[i].deviceid;
    } else if (ev->xcookie.evtype == XI_PropertyEvent) {
        const XIPropertyEvent *pe = ev->xcookie.data;
        if (pe->what != XIPropertyDeleted && watched_property(pe->property)) {
            mark_changed(pe->deviceid);
            result |= HDE_INPUT_EV_CHANGED;
        }
    }
    XFreeEventData(dpy, &ev->xcookie);
    for (int i = 0; i < n; i++) {
        if (debug_on() && tag) fprintf(stderr, "%s: device %d added or enabled\n", tag, ids[i]);
        hde_input_apply(dpy, ids[i], p, tag);
    }
    if (n) result |= HDE_INPUT_EV_ADDED;
    return result;
}

int hde_input_apply_changed(Display *dpy, const HdeInputPrefs *p, const char *tag)
{
    int ids[G_N_ELEMENTS(changed_ids)], n = n_changed, fixed = 0;
    memcpy(ids, changed_ids, sizeof ids);
    n_changed = 0;
    gint64 now = g_get_monotonic_time();
    for (int i = 0; dpy && p && i < n; i++) {
        Guard *g = guard_for(ids[i]);
        if (g->quiet_until > now) continue;
        if (!hde_input_apply(dpy, ids[i], p, tag)) continue;      /* nothing differed: HDE's own change */
        fixed++;
        if (now - g->since > 30 * G_USEC_PER_SEC) {
            g->since = now;
            g->count = 0;
        }
        if (++g->count >= 5) {
            g->quiet_until = now + 60 * G_USEC_PER_SEC;
            g->count = 0;
            if (tag)
                fprintf(stderr, "%s: device %d: another program keeps changing it back; HDE leaves it alone for a "
                                "minute (two touchpad settings tools are running?)\n", tag, ids[i]);
        }
    }
    return fixed;
}

#else /* built without libxi-dev */

gboolean hde_input_supported(void) { return FALSE; }
gboolean hde_input_init(Display *dpy) { (void)dpy; return FALSE; }
Display *hde_input_open(void) { return NULL; }
int hde_input_apply(Display *dpy, int deviceid, const HdeInputPrefs *p, const char *tag)
{
    (void)dpy; (void)deviceid; (void)p; (void)tag;
    return 0;
}
int hde_input_list(Display *dpy, HdeInputDevice *out, int max) { (void)dpy; (void)out; (void)max; return 0; }
int hde_input_list_all(Display *dpy, HdeInputDevice *out, int max) { (void)dpy; (void)out; (void)max; return 0; }
gboolean hde_input_lookup(Display *dpy, int deviceid, HdeInputDevice *out)
{
    (void)dpy; (void)deviceid; (void)out;
    return FALSE;
}
int hde_input_device_natural(Display *dpy, int deviceid) { (void)dpy; (void)deviceid; return -1; }
gboolean hde_input_watch(Display *dpy) { (void)dpy; return FALSE; }
int hde_input_handle_event(Display *dpy, XEvent *ev, const HdeInputPrefs *p, const char *tag)
{
    (void)dpy; (void)ev; (void)p; (void)tag;
    return -1;
}
int hde_input_apply_changed(Display *dpy, const HdeInputPrefs *p, const char *tag)
{
    (void)dpy; (void)p; (void)tag;
    return 0;
}

#endif

/* ---- with or without XInput 2 ---- */
void hde_input_wanted(const HdeInputDevice *d, const HdeInputPrefs *p, int *natural, int *tapping)
{
    *natural = *tapping = -1;
    if (!d || !p || !d->configurable) return;
    if (d->kind == HDE_INPUT_TOUCHPAD) {
        *natural = p->touchpad_natural ? 1 : 0;
        *tapping = p->tap_to_click ? 1 : 0;
    } else if (hde_input_prefs_as_touchpad(p, d->name)) {
        *natural = p->touchpad_natural ? 1 : 0;
    } else if (d->kind == HDE_INPUT_MOUSE && p->has_mouse_natural) {
        *natural = p->mouse_natural ? 1 : 0;
    }
}

gboolean hde_input_service_running(Display *dpy)
{
    if (!dpy) return FALSE;
    char name[32];
    g_snprintf(name, sizeof name, HDE_INPUT_SELECTION, DefaultScreen(dpy));
    return XGetSelectionOwner(dpy, XInternAtom(dpy, name, False)) != None;
}
