/* Hyggshi Settings — About: HDE (version, build), this computer, the system, and how much memory (RAM) the
 * desktop uses right now, measured live (src/hde-sysinfo.c). `hde-settings --about` prints the same as text. */
#include "hde-settings.h"
#include "hde-sysinfo.h"
#include "hde-randr.h"
#include "hde-theme.h"
#include "hde-build.h"
#include <gdk/gdkx.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <string.h>

#define PROJECT_URL "https://github.com/Hyggshi-OS-Research-Technology/NexWM"

static GtkWidget *mem_card, *mem_title, *mem_desc, *mem_bar;
static guint mem_timer;

/* ---------------------------------------------------------------- facts that need the X server */
static char *wm_name(Display *d)
{
    Atom type;
    int fmt;
    unsigned long n, after;
    unsigned char *data = NULL;
    Window check = None;
    char *res = NULL;
    Window root = DefaultRootWindow(d);
    if (XGetWindowProperty(d, root, XInternAtom(d, "_NET_SUPPORTING_WM_CHECK", False), 0, 1, False, XA_WINDOW, &type,
                           &fmt, &n, &after, &data) == Success && data) {
        if (n == 1 && fmt == 32) check = *(Window *)data;
        XFree(data);
        data = NULL;
    }
    if (check != None &&
        XGetWindowProperty(d, check, XInternAtom(d, "_NET_WM_NAME", False), 0, 64, False,
                           XInternAtom(d, "UTF8_STRING", False), &type, &fmt, &n, &after, &data) == Success && data) {
        if (n > 0 && fmt == 8) res = g_strndup((const char *)data, n);
        XFree(data);
    }
    return res;
}

static char *xserver(Display *d)
{
    const char *vendor = ServerVendor(d);
    int v = VendorRelease(d);
    if (vendor && (strstr(vendor, "X.Org") || strstr(vendor, "X11Libre")) && v > 10000000) {
        int maj = v / 10000000, min = (v / 100000) % 100, pat = (v / 1000) % 100, snap = v % 1000;
        return snap ? g_strdup_printf("X11 (%s %d.%d.%d.%d)", strstr(vendor, "X11Libre") ? "X11Libre" : "X.Org", maj, min, pat, snap)
                    : g_strdup_printf("X11 (%s %d.%d.%d)", strstr(vendor, "X11Libre") ? "X11Libre" : "X.Org", maj, min, pat);
    }
    return g_strdup_printf("X11 (%s %d)", vendor ? vendor : "?", v);
}

static char *screens_text(Display *d)
{
    HdeRandrState *s = g_new0(HdeRandrState, 1);
    GString *g = g_string_new(NULL);
    if (d && hde_randr_read(d, s, 0)) {
        for (int i = 0; i < s->n_out; i++) {
            const HdeRandrOutput *o = &s->out[i];
            if (!o->connected) continue;
            char l[128];
            hde_randr_output_label(o, l, sizeof l);
            if (o->crtc) g_string_append_printf(g, "%s%d × %d (%s)", g->len ? " + " : "", o->width, o->height, l);
            else g_string_append_printf(g, "%s%s (off)", g->len ? " + " : "", l);
        }
    }
    if (!g->len) {
        GdkDisplay *gd = gdk_display_get_default();
        for (int i = 0; gd && i < gdk_display_get_n_monitors(gd); i++) {
            GdkRectangle r;
            gdk_monitor_get_geometry(gdk_display_get_monitor(gd, i), &r);
            g_string_append_printf(g, "%s%d × %d", g->len ? " + " : "", r.width, r.height);
        }
    }
    g_free(s);
    return g_string_free(g, FALSE);
}

static char *uptime_text(double sec)
{
    long m = (long)(sec / 60), h = m / 60, dd = h / 24;
    if (dd > 0) return g_strdup_printf("%ld day%s %ld h", dd, dd == 1 ? "" : "s", h % 24);
    if (h > 0) return g_strdup_printf("%ld h %ld min", h, m % 60);
    return g_strdup_printf("%ld min", m);
}

/* lines for hde_sysinfo_report(): window manager, display server, screens, session */
static char *extra_lines(Display *d, gboolean with_gtk)
{
    GString *g = g_string_new(NULL);
    if (d) {
        char *wm = wm_name(d), *xs = xserver(d), *sc = screens_text(d);
        g_string_append_printf(g, "Window manager: %s\n", wm ? wm : "none running");
        g_string_append_printf(g, "Display server: %s\n", xs);
        if (*sc) g_string_append_printf(g, "Screens: %s\n", sc);
        g_free(wm); g_free(xs); g_free(sc);
    }
    if (with_gtk) g_string_append_printf(g, "GTK: %u.%u.%u\n", gtk_get_major_version(), gtk_get_minor_version(),
                                         gtk_get_micro_version());
    g_string_append_printf(g, "Session: %s\n", g_getenv("HDE_SESSION_PID") ? "HDE (X11)" : "not an HDE session");
    return g_string_free(g, FALSE);
}

int about_cli(void)
{
    hde_sysinfo_exclude_self(TRUE);
    HdeSysInfo si;
    hde_sysinfo_load(&si);
    Display *d = XOpenDisplay(NULL);
    char *extra = extra_lines(d, FALSE);
    char *r = hde_sysinfo_report(&si, extra);
    printf("%s", r);
    g_free(r);
    g_free(extra);
    if (d) XCloseDisplay(d);
    hde_sysinfo_clear(&si);
    return 0;
}

/* ---------------------------------------------------------------- the page */
static Display *xdisplay(void)
{
    GdkDisplay *d = gdk_display_get_default();
    return d && GDK_IS_X11_DISPLAY(d) ? GDK_DISPLAY_XDISPLAY(d) : NULL;
}

static gboolean draw_logo(GtkWidget *w, cairo_t *cr, gpointer data)
{
    (void)data;
    int W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
    double s = MIN(W, H), x = (W - s) / 2, y = (H - s) / 2, r = s * 0.24;
    HdeThemeInfo ti;
    hde_theme_info_load(&ti);
    GdkRGBA a;
    if (!gdk_rgba_parse(&a, ti.accent)) gdk_rgba_parse(&a, "#3584e4");
    hde_theme_info_clear(&ti);
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + s - r, y + r, r, -G_PI / 2, 0);
    cairo_arc(cr, x + s - r, y + s - r, r, 0, G_PI / 2);
    cairo_arc(cr, x + r, y + s - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
    cairo_pattern_t *g = cairo_pattern_create_linear(x, y, x + s, y + s);
    cairo_pattern_add_color_stop_rgb(g, 0, MIN(1, a.red * 1.25 + 0.08), MIN(1, a.green * 1.25 + 0.08), MIN(1, a.blue * 1.25 + 0.08));
    cairo_pattern_add_color_stop_rgb(g, 1, a.red * 0.55, a.green * 0.55, a.blue * 0.65);
    cairo_set_source(cr, g);
    cairo_fill(cr);
    cairo_pattern_destroy(g);
    /* a window with a title bar, and the H */
    cairo_set_source_rgba(cr, 1, 1, 1, 0.22);
    cairo_rectangle(cr, x + s * 0.16, y + s * 0.18, s * 0.68, s * 0.10);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.95);
    double lw = s * 0.12, top = y + s * 0.36, bot = y + s * 0.82;
    cairo_rectangle(cr, x + s * 0.26, top, lw, bot - top);
    cairo_rectangle(cr, x + s * 0.74 - lw, top, lw, bot - top);
    cairo_rectangle(cr, x + s * 0.26, (top + bot) / 2 - lw / 2, s * 0.48, lw);
    cairo_fill(cr);
    return FALSE;
}

static void add_fact(GtkWidget *card, const char *title, const char *value)
{
    GtkWidget *v = gtk_label_new(value && *value ? value : "Unknown");
    gtk_label_set_selectable(GTK_LABEL(v), TRUE);
    gtk_label_set_line_wrap(GTK_LABEL(v), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(v), 52);
    gtk_label_set_xalign(GTK_LABEL(v), 1);
    gtk_widget_set_can_focus(v, FALSE);
    gtk_container_add(GTK_CONTAINER(card), row_box(title, NULL, v));
}

static void mem_refresh(void)
{
    if (!mem_card) return;
    HdeProcMem p[48];
    guint64 pss = 0, rss = 0;
    int n = hde_sysinfo_desktop_memory(p, G_N_ELEMENTS(p), &pss, &rss);
    HdeSysInfo si;
    memset(&si, 0, sizeof si);
    char *mi = NULL;
    if (g_file_get_contents("/proc/meminfo", &mi, NULL, NULL)) {
        const char *t = strstr(mi, "MemTotal:"), *a = strstr(mi, "MemAvailable:");
        if (t) si.mem_total = g_ascii_strtoull(t + 9, NULL, 10) * 1024;
        if (a) si.mem_available = g_ascii_strtoull(a + 13, NULL, 10) * 1024;
        g_free(mi);
    }
    char *ps = hde_format_bytes(pss), *tot = hde_format_bytes(si.mem_total);
    char *used = hde_format_bytes(si.mem_total > si.mem_available ? si.mem_total - si.mem_available : 0);
    char *t = g_strdup_printf("HDE uses %s of RAM right now", ps);
    gtk_label_set_text(GTK_LABEL(mem_title), t);
    g_free(t);
    double frac = si.mem_total ? (double)pss / si.mem_total : 0;
    char *d = g_strdup_printf("%.1f%% of this computer's %s, for the panel, desktop, window manager and HDE's services "
                              "(%d processes; libraries they share are counted once). Everything running uses %s.",
                              frac * 100, tot, n, used);
    gtk_label_set_text(GTK_LABEL(mem_desc), d);
    g_free(d);
    gtk_level_bar_set_value(GTK_LEVEL_BAR(mem_bar), MIN(1.0, frac * 4));    /* full bar = a quarter of the RAM */
    /* one row per process, under the summary row */
    GList *rows = gtk_container_get_children(GTK_CONTAINER(mem_card));
    int k = 0;
    for (GList *l = rows; l; l = l->next, k++)
        if (k > 0) gtk_widget_destroy(l->data);
    g_list_free(rows);
    for (int i = 0; i < n; i++) {
        char *b = hde_format_bytes(p[i].pss), *desc = g_strdup_printf("%s · process %d", p[i].name, p[i].pid);
        GtkWidget *v = gtk_label_new(b);
        gtk_container_add(GTK_CONTAINER(mem_card), row_box(p[i].role, desc, v));
        g_free(b);
        g_free(desc);
    }
    if (n == 0) gtk_container_add(GTK_CONTAINER(mem_card), card_placeholder("No HDE process found (not an HDE session?)"));
    gtk_widget_show_all(mem_card);
    static gboolean logged;
    if (!logged && g_getenv("HDE_DEBUG")) {
        logged = TRUE;
        GString *g = g_string_new(NULL);
        for (int i = 0; i < n; i++) {
            char *b = hde_format_bytes(p[i].pss);
            g_string_append_printf(g, "%s%s %s", i ? ", " : "", p[i].name, b);
            g_free(b);
        }
        fprintf(stderr, "hde-settings: about: HDE uses %s (%d processes: %s); system %s of %s in use\n", ps, n, g->str,
                used, tot);
        g_string_free(g, TRUE);
    }
    g_free(ps); g_free(tot); g_free(used);
}

static gboolean mem_tick(gpointer d) { (void)d; mem_refresh(); return G_SOURCE_CONTINUE; }

static void on_about_map(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    mem_refresh();
    if (!mem_timer) mem_timer = g_timeout_add_seconds(3, mem_tick, NULL);
}

static void on_about_unmap(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    if (mem_timer) g_source_remove(mem_timer);
    mem_timer = 0;
}

static void on_about_destroy(GtkWidget *w, gpointer d)
{
    on_about_unmap(w, d);
    mem_card = mem_title = mem_desc = mem_bar = NULL;
}

static void on_copy(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    HdeSysInfo si;
    hde_sysinfo_load(&si);
    char *extra = extra_lines(xdisplay(), TRUE);
    char *r = hde_sysinfo_report(&si, extra);
    gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), r, -1);
    gtk_clipboard_store(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD));
    settings_status("System information copied to the clipboard");
    g_free(r);
    g_free(extra);
    hde_sysinfo_clear(&si);
}

static void open_uri(const char *uri)
{
    GError *e = NULL;
    if (!gtk_show_uri_on_window(GTK_WINDOW(settings_window()), uri, GDK_CURRENT_TIME, &e)) {
        settings_status("Could not open %s: %s", uri, e ? e->message : "?");
        g_clear_error(&e);
    }
}

static void on_project(GtkButton *b, gpointer d) { (void)b; (void)d; open_uri(PROJECT_URL); }

static void on_log(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    char *p = g_build_filename(g_get_user_cache_dir(), "hde", "session.log", NULL);
    if (!g_file_test(p, G_FILE_TEST_EXISTS)) settings_status("No session log yet (%s)", p);
    else {
        char *u = g_filename_to_uri(p, NULL, NULL);
        if (u) open_uri(u);
        g_free(u);
    }
    g_free(p);
}

GtkWidget *page_about_new(void)
{
    GtkWidget *box = page_base();
    HdeSysInfo si;
    hde_sysinfo_load(&si);
    Display *dpy = xdisplay();

    GtkWidget *hero = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 22);
    gtk_widget_set_margin_top(hero, 6);
    gtk_widget_set_margin_bottom(hero, 4);
    GtkWidget *logo = gtk_drawing_area_new();
    gtk_widget_set_size_request(logo, 104, 104);
    gtk_widget_set_valign(logo, GTK_ALIGN_START);
    g_signal_connect(logo, "draw", G_CALLBACK(draw_logo), NULL);
    gtk_box_pack_start(GTK_BOX(hero), logo, FALSE, FALSE, 0);
    GtkWidget *txt = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *title = gtk_label_new("Hyggshi Desktop Environment");
    gtk_style_context_add_class(gtk_widget_get_style_context(title), "about-title");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    char *ver = g_strdup_printf("Version %s  ·  Build %s", HDE_RELEASE, HDE_VERSION);
    GtkWidget *vl = gtk_label_new(ver);
    g_free(ver);
    gtk_label_set_selectable(GTK_LABEL(vl), TRUE);
    gtk_widget_set_can_focus(vl, FALSE);
    gtk_style_context_add_class(gtk_widget_get_style_context(vl), "page-description");
    gtk_widget_set_halign(vl, GTK_ALIGN_START);
    GtkWidget *tag = gtk_label_new("A light and fast desktop for Hyggshi OS — GTK 3 on X11, with any GTK window manager.");
    gtk_label_set_line_wrap(GTK_LABEL(tag), TRUE);
    gtk_label_set_xalign(GTK_LABEL(tag), 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(tag), "row-description");
    GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(btns, 8);
    GtkWidget *copy = gtk_button_new_with_mnemonic("_Copy system info");
    GtkWidget *proj = gtk_button_new_with_mnemonic("_Project page");
    GtkWidget *log = gtk_button_new_with_mnemonic("Session _log");
    gtk_widget_set_tooltip_text(copy, "Copies everything on this page as text, e.g. for a bug report");
    gtk_widget_set_tooltip_text(log, "~/.cache/hde/session.log: what HDE did since you logged in");
    g_signal_connect(copy, "clicked", G_CALLBACK(on_copy), NULL);
    g_signal_connect(proj, "clicked", G_CALLBACK(on_project), NULL);
    g_signal_connect(log, "clicked", G_CALLBACK(on_log), NULL);
    gtk_box_pack_start(GTK_BOX(btns), copy, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btns), proj, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btns), log, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(txt), title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(txt), vl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(txt), tag, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(txt), btns, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hero), txt, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), hero, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), section("This computer"), FALSE, FALSE, 0);
    GtkWidget *pc = card_new();
    char *cpu = g_strdup_printf("%s (%d threads)", si.cpu ? si.cpu : "Unknown", si.cpu_threads);
    char *mt = hde_format_bytes(si.mem_total), *mu = hde_format_bytes(si.mem_total - si.mem_available);
    char *mem = g_strdup_printf("%s (%s in use)", mt, mu);
    char *dt = hde_format_bytes(si.disk_total), *df = hde_format_bytes(si.disk_free);
    char *disk = g_strdup_printf("%s (%s free)", dt, df);
    char *sc = screens_text(dpy);
    add_fact(pc, "Device name", si.hostname);
    add_fact(pc, "Model", si.model);
    add_fact(pc, "Processor", cpu);
    add_fact(pc, "Memory (RAM)", mem);
    add_fact(pc, "Graphics", si.gpu);
    add_fact(pc, "Storage", disk);
    add_fact(pc, "Screens", sc);
    gtk_box_pack_start(GTK_BOX(box), pc, FALSE, FALSE, 0);
    g_free(cpu); g_free(mt); g_free(mu); g_free(mem); g_free(dt); g_free(df); g_free(disk); g_free(sc);

    gtk_box_pack_start(GTK_BOX(box), section("Software"), FALSE, FALSE, 0);
    GtkWidget *sw = card_new();
    char *kern = g_strdup_printf("%s (%s)", si.kernel ? si.kernel : "?", si.arch ? si.arch : "?");
    char *wm = dpy ? wm_name(dpy) : NULL, *xs = dpy ? xserver(dpy) : g_strdup("Not X11");
    char *gtkv = g_strdup_printf("%u.%u.%u", gtk_get_major_version(), gtk_get_minor_version(), gtk_get_micro_version());
    char *desk = g_strdup_printf("HDE %s (build %s)%s", HDE_RELEASE, HDE_VERSION,
                                 g_getenv("HDE_SESSION_PID") ? "" : " — not running as the session");
    char *up = uptime_text(si.uptime);
    add_fact(sw, "Operating system", si.os);
    add_fact(sw, "Kernel", kern);
    add_fact(sw, "Desktop", desk);
    add_fact(sw, "Window manager", wm ? wm : "None running");
    add_fact(sw, "Display server", xs);
    add_fact(sw, "GTK", gtkv);
    add_fact(sw, "Running for", up);
    gtk_box_pack_start(GTK_BOX(box), sw, FALSE, FALSE, 0);
    g_free(kern); g_free(wm); g_free(xs); g_free(gtkv); g_free(desk); g_free(up);

    gtk_box_pack_start(GTK_BOX(box), section("Memory used by the desktop"), FALSE, FALSE, 0);
    mem_card = card_new();
    GtkWidget *summary = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    gtk_widget_set_margin_top(summary, 10);
    gtk_widget_set_margin_bottom(summary, 10);
    gtk_widget_set_margin_start(summary, 6);
    gtk_widget_set_margin_end(summary, 6);
    GtkWidget *sl = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    mem_title = gtk_label_new("");
    gtk_style_context_add_class(gtk_widget_get_style_context(mem_title), "mem-big");
    gtk_widget_set_halign(mem_title, GTK_ALIGN_START);
    mem_desc = gtk_label_new("");
    gtk_label_set_line_wrap(GTK_LABEL(mem_desc), TRUE);
    gtk_label_set_xalign(GTK_LABEL(mem_desc), 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(mem_desc), "row-description");
    gtk_box_pack_start(GTK_BOX(sl), mem_title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sl), mem_desc, FALSE, FALSE, 0);
    mem_bar = gtk_level_bar_new_for_interval(0, 1);
    gtk_widget_set_size_request(mem_bar, 150, 10);
    gtk_widget_set_valign(mem_bar, GTK_ALIGN_CENTER);
    gtk_level_bar_remove_offset_value(GTK_LEVEL_BAR(mem_bar), GTK_LEVEL_BAR_OFFSET_LOW);
    gtk_level_bar_remove_offset_value(GTK_LEVEL_BAR(mem_bar), GTK_LEVEL_BAR_OFFSET_HIGH);
    gtk_level_bar_remove_offset_value(GTK_LEVEL_BAR(mem_bar), "full");
    gtk_box_pack_start(GTK_BOX(summary), sl, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(summary), mem_bar, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(mem_card), summary);
    gtk_box_pack_start(GTK_BOX(box), mem_card, FALSE, FALSE, 0);
    debug_geometry_watch(mem_card, "about-memory");

    gtk_box_pack_start(GTK_BOX(box), section("How much RAM is needed?"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), info_label(
        "HDE itself needs only what is shown above. For the whole system, Debian asks for at least 1 GB of RAM for a "
        "desktop and recommends 2 GB (with swap; a live USB stick needs more, it runs from RAM). Applications come on "
        "top: a web browser alone often takes several hundred MB to a few GB depending on the tabs, so 4 GB or more is "
        "best for everyday browsing. Give a virtual machine at least 2 GB."), FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), section("Credits"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), info_label("Made by HyggshiOSDeveloper and contributors. Free software under the "
                                                "MIT License. Configuration: ~/.config/hde/settings.ini"),
                       FALSE, FALSE, 0);

    g_signal_connect(box, "map", G_CALLBACK(on_about_map), NULL);
    g_signal_connect(box, "unmap", G_CALLBACK(on_about_unmap), NULL);
    g_signal_connect(box, "destroy", G_CALLBACK(on_about_destroy), NULL);
    hde_sysinfo_clear(&si);
    return box;
}
