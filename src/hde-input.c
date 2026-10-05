/* hde-input.c — touchpad and mouse settings for X11 pointer devices. See hde-input.h.
 *
 * Device properties used (XInput 2):
 *   libinput driver   "libinput Natural Scrolling Enabled", "libinput Tapping Enabled" (only touchpads have it),
 *                     "libinput Accel Speed" (FLOAT -1..1), "libinput Accel Profile Enabled" (adaptive, flat[, custom])
 *   synaptics driver  "Synaptics Scrolling Distance" (negative = natural scrolling),
 *                     "Synaptics Tap Action" (RT RB LT LB F1 F2 F3: 1/2/3-finger taps -> buttons 1/3/2)
 * A touchpad is recognised the way GTK does it: a "libinput Tapping Enabled" or "Synaptics Off" property.
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
    return a->touchpad_natural == b->touchpad_natural && a->tap_to_click == b->tap_to_click &&
           a->has_mouse_natural == b->has_mouse_natural && a->mouse_natural == b->mouse_natural &&
           a->has_speed == b->has_speed && ds < 1e-6 && ds > -1e-6 &&
           a->has_acceleration == b->has_acceleration && a->acceleration == b->acceleration;
}

#ifdef HAVE_XI2

gboolean hde_input_supported(void) { return TRUE; }

enum { A_TAP, A_NATURAL, A_SPEED, A_PROFILE, A_PROFILES_AVAILABLE, A_SCROLL_METHODS,
       A_SYN_OFF, A_SYN_SCROLL_DIST, A_SYN_TAP_ACTION, A_FLOAT, N_ATOMS };
static const char *const atom_names[N_ATOMS] = {
    "libinput Tapping Enabled", "libinput Natural Scrolling Enabled", "libinput Accel Speed",
    "libinput Accel Profile Enabled", "libinput Accel Profiles Available", "libinput Scroll Methods Available",
    "Synaptics Off", "Synaptics Scrolling Distance", "Synaptics Tap Action", "FLOAT",
};

static int watch_opcode = -1;

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
    if (!d->has[A_TAP] && !d->has[A_NATURAL] && !d->has[A_SPEED]) return FALSE;
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
    gboolean libinput = !strcmp(d->driver, "libinput");
    trap_push();
    if (!libinput) {
        syn_natural(dpy, d, atoms[A_SYN_SCROLL_DIST], p->touchpad_natural, log);
        syn_tap(dpy, d, atoms[A_SYN_TAP_ACTION], p->tap_to_click, log);
    } else if (d->kind == HDE_INPUT_TOUCHPAD) {
        set_bool8(dpy, d, atoms[A_NATURAL], p->touchpad_natural, "natural scrolling", log);
        set_bool8(dpy, d, atoms[A_TAP], p->tap_to_click, "tap to click", log);
    } else if (p->has_mouse_natural) {
        set_bool8(dpy, d, atoms[A_NATURAL], p->mouse_natural, "natural scrolling", log);
    }
    if (libinput && p->has_speed && d->has[A_SPEED]) set_speed(dpy, d, atoms, p->speed, log);
    if (libinput && p->has_acceleration && d->has[A_PROFILE]) set_profile(dpy, d, atoms, p->acceleration, log);
    int err = trap_pop(dpy);
    int changed = 0;
    if (err) {
        if (tag && (log->len || debug_on()))
            fprintf(stderr, "%s: %s: not changed now (X error %d: device disabled or unplugged)\n", tag, d->name, err);
    } else if (log->len) {
        changed = 1;
        if (tag) fprintf(stderr, "%s: %s (%s, %s): %s\n", tag, d->name, kind_name(d->kind), d->driver, log->str);
    } else if (tag && debug_on()) {
        fprintf(stderr, "%s: %s (%s, %s): already up to date\n", tag, d->name, kind_name(d->kind), d->driver);
    }
    g_string_free(log, TRUE);
    return changed;
}

/* Calls fn for every enabled slave pointer HDE can configure (or only for deviceid). */
typedef void (*DevFn)(Display *dpy, const Atom *atoms, const Dev *d, void *data);

static void for_each_device(Display *dpy, int deviceid, DevFn fn, void *data)
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
        if (trap_pop(dpy)) ok = FALSE;
        if (ok) fn(dpy, atoms, &d, data);
    }
    if (info) XIFreeDeviceInfo(info);
}

typedef struct { const HdeInputPrefs *p; const char *tag; int changed; } ApplyCtx;

static void apply_cb(Display *dpy, const Atom *atoms, const Dev *d, void *data)
{
    ApplyCtx *c = data;
    c->changed += apply_dev(dpy, atoms, d, c->p, c->tag);
}

int hde_input_apply(Display *dpy, int deviceid, const HdeInputPrefs *p, const char *tag)
{
    ApplyCtx c = { p, tag, 0 };
    if (dpy && p) for_each_device(dpy, deviceid, apply_cb, &c);
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

static void list_cb(Display *dpy, const Atom *atoms, const Dev *d, void *data)
{
    ListCtx *c = data;
    if (c->n >= c->max) return;
    HdeInputDevice *o = &c->out[c->n];
    memset(o, 0, sizeof *o);
    o->id = d->id;
    g_strlcpy(o->name, d->name, sizeof o->name);
    o->kind = d->kind;
    o->driver = d->driver;
    o->natural = o->tapping = -1;
    trap_push();
    if (!strcmp(d->driver, "libinput")) {
        o->natural = read_flag(dpy, d->id, atoms[A_NATURAL]);
        if (d->kind == HDE_INPUT_TOUCHPAD) o->tapping = read_flag(dpy, d->id, atoms[A_TAP]);
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
    if (dpy && out && max > 0) for_each_device(dpy, -1, list_cb, &c);
    return c.n;
}

gboolean hde_input_watch(Display *dpy)
{
    int op, ev, err;
    if (!hde_input_init(dpy) || !XQueryExtension(dpy, "XInputExtension", &op, &ev, &err)) return FALSE;
    unsigned char bits[XIMaskLen(XI_LASTEVENT)];
    memset(bits, 0, sizeof bits);
    XISetMask(bits, XI_HierarchyChanged);
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

int hde_input_handle_event(Display *dpy, XEvent *ev, const HdeInputPrefs *p, const char *tag)
{
    if (watch_opcode < 0 || ev->type != GenericEvent || ev->xcookie.extension != watch_opcode) return -1;
    if (!XGetEventData(dpy, &ev->xcookie)) return 0;
    int ids[64], n = 0;
    if (ev->xcookie.evtype == XI_HierarchyChanged) {
        const XIHierarchyEvent *he = ev->xcookie.data;
        for (int i = 0; i < he->num_info && n < (int)G_N_ELEMENTS(ids); i++)
            if (he->info[i].flags & (XISlaveAdded | XIDeviceEnabled | XISlaveAttached)) ids[n++] = he->info[i].deviceid;
    }
    XFreeEventData(dpy, &ev->xcookie);
    for (int i = 0; i < n; i++) {
        if (debug_on() && tag) fprintf(stderr, "%s: device %d added or enabled\n", tag, ids[i]);
        hde_input_apply(dpy, ids[i], p, tag);
    }
    return n;
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
gboolean hde_input_watch(Display *dpy) { (void)dpy; return FALSE; }
int hde_input_handle_event(Display *dpy, XEvent *ev, const HdeInputPrefs *p, const char *tag)
{
    (void)dpy; (void)ev; (void)p; (void)tag;
    return -1;
}

#endif
