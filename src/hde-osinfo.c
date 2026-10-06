/* hde-osinfo.c — see hde-osinfo.h */
#include "hde-osinfo.h"
#include "hde-svgpath.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef HDE_DATADIR
#define HDE_DATADIR "/usr/local/share/hde"
#endif

/* ---------------------------------------------------------------- /etc/os-release */
/* shell-like value: "double quoted \"x\"", 'single quoted', or a bare word */
static char *unquote(const char *v)
{
    GString *s = g_string_new(NULL);
    char q = 0;
    for (const char *p = v; *p; p++) {
        if (!q && (*p == '"' || *p == '\'')) { q = *p; continue; }
        if (q && *p == q) { q = 0; continue; }
        if (*p == '\\' && q != '\'' && p[1]) { p++; g_string_append_c(s, *p); continue; }
        if (!q && (*p == '#' || *p == '\r' || *p == '\n')) break;
        g_string_append_c(s, *p);
    }
    char *r = g_string_free(s, FALSE);
    g_strstrip(r);
    return r;
}

void hde_os_info_load(HdeOsInfo *os)
{
    memset(os, 0, sizeof *os);
    const char *cands[] = { g_getenv("HDE_OS_RELEASE"), "/etc/os-release", "/usr/lib/os-release", NULL };
    char *txt = NULL;
    for (int i = 0; i < 3 && !txt; i++) {
        if (!cands[i] || !*cands[i]) continue;
        if (g_file_get_contents(cands[i], &txt, NULL, NULL)) os->source = g_strdup(cands[i]);
    }
    if (txt) {
        char **lines = g_strsplit(txt, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            char *l = g_strstrip(lines[i]);
            if (!*l || *l == '#') continue;
            char *eq = strchr(l, '=');
            if (!eq) continue;
            *eq = '\0';
            const char *k = g_strstrip(l);
            char **dst = NULL;
            if (!strcmp(k, "ID")) dst = &os->id;
            else if (!strcmp(k, "ID_LIKE")) dst = &os->id_like;
            else if (!strcmp(k, "NAME")) dst = &os->name;
            else if (!strcmp(k, "PRETTY_NAME")) dst = &os->pretty_name;
            else if (!strcmp(k, "VERSION")) dst = &os->version;
            else if (!strcmp(k, "VERSION_ID")) dst = &os->version_id;
            else if (!strcmp(k, "VERSION_CODENAME")) dst = &os->codename;
            else if (!strcmp(k, "VARIANT")) dst = &os->variant;
            else if (!strcmp(k, "BUILD_ID")) dst = &os->build_id;
            else if (!strcmp(k, "HOME_URL")) dst = &os->home_url;
            else if (!strcmp(k, "SUPPORT_URL")) dst = &os->support_url;
            else if (!strcmp(k, "BUG_REPORT_URL")) dst = &os->bug_url;
            else if (!strcmp(k, "LOGO")) dst = &os->logo;
            else if (!strcmp(k, "ANSI_COLOR")) dst = &os->ansi_color;
            else if (!strcmp(k, "HYGGSHI_BASE_CODENAME") || !strcmp(k, "UBUNTU_CODENAME") || !strcmp(k, "DEBIAN_CODENAME"))
                dst = &os->base_codename;
            if (!dst) continue;
            char *v = unquote(eq + 1);
            if (!*v) { g_free(v); continue; }
            g_free(*dst);
            *dst = v;
        }
        g_strfreev(lines);
        g_free(txt);
    }
    if (os->id) {
        char *low = g_ascii_strdown(os->id, -1);
        g_free(os->id);
        os->id = low;
    }
    if (!os->id) os->id = g_strdup("linux");
    if (!os->name) os->name = g_strdup(os->pretty_name ? os->pretty_name : "Linux");
}

void hde_os_info_clear(HdeOsInfo *os)
{
    char **f[] = { &os->id, &os->id_like, &os->name, &os->pretty_name, &os->version, &os->version_id, &os->codename,
                   &os->variant, &os->build_id, &os->home_url, &os->support_url, &os->bug_url, &os->logo,
                   &os->ansi_color, &os->base_codename, &os->source };
    for (guint i = 0; i < G_N_ELEMENTS(f); i++) { g_free(*f[i]); *f[i] = NULL; }
}

static gboolean contains_ci(const char *hay, const char *needle)
{
    if (!hay || !needle || !*needle) return FALSE;
    char *h = g_utf8_casefold(hay, -1), *n = g_utf8_casefold(needle, -1);
    gboolean r = strstr(h, n) != NULL;
    g_free(h);
    g_free(n);
    return r;
}

char *hde_os_title(const HdeOsInfo *os)
{
    char *t = NULL;
    if (os->pretty_name) t = g_strdup(os->pretty_name);
    else if (os->version) t = g_strdup_printf("%s %s", os->name, os->version);
    else if (os->version_id) t = g_strdup_printf("%s %s", os->name, os->version_id);
    else t = g_strdup(os->name);
    /* a derivative that names its base in PRETTY_NAME ("Hyggshi OS 1.0 "Sen Vàng" (dựa trên Debian 13)"): that part
     * is shown as "Based on ..." with the base's logo instead */
    if (os->id_like && *os->id_like) {
        char *open = strrchr(t, '(');
        size_t len = strlen(t);
        if (open && open > t && t[len - 1] == ')') {
            char **likes = g_strsplit(os->id_like, " ", -1);
            gboolean names_base = FALSE;
            for (int i = 0; likes[i] && !names_base; i++)
                names_base = *likes[i] && contains_ci(open, likes[i]) && !contains_ci(os->name, likes[i]);
            g_strfreev(likes);
            if (names_base) {
                *open = '\0';
                g_strchomp(t);
            }
        }
    }
    return t;
}

typedef struct { const char *codename, *version; } Release;
static const Release debian_releases[] = {
    { "stretch", "9" }, { "buster", "10" }, { "bullseye", "11" }, { "bookworm", "12" }, { "trixie", "13" },
    { "forky", "14" }, { "duke", "15" }, { NULL, NULL }
};
static const Release ubuntu_releases[] = {
    { "focal", "20.04 LTS" }, { "jammy", "22.04 LTS" }, { "mantic", "23.10" }, { "noble", "24.04 LTS" },
    { "oracular", "24.10" }, { "plucky", "25.04" }, { "questing", "25.10" }, { "resolute", "26.04 LTS" }, { NULL, NULL }
};

static const char *release_version(const Release *r, const char *codename)
{
    for (int i = 0; codename && r[i].codename; i++)
        if (!g_ascii_strcasecmp(r[i].codename, codename)) return r[i].version;
    return NULL;
}

static char *file_value(const char *path, const char *key)
{
    char *txt = NULL, *res = NULL;
    if (!g_file_get_contents(path, &txt, NULL, NULL)) return NULL;
    char **lines = g_strsplit(txt, "\n", -1);
    size_t kl = strlen(key);
    for (int i = 0; lines[i] && !res; i++)
        if (!strncmp(lines[i], key, kl) && lines[i][kl] == '=') res = unquote(lines[i] + kl + 1);
    g_strfreev(lines);
    g_free(txt);
    return res;
}

char *hde_os_base_text(const HdeOsInfo *os, char **base_id)
{
    if (base_id) *base_id = NULL;
    if (!os->id_like || !*os->id_like) return NULL;
    char **likes = g_strsplit_set(os->id_like, " \t", -1);
    const char *base = NULL;
    for (int i = 0; likes[i]; i++) if (!strcmp(likes[i], "ubuntu")) base = "ubuntu";
    for (int i = 0; likes[i] && !base; i++) if (*likes[i]) base = likes[i];
    if (!base) { g_strfreev(likes); return NULL; }
    char *res = NULL;
    const char *cn = os->base_codename;
    if (!strcmp(base, "ubuntu")) {
        const char *v = release_version(ubuntu_releases, cn);
        if (v) res = g_strdup_printf("Ubuntu %s (%s)", v, cn);
        else {
            char *d = file_value("/etc/upstream-release/lsb-release", "DISTRIB_DESCRIPTION");
            res = d ? d : cn ? g_strdup_printf("Ubuntu (%s)", cn) : g_strdup("Ubuntu");
        }
    } else if (!strcmp(base, "debian")) {
        const char *v = release_version(debian_releases, cn);
        char *dv = NULL;
        if (g_file_get_contents(g_getenv("HDE_DEBIAN_VERSION") ? g_getenv("HDE_DEBIAN_VERSION") : "/etc/debian_version",
                                &dv, NULL, NULL))
            g_strstrip(dv);
        gboolean numeric = dv && g_ascii_isdigit(dv[0]);
        if (v && numeric && g_str_has_prefix(dv, v) && (dv[strlen(v)] == '.' || dv[strlen(v)] == '\0'))
            res = g_strdup_printf("Debian %s (%s)", dv, cn);                     /* point release: "Debian 13.1" */
        else if (v) res = g_strdup_printf("Debian %s (%s)", v, cn);
        else if (numeric) res = g_strdup_printf("Debian %s", dv);
        else if (dv && *dv) res = g_strdup_printf("Debian %s", dv);              /* "trixie/sid" (Ubuntu) */
        else res = g_strdup("Debian");
        g_free(dv);
    } else {
        static const struct { const char *id, *name; } names[] = {
            { "arch", "Arch Linux" }, { "fedora", "Fedora" }, { "rhel", "Red Hat Enterprise Linux" },
            { "centos", "CentOS" }, { "suse", "SUSE" }, { "opensuse", "openSUSE" }, { "gentoo", "Gentoo" },
            { "slackware", "Slackware" }, { "alpine", "Alpine Linux" },
        };
        for (guint i = 0; i < G_N_ELEMENTS(names) && !res; i++)
            if (!strcmp(base, names[i].id)) res = g_strdup(names[i].name);
        if (!res) {
            res = g_strdup(base);
            res[0] = g_ascii_toupper(res[0]);
        }
    }
    if (base_id) *base_id = g_strdup(base);
    g_strfreev(likes);
    return res;
}

/* ---------------------------------------------------------------- data files */
char *hde_data_file(const char *rel)
{
    char *p = NULL;
    const char *env = g_getenv("HDE_DATADIR");
    if (env && *env) {
        p = g_build_filename(env, rel, NULL);
        if (g_file_test(p, G_FILE_TEST_EXISTS)) return p;
        g_free(p);
    }
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) {
        exe[n] = '\0';
        char *dir = g_path_get_dirname(exe);
        const char *subs[] = { "../share/hde", "../data", NULL };      /* installed: bin/../share/hde; build/../data */
        for (int i = 0; subs[i]; i++) {
            p = g_build_filename(dir, subs[i], rel, NULL);
            if (g_file_test(p, G_FILE_TEST_EXISTS)) { g_free(dir); return p; }
            g_free(p);
        }
        g_free(dir);
    }
    p = g_build_filename(HDE_DATADIR, rel, NULL);
    if (g_file_test(p, G_FILE_TEST_EXISTS)) return p;
    g_free(p);
    return NULL;
}

/* ---------------------------------------------------------------- drawing */
static cairo_surface_t *new_surface(int size, int scale)
{
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size * scale, size * scale);
    cairo_surface_set_device_scale(s, scale, scale);
    return s;
}

static void sink_move(void *u, double x, double y) { cairo_move_to(u, x, y); }
static void sink_line(void *u, double x, double y) { cairo_line_to(u, x, y); }
static void sink_curve(void *u, double a, double b, double c, double d, double e, double f) { cairo_curve_to(u, a, b, c, d, e, f); }
static void sink_close(void *u) { cairo_close_path(u); }
static const HdeSvgPathSink cairo_sink = { sink_move, sink_line, sink_curve, sink_close };

static double luminance(const GdkRGBA *c) { return 0.2126 * c->red + 0.7152 * c->green + 0.0722 * c->blue; }

static char *attr(const char *tag_start, const char *name)
{
    const char *end = strchr(tag_start, '>');
    char *needle = g_strdup_printf(" %s=\"", name);
    const char *p = strstr(tag_start, needle);
    char *r = NULL;
    if (p && (!end || p < end)) {
        p += strlen(needle);
        const char *q = strchr(p, '"');
        if (q) r = g_strndup(p, q - p);
    }
    g_free(needle);
    return r;
}

/* data/logos/<id>.svg: one <path> (Simple Icons format), drawn with our own path reader */
static cairo_surface_t *bundled_logo(const char *id, int size, int scale, gboolean dark, char **how)
{
    char *rel = g_strdup_printf("logos/%s.svg", id);
    char *file = hde_data_file(rel);
    g_free(rel);
    if (!file) return NULL;
    char *txt = NULL;
    cairo_surface_t *surf = NULL;
    if (g_file_get_contents(file, &txt, NULL, NULL)) {
        const char *svg = strstr(txt, "<svg"), *path = strstr(txt, "<path");
        char *vb = svg ? attr(svg, "viewBox") : NULL, *d = path ? attr(path, "d") : NULL;
        char *fill = path ? attr(path, "fill") : NULL;
        double vx = 0, vy = 0, vw = 24, vh = 24;
        if (vb) {
            char **v = g_strsplit_set(vb, " ,", -1);
            if (g_strv_length(v) == 4) {
                vx = g_ascii_strtod(v[0], NULL); vy = g_ascii_strtod(v[1], NULL);
                vw = g_ascii_strtod(v[2], NULL); vh = g_ascii_strtod(v[3], NULL);
            }
            g_strfreev(v);
        }
        GdkRGBA c;
        if (!fill || !gdk_rgba_parse(&c, fill)) gdk_rgba_parse(&c, "#555555");
        if (dark && luminance(&c) < 0.18) gdk_rgba_parse(&c, "#e6e9ef");     /* black logos on a dark background */
        if (d && vw > 0 && vh > 0) {
            surf = new_surface(size, scale);
            cairo_t *cr = cairo_create(surf);
            double k = size / MAX(vw, vh);
            cairo_translate(cr, (size - vw * k) / 2, (size - vh * k) / 2);
            cairo_scale(cr, k, k);
            cairo_translate(cr, -vx, -vy);
            if (hde_svg_path_parse(d, &cairo_sink, cr) > 0) {
                gdk_cairo_set_source_rgba(cr, &c);
                cairo_fill(cr);
                if (how) *how = g_strdup_printf("HDE: %s", file);
            } else {
                cairo_destroy(cr);
                cairo_surface_destroy(surf);
                surf = NULL;
                cr = NULL;
            }
            if (cr) cairo_destroy(cr);
        }
        g_free(vb); g_free(d); g_free(fill);
    }
    g_free(txt);
    g_free(file);
    return surf;
}

static void grad_rect(cairo_t *cr, double x, double y, double w, double h, double gx0, double gy0, double gx1, double gy1,
                      const char *c0, const char *c1)
{
    GdkRGBA a, b;
    gdk_rgba_parse(&a, c0);
    gdk_rgba_parse(&b, c1);
    cairo_pattern_t *p = cairo_pattern_create_linear(gx0, gy0, gx1, gy1);
    cairo_pattern_add_color_stop_rgb(p, 0, a.red, a.green, a.blue);
    cairo_pattern_add_color_stop_rgb(p, 1, b.red, b.green, b.blue);
    cairo_rectangle(cr, x, y, w, h);
    cairo_set_source(cr, p);
    cairo_fill(cr);
    cairo_pattern_destroy(p);
}

/* The Hyggshi OS logo (iso-config/branding/Logo.svg of Hyggshi-OS: four bars around a centre, 371 x 371) */
static void draw_hyggshi(cairo_t *cr, double size)
{
    cairo_save(cr);
    double k = size / 371.0;
    cairo_scale(cr, k, k);
    if (size >= 40) {                                         /* the soft drop shadow of the original */
        for (int i = 0; i < 4; i++) {
            double o = 6 + i * 4, g = i * 3;
            cairo_set_source_rgba(cr, 0, 0, 0, 0.045);
            cairo_rectangle(cr, 97.03 - g, 5 + o - g, 72.03 + 2 * g, 150.83 + 2 * g);
            cairo_rectangle(cr, 201.46 - g, 174.68 + o - g, 72.03 + 2 * g, 150.83 + 2 * g);
            cairo_rectangle(cr, 25 - g, 174.68 + o - g, 144.05 + 2 * g, 75.41 + 2 * g);
            cairo_rectangle(cr, 201.46 - g, 80.41 + o - g, 144.05 + 2 * g, 75.41 + 2 * g);
            cairo_fill(cr);
        }
    }
    grad_rect(cr, 97.0254, 5, 72.025, 150.829, 97.0254, 0, 169.05, 0, "#00FF85", "#40646D");
    grad_rect(cr, 201.461, 174.684, 72.025, 150.829, 201.461, 0, 273.486, 0, "#C3DE57", "#354B84");
    grad_rect(cr, 25, 174.684, 144.05, 75.4144, 25, 0, 169.05, 0, "#68CEFF", "#4563AF");
    grad_rect(cr, 201.461, 80.4137, 144.05, 75.4143, 345.511, 0, 201.461, 0, "#757373", "#00FFD9");
    cairo_restore(cr);
}

static cairo_surface_t *builtin_logo(const char *id, int size, int scale, char **how)
{
    if (strcmp(id, "hyggshios") && strcmp(id, "hyggshi")) return NULL;
    cairo_surface_t *s = new_surface(size, scale);
    cairo_t *cr = cairo_create(s);
    draw_hyggshi(cr, size);
    cairo_destroy(cr);
    if (how) *how = g_strdup_printf("built in: %s", id);
    return s;
}

/* ANSI_COLOR="0;31" / "1;34" / "38;2;R;G;B" -> a colour for the letter badge */
static void ansi_rgba(const char *ansi, GdkRGBA *c)
{
    gdk_rgba_parse(c, "#4f6d8f");
    if (!ansi) return;
    char **p = g_strsplit(ansi, ";", -1);
    int n = (int)g_strv_length(p);
    for (int i = 0; i < n; i++) {
        int v = atoi(p[i]);
        if (v == 38 && i + 4 < n && atoi(p[i + 1]) == 2) {
            c->red = atoi(p[i + 2]) / 255.0; c->green = atoi(p[i + 3]) / 255.0; c->blue = atoi(p[i + 4]) / 255.0;
            break;
        }
        static const char *const basic[] = { "#5e5c64", "#c01c28", "#26a269", "#c88800", "#1c71d8", "#a347ba",
                                             "#2aa1b3", "#77767b" };
        if (v >= 30 && v <= 37) gdk_rgba_parse(c, basic[v - 30]);
        if (v >= 90 && v <= 97) gdk_rgba_parse(c, basic[v - 90]);
    }
    g_strfreev(p);
    c->alpha = 1;
}

static cairo_surface_t *badge_logo(const char *name, const char *ansi, int size, int scale, char **how)
{
    cairo_surface_t *s = new_surface(size, scale);
    cairo_t *cr = cairo_create(s);
    GdkRGBA c;
    ansi_rgba(ansi, &c);
    cairo_arc(cr, size / 2.0, size / 2.0, size / 2.0 - 0.5, 0, 2 * G_PI);
    gdk_cairo_set_source_rgba(cr, &c);
    cairo_fill(cr);
    char letter[8] = "L";
    if (name && *name) {
        gunichar u = g_unichar_toupper(g_utf8_get_char_validated(name, -1));
        if (u != (gunichar)-1 && u != (gunichar)-2 && u) letter[g_unichar_to_utf8(u, letter)] = '\0';
    }
    PangoLayout *pl = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string("Sans Bold");
    pango_font_description_set_absolute_size(fd, size * 0.52 * PANGO_SCALE);
    pango_layout_set_font_description(pl, fd);
    pango_layout_set_text(pl, letter, -1);
    int w, h;
    pango_layout_get_pixel_size(pl, &w, &h);
    cairo_move_to(cr, (size - w) / 2.0, (size - h) / 2.0);
    cairo_set_source_rgb(cr, 1, 1, 1);
    pango_cairo_show_layout(cr, pl);
    pango_font_description_free(fd);
    g_object_unref(pl);
    cairo_destroy(cr);
    if (how) *how = g_strdup_printf("badge: %s", letter);
    return s;
}

static cairo_surface_t *theme_icon(const char *name, int size, int scale, char **how)
{
    if (!name || !*name) return NULL;
    GtkIconTheme *t = gtk_icon_theme_get_default();
    GtkIconInfo *ii = gtk_icon_theme_lookup_icon_for_scale(t, name, size, scale, GTK_ICON_LOOKUP_FORCE_SIZE);
    if (!ii) return NULL;
    cairo_surface_t *s = gtk_icon_info_load_surface(ii, NULL, NULL);
    g_object_unref(ii);
    if (s && how) *how = g_strdup_printf("icon theme: %s", name);
    return s;
}

static cairo_surface_t *pixmap_file(const char *base, int size, int scale, char **how)
{
    const char *exts[] = { ".png", ".svg", ".xpm", NULL };
    for (int i = 0; exts[i]; i++) {
        char *p = g_strdup_printf("/usr/share/pixmaps/%s%s", base, exts[i]);
        GdkPixbuf *pb = g_file_test(p, G_FILE_TEST_EXISTS)
                            ? gdk_pixbuf_new_from_file_at_scale(p, size * scale, size * scale, TRUE, NULL) : NULL;
        if (pb) {
            cairo_surface_t *s = gdk_cairo_surface_create_from_pixbuf(pb, scale, NULL);
            g_object_unref(pb);
            if (how) *how = g_strdup_printf("file: %s", p);
            g_free(p);
            return s;
        }
        g_free(p);
    }
    return NULL;
}

/* opensuse-tumbleweed, opensuse-leap, ... share one logo */
static const char *logo_alias(const char *id)
{
    if (g_str_has_prefix(id, "opensuse") || !strcmp(id, "suse") || !strcmp(id, "sles")) return "opensuse";
    if (!strcmp(id, "raspios")) return "raspbian";
    return id;
}

static cairo_surface_t *distro_logo(const char *id, int size, int scale, gboolean dark, char **how)
{
    if (!id || !*id) return NULL;
    cairo_surface_t *s = NULL;
    char *names[] = { g_strdup_printf("distributor-logo-%s", id), g_strdup_printf("%s-logo", id),
                      g_strdup_printf("%s-logo-icon", id), g_strdup_printf("start-here-%s", id),
                      g_strdup_printf("emblem-%s", id) };
    for (guint i = 0; i < G_N_ELEMENTS(names) && !s; i++) s = theme_icon(names[i], size, scale, how);
    for (guint i = 0; i < G_N_ELEMENTS(names); i++) g_free(names[i]);
    if (!s) {
        char *b = g_strdup_printf("%s-logo", id);
        s = pixmap_file(b, size, scale, how);
        g_free(b);
    }
    if (!s) s = builtin_logo(id, size, scale, how);
    if (!s) s = bundled_logo(logo_alias(id), size, scale, dark, how);
    return s;
}

cairo_surface_t *hde_distro_logo_surface(const char *id, const char *name, int size, int scale, gboolean dark, char **how)
{
    if (how) *how = NULL;
    cairo_surface_t *s = distro_logo(id, size, scale, dark, how);
    return s ? s : badge_logo(name ? name : id, NULL, size, scale, how);
}

cairo_surface_t *hde_os_logo_surface(const HdeOsInfo *os, int size, int scale, gboolean dark, char **how)
{
    if (how) *how = NULL;
    if (scale < 1) scale = 1;
    cairo_surface_t *s = NULL;
    if (os->logo) {
        s = theme_icon(os->logo, size, scale, how);
        if (!s) s = pixmap_file(os->logo, size, scale, how);
    }
    if (!s) s = distro_logo(os->id, size, scale, dark, how);
    if (!s && os->id_like) {
        char **likes = g_strsplit_set(os->id_like, " \t", -1);
        for (int i = 0; likes[i] && !s; i++) s = distro_logo(likes[i], size, scale, dark, how);
        g_strfreev(likes);
    }
    if (!s) s = badge_logo(os->name, os->ansi_color, size, scale, how);
    return s;
}

/* ---------------------------------------------------------------- HDE's own logo */
void hde_draw_hde_logo(cairo_t *cr, double x, double y, double s, const GdkRGBA *accent)
{
    GdkRGBA a = *accent;
    double r = s * 0.24;
    cairo_save(cr);
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + s - r, y + r, r, -G_PI / 2, 0);
    cairo_arc(cr, x + s - r, y + s - r, r, 0, G_PI / 2);
    cairo_arc(cr, x + r, y + s - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
    cairo_pattern_t *g = cairo_pattern_create_linear(x, y, x + s, y + s);
    cairo_pattern_add_color_stop_rgb(g, 0, MIN(1, a.red * 1.25 + 0.08), MIN(1, a.green * 1.25 + 0.08),
                                     MIN(1, a.blue * 1.25 + 0.08));
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
    cairo_restore(cr);
}

cairo_surface_t *hde_hde_logo_surface(int size, int scale, const char *accent)
{
    GdkRGBA a;
    if (!accent || !gdk_rgba_parse(&a, accent)) gdk_rgba_parse(&a, "#3584e4");
    cairo_surface_t *s = new_surface(size, scale < 1 ? 1 : scale);
    cairo_t *cr = cairo_create(s);
    hde_draw_hde_logo(cr, 0, 0, size, &a);
    cairo_destroy(cr);
    return s;
}
