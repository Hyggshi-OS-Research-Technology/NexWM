/* hde-measure.c — measuring the screen and the panel the way the X server really has them. See hde-measure.h. */
#include "hde-measure.h"
#include "hde-wl.h"
#include <string.h>
#include <gdk/gdkx.h>
#include <X11/Xatom.h>
#include "hde-ipc.h"
#ifdef HAVE_XRANDR
#include <X11/extensions/Xrandr.h>
#endif

static Display *xdisplay(void)
{
    GdkDisplay *d = gdk_display_get_default();
    return d && GDK_IS_X11_DISPLAY(d) ? GDK_DISPLAY_XDISPLAY(d) : NULL;
}

/* up to max CARDINALs of property name of window w, from the offset-th on; returns how many were read */
static int get_longs(Display *x, Window w, const char *name, long offset, long *out, int max)
{
    Atom type = None;
    int fmt = 0, got = 0;
    unsigned long n = 0, after = 0;
    unsigned char *data = NULL;
    if (XGetWindowProperty(x, w, XInternAtom(x, name, False), offset, max, False, XA_CARDINAL, &type, &fmt, &n, &after,
                           &data) == Success && data) {
        if (type == XA_CARDINAL && fmt == 32) {
            got = (int)MIN(n, (unsigned long)max);
            memcpy(out, data, (size_t)got * sizeof(long));
        }
        XFree(data);
    }
    return got;
}

#ifdef HAVE_XRANDR
/* The main monitor straight from the X server, the way GTK chooses it (RandR 1.5 monitors: the primary one, else the
 * one GTK means, else the first). GTK learns about a new resolution only when it gets to the event; a panel placed in
 * between would be placed for the old one. */
static gboolean xrandr_main_monitor(Display *x, const GdkRectangle *gtk_px, GdkRectangle *out, char *name, size_t len)
{
    int ev = 0, er = 0, major = 0, minor = 0;
    if (!XRRQueryExtension(x, &ev, &er) || !XRRQueryVersion(x, &major, &minor) || major < 1 || (major == 1 && minor < 5))
        return FALSE;
    int n = 0;
    XRRMonitorInfo *mi = XRRGetMonitors(x, DefaultRootWindow(x), True, &n);
    if (!mi) return FALSE;
    int best = -1;
    for (int i = 0; i < n && best < 0; i++)
        if (mi[i].primary) best = i;
    for (int i = 0; i < n && best < 0; i++)
        if (mi[i].x == gtk_px->x && mi[i].y == gtk_px->y) best = i;
    if (best < 0 && n > 0) best = 0;
    if (best >= 0) {
        *out = (GdkRectangle){ mi[best].x, mi[best].y, mi[best].width, mi[best].height };
        char *an = mi[best].name ? XGetAtomName(x, mi[best].name) : NULL;
        if (an) {
            g_strlcpy(name, an, len);
            XFree(an);
        }
    }
    XRRFreeMonitors(mi);
    return best >= 0 && out->width > 0 && out->height > 0;
}
#endif

void hde_measure_screen(HdeScreen *s)
{
    memset(s, 0, sizeof *s);
    s->scale = 1;
    s->dpi = 96;
    s->source = "GTK";
    GdkDisplay *d = gdk_display_get_default();
    if (!d) return;
    s->n_monitors = gdk_display_get_n_monitors(d);
    GdkMonitor *m = hde_main_monitor();
    if (m) {
        gdk_monitor_get_geometry(m, &s->mon);
        s->scale = MAX(1, gdk_monitor_get_scale_factor(m));
    }
    GtkSettings *gs = gtk_settings_get_default();
    int xdpi = 0;
    if (gs) g_object_get(gs, "gtk-xft-dpi", &xdpi, NULL);
    if (xdpi > 0) s->dpi = (xdpi + 512) / 1024;
    s->mon_px = (GdkRectangle){ s->mon.x * s->scale, s->mon.y * s->scale, s->mon.width * s->scale, s->mon.height * s->scale };
    s->gtk_px = s->mon_px;
    Display *x = xdisplay();
    if (!x) return;
    s->x11 = TRUE;
    Window r = 0;
    int gx = 0, gy = 0;
    unsigned int gw = 0, gh = 0, bw = 0, depth = 0;
    if (XGetGeometry(x, DefaultRootWindow(x), &r, &gx, &gy, &gw, &gh, &bw, &depth)) {
        s->root_w = (int)gw;
        s->root_h = (int)gh;
    }
#ifdef HAVE_XRANDR
    GdkRectangle fresh;
    char name[32] = "";
    if (xrandr_main_monitor(x, &s->gtk_px, &fresh, name, sizeof name)) {
        g_strlcpy(s->output, name, sizeof s->output);
        /* GTK rounds to whole application pixels: a difference below the scale is no difference */
        if (ABS(fresh.x - s->mon_px.x) >= s->scale || ABS(fresh.y - s->mon_px.y) >= s->scale ||
            ABS(fresh.width - s->mon_px.width) >= s->scale || ABS(fresh.height - s->mon_px.height) >= s->scale) {
            s->mon_px = fresh;
            s->source = "XRandR";
        }
    }
#endif
    /* never outside the X screen (always up to date: the X server answers for it) */
    if (s->root_w > 0 && s->root_h > 0) {
        GdkRectangle scr = { 0, 0, s->root_w, s->root_h }, in;
        if (s->mon_px.width <= 0 || s->mon_px.height <= 0 || !gdk_rectangle_intersect(&s->mon_px, &scr, &in)) {
            s->mon_px = scr;
            s->source = "X screen";
        } else if (!gdk_rectangle_equal(&in, &s->mon_px)) {
            s->mon_px = in;
            s->source = "X screen";
        }
    }
    if (strcmp(s->source, "GTK"))
        s->mon = (GdkRectangle){ s->mon_px.x / s->scale, s->mon_px.y / s->scale, s->mon_px.width / s->scale,
                                 s->mon_px.height / s->scale };
}

gboolean hde_measure_panel(unsigned long xid, HdePanelGeo *p)
{
    memset(p, 0, sizeof *p);
    Display *x = xdisplay();
    if (!x) return FALSE;
    GdkDisplay *gd = gdk_display_get_default();
    Window root = DefaultRootWindow(x);
    long cur = 0, wa[4];
    get_longs(x, root, "_NET_CURRENT_DESKTOP", 0, &cur, 1);
    if (get_longs(x, root, "_NET_WORKAREA", 4 * MAX(0, cur), wa, 4) == 4 ||
        get_longs(x, root, "_NET_WORKAREA", 0, wa, 4) == 4) {
        p->work = (GdkRectangle){ (int)wa[0], (int)wa[1], (int)wa[2], (int)wa[3] };
        p->have_work = wa[2] > 0 && wa[3] > 0;
    }
    if (!xid) xid = hde_ipc_find_panel(x);
    if (!xid) return FALSE;
    gdk_x11_display_error_trap_push(gd);
    XWindowAttributes a;
    if (XGetWindowAttributes(x, (Window)xid, &a)) {
        Window child = 0;
        int rx = 0, ry = 0;
        if (XTranslateCoordinates(x, (Window)xid, root, 0, 0, &rx, &ry, &child)) {
            p->win = (GdkRectangle){ rx, ry, a.width, a.height };
            p->mapped = a.map_state == IsViewable;
            p->found = TRUE;
            p->have_strut = get_longs(x, (Window)xid, "_NET_WM_STRUT_PARTIAL", 0, p->strut, 12) == 12;
        }
    }
    gdk_x11_display_error_trap_pop_ignored(gd);
    return p->found;
}

GdkRectangle hde_measure_panel_rect(const HdeScreen *s, gboolean top, int height_px)
{
    GdkRectangle r = { s->mon_px.x, top ? s->mon_px.y : s->mon_px.y + s->mon_px.height - height_px, s->mon_px.width,
                       height_px };
    return r;
}

void hde_measure_strut(const HdeScreen *s, gboolean top, const GdkRectangle *r, long st[12])
{
    memset(st, 0, 12 * sizeof st[0]);
    int root_h = s->root_h > 0 ? s->root_h : s->mon_px.y + s->mon_px.height;
    if (top) {
        st[2] = MAX(0, r->y + r->height);         /* from the top of the X screen to the bottom of the panel */
        st[8] = r->x;
        st[9] = r->x + r->width - 1;
    } else {
        st[3] = MAX(0, root_h - r->y);            /* from the top of the panel to the bottom of the X screen */
        st[10] = r->x;
        st[11] = r->x + r->width - 1;
    }
}

long hde_measure_strut_size(const long st[12], gboolean top)
{
    return top ? st[2] : st[3];
}

static void add_why(GString *why, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static void add_why(GString *why, const char *fmt, ...)
{
    if (!why) return;
    if (why->len) g_string_append(why, "; ");
    va_list ap;
    va_start(ap, fmt);
    g_string_append_vprintf(why, fmt, ap);
    va_end(ap);
}

int hde_measure_check(const HdeScreen *s, const HdePanelGeo *p, gboolean top, int size, GString *why)
{
    if (!p->found || !p->mapped) return 0;
    int bad = 0;
    const GdkRectangle *w = &p->win, *m = &s->mon_px;
    GdkRectangle in;
    if (!gdk_rectangle_intersect(w, m, &in) || !gdk_rectangle_equal(&in, w)) {
        bad |= HDE_FIT_OFF_SCREEN;
        int below = w->y + w->height - (m->y + m->height), above = m->y - w->y;
        if (below > 0) add_why(why, "%d px of the panel are below the bottom edge of the screen", MIN(below, w->height));
        if (above > 0) add_why(why, "%d px of the panel are above the top edge of the screen", MIN(above, w->height));
        if (below <= 0 && above <= 0)
            add_why(why, "the panel sticks out at the side of the screen (%d px wide from x = %d, the screen %d px from x = %d)",
                    w->width, w->x, m->width, m->x);
    } else if (top ? w->y != m->y : w->y + w->height != m->y + m->height) {
        bad |= HDE_FIT_NOT_AT_EDGE;
        add_why(why, "the panel is %d px away from the %s edge of the screen",
                top ? w->y - m->y : m->y + m->height - (w->y + w->height), top ? "top" : "bottom");
    }
    if (w->height != size * s->scale) {
        bad |= HDE_FIT_HEIGHT;
        add_why(why, "the panel is %d px high instead of %d", w->height, size * s->scale);
    }
    long need[12];
    hde_measure_strut(s, top, w, need);
    if (!p->have_strut) {
        bad |= HDE_FIT_STRUT;
        add_why(why, "no space is reserved for the panel");
    } else if (hde_measure_strut_size(p->strut, top) < hde_measure_strut_size(need, top)) {
        bad |= HDE_FIT_STRUT;
        add_why(why, "only %ld px are reserved for the panel, it needs %ld", hde_measure_strut_size(p->strut, top),
                hde_measure_strut_size(need, top));
    }
    if (p->have_work && gdk_rectangle_intersect(&p->work, w, NULL)) {
        bad |= HDE_FIT_WORKAREA;
        add_why(why, "maximized windows can cover the panel: the window manager gives them %dx%d at %d,%d",
                p->work.width, p->work.height, p->work.x, p->work.y);
    }
    return bad;
}

char *hde_measure_screen_text(const HdeScreen *s, const char *times)
{
    GString *t = g_string_new(NULL);
    /* "1920x1080" in logs, "1920 × 1080" in Settings */
    g_string_append_printf(t, strcmp(times, "x") ? "%d %s %d" : "%d%s%d", s->mon_px.width, times, s->mon_px.height);
    GPtrArray *parts = g_ptr_array_new_with_free_func(g_free);
    if (*s->output) g_ptr_array_add(parts, g_strdup(s->output));
    if (s->scale > 1) g_ptr_array_add(parts, g_strdup_printf("scale %d", s->scale));
    if (s->dpi != 96) g_ptr_array_add(parts, g_strdup_printf("text %d %%", (s->dpi * 100 + 48) / 96));
    if (s->n_monitors > 1) g_ptr_array_add(parts, g_strdup_printf("the main one of %d screens", s->n_monitors));
    if (parts->len) {
        g_ptr_array_add(parts, NULL);
        char *j = g_strjoinv(", ", (char **)parts->pdata);
        g_string_append_printf(t, " (%s)", j);
        g_free(j);
    }
    g_ptr_array_free(parts, TRUE);
    return g_string_free(t, FALSE);
}

char *hde_measure_report(const HdeScreen *s, const HdePanelGeo *p, gboolean top, int size)
{
    GString *t = g_string_new(NULL);
    g_string_append_printf(t, "Screen:      %dx%d at %d,%d%s%s%s (from %s", s->mon_px.width, s->mon_px.height, s->mon_px.x,
                           s->mon_px.y, *s->output ? " (" : "", s->output, *s->output ? ")" : "", s->source);
    if (strcmp(s->source, "GTK"))
        g_string_append_printf(t, "; GTK said %dx%d at %d,%d", s->gtk_px.width, s->gtk_px.height, s->gtk_px.x, s->gtk_px.y);
    g_string_append_printf(t, ")\n             X screen %dx%d, %d screen(s), scale %d, text %d dpi (%d %%)\n", s->root_w,
                           s->root_h, s->n_monitors, s->scale, s->dpi, (s->dpi * 100 + 48) / 96);
    g_string_append_printf(t, "Panel:       %s, %d px (Settings > Panel)", top ? "top" : "bottom", size);
    if (s->scale > 1) g_string_append_printf(t, " = %d device px", size * s->scale);
    g_string_append_c(t, '\n');
    if (!p->found) {
        g_string_append(t, "  window:    none (hde-panel is not running)\n");
    } else {
        g_string_append_printf(t, "  window:    %dx%d at %d,%d%s\n", p->win.width, p->win.height, p->win.x, p->win.y,
                               p->mapped ? "" : " (not shown)");
        if (p->have_strut)
            g_string_append_printf(t, "  reserved:  %ld px at the %s of the X screen (_NET_WM_STRUT_PARTIAL)\n",
                                   hde_measure_strut_size(p->strut, top), top ? "top" : "bottom");
        else
            g_string_append(t, "  reserved:  nothing (no _NET_WM_STRUT_PARTIAL)\n");
    }
    if (p->have_work)
        g_string_append_printf(t, "Windows get: %dx%d at %d,%d (_NET_WORKAREA)\n", p->work.width, p->work.height, p->work.x,
                               p->work.y);
    else
        g_string_append(t, "Windows get: unknown (the window manager publishes no _NET_WORKAREA)\n");
    if (p->found) {
        GString *why = g_string_new(NULL);
        int bad = hde_measure_check(s, p, top, size, why);
        if (!bad) g_string_append(t, "Result:      fits: all of the panel is on the screen and windows keep clear of it\n");
        else g_string_append_printf(t, "Result:      does not fit: %s\n", why->str);
        g_string_free(why, TRUE);
    }
    return g_string_free(t, FALSE);
}
