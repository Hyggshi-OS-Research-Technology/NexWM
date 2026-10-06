/* hde-randr.c — screens, the Project layouts (F8) and gamma ramps through XRandR. See hde-randr.h. */
#define _DEFAULT_SOURCE
#include "hde-randr.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#ifdef HAVE_XRANDR
#include <X11/Xatom.h>
#include <X11/extensions/Xrandr.h>
#endif

static const char *const project_ids[HDE_PROJECT_N] = { "pc", "duplicate", "extend", "second" };
static const char *const project_labels[HDE_PROJECT_N] = { "PC screen only", "Duplicate", "Extend", "Second screen only" };

const char *hde_project_id(HdeProjectMode m)
{
    return m >= 0 && m < HDE_PROJECT_N ? project_ids[m] : "other";
}

const char *hde_project_label(HdeProjectMode m)
{
    return m >= 0 && m < HDE_PROJECT_N ? project_labels[m] : "Custom layout";
}

HdeProjectMode hde_project_from_id(const char *id)
{
    static const struct { const char *name; HdeProjectMode m; } names[] = {
        { "pc", HDE_PROJECT_PC }, { "pc-only", HDE_PROJECT_PC }, { "internal", HDE_PROJECT_PC },
        { "computer", HDE_PROJECT_PC }, { "1", HDE_PROJECT_PC },
        { "duplicate", HDE_PROJECT_DUPLICATE }, { "mirror", HDE_PROJECT_DUPLICATE }, { "clone", HDE_PROJECT_DUPLICATE },
        { "2", HDE_PROJECT_DUPLICATE },
        { "extend", HDE_PROJECT_EXTEND }, { "3", HDE_PROJECT_EXTEND },
        { "second", HDE_PROJECT_SECOND }, { "second-only", HDE_PROJECT_SECOND }, { "external", HDE_PROJECT_SECOND },
        { "projector", HDE_PROJECT_SECOND }, { "4", HDE_PROJECT_SECOND },
    };
    if (!id) return HDE_PROJECT_OTHER;
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!strcasecmp(id, names[i].name)) return names[i].m;
    return HDE_PROJECT_OTHER;
}

/* ======================================================================= pure part */
int hde_randr_name_is_builtin(const char *name)
{
    static const char *const prefixes[] = { "eDP", "LVDS", "DSI", "DPI", "PANEL", "Internal" };
    if (!name) return 0;
    for (unsigned i = 0; i < sizeof prefixes / sizeof prefixes[0]; i++)
        if (!strncasecmp(name, prefixes[i], strlen(prefixes[i]))) return 1;
    return 0;
}

int hde_randr_n_connected(const HdeRandrState *s)
{
    int n = 0;
    for (int i = 0; i < s->n_out; i++) n += s->out[i].connected != 0;
    return n;
}

int hde_randr_main_index(const HdeRandrState *s)
{
    for (int i = 0; i < s->n_out; i++)
        if (s->out[i].connected && s->out[i].builtin) return i;
    for (int i = 0; i < s->n_out; i++)
        if (s->out[i].connected) return i;
    return -1;
}

static int rects_overlap(const HdeRandrOutput *a, const HdeRandrOutput *b)
{
    return a->x < b->x + b->width && b->x < a->x + a->width && a->y < b->y + b->height && b->y < a->y + a->height;
}

HdeProjectMode hde_randr_mode_of(const HdeRandrState *s)
{
    int m = hde_randr_main_index(s);
    if (m < 0) return HDE_PROJECT_OTHER;
    int main_on = s->out[m].crtc != 0, others_on = 0;
    for (int i = 0; i < s->n_out; i++)
        if (i != m && s->out[i].connected && s->out[i].crtc) others_on++;
    if (main_on && !others_on) return HDE_PROJECT_PC;
    if (!main_on) return others_on ? HDE_PROJECT_SECOND : HDE_PROJECT_OTHER;
    int same = 1, overlap = 0;
    for (int i = 0; i < s->n_out; i++) {
        const HdeRandrOutput *a = &s->out[i];
        if (!a->connected || !a->crtc) continue;
        for (int j = i + 1; j < s->n_out; j++) {
            const HdeRandrOutput *b = &s->out[j];
            if (!b->connected || !b->crtc) continue;
            if (a->x != b->x || a->y != b->y) same = 0;
            if (rects_overlap(a, b)) overlap = 1;
        }
    }
    if (same) return HDE_PROJECT_DUPLICATE;
    return overlap ? HDE_PROJECT_OTHER : HDE_PROJECT_EXTEND;
}

static int mode_index(const HdeRandrOutput *o, unsigned long id)
{
    for (int i = 0; i < o->nmode; i++)
        if (o->modes[i].id == id) return i;
    return -1;
}

/* The mode of a given size with the highest refresh rate (preferred modes win a tie). */
static int find_size(const HdeRandrOutput *o, int w, int h)
{
    int best = -1;
    for (int i = 0; i < o->nmode; i++) {
        const HdeRandrMode *m = &o->modes[i];
        if (m->width != w || m->height != h) continue;
        if (best < 0 || m->refresh > o->modes[best].refresh + 0.5 ||
            (fabs(m->refresh - o->modes[best].refresh) <= 0.5 && m->preferred && !o->modes[best].preferred))
            best = i;
    }
    return best;
}

/* Native resolution (the preferred mode), else the largest one; keep_current: the mode in use if it is on. */
static int best_mode(const HdeRandrOutput *o, int keep_current)
{
    if (keep_current && o->crtc && o->mode) {
        int i = mode_index(o, o->mode);
        if (i >= 0) return i;
    }
    for (int i = 0; i < o->nmode; i++)
        if (o->modes[i].preferred) return find_size(o, o->modes[i].width, o->modes[i].height);
    int best = -1;
    for (int i = 0; i < o->nmode; i++) {
        long a = (long)o->modes[i].width * o->modes[i].height;
        long b = best < 0 ? -1 : (long)o->modes[best].width * o->modes[best].height;
        if (a > b || (a == b && o->modes[i].refresh > o->modes[best].refresh)) best = i;
    }
    return best;
}

static int rotated(int rot) { return (rot & (HDE_ROT_90 | HDE_ROT_270)) != 0; }

static int crtc_index(const HdeRandrState *s, unsigned long id)
{
    for (int i = 0; i < s->n_crtc; i++)
        if (s->crtc[i].id == id) return i;
    return -1;
}

static int can_drive(const HdeRandrOutput *o, unsigned long crtc)
{
    for (int i = 0; i < o->ncrtc; i++)
        if (o->crtcs[i] == crtc) return 1;
    return 0;
}

/* Give every output that is on a CRTC: its current one when possible, else a free one (preferring CRTCs that are
 * off now). 0 + error when the graphics card has too few. */
static int assign_crtcs(const HdeRandrState *s, const int *on, unsigned long *crtc_of, char *err, size_t errlen)
{
    unsigned long used[HDE_RANDR_MAX];
    int nused = 0;
    for (int i = 0; i < s->n_out; i++) {
        crtc_of[i] = 0;
        const HdeRandrOutput *o = &s->out[i];
        if (!on[i] || !o->crtc || !can_drive(o, o->crtc)) continue;
        int taken = 0;
        for (int k = 0; k < nused; k++) taken |= used[k] == o->crtc;
        if (taken) continue;
        crtc_of[i] = o->crtc;
        used[nused++] = o->crtc;
    }
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < s->n_out; i++) {
            const HdeRandrOutput *o = &s->out[i];
            if (!on[i] || crtc_of[i]) continue;
            for (int c = 0; c < o->ncrtc && !crtc_of[i]; c++) {
                int taken = 0;
                for (int k = 0; k < nused; k++) taken |= used[k] == o->crtcs[c];
                if (taken) continue;
                int ci = crtc_index(s, o->crtcs[c]);
                int busy = ci >= 0 && s->crtc[ci].mode != 0;
                if (pass == 0 && busy) continue;       /* first try the CRTCs that are off now */
                crtc_of[i] = o->crtcs[c];
                used[nused++] = o->crtcs[c];
            }
        }
    }
    int want = 0, got = 0;
    for (int i = 0; i < s->n_out; i++) {
        want += on[i] != 0;
        got += on[i] && crtc_of[i];
    }
    if (got < want) {
        snprintf(err, errlen, "The graphics card cannot drive %d screens at the same time.", want);
        return 0;
    }
    return 1;
}

static void add_target(HdeRandrPlan *p, int out, int on, unsigned long crtc, unsigned long mode, int x, int y, int w,
                       int h, int rot)
{
    if (p->n >= HDE_RANDR_MAX) return;
    HdeRandrTarget *t = &p->t[p->n++];
    t->out = out;
    t->on = on;
    t->crtc = crtc;
    t->mode = mode;
    t->x = x;
    t->y = y;
    t->width = w;
    t->height = h;
    t->rotation = rot;
}

int hde_randr_plan(const HdeRandrState *s, HdeProjectMode mode, HdeRandrPlan *p)
{
    memset(p, 0, sizeof *p);
    p->mode = mode;
    if (mode < 0 || mode >= HDE_PROJECT_N) {
        snprintf(p->error, sizeof p->error, "Unknown layout.");
        return 0;
    }
    int main = hde_randr_main_index(s);
    if (main < 0) {
        snprintf(p->error, sizeof p->error, "No screen is connected.");
        return 0;
    }
    int ord[HDE_RANDR_MAX], no = 0;
    ord[no++] = main;
    for (int i = 0; i < s->n_out; i++)
        if (i != main && s->out[i].connected) ord[no++] = i;
    if (mode != HDE_PROJECT_PC && no < 2) {
        snprintf(p->error, sizeof p->error, "Only one screen is connected. Connect a monitor or a projector "
                 "(HDMI, DisplayPort, USB-C or VGA) first.");
        return 0;
    }
    HdeProjectMode cur = hde_randr_mode_of(s);
    int on[HDE_RANDR_MAX] = { 0 }, mi[HDE_RANDR_MAX], px[HDE_RANDR_MAX] = { 0 }, py[HDE_RANDR_MAX] = { 0 };
    int rot[HDE_RANDR_MAX], ew[HDE_RANDR_MAX] = { 0 }, eh[HDE_RANDR_MAX] = { 0 };
    for (int i = 0; i < HDE_RANDR_MAX; i++) { mi[i] = -1; rot[i] = HDE_ROT_0; }
    if (mode == HDE_PROJECT_PC) on[main] = 1;
    else if (mode == HDE_PROJECT_SECOND) for (int k = 1; k < no; k++) on[ord[k]] = 1;
    else for (int k = 0; k < no; k++) on[ord[k]] = 1;

    for (int k = 0; k < no; k++) {
        const HdeRandrOutput *o = &s->out[ord[k]];
        if (on[ord[k]] && o->crtc && mode != HDE_PROJECT_DUPLICATE) rot[ord[k]] = o->rotation ? o->rotation : HDE_ROT_0;
    }
    if (mode == HDE_PROJECT_DUPLICATE) {
        /* the largest resolution every screen can show, the PC screen's native one first (like Windows) */
        const HdeRandrOutput *m = &s->out[main];
        int cw = 0, ch = 0, found = 0;
        int pref = best_mode(m, 0);
        for (int pass = 0; pass < 2 && !found; pass++) {
            long best_area = -1;
            for (int i = 0; i < m->nmode; i++) {
                if (pass == 0 && i != pref) continue;
                int w = m->modes[i].width, h = m->modes[i].height, all = 1;
                for (int k = 1; k < no && all; k++) all = find_size(&s->out[ord[k]], w, h) >= 0;
                if (all && (long)w * h > best_area) {
                    best_area = (long)w * h;
                    cw = w;
                    ch = h;
                    found = 1;
                }
            }
        }
        for (int k = 0; k < no; k++) {
            const HdeRandrOutput *o = &s->out[ord[k]];
            mi[ord[k]] = found ? find_size(o, cw, ch) : best_mode(o, 0);
        }
    } else {
        int keep = cur != HDE_PROJECT_DUPLICATE;          /* back from Duplicate: native resolutions again */
        for (int k = 0; k < no; k++)
            if (on[ord[k]]) mi[ord[k]] = best_mode(&s->out[ord[k]], keep);
    }
    for (int k = 0; k < no; k++) {
        int i = ord[k];
        if (!on[i]) continue;
        if (mi[i] < 0) {
            snprintf(p->error, sizeof p->error, "%s has no usable resolution.", s->out[i].name);
            return 0;
        }
        const HdeRandrMode *md = &s->out[i].modes[mi[i]];
        ew[i] = rotated(rot[i]) ? md->height : md->width;
        eh[i] = rotated(rot[i]) ? md->width : md->height;
    }

    /* positions */
    int keep_layout = mode == HDE_PROJECT_EXTEND && cur == HDE_PROJECT_EXTEND;   /* an arrangement made earlier */
    if (mode != HDE_PROJECT_DUPLICATE) {
        int x = 0, minx = 1 << 30, miny = 1 << 30, maxx = 0;
        if (keep_layout) {
            for (int k = 0; k < no; k++) {
                int i = ord[k];
                if (!on[i] || !s->out[i].crtc) continue;
                if (s->out[i].x < minx) minx = s->out[i].x;
                if (s->out[i].y < miny) miny = s->out[i].y;
            }
            if (minx == 1 << 30) keep_layout = 0;
        }
        for (int k = 0; k < no; k++) {
            int i = ord[k];
            if (!on[i] || !keep_layout || !s->out[i].crtc) continue;
            px[i] = s->out[i].x - minx;
            py[i] = s->out[i].y - miny;
            if (px[i] + ew[i] > maxx) maxx = px[i] + ew[i];
        }
        x = keep_layout ? maxx : 0;
        for (int k = 0; k < no; k++) {
            int i = ord[k];
            if (!on[i] || (keep_layout && s->out[i].crtc)) continue;
            px[i] = x;
            py[i] = 0;
            x += ew[i];
        }
    }
    int sw = 0, sh = 0;
    for (int k = 0; k < no; k++) {
        int i = ord[k];
        if (!on[i]) continue;
        if (px[i] + ew[i] > sw) sw = px[i] + ew[i];
        if (py[i] + eh[i] > sh) sh = py[i] + eh[i];
    }
    if (s->max_w > 0 && s->max_h > 0 && (sw > s->max_w || sh > s->max_h) && mode != HDE_PROJECT_DUPLICATE) {
        /* too wide for the graphics card: one below the other instead */
        int y = 0;
        sw = sh = 0;
        for (int k = 0; k < no; k++) {
            int i = ord[k];
            if (!on[i]) continue;
            px[i] = 0;
            py[i] = y;
            y += eh[i];
            if (ew[i] > sw) sw = ew[i];
            sh = y;
        }
    }
    if (s->max_w > 0 && s->max_h > 0 && (sw > s->max_w || sh > s->max_h)) {
        snprintf(p->error, sizeof p->error, "The desktop would be %dx%d, larger than this graphics card allows (%dx%d).",
                 sw, sh, s->max_w, s->max_h);
        return 0;
    }
    if (sw < s->min_w) sw = s->min_w;
    if (sh < s->min_h) sh = s->min_h;

    unsigned long crtc_of[HDE_RANDR_MAX];
    if (!assign_crtcs(s, on, crtc_of, p->error, sizeof p->error)) return 0;
    for (int k = 0; k < no; k++) {
        int i = ord[k];
        if (on[i])
            add_target(p, i, 1, crtc_of[i], s->out[i].modes[mi[i]].id, px[i], py[i], ew[i], eh[i], rot[i]);
    }
    for (int i = 0; i < s->n_out; i++)            /* turned off: connected screens left out, and unplugged ones */
        if (!on[i] && s->out[i].crtc) add_target(p, i, 0, 0, 0, 0, 0, 0, 0, HDE_ROT_0);
    p->screen_w = sw;
    p->screen_h = sh;
    p->primary = mode == HDE_PROJECT_SECOND ? s->out[ord[1]].id : s->out[main].id;
    return 1;
}

int hde_randr_plan_restore(const HdeRandrState *now, const HdeRandrState *saved, HdeRandrPlan *p)
{
    memset(p, 0, sizeof *p);
    p->mode = hde_randr_mode_of(saved);
    int on[HDE_RANDR_MAX] = { 0 };
    for (int i = 0; i < now->n_out; i++) {
        const HdeRandrOutput *o = &now->out[i];
        const HdeRandrOutput *was = NULL;
        for (int j = 0; j < saved->n_out; j++)
            if (saved->out[j].id == o->id) was = &saved->out[j];
        if (was && was->crtc && o->connected && mode_index(o, was->mode) >= 0) {
            on[i] = 1;
            add_target(p, i, 1, was->crtc, was->mode, was->x, was->y, was->width, was->height,
                       was->rotation ? was->rotation : HDE_ROT_0);
        }
    }
    int any = 0;
    for (int i = 0; i < now->n_out; i++) any |= on[i];
    if (!any) {
        snprintf(p->error, sizeof p->error, "None of the screens of the previous layout is connected any more.");
        return 0;
    }
    for (int i = 0; i < now->n_out; i++)
        if (!on[i] && now->out[i].crtc) add_target(p, i, 0, 0, 0, 0, 0, 0, 0, HDE_ROT_0);
    int sw = 0, sh = 0;
    for (int k = 0; k < p->n; k++) {
        const HdeRandrTarget *t = &p->t[k];
        if (!t->on) continue;
        if (t->x + t->width > sw) sw = t->x + t->width;
        if (t->y + t->height > sh) sh = t->y + t->height;
    }
    p->screen_w = sw > now->min_w ? sw : now->min_w;
    p->screen_h = sh > now->min_h ? sh : now->min_h;
    p->primary = saved->primary;
    /* two outputs of the old layout may not share a CRTC now (another screen took one meanwhile) */
    for (int a = 0; a < p->n; a++)
        for (int b = a + 1; b < p->n; b++)
            if (p->t[a].on && p->t[b].on && p->t[a].crtc == p->t[b].crtc) {
                snprintf(p->error, sizeof p->error, "The previous layout cannot be restored.");
                return 0;
            }
    return 1;
}

int hde_randr_needs_fix(const HdeRandrState *s, HdeProjectMode *mode, char *why, unsigned long why_len)
{
    int nconn = hde_randr_n_connected(s), conn_on = 0, stale = -1;
    for (int i = 0; i < s->n_out; i++) {
        const HdeRandrOutput *o = &s->out[i];
        if (o->connected && o->crtc) conn_on++;
        if (!o->connected && o->crtc && stale < 0) stale = i;
    }
    if (nconn == 0) return 0;                     /* nothing to show anything on (e.g. a laptop with its lid closed) */
    if (conn_on == 0) {
        *mode = HDE_PROJECT_PC;
        if (why && stale >= 0)
            snprintf(why, why_len, "no connected screen is on (%s was unplugged): turning the PC screen on",
                     s->out[stale].name);
        else if (why)
            snprintf(why, why_len, "no connected screen is on: turning the PC screen on");
        return 1;
    }
    if (stale >= 0) {
        HdeProjectMode m = hde_randr_mode_of(s);
        if (nconn < 2 || m == HDE_PROJECT_OTHER) m = nconn < 2 ? HDE_PROJECT_PC : HDE_PROJECT_EXTEND;
        *mode = m;
        if (why) snprintf(why, why_len, "%s was unplugged but the desktop still covers it: switching to %s",
                          s->out[stale].name, hde_project_label(m));
        return 1;
    }
    return 0;
}

void hde_randr_output_label(const HdeRandrOutput *o, char *buf, unsigned long len)
{
    if (o->builtin) snprintf(buf, len, "Built-in screen");
    else if (o->monitor[0]) snprintf(buf, len, "%s (%s)", o->monitor, o->name);
    else snprintf(buf, len, "%s", o->name);
}

void hde_randr_describe(const HdeRandrState *s, char *buf, unsigned long len)
{
    size_t o = 0;
    int main = hde_randr_main_index(s), any = 0;
    buf[0] = '\0';
    for (int i = 0; i < s->n_out && o < len; i++) {
        const HdeRandrOutput *d = &s->out[i];
        if (!d->connected && !d->crtc) continue;
        int w;
        if (d->crtc)
            w = snprintf(buf + o, len - o, "%s%s %dx%d+%d+%d%s%s%s", any ? ", " : "", d->name, d->width, d->height, d->x,
                         d->y, i == main ? " (PC screen)" : "", d->connected ? "" : " (unplugged)",
                         s->primary == d->id && i != main ? " (primary)" : "");
        else
            w = snprintf(buf + o, len - o, "%s%s off%s", any ? ", " : "", d->name, i == main ? " (PC screen)" : "");
        any = 1;
        if (w < 0) break;
        o += (size_t)w;
    }
    if (o < len) snprintf(buf + o, len - o, "%sscreen %dx%d", any ? "; " : "", s->screen_w, s->screen_h);
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

void hde_randr_connected_names(const HdeRandrState *s, char *buf, unsigned long len)
{
    const char *names[HDE_RANDR_MAX];
    int n = 0;
    for (int i = 0; i < s->n_out; i++)
        if (s->out[i].connected) names[n++] = s->out[i].name;
    qsort(names, (size_t)n, sizeof names[0], cmp_str);
    size_t o = 0;
    buf[0] = '\0';
    for (int i = 0; i < n && o < len; i++) {
        int w = snprintf(buf + o, len - o, "%s%s", i ? "," : "", names[i]);
        if (w < 0) break;
        o += (size_t)w;
    }
}

void hde_randr_temperature_rgb(int kelvin, double *r, double *g, double *b)
{
    *r = *g = *b = 1.0;
    if (kelvin <= 0 || kelvin >= 6500) return;
    if (kelvin < 1000) kelvin = 1000;
    /* black-body approximation (Tanner Helland), normalised so that 6500 K is white */
    double t = kelvin / 100.0, t65 = 65.0;
    double gg = 99.4708025861 * log(t) - 161.1195681661;
    double bb = t <= 19 ? 0 : 138.5177312231 * log(t - 10) - 305.0447927307;
    double g65 = 99.4708025861 * log(t65) - 161.1195681661;
    double b65 = 138.5177312231 * log(t65 - 10) - 305.0447927307;
    *g = gg / g65;
    *b = bb / b65;
    if (*g < 0) *g = 0;
    if (*g > 1) *g = 1;
    if (*b < 0) *b = 0;
    if (*b > 1) *b = 1;
}

/* ======================================================================= X server */
#ifdef HAVE_XRANDR
int hde_randr_supported(void) { return 1; }

int hde_randr_available(Display *dpy)
{
    int ev, er, major = 0, minor = 0;
    if (!dpy || !XRRQueryExtension(dpy, &ev, &er) || !XRRQueryVersion(dpy, &major, &minor)) return 0;
    return major > 1 || (major == 1 && minor >= 2);
}

static int trapped;
static int trap_handler(Display *d, XErrorEvent *e) { (void)d; trapped = e->error_code; return 0; }

static double mode_refresh(const XRRModeInfo *m)
{
    double v = (double)m->hTotal * m->vTotal;
    if (m->modeFlags & RR_DoubleScan) v *= 2;
    if (m->modeFlags & RR_Interlace) v /= 2;
    return v > 0 ? (double)m->dotClock / v : 0;
}

static unsigned char *output_prop(Display *dpy, RROutput o, const char *name, Atom *type, int *fmt, unsigned long *n)
{
    Atom a = XInternAtom(dpy, name, True);
    unsigned char *data = NULL;
    unsigned long after = 0;
    if (a == None) return NULL;
    if (XRRGetOutputProperty(dpy, o, a, 0, 256, False, False, AnyPropertyType, type, fmt, n, &after, &data) != Success)
        return NULL;
    if (!data || *n == 0) {
        if (data) XFree(data);
        return NULL;
    }
    return data;
}

/* monitor name (descriptor 0xFC) of an EDID block */
static void edid_name(const unsigned char *e, unsigned long n, char *out, size_t len)
{
    out[0] = '\0';
    if (n < 128 || e[0] != 0x00 || e[1] != 0xff) return;
    for (int d = 54; d + 18 <= 126; d += 18) {
        const unsigned char *p = e + d;
        if (p[0] || p[1] || p[2] || p[3] != 0xfc) continue;
        char s[14];
        int k = 0;
        for (int i = 5; i < 18 && p[i] != 0x0a && p[i] != 0; i++) s[k++] = (char)(p[i] >= 32 && p[i] < 127 ? p[i] : ' ');
        while (k > 0 && s[k - 1] == ' ') k--;
        s[k] = '\0';
        snprintf(out, len, "%s", s);
        return;
    }
}

int hde_randr_read(Display *dpy, HdeRandrState *s, int probe)
{
    memset(s, 0, sizeof *s);
    if (!hde_randr_available(dpy)) return 0;
    Window root = DefaultRootWindow(dpy);
    trapped = 0;
    int (*old)(Display *, XErrorEvent *) = XSetErrorHandler(trap_handler);
    XRRScreenResources *res = probe ? XRRGetScreenResources(dpy, root) : XRRGetScreenResourcesCurrent(dpy, root);
    if (!res) {
        XSetErrorHandler(old);
        return 0;
    }
    XWindowAttributes ra;
    if (XGetWindowAttributes(dpy, root, &ra)) {
        s->screen_w = ra.width;
        s->screen_h = ra.height;
    }
    s->mm_w = DisplayWidthMM(dpy, DefaultScreen(dpy));
    s->mm_h = DisplayHeightMM(dpy, DefaultScreen(dpy));
    if (!XRRGetScreenSizeRange(dpy, root, &s->min_w, &s->min_h, &s->max_w, &s->max_h)) {
        s->min_w = s->min_h = 0;
        s->max_w = s->max_h = 0;
    }
    s->primary = XRRGetOutputPrimary(dpy, root);
    for (int i = 0; i < res->ncrtc && s->n_crtc < HDE_RANDR_MAX; i++) {
        XRRCrtcInfo *ci = XRRGetCrtcInfo(dpy, res, res->crtcs[i]);
        if (!ci) continue;
        HdeRandrCrtc *c = &s->crtc[s->n_crtc++];
        c->id = res->crtcs[i];
        c->mode = ci->mode;
        c->x = ci->x;
        c->y = ci->y;
        c->width = (int)ci->width;
        c->height = (int)ci->height;
        c->rotation = ci->rotation & 0x0f;
        for (int k = 0; k < ci->noutput && c->noutput < HDE_RANDR_MAX; k++) c->outputs[c->noutput++] = ci->outputs[k];
        c->gamma_size = XRRGetCrtcGammaSize(dpy, c->id);
        XRRFreeCrtcInfo(ci);
    }
    for (int i = 0; i < res->noutput && s->n_out < HDE_RANDR_MAX; i++) {
        XRROutputInfo *oi = XRRGetOutputInfo(dpy, res, res->outputs[i]);
        if (!oi) continue;
        HdeRandrOutput *o = &s->out[s->n_out];
        memset(o, 0, sizeof *o);
        o->id = res->outputs[i];
        snprintf(o->name, sizeof o->name, "%.*s", oi->nameLen, oi->name);
        o->crtc = oi->crtc;
        o->mm_width = (int)oi->mm_width;
        o->mm_height = (int)oi->mm_height;
        for (int k = 0; k < oi->ncrtc && o->ncrtc < HDE_RANDR_MAX; k++) o->crtcs[o->ncrtc++] = oi->crtcs[k];
        for (int k = 0; k < oi->nmode && o->nmode < HDE_RANDR_MAX_MODES; k++) {
            for (int m = 0; m < res->nmode; m++) {
                if (res->modes[m].id != oi->modes[k]) continue;
                HdeRandrMode *md = &o->modes[o->nmode++];
                md->id = res->modes[m].id;
                md->width = (int)res->modes[m].width;
                md->height = (int)res->modes[m].height;
                md->refresh = mode_refresh(&res->modes[m]);
                md->preferred = k < oi->npreferred;
                break;
            }
        }
        /* RR_UnknownConnection: only when it is in use (some VGA outputs cannot tell) */
        o->connected = oi->connection == RR_Connected || (oi->connection == RR_UnknownConnection && oi->crtc);
        Atom type;
        int fmt;
        unsigned long n;
        unsigned char *d = output_prop(dpy, o->id, "non-desktop", &type, &fmt, &n);
        if (d) {
            if (fmt == 32 && ((long *)d)[0]) o->connected = 0;     /* a VR headset is not a screen for the desktop */
            XFree(d);
        }
        o->builtin = hde_randr_name_is_builtin(o->name);
        d = output_prop(dpy, o->id, "ConnectorType", &type, &fmt, &n);
        if (d) {
            if (type == XA_ATOM && fmt == 32) {
                char *an = XGetAtomName(dpy, (Atom)((long *)d)[0]);
                if (an) {
                    if (!strcasecmp(an, "Panel")) o->builtin = 1;
                    XFree(an);
                }
            }
            XFree(d);
        }
        d = output_prop(dpy, o->id, "EDID", &type, &fmt, &n);
        if (d) {
            if (fmt == 8) edid_name(d, n, o->monitor, sizeof o->monitor);
            XFree(d);
        }
        if (o->crtc) {
            for (int c = 0; c < s->n_crtc; c++) {
                if (s->crtc[c].id != o->crtc) continue;
                o->mode = s->crtc[c].mode;
                o->x = s->crtc[c].x;
                o->y = s->crtc[c].y;
                o->width = s->crtc[c].width;
                o->height = s->crtc[c].height;
                o->rotation = s->crtc[c].rotation ? s->crtc[c].rotation : HDE_ROT_0;
            }
            if (!o->mode) o->crtc = 0;             /* assigned to a CRTC that is off */
        }
        XRRFreeOutputInfo(oi);
        s->n_out++;
    }
    XRRFreeScreenResources(res);
    XSync(dpy, False);
    XSetErrorHandler(old);
    return 1;
}

int hde_randr_apply(Display *dpy, const HdeRandrState *s, const HdeRandrPlan *p, char *err, unsigned long err_len)
{
    if (err && err_len) err[0] = '\0';
    if (!hde_randr_available(dpy)) {
        if (err) snprintf(err, err_len, "The X server has no RandR 1.2.");
        return 0;
    }
    Window root = DefaultRootWindow(dpy);
    XRRScreenResources *res = XRRGetScreenResourcesCurrent(dpy, root);
    if (!res) {
        if (err) snprintf(err, err_len, "Could not read the screen resources.");
        return 0;
    }
    trapped = 0;
    int (*old)(Display *, XErrorEvent *) = XSetErrorHandler(trap_handler);
    int ok = 1;
    int keep[HDE_RANDR_MAX] = { 0 };
    XGrabServer(dpy);
    /* 1. turn off every CRTC that changes, goes away, or does not fit into the new desktop size */
    for (int c = 0; c < s->n_crtc; c++) {
        const HdeRandrCrtc *cr = &s->crtc[c];
        if (!cr->mode) continue;
        const HdeRandrTarget *t = NULL;
        for (int k = 0; k < p->n; k++)
            if (p->t[k].on && p->t[k].crtc == cr->id) t = &p->t[k];
        int same = t && t->mode == cr->mode && t->x == cr->x && t->y == cr->y &&
                   (t->rotation ? t->rotation : HDE_ROT_0) == (cr->rotation ? cr->rotation : HDE_ROT_0) &&
                   cr->noutput == 1 && cr->outputs[0] == s->out[t->out].id;
        int fits = cr->x + cr->width <= p->screen_w && cr->y + cr->height <= p->screen_h;
        if (same && fits) {
            keep[c] = 1;
            continue;
        }
        Status st = XRRSetCrtcConfig(dpy, res, cr->id, CurrentTime, 0, 0, None, RR_Rotate_0, NULL, 0);
        if (st != RRSetConfigSuccess) {
            ok = 0;
            if (err) snprintf(err, err_len, "Could not turn off a screen (status %d).", (int)st);
        }
    }
    /* 2. the size of the desktop, keeping its DPI */
    if (ok && (p->screen_w != s->screen_w || p->screen_h != s->screen_h)) {
        double dpi = s->mm_h > 0 && s->screen_h > 0 ? 25.4 * s->screen_h / s->mm_h : 96.0;
        if (dpi < 30 || dpi > 600) dpi = 96.0;
        int mmw = (int)(25.4 * p->screen_w / dpi + 0.5), mmh = (int)(25.4 * p->screen_h / dpi + 0.5);
        XRRSetScreenSize(dpy, root, p->screen_w, p->screen_h, mmw > 0 ? mmw : 1, mmh > 0 ? mmh : 1);
    }
    /* 3. the screens of the new layout */
    for (int k = 0; ok && k < p->n; k++) {
        const HdeRandrTarget *t = &p->t[k];
        if (!t->on) continue;
        int ci = -1;
        for (int c = 0; c < s->n_crtc; c++)
            if (s->crtc[c].id == t->crtc) ci = c;
        if (ci >= 0 && keep[ci]) continue;
        RROutput out = (RROutput)s->out[t->out].id;
        Status st = XRRSetCrtcConfig(dpy, res, (RRCrtc)t->crtc, CurrentTime, t->x, t->y, (RRMode)t->mode,
                                     (Rotation)(t->rotation ? t->rotation : RR_Rotate_0), &out, 1);
        if (st != RRSetConfigSuccess) {
            ok = 0;
            if (err) snprintf(err, err_len, "Could not turn on %s (status %d).", s->out[t->out].name, (int)st);
        }
    }
    if (ok && p->primary) XRRSetOutputPrimary(dpy, root, (RROutput)p->primary);
    XUngrabServer(dpy);
    XSync(dpy, False);
    XSetErrorHandler(old);
    XRRFreeScreenResources(res);
    if (ok && trapped) {
        ok = 0;
        if (err) snprintf(err, err_len, "The X server refused the layout (X error %d).", trapped);
    }
    return ok;
}

int hde_randr_watch(Display *dpy, int *event_base)
{
    int er;
    if (!hde_randr_available(dpy) || !XRRQueryExtension(dpy, event_base, &er)) return 0;
    XRRSelectInput(dpy, DefaultRootWindow(dpy),
                   RRScreenChangeNotifyMask | RRCrtcChangeNotifyMask | RROutputChangeNotifyMask);
    return 1;
}

int hde_randr_is_event(Display *dpy, XEvent *ev, int event_base)
{
    (void)dpy;
    if (event_base < 0) return 0;
    if (ev->type == event_base + RRScreenChangeNotify) {
        XRRUpdateConfiguration(ev);
        return 1;
    }
    return ev->type == event_base + RRNotify;
}

int hde_randr_gamma_screens(Display *dpy)
{
    HdeRandrState *s = calloc(1, sizeof *s);
    int n = 0;
    if (s && hde_randr_read(dpy, s, 0))
        for (int c = 0; c < s->n_crtc; c++) n += s->crtc[c].mode && s->crtc[c].gamma_size > 1;
    free(s);
    return n;
}

int hde_randr_set_gamma(Display *dpy, int percent, int kelvin)
{
    if (!hde_randr_available(dpy)) return 0;
    Window root = DefaultRootWindow(dpy);
    XRRScreenResources *res = XRRGetScreenResourcesCurrent(dpy, root);
    if (!res) return 0;
    if (percent < 1) percent = 1;
    if (percent > 100) percent = 100;
    double fr, fg, fb, k = percent / 100.0;
    hde_randr_temperature_rgb(kelvin, &fr, &fg, &fb);
    trapped = 0;
    int (*old)(Display *, XErrorEvent *) = XSetErrorHandler(trap_handler);
    int n = 0;
    for (int i = 0; i < res->ncrtc; i++) {
        XRRCrtcInfo *ci = XRRGetCrtcInfo(dpy, res, res->crtcs[i]);
        int on = ci && ci->mode != None;
        if (ci) XRRFreeCrtcInfo(ci);
        if (!on) continue;
        int size = XRRGetCrtcGammaSize(dpy, res->crtcs[i]);
        if (size <= 1) continue;
        XRRCrtcGamma *g = XRRAllocGamma(size);
        if (!g) continue;
        for (int j = 0; j < size; j++) {
            double v = (double)j / (size - 1) * k * 65535.0;
            g->red[j] = (unsigned short)(v * fr + 0.5);
            g->green[j] = (unsigned short)(v * fg + 0.5);
            g->blue[j] = (unsigned short)(v * fb + 0.5);
        }
        XRRSetCrtcGamma(dpy, res->crtcs[i], g);
        XRRFreeGamma(g);
        n++;
    }
    XSync(dpy, False);
    XSetErrorHandler(old);
    XRRFreeScreenResources(res);
    return trapped ? 0 : n;
}

#else  /* !HAVE_XRANDR */
int hde_randr_supported(void) { return 0; }
int hde_randr_available(Display *dpy) { (void)dpy; return 0; }
int hde_randr_read(Display *dpy, HdeRandrState *s, int probe) { (void)dpy; (void)probe; memset(s, 0, sizeof *s); return 0; }
int hde_randr_apply(Display *dpy, const HdeRandrState *s, const HdeRandrPlan *p, char *err, unsigned long err_len)
{
    (void)dpy; (void)s; (void)p;
    if (err) snprintf(err, err_len, "HDE was built without libxrandr-dev.");
    return 0;
}
int hde_randr_watch(Display *dpy, int *event_base) { (void)dpy; *event_base = -1; return 0; }
int hde_randr_is_event(Display *dpy, XEvent *ev, int event_base) { (void)dpy; (void)ev; (void)event_base; return 0; }
int hde_randr_gamma_screens(Display *dpy) { (void)dpy; return 0; }
int hde_randr_set_gamma(Display *dpy, int percent, int kelvin) { (void)dpy; (void)percent; (void)kelvin; return 0; }
#endif
