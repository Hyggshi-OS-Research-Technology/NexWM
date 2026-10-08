/* Hyggshi Settings — About: the system HDE runs on with its logo (ID / ID_LIKE / LOGO of /etc/os-release:
 * Hyggshi OS, Ubuntu, Debian, Linux Mint, ...; see src/hde-osinfo.h) and the base it is built on, HDE (version,
 * build), this computer, and how much memory (RAM) the desktop uses right now, measured live (src/hde-sysinfo.c).
 * `hde-settings --about` prints the same as text; `hde-settings --about-window` is the small "About HDE" window of
 * the desktop menu. */
#include "hde-distro.h"
#include "hde-settings.h"
#include "hde-sysinfo.h"
#include "hde-randr.h"
#include "hde-theme.h"
#include "hde-build.h"
#include "hde-osinfo.h"
#include "hde-wl.h"
#include <gdk/gdkx.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

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

/* Wayland: "Wayland (labwc 0.8.4)" — labwc gives the programs it starts LABWC_PID */
static char *wayland_server(void)
{
    const char *desk = g_getenv("XDG_SESSION_DESKTOP");
    if (g_getenv("LABWC_PID")) {
        char *out = NULL, *res = NULL;
        const char *argv[] = { "labwc", "--version", NULL };
        if (g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &out,
                         NULL, NULL, NULL) && out && g_str_has_prefix(out, "labwc ")) {
            char *v = g_strndup(out + 6, strcspn(out + 6, " \n"));
            res = g_strdup_printf("Wayland (labwc %s)", v);
            g_free(v);
        }
        g_free(out);
        return res ? res : g_strdup("Wayland (labwc)");
    }
    return g_strdup_printf("Wayland%s%s%s", desk ? " (" : "", desk ? desk : "", desk ? ")" : "");
}

static char *base_line(void)
{
    HdeOsInfo os;
    hde_os_info_load(&os);
    char *b = hde_os_base_text(&os, NULL);
    hde_os_info_clear(&os);
    return b;
}

/* lines for hde_sysinfo_report(): base system, window manager, display server, screens, session */
static char *extra_lines(Display *d, gboolean with_gtk)
{
    GString *g = g_string_new(NULL);
    char *base = base_line();
    if (base) g_string_append_printf(g, "Based on: %s\n", base);
    g_free(base);
    if (!d && (g_getenv("WAYLAND_DISPLAY") || hde_is_wayland())) {
        char *ws = wayland_server();
        g_string_append_printf(g, "Window manager: %s\n", g_getenv("LABWC_PID") ? "labwc (Wayland compositor)" : "the Wayland compositor");
        g_string_append_printf(g, "Display server: %s\n", ws);
        g_free(ws);
    }
    if (d) {
        char *wm = wm_name(d), *xs = xserver(d), *sc = screens_text(d);
        g_string_append_printf(g, "Window manager: %s\n", wm ? wm : "none running");
        g_string_append_printf(g, "Display server: %s\n", xs);
        if (*sc) g_string_append_printf(g, "Screens: %s\n", sc);
        g_free(wm); g_free(xs); g_free(sc);
    }
    if (with_gtk) g_string_append_printf(g, "GTK: %u.%u.%u\n", gtk_get_major_version(), gtk_get_minor_version(),
                                         gtk_get_micro_version());
    g_string_append_printf(g, "Session: %s\n", !g_getenv("HDE_SESSION_PID") ? "not an HDE session"
                                               : g_getenv("WAYLAND_DISPLAY") && !d ? "HDE (Wayland)" : "HDE (X11)");
    return g_string_free(g, FALSE);
}

int about_cli(void)
{
    hde_sysinfo_exclude_self(TRUE);
    HdeSysInfo si;
    hde_sysinfo_load(&si);
    Display *d = g_getenv("WAYLAND_DISPLAY") ? NULL : XOpenDisplay(NULL);
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

#define TAGLINE "A light and fast desktop for Hyggshi OS — GTK 3 on X11 with any GTK window manager, or on Wayland."
#define CREDITS "Made by HyggshiOSDeveloper and contributors. Free software under the MIT License. Configuration: " \
                "~/.config/hde/settings.ini. The logos of the distributions belong to their owners; HDE uses the one " \
                "your system installs, or its own copy drawn from Simple Icons (CC0, see data/logos/LICENSES.md)."

static gboolean dark_ui(void)
{
    HdeThemeInfo ti;
    hde_theme_info_load(&ti);
    gboolean d = ti.style == HDE_STYLE_DARK;
    hde_theme_info_clear(&ti);
    return d;
}

static GtkWidget *surface_image(cairo_surface_t *s)
{
    GtkWidget *im = gtk_image_new_from_surface(s);
    cairo_surface_destroy(s);
    return im;
}

static int ui_scale(void)
{
    GdkMonitor *m = hde_main_monitor();
    return m ? MAX(1, gdk_monitor_get_scale_factor(m)) : 1;
}

static GtkWidget *hde_logo_image(int size)
{
    HdeThemeInfo ti;
    hde_theme_info_load(&ti);
    GtkWidget *im = surface_image(hde_hde_logo_surface(size, ui_scale(), ti.accent));
    hde_theme_info_clear(&ti);
    return im;
}

static void on_link(GtkButton *b, gpointer uri) { (void)b; open_uri(uri); }

static GtkWidget *link_button(const char *label, const char *uri)
{
    GtkWidget *b = gtk_button_new_with_label(label);
    gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
    gtk_style_context_add_class(gtk_widget_get_style_context(b), "about-link");
    gtk_widget_set_tooltip_text(b, uri);
    g_signal_connect_data(b, "clicked", G_CALLBACK(on_link), g_strdup(uri), (GClosureNotify)(void (*)(void))g_free, 0);
    return b;
}

/* The system HDE runs on: its logo, name and version, the base it is built on (with that logo too), its links. */
static GtkWidget *os_hero(gboolean compact)
{
    HdeOsInfo os;
    hde_os_info_load(&os);
    gboolean dark = dark_ui();
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, compact ? 4 : 6);
    gtk_widget_set_halign(v, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(v, compact ? 4 : 10);
    gtk_widget_set_margin_bottom(v, compact ? 4 : 6);
    char *how = NULL;
    GtkWidget *logo = surface_image(hde_os_logo_surface(&os, compact ? 96 : 128, ui_scale(), dark, &how));
    gtk_box_pack_start(GTK_BOX(v), logo, FALSE, FALSE, 0);
    char *t = hde_os_title(&os);
    GtkWidget *title = gtk_label_new(t);
    gtk_label_set_selectable(GTK_LABEL(title), TRUE);
    gtk_widget_set_can_focus(title, FALSE);
    gtk_label_set_line_wrap(GTK_LABEL(title), TRUE);
    gtk_label_set_justify(GTK_LABEL(title), GTK_JUSTIFY_CENTER);
    gtk_style_context_add_class(gtk_widget_get_style_context(title), compact ? "about-os-small" : "about-title");
    gtk_box_pack_start(GTK_BOX(v), title, FALSE, FALSE, 0);
    char *base_id = NULL, *base_how = NULL;
    char *base = hde_os_base_text(&os, &base_id);
    if (base) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_set_halign(row, GTK_ALIGN_CENTER);
        GtkWidget *bl = gtk_label_new("Based on");
        gtk_style_context_add_class(gtk_widget_get_style_context(bl), "row-description");
        gtk_box_pack_start(GTK_BOX(row), bl, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), surface_image(hde_distro_logo_surface(base_id, base, 18, ui_scale(), dark, &base_how)),
                           FALSE, FALSE, 0);
        GtkWidget *bn = gtk_label_new(base);
        gtk_style_context_add_class(gtk_widget_get_style_context(bn), "about-base");
        gtk_box_pack_start(GTK_BOX(row), bn, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(v), row, FALSE, FALSE, 0);
    }
    if (!compact && (os.home_url || os.support_url || os.bug_url)) {
        GtkWidget *links = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        gtk_widget_set_halign(links, GTK_ALIGN_CENTER);
        if (os.home_url) gtk_box_pack_start(GTK_BOX(links), link_button("Website", os.home_url), FALSE, FALSE, 0);
        if (os.support_url && g_strcmp0(os.support_url, os.home_url))
            gtk_box_pack_start(GTK_BOX(links), link_button("Support", os.support_url), FALSE, FALSE, 0);
        if (os.bug_url && g_strcmp0(os.bug_url, os.support_url))
            gtk_box_pack_start(GTK_BOX(links), link_button("Report a bug", os.bug_url), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(v), links, FALSE, FALSE, 0);
    }
    if (g_getenv("HDE_DEBUG"))
        fprintf(stderr, "hde-settings: about: %s (ID=%s%s%s, from %s): logo from %s%s%s%s%s\n", t, os.id,
                os.id_like ? ", ID_LIKE=" : "", os.id_like ? os.id_like : "", os.source ? os.source : "nowhere",
                how ? how : "?", base ? "; based on " : "", base ? base : "", base ? ", its logo from " : "",
                base ? (base_how ? base_how : "?") : "");
    g_free(how); g_free(base_how); g_free(t); g_free(base); g_free(base_id);
    hde_os_info_clear(&os);
    return v;
}

GtkWidget *page_about_new(void)
{
    GtkWidget *box = page_base();
    HdeSysInfo si;
    hde_sysinfo_load(&si);
    Display *dpy = xdisplay();

    gtk_box_pack_start(GTK_BOX(box), os_hero(FALSE), FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), section("Desktop"), FALSE, FALSE, 0);
    GtkWidget *dcard = card_new();
    GtkWidget *drow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    gtk_container_set_border_width(GTK_CONTAINER(drow), 12);
    gtk_box_pack_start(GTK_BOX(drow), hde_logo_image(48), FALSE, FALSE, 0);
    GtkWidget *txt = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    GtkWidget *title = gtk_label_new("Hyggshi Desktop Environment");
    gtk_style_context_add_class(gtk_widget_get_style_context(title), "row-title");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    char *ver = g_strdup_printf("Version %s  ·  Build %s", HDE_RELEASE, HDE_VERSION);
    GtkWidget *vl = gtk_label_new(ver);
    g_free(ver);
    gtk_label_set_selectable(GTK_LABEL(vl), TRUE);
    gtk_widget_set_can_focus(vl, FALSE);
    gtk_style_context_add_class(gtk_widget_get_style_context(vl), "row-description");
    gtk_widget_set_halign(vl, GTK_ALIGN_START);
    GtkWidget *tag = gtk_label_new(TAGLINE);
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
    gtk_box_pack_start(GTK_BOX(drow), txt, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(dcard), drow);
    gtk_box_pack_start(GTK_BOX(box), dcard, FALSE, FALSE, 0);

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
    char *wm = dpy ? wm_name(dpy) : hde_is_wayland() && g_getenv("LABWC_PID") ? g_strdup("labwc (Wayland compositor)") : NULL;
    char *xs = dpy ? xserver(dpy) : hde_is_wayland() ? wayland_server() : g_strdup("Not X11");
    char *base = base_line();
    char *gtkv = g_strdup_printf("%u.%u.%u", gtk_get_major_version(), gtk_get_minor_version(), gtk_get_micro_version());
    char *desk = g_strdup_printf("HDE %s (build %s)%s", HDE_RELEASE, HDE_VERSION,
                                 g_getenv("HDE_SESSION_PID") ? "" : " — not running as the session");
    char *up = uptime_text(si.uptime);
    add_fact(sw, "Operating system", si.os);
    if (base) add_fact(sw, "Based on", base);
    if (g_getenv("HDE_DEBUG"))
        fprintf(stderr, "hde-settings: about: window manager: %s; display server: %s\n", wm ? wm : "none", xs);
    add_fact(sw, "Kernel", kern);
    add_fact(sw, "Desktop", desk);
    add_fact(sw, "Window manager", wm ? wm : "None running");
    add_fact(sw, "Display server", xs);
    add_fact(sw, "GTK", gtkv);
    add_fact(sw, "Running for", up);
    gtk_box_pack_start(GTK_BOX(box), sw, FALSE, FALSE, 0);
    g_free(kern); g_free(wm); g_free(xs); g_free(gtkv); g_free(desk); g_free(up); g_free(base);

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
    {
        char *advice = hde_distro_ram_advice();
        char *ram = g_strdup_printf(
            "HDE itself needs only what is shown above. %s Applications come on top: a web browser alone often takes "
            "several hundred MB to a few GB depending on the tabs, so 4 GB or more is best for everyday browsing. Give "
            "a virtual machine at least 2 GB.", advice);
        gtk_box_pack_start(GTK_BOX(box), info_label(ram), FALSE, FALSE, 0);
        g_free(ram);
        g_free(advice);
    }

    gtk_box_pack_start(GTK_BOX(box), section("Credits"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), info_label(CREDITS), FALSE, FALSE, 0);

    g_signal_connect(box, "map", G_CALLBACK(on_about_map), NULL);
    g_signal_connect(box, "unmap", G_CALLBACK(on_about_unmap), NULL);
    g_signal_connect(box, "destroy", G_CALLBACK(on_about_destroy), NULL);
    hde_sysinfo_clear(&si);
    return box;
}

/* ---------------------------------------------------------------- the "About HDE" window (desktop menu) */
static void on_details(GtkButton *b, gpointer w)
{
    (void)b;
    char self[4096];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n > 0) self[n] = '\0';
    char *argv[] = { n > 0 ? self : (char *)"hde-settings", (char *)"about", NULL };
    GError *e = NULL;
    if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &e)) {
        g_printerr("hde-settings: %s\n", e->message);
        g_clear_error(&e);
    }
    gtk_widget_destroy(GTK_WIDGET(w));
}

static void on_credits(GtkToggleButton *b, gpointer stack)
{
    gtk_stack_set_visible_child_name(GTK_STACK(stack), gtk_toggle_button_get_active(b) ? "credits" : "about");
}

static gboolean on_about_key(GtkWidget *w, GdkEventKey *e, gpointer d)
{
    (void)d;
    if (e->keyval == GDK_KEY_Escape) { gtk_widget_destroy(w); return TRUE; }
    return FALSE;
}

static GtkWidget *centered_label(const char *text, const char *cls)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_justify(GTK_LABEL(l), GTK_JUSTIFY_CENTER);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 48);
    if (cls) gtk_style_context_add_class(gtk_widget_get_style_context(l), cls);
    return l;
}

int about_window_main(void)
{
    GtkWidget *w = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(w), "About HDE");
    gtk_window_set_icon_name(GTK_WINDOW(w), "help-about");
    gtk_window_set_resizable(GTK_WINDOW(w), FALSE);
    gtk_window_set_position(GTK_WINDOW(w), GTK_WIN_POS_CENTER);
    gtk_window_set_default_size(GTK_WINDOW(w), 460, -1);
    g_signal_connect(w, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(w, "key-press-event", G_CALLBACK(on_about_key), NULL);
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(v), 22);
    gtk_container_add(GTK_CONTAINER(w), v);
    gtk_box_pack_start(GTK_BOX(v), os_hero(TRUE), FALSE, FALSE, 0);
    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top(sep, 10);
    gtk_widget_set_margin_bottom(sep, 10);
    gtk_box_pack_start(GTK_BOX(v), sep, FALSE, FALSE, 0);

    GtkWidget *stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_vhomogeneous(GTK_STACK(stack), FALSE);
    GtkWidget *about = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *hl = hde_logo_image(48);
    gtk_widget_set_halign(hl, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(about), hl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(about), centered_label("Hyggshi Desktop Environment", "about-hde"), FALSE, FALSE, 2);
    char *ver = g_strdup_printf("Version %s  ·  Build %s", HDE_RELEASE, HDE_VERSION);
    gtk_box_pack_start(GTK_BOX(about), centered_label(ver, "row-description"), FALSE, FALSE, 0);
    g_free(ver);
    gtk_box_pack_start(GTK_BOX(about), centered_label(TAGLINE, NULL), FALSE, FALSE, 6);
    Display *dpy = xdisplay();
    char *wm = dpy ? wm_name(dpy) : hde_is_wayland() && g_getenv("LABWC_PID") ? g_strdup("labwc") : NULL;
    char *xs = dpy ? xserver(dpy) : hde_is_wayland() ? wayland_server() : g_strdup("?");
    HdeProcMem p[48];
    guint64 pss = 0, rss = 0;
    int n = hde_sysinfo_desktop_memory(p, G_N_ELEMENTS(p), &pss, &rss);
    char *mem = hde_format_bytes(pss);
    char *facts = n ? g_strdup_printf("%s%s%s  ·  HDE uses %s of RAM", wm ? wm : "", wm ? " on " : "", xs, mem)
                    : g_strdup_printf("%s%s%s", wm ? wm : "", wm ? " on " : "", xs);
    gtk_box_pack_start(GTK_BOX(about), centered_label(facts, "row-description"), FALSE, FALSE, 0);
    g_free(wm); g_free(xs); g_free(mem);
    gtk_stack_add_named(GTK_STACK(stack), about, "about");
    GtkWidget *credits = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_pack_start(GTK_BOX(credits), centered_label("Credits", "about-hde"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(credits), centered_label(CREDITS, NULL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(credits), centered_label("Built with GTK 3, libwnck, gtk-layer-shell and labwc. Project page: "
                                                        PROJECT_URL, "row-description"), FALSE, FALSE, 0);
    gtk_stack_add_named(GTK_STACK(stack), credits, "credits");
    gtk_box_pack_start(GTK_BOX(v), stack, FALSE, FALSE, 0);

    GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(btns, 16);
    GtkWidget *cr = gtk_toggle_button_new_with_mnemonic("C_redits");
    g_signal_connect(cr, "toggled", G_CALLBACK(on_credits), stack);
    GtkWidget *det = gtk_button_new_with_mnemonic("System _details…");
    gtk_widget_set_tooltip_text(det, "Settings > About: this computer, the memory each part of HDE uses, …");
    g_signal_connect(det, "clicked", G_CALLBACK(on_details), w);
    GtkWidget *close = gtk_button_new_with_mnemonic("_Close");
    g_signal_connect_swapped(close, "clicked", G_CALLBACK(gtk_widget_destroy), w);
    gtk_box_pack_start(GTK_BOX(btns), cr, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(btns), close, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(btns), det, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), btns, FALSE, FALSE, 0);
    gtk_widget_show_all(w);
    gtk_widget_grab_focus(close);
    if (g_getenv("HDE_DEBUG")) fprintf(stderr, "hde-settings: about window shown (%s)\n", facts);
    g_free(facts);
    gtk_main();
    return 0;
}
