/*
 * apps/store/hde-store.c — Hyggshi Store, the store of Hyggshi OS.
 *
 *   Discover  — what can be installed, by category, and what is already there
 *   Search    — apt-cache search, with the installed packages of the machine folded in
 *   Install   — the detail page: version, source, license and how far it can be trusted, then the
 *               package is installed (or removed) through pkexec + apt-get, with its output on screen
 *   Updates   — everything the repositories offer to replace, one by one or all at once
 *   Sources   — /etc/apt/sources.list and sources.list.d (classic and deb822), plus Flatpak and a per-user Flathub connection
 *
 * Everything the Store knows about a package is parsed by apps/store/hde-store-core.c, which has no GTK
 * and is tested without a package manager (tests/store-test.c).  .deb is the first half of the store;
 * The Store can connect the per-user Flathub remote; browsing/installing Flatpaks remains read-only for now.
 */
#define _POSIX_C_SOURCE 200809L

#include "hde-build.h"
#include "hde-store-core.h"
#include "nexsdk/nexsdk.h"
#include "hde-theme.h"

#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>
#include <locale.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#define APP_TITLE "Hyggshi Store"
#define APP_ICON "system-software-install"

/* ------------------------------------------------------------------ state */

static GtkWidget *main_win;
static GtkWidget *stack;
static GtkWidget *back_button;
static char *detail_return_page;
static GtkWidget *sidebar;
/* NexSDK's check of a local .deb (nexsdk/pkg.h): the same findings `hde-pkg-check` prints, so the Store can
 * say what is wrong with a file BEFORE it is installed instead of after. */
static NexSdkPkgReport file_check;
static gboolean file_check_done;
static GtkWidget *search_entry;
static GtkWidget *status_label;
static GtkWidget *busy_spinner;

static GtkWidget *discover_inner;
static GtkWidget *installed_inner;
static GtkWidget *updates_inner;
static GtkWidget *sources_inner;
static GtkWidget *results_inner;
static GtkWidget *detail_inner;

static GPtrArray *pkgs_installed;   /* HdeStorePackage*, sorted by name */
static GPtrArray *pkgs_updates;     /* HdeStorePackage*, sorted by name */
static GPtrArray *pkgs_search;      /* HdeStorePackage*, ranked for search_query */
static GPtrArray *pkgs_featured;    /* HdeStorePackage*, the curated app list */
static GPtrArray *pkgs_flatpak;     /* HdeStorePackage*, read-only */
static GHashTable *by_name;         /* name -> HdeStorePackage*, the installed ones */
static guint64 installed_kb;

static HdeStorePackage *detail;     /* the package of the detail page (we own it) */
static char *search_query;
static char *pending_search;
static char *pending_file;
static gboolean pending_updates;
static GtkCssProvider *store_css;

/* What the Discover page offers when nothing was typed yet: the apps people look for. */
static const char *const curated_apps[] = {
    "firefox-esr", "firefox", "chromium", "thunderbird", "libreoffice-writer", "libreoffice-calc",
    "gimp", "inkscape", "krita", "vlc", "mpv", "rhythmbox", "audacity", "obs-studio", "blender",
    "gnome-weather", "gnome-clocks", "gnome-calculator", "gnome-text-editor", "gnome-maps",
    "gnome-software", "gnome-firmware", "evolution", "transmission-gtk",
    "vim", "emacs", "code", "git", "python3", "build-essential", "htop", "neofetch",
    "steam", "flatpak", "gparted", "timeshift", "synaptic", "filezilla", "keepassxc",
    NULL
};

typedef struct {
    const char *label_en;
    const char *label_vi;
    const char *query;
} StoreCategory;

static const StoreCategory categories[] = {
    { "Graphics & Photos", "Ảnh & Đồ họa", "image editor photo graphics" },
    { "Sound & Music", "Âm thanh & Nhạc", "audio music player sound" },
    { "Video", "Video", "video player media" },
    { "Office", "Văn phòng", "office document word processor spreadsheet" },
    { "Internet", "Internet", "browser web mail network" },
    { "Development", "Phát triển", "development compiler programming" },
    { "Games", "Trò chơi", "game" },
    { "Education", "Giáo dục", "education learning" },
    { "Utilities", "Tiện ích", "utility tools" },
    { "System", "Hệ thống", "system administration monitor" },
    { NULL, NULL, NULL }
};

/* ------------------------------------------------------------------ forward declarations */

static void rebuild_installed(void);
static void rebuild_updates(void);
static void rebuild_sources(void);
static void rebuild_discover(void);
static void rebuild_results(void);
static void rebuild_detail(void);
static void after_change(void);
static void open_detail(const HdeStorePackage *src);
static void open_local_file(const char *path);
static void do_search(const char *query);
static void select_sidebar_row(gpointer index);
static void on_update_lists(GtkButton *b, gpointer data);
static void on_upgrade_all(GtkButton *b, gpointer data);
static void on_install_file(GtkButton *b, gpointer data);
static void on_refresh_clicked(GtkButton *b, gpointer data);
static void on_search_changed(GtkSearchEntry *entry, gpointer data);
static void on_category_clicked(GtkButton *b, gpointer data);
static void on_filter_changed(GtkSearchEntry *entry, gpointer data);
static void on_install_clicked(GtkButton *b, gpointer data);
static void on_remove_clicked(GtkButton *b, gpointer data);
static void on_open_homepage(GtkButton *b, gpointer data);
static void on_copy_name(GtkButton *b, gpointer data);
static void on_show_copyright(GtkButton *b, gpointer data);
static void on_install_file_clicked(GtkButton *b, gpointer data);
static void on_row_activated(GtkListBox *list, GtkListBoxRow *row, gpointer data);
static void on_nav_clicked(GtkButton *b, gpointer data);
static HdeStoreVerify verify_for_source(const HdeStoreSourceEntry *s);

/* ------------------------------------------------------------------ small helpers */

static gboolean ui_is_vietnamese(void)
{
    static int cached = -1;
    if (cached >= 0) return cached;
    /* LANGUAGE is a preference list ("vi:en"); the first entry decides.  LC_ALL, LC_MESSAGES and LANG are the
     * single locale of the session, in the order glibc gives them.  hde_store_speak_vietnamese() has the rule
     * itself (and its tests): Vietnamese unless English is asked for by name. */
    const char *lang = g_getenv("LANGUAGE");
    if (!lang || !*lang) {
        lang = g_getenv("LC_ALL");
        if (!lang || !*lang) lang = g_getenv("LC_MESSAGES");
        if (!lang || !*lang) lang = g_getenv("LANG");
    }
    cached = hde_store_speak_vietnamese(lang, g_getenv("HDE_STORE_LANG"));
    return cached;
}

/* The Store speaks Vietnamese unless the session asks for English (or HDE_STORE_LANG says otherwise).
 * The order of the arguments is English first, Vietnamese second — the Vietnamese is what is shown by default. */
static const char *tr(const char *en, const char *vi)
{
    return ui_is_vietnamese() ? vi : en;
}

static void set_status(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
static void set_status(const char *fmt, ...)
{
    if (!status_label) return;
    va_list ap;
    va_start(ap, fmt);
    char *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    gtk_label_set_text(GTK_LABEL(status_label), msg);
    g_free(msg);
}

static void set_busy(gboolean busy)
{
    if (!busy_spinner) return;
    if (busy) {
        gtk_widget_show(busy_spinner);
        gtk_spinner_start(GTK_SPINNER(busy_spinner));
    } else {
        gtk_spinner_stop(GTK_SPINNER(busy_spinner));
        gtk_widget_hide(busy_spinner);
    }
}

static GtkWidget *label_with_class(const char *text, const char *css)
{
    GtkWidget *l = gtk_label_new(text ? text : "");
    gtk_label_set_xalign(GTK_LABEL(l), 0.0);
    if (css) gtk_style_context_add_class(gtk_widget_get_style_context(l), css);
    return l;
}

static GtkWidget *badge(const char *text, const char *kind)
{
    GtkWidget *l = label_with_class(text, "store-badge");
    GtkStyleContext *sc = gtk_widget_get_style_context(l);
    if (kind) gtk_style_context_add_class(sc, kind);
    gtk_widget_set_valign(l, GTK_ALIGN_CENTER);
    return l;
}

static const char *badge_kind_for_state(HdeStoreState s)
{
    switch (s) {
    case HDE_STORE_STATE_INSTALLED: return "store-badge-installed";
    case HDE_STORE_STATE_UPGRADABLE: return "store-badge-update";
    case HDE_STORE_STATE_BROKEN: return "store-badge-error";
    case HDE_STORE_STATE_RESIDUAL: return "store-badge-warn";
    default: return "store-badge-plain";
    }
}

static const char *badge_kind_for_verify(HdeStoreVerify v)
{
    switch (v) {
    case HDE_STORE_VERIFY_VERIFIED: return "store-badge-ok";
    case HDE_STORE_VERIFY_THIRD_PARTY: return "store-badge-warn";
    default: return "store-badge-error";
    }
}

/* How far a source of this machine can be trusted: a suite and a host of the distribution itself is
 * verified; anything else somebody added is third party; a Flatpak is outside the .deb trust chain. */
static HdeStoreVerify verify_for_source(const HdeStoreSourceEntry *s)
{
    if (!s) return HDE_STORE_VERIFY_UNKNOWN;
    if (s->flatpak) return HDE_STORE_VERIFY_UNVERIFIED;
    if (!s->enabled) return HDE_STORE_VERIFY_UNKNOWN;

    const char *host = s->uri;
    const char *sep = strstr(host, "://");
    if (sep) host = sep + 3;
    static const char *const known_hosts[] = {
        "deb.debian.org", "security.debian.org", "ftp.debian.org", "deb-mirror",
        "archive.ubuntu.com", "security.ubuntu.com", "ports.ubuntu.com", "mirrors.",
        "hyggshi", "download.opensuse.org", NULL
    };
    int host_known = 0;
    for (int i = 0; known_hosts[i]; i++)
        if (strstr(host, known_hosts[i])) { host_known = 1; break; }

    char suite[96];
    g_strlcpy(suite, s->suite, sizeof suite);
    char *space = strchr(suite, ' ');
    if (space) *space = 0;                        /* "trixie trixie-updates": the first one decides */
    int suite_known = hde_store_verify_for(suite, HDE_STORE_SOURCE_DEB) == HDE_STORE_VERIFY_VERIFIED;

    return (host_known && suite_known) ? HDE_STORE_VERIFY_VERIFIED : HDE_STORE_VERIFY_THIRD_PARTY;
}

/* ------------------------------------------------------------------ running tools */

static gchar **dup_argv(const char *const *argv)
{
    gsize n = 0;
    if (argv) while (argv[n]) n++;
    gchar **out = g_new0(gchar *, n + 1);
    for (gsize i = 0; i < n; i++) out[i] = g_strdup(argv[i]);
    return out;
}

static gchar **store_env(void)
{
    gchar **env = g_get_environ();
    env = g_environ_setenv(env, "LC_ALL", "C", TRUE);
    env = g_environ_setenv(env, "LANGUAGE", "C", TRUE);
    env = g_environ_setenv(env, "DEBIAN_FRONTEND", "noninteractive", TRUE);
    return env;
}

/* Run a tool and return its standard output ("" when it could not run). */
static char *run_capture(const char *const *argv, int *exit_status)
{
    gchar **av = dup_argv(argv);
    gchar **env = store_env();
    gchar *out = NULL, *err = NULL;
    GError *e = NULL;
    gint status = -1;
    gboolean ok = g_spawn_sync(NULL, av, env, G_SPAWN_SEARCH_PATH, NULL, NULL, &out, &err, &status, &e);
    g_strfreev(av);
    g_strfreev(env);
    if (exit_status) *exit_status = ok ? status : -1;
    if (!ok) {
        g_clear_error(&e);
        g_free(out);
        out = NULL;
    }
    g_free(err);
    return out ? out : g_strdup("");
}

/* ------------------------------------------------------------------ package collections */

static GPtrArray *pkgs_new(void)
{
    return g_ptr_array_new_with_free_func(g_free);
}

static void pkgs_clear(GPtrArray **arr)
{
    if (*arr) g_ptr_array_free(*arr, TRUE);
    *arr = NULL;
}

static gint cmp_by_name(gconstpointer a, gconstpointer b)
{
    const HdeStorePackage *pa = *(HdeStorePackage * const *)a;
    const HdeStorePackage *pb = *(HdeStorePackage * const *)b;
    char fa[160], fb[160];
    hde_store_fold(pa->name, fa, sizeof fa);
    hde_store_fold(pb->name, fb, sizeof fb);
    return strcmp(fa, fb);
}

static gint cmp_by_score(gconstpointer a, gconstpointer b)
{
    const HdeStorePackage *pa = *(HdeStorePackage * const *)a;
    const HdeStorePackage *pb = *(HdeStorePackage * const *)b;
    int sa = hde_store_match(pa, search_query);
    int sb = hde_store_match(pb, search_query);
    /* Desktop applications first; technical packages remain searchable below them. */
    for (int i = 0; curated_apps[i]; i++) {
        if (!strcmp(pa->name, curated_apps[i])) sa += 8;
        if (!strcmp(pb->name, curated_apps[i])) sb += 8;
    }
    if (sa != sb) return sb - sa;
    if (pa->installed != pb->installed) return pb->installed - pa->installed;
    return cmp_by_name(a, b);
}

static void add_stanzas(const char *text, GPtrArray *out)
{
    if (!text || !out) return;
    char **chunks = g_strsplit(text, "\n\n", -1);
    for (guint i = 0; chunks[i]; i++) {
        if (!strstr(chunks[i], "Package:")) continue;
        HdeStorePackage tmp;
        if (!hde_store_parse_control_stanza(chunks[i], &tmp)) continue;
        HdeStorePackage *p = g_new0(HdeStorePackage, 1);
        *p = tmp;
        g_ptr_array_add(out, p);
    }
    g_strfreev(chunks);
}

/* ------------------------------------------------------------------ loading the machine */

static void load_installed(void)
{
    pkgs_clear(&pkgs_installed);
    pkgs_installed = pkgs_new();
    installed_kb = 0;

    if (by_name) g_hash_table_destroy(by_name);
    by_name = g_hash_table_new(g_str_hash, g_str_equal);

    /* apt knows where each installed package came from ("trixie", "now/local", …) and which ones were
     * pulled in as dependencies; dpkg-query has the summaries and the sizes. */
    GHashTable *origins = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    char *apt_out = run_capture((const char *const[]){ "apt", "list", "--installed", NULL }, NULL);
    char **lines = g_strsplit(apt_out, "\n", -1);
    for (guint i = 0; lines[i]; i++) {
        HdeStorePackage tmp;
        if (!hde_store_parse_apt_list_line(lines[i], &tmp)) continue;
        char *value = g_strdup_printf("%s|%d", tmp.origin, tmp.automatic);
        g_hash_table_replace(origins, g_strdup(tmp.name), value);
    }
    g_strfreev(lines);
    g_free(apt_out);

    char *dpkg_out = run_capture(HDE_STORE_CMD_DPKG_QUERY, NULL);
    lines = g_strsplit(dpkg_out, "\n", -1);
    for (guint i = 0; lines[i]; i++) {
        HdeStorePackage tmp;
        if (!hde_store_parse_dpkg_line(lines[i], &tmp)) continue;
        if (!tmp.installed && !tmp.residual) continue;
        HdeStorePackage *p = g_new0(HdeStorePackage, 1);
        *p = tmp;
        const char *where = g_hash_table_lookup(origins, p->name);
        if (where) {
            char **bits = g_strsplit(where, "|", 2);
            if (bits[0]) g_strlcpy(p->origin, bits[0], sizeof p->origin);
            p->automatic = bits[1] ? atoi(bits[1]) : 0;
            g_strfreev(bits);
        }
        p->source = (!p->origin[0] || !strcmp(p->origin, "now") || !strncmp(p->origin, "now/", 4))
                        ? HDE_STORE_SOURCE_DPKG_STATUS
                        : HDE_STORE_SOURCE_DEB;
        p->verify = hde_store_verify_for(p->origin, p->source);
        installed_kb += p->size_kb;
        g_ptr_array_add(pkgs_installed, p);
        g_hash_table_insert(by_name, p->name, p);
    }
    g_strfreev(lines);
    g_free(dpkg_out);
    g_hash_table_destroy(origins);

    g_ptr_array_sort(pkgs_installed, cmp_by_name);
}

static void load_updates(void)
{
    pkgs_clear(&pkgs_updates);
    pkgs_updates = pkgs_new();

    char *out = run_capture(HDE_STORE_CMD_UPGRADABLE, NULL);
    char **lines = g_strsplit(out, "\n", -1);
    for (guint i = 0; lines[i]; i++) {
        HdeStorePackage tmp;
        if (!hde_store_parse_apt_list_line(lines[i], &tmp)) continue;
        if (!tmp.upgradable) continue;
        HdeStorePackage *p = g_new0(HdeStorePackage, 1);
        *p = tmp;
        HdeStorePackage *known = g_hash_table_lookup(by_name, p->name);
        if (known) {   /* the summary, the section and the size of what is installed right now */
            g_strlcpy(p->summary, known->summary, sizeof p->summary);
            g_strlcpy(p->section, known->section, sizeof p->section);
            g_strlcpy(p->arch, known->arch, sizeof p->arch);
            p->size_kb = known->size_kb;
        }
        g_ptr_array_add(pkgs_updates, p);
    }
    g_strfreev(lines);
    g_free(out);
    g_ptr_array_sort(pkgs_updates, cmp_by_name);
}

static void load_flatpak(void)
{
    pkgs_clear(&pkgs_flatpak);
    pkgs_flatpak = pkgs_new();
    if (!g_find_program_in_path("flatpak")) return;

    char *out = run_capture(HDE_STORE_CMD_FLATPAK, NULL);
    char **lines = g_strsplit(out, "\n", -1);
    for (guint i = 0; lines[i]; i++) {
        HdeStorePackage tmp;
        if (!hde_store_parse_flatpak_line(lines[i], &tmp)) continue;
        HdeStorePackage *p = g_new0(HdeStorePackage, 1);
        *p = tmp;
        g_ptr_array_add(pkgs_flatpak, p);
    }
    g_strfreev(lines);
    g_free(out);
}

static void load_featured(void)
{
    pkgs_clear(&pkgs_featured);
    pkgs_featured = pkgs_new();

    const char *argv[G_N_ELEMENTS(curated_apps) + 3];
    int n = 0;
    argv[n++] = "apt-cache";
    argv[n++] = "show";
    for (int i = 0; curated_apps[i]; i++) argv[n++] = curated_apps[i];
    argv[n] = NULL;

    char *out = run_capture(argv, NULL);
    GPtrArray *found = g_ptr_array_new();          /* no free func: the pointers move from here */
    add_stanzas(out, found);
    g_free(out);

    for (guint i = 0; i < found->len; i++) {
        HdeStorePackage *p = g_ptr_array_index(found, i);
        HdeStorePackage *installed = g_hash_table_lookup(by_name, p->name);
        if (installed) {
            g_free(p);                              /* the installed copy knows more */
            p = g_new0(HdeStorePackage, 1);
            *p = *installed;
        } else {
            p->verify = hde_store_verify_for(p->origin, p->source);
        }
        g_ptr_array_add(pkgs_featured, p);
    }
    g_ptr_array_free(found, FALSE);
    g_ptr_array_sort(pkgs_featured, cmp_by_name);
}

/* ------------------------------------------------------------------ the license of a package */

/* APT does not carry the license; the copyright file of the installed package does. */
static void package_read_license(HdeStorePackage *p)
{
    if (p->license[0]) return;
    char *base = g_strdup_printf("/usr/share/doc/%s", p->name);
    char *plain = g_build_filename(base, "copyright", NULL);
    char *gz = g_build_filename(base, "copyright.gz", NULL);
    char *text = NULL;
    gsize len = 0;

    if (g_file_get_contents(plain, &text, &len, NULL)) {
        char lic[48];
        if (hde_store_license_from_copyright(text, lic, sizeof lic))
            g_strlcpy(p->license, lic, sizeof p->license);
        g_free(text);
    } else if (g_file_test(gz, G_FILE_TEST_EXISTS)) {
        const char *argv[] = { "gzip", "-dc", gz, NULL };
        char *out = run_capture(argv, NULL);
        char lic[48];
        if (out && hde_store_license_from_copyright(out, lic, sizeof lic))
            g_strlcpy(p->license, lic, sizeof p->license);
        g_free(out);
    }
    g_free(plain);
    g_free(gz);
    g_free(base);
}

/* ------------------------------------------------------------------ icons and previews
 *
 * A package name is not a picture.  What the Store shows comes from two places on the machine: the desktop
 * entries in /usr/share/applications (they say which icon the program asks for) and AppStream metadata (it
 * names the package behind an application, its cached icon and its screenshots).  hde-store-core.c parses
 * them; the code below finds them, caches them, and turns them into widgets.  When a program has no picture
 * at all the Store draws a tile with its initial instead of an empty space.
 */

static GPtrArray *store_desktops;     /* HdeStoreDesktop*, filled once from the application directories */
static GHashTable *desktop_lookup;    /* package -> HdeStoreDesktop* (or a NULL marker) */
static GHashTable *store_appstreams;  /* package -> HdeStoreAppStream* (empty when the machine has none) */
static GHashTable *store_shot_lists;  /* package -> GPtrArray of the pictures that are here now */
static GHashTable *shots_fetching;    /* package -> how many downloads are running */

/* Where an icon can be: the hicolor theme (which every theme inherits), the loose pictures of /usr/share/
 * pixmaps, and what Flatpak exports for the applications it installed. */
static GPtrArray *icon_dirs;   /* char*, built once */

static void icon_dirs_build(void)
{
    if (icon_dirs) return;
    icon_dirs = g_ptr_array_new_with_free_func(g_free);
    static const char *const sizes[] = { "256x256", "128x128", "96x96", "64x64", "48x48", "scalable", NULL };
    static const char *const themes[] = {
        "/usr/share/icons/hicolor", "/usr/local/share/icons/hicolor",
        "/var/lib/flatpak/exports/share/icons/hicolor", NULL
    };
    for (int t = 0; themes[t]; t++)
        for (int s = 0; sizes[s]; s++) {
            char *dir = g_strdup_printf("%s/%s/apps", themes[t], sizes[s]);
            g_ptr_array_add(icon_dirs, dir);
        }
    g_ptr_array_add(icon_dirs, g_build_filename("/usr/share", "pixmaps", NULL));
    g_ptr_array_add(icon_dirs, g_build_filename("/usr/local/share", "pixmaps", NULL));

    char *user_data = g_build_filename(g_get_user_data_dir(), "flatpak", "exports", "share", "icons",
                                       "hicolor", NULL);
    for (int s = 0; sizes[s]; s++) {
        char *dir = g_strdup_printf("%s/%s/apps", user_data, sizes[s]);
        g_ptr_array_add(icon_dirs, dir);
    }
    g_free(user_data);
}

static const char *const *store_icon_dirs(void)
{
    icon_dirs_build();
    /* hde_store_icon_file() walks a NULL-terminated array: keep one behind the last element */
    if (!icon_dirs->len || g_ptr_array_index(icon_dirs, icon_dirs->len - 1) != NULL)
        g_ptr_array_add(icon_dirs, NULL);
    return (const char *const *)icon_dirs->pdata;
}

static const char *const store_desktop_dirs[] = {
    "/usr/share/applications",
    "/usr/local/share/applications",
    "/var/lib/flatpak/exports/share/applications",
    NULL                                  /* the user's own entries are added at load time */
};

static void store_desktops_load(void)
{
    if (store_desktops) return;
    store_desktops = g_ptr_array_new_with_free_func(g_free);
    desktop_lookup = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    char *user_dir = g_build_filename(g_get_user_data_dir(), "applications", NULL);
    char *flatpak_dir = g_build_filename(g_get_user_data_dir(), "flatpak", "exports", "share",
                                         "applications", NULL);
    const char *dirs[6];
    int n = 0;
    for (int i = 0; store_desktop_dirs[i]; i++) dirs[n++] = store_desktop_dirs[i];
    dirs[n++] = flatpak_dir;
    dirs[n++] = user_dir;                 /* last: the user's own entry wins over the system one */
    dirs[n] = NULL;

    for (int i = 0; dirs[i]; i++) {
        GDir *d = g_dir_open(dirs[i], 0, NULL);
        if (!d) continue;
        const char *name;
        while ((name = g_dir_read_name(d)) != NULL) {
            if (!g_str_has_suffix(name, ".desktop")) continue;
            char *path = g_build_filename(dirs[i], name, NULL);
            char *text = NULL;
            if (g_file_get_contents(path, &text, NULL, NULL)) {
                HdeStoreDesktop *de = g_new0(HdeStoreDesktop, 1);
                if (hde_store_parse_desktop(text, de)) {
                    hde_store_desktop_id(path, de);
                    if (!de->name[0]) g_strlcpy(de->name, de->app_id, sizeof de->name);
                    g_ptr_array_add(store_desktops, de);
                } else {
                    g_free(de);
                }
                g_free(text);
            }
            g_free(path);
        }
        g_dir_close(d);
    }
    g_free(flatpak_dir);
    g_free(user_dir);
}

/* The desktop entry of a package: the one that says X-HDE-Package= wins, then any that matches the name. */
static const HdeStoreDesktop *desktop_for(const char *package)
{
    store_desktops_load();
    gpointer cached = NULL;
    if (g_hash_table_lookup_extended(desktop_lookup, package, NULL, &cached)) return cached;

    const HdeStoreDesktop *fallback = NULL;
    if (store_desktops) {
        for (guint i = 0; i < store_desktops->len; i++) {
            const HdeStoreDesktop *de = g_ptr_array_index(store_desktops, i);
            if (!hde_store_desktop_matches_package(de, package)) continue;
            if (de->package[0]) { fallback = de; break; }
            if (!fallback) fallback = de;
        }
    }
    g_hash_table_insert(desktop_lookup, g_strdup(package), (gpointer)fallback);
    return fallback;
}

static gboolean appstream_belongs_to(const HdeStoreAppStream *as, const char *package)
{
    if (as->package[0]) return strcmp(as->package, package) == 0;
    if (!as->app_id[0]) return FALSE;
    HdeStoreDesktop d;
    memset(&d, 0, sizeof d);
    g_strlcpy(d.app_id, as->app_id, sizeof d.app_id);
    return hde_store_desktop_matches_package(&d, package);
}

/* Every AppStream file of the machine, read once.  The catalogue is a handful of large files, so the index
 * only remembers which file names which package; the file itself is read when a page asks for it. */
static const char *const appstream_dirs[] = {
    "/usr/share/metadata", "/usr/share/appdata",
    "/var/lib/app-info/xmls", "/var/lib/swcatalog/xmls", NULL
};

static GHashTable *appstream_index;   /* package -> path of the file that names it */
static GHashTable *appstream_texts;   /* path -> the text of that file, a few of them */

static char *read_any(const char *path)
{
    char *text = NULL;
    if (g_str_has_suffix(path, ".gz")) {
        const char *argv[] = { "gzip", "-dc", path, NULL };
        text = run_capture(argv, NULL);
    } else {
        g_file_get_contents(path, &text, NULL, NULL);
    }
    return text;
}

static char *appstream_text(const char *path)
{
    if (!appstream_texts)
        appstream_texts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    char *cached = g_hash_table_lookup(appstream_texts, path);
    if (cached) return cached;
    if (g_hash_table_size(appstream_texts) >= 4) g_hash_table_remove_all(appstream_texts);
    char *text = read_any(path);
    if (!text) return NULL;
    g_hash_table_insert(appstream_texts, g_strdup(path), text);
    return text;
}

static void appstream_index_build(void)
{
    if (appstream_index) return;
    appstream_index = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

    for (int i = 0; appstream_dirs[i]; i++) {
        GDir *d = g_dir_open(appstream_dirs[i], 0, NULL);
        if (!d) continue;
        const char *name;
        while ((name = g_dir_read_name(d)) != NULL) {
            if (!g_str_has_suffix(name, ".xml") && !g_str_has_suffix(name, ".xml.gz")) continue;
            char *path = g_build_filename(appstream_dirs[i], name, NULL);
            char *text = appstream_text(path);
            if (text) {
                for (const char *p = strstr(text, "<pkgname>"); p; p = strstr(p + 1, "<pkgname>")) {
                    const char *begin = p + strlen("<pkgname>");
                    const char *end = strstr(begin, "</pkgname>");
                    if (!end) break;
                    char pkg[96];
                    size_t n = (size_t)(end - begin);
                    if (n >= sizeof pkg) n = sizeof pkg - 1;
                    memcpy(pkg, begin, n);
                    pkg[n] = 0;
                    if (pkg[0] && !g_hash_table_contains(appstream_index, pkg))
                        g_hash_table_insert(appstream_index, g_strdup(pkg), g_strdup(path));
                }
            }
            g_free(path);
        }
        g_dir_close(d);
    }
}

/* The AppStream component of a package, read once.  Empty when this machine has no catalogue (a fresh
 * install without `apt update` has none), which is not an error: the icon then comes from the .desktop. */
static HdeStoreAppStream *appstream_for(const char *package)
{
    if (!store_appstreams)
        store_appstreams = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    HdeStoreAppStream *cached = g_hash_table_lookup(store_appstreams, package);
    if (cached) return cached;

    appstream_index_build();
    HdeStoreAppStream *found = NULL;

    const char *path = g_hash_table_lookup(appstream_index, package);
    if (path) {
        char *text = appstream_text(path);
        if (text) {
            HdeStoreAppStream *as = g_new0(HdeStoreAppStream, 1);
            if (hde_store_parse_appstream(text, package, as) && appstream_belongs_to(as, package))
                found = as;
            else
                g_free(as);
        }
    }

    /* An appdata file of a single application does not have to name its package: look inside those too. */
    for (int i = 0; appstream_dirs[i] && !found; i++) {
        GDir *d = g_dir_open(appstream_dirs[i], 0, NULL);
        if (!d) continue;
        const char *name;
        while ((name = g_dir_read_name(d)) != NULL && !found) {
            if (!g_str_has_suffix(name, ".xml")) continue;      /* the gz files are the catalogue, indexed above */
            char *file = g_build_filename(appstream_dirs[i], name, NULL);
            if (g_strcmp0(file, path) != 0) {
                char *text = appstream_text(file);
                if (text) {
                    HdeStoreAppStream *as = g_new0(HdeStoreAppStream, 1);
                    if (hde_store_parse_appstream(text, package, as) && appstream_belongs_to(as, package))
                        found = as;
                    else
                        g_free(as);
                }
            }
            g_free(file);
        }
        g_dir_close(d);
    }

    g_hash_table_insert(store_appstreams, g_strdup(package), found ? found : g_new0(HdeStoreAppStream, 1));
    return g_hash_table_lookup(store_appstreams, package);
}

/* The first UTF-8 character of a name, in upper case when it is ASCII — what goes in the tile. */
static void initial_of(const char *text, char *out, size_t outsz)
{
    if (outsz < 2) return;
    out[0] = 0;
    if (!text || !*text) { g_strlcpy(out, "?", outsz); return; }
    unsigned char c = (unsigned char)text[0];
    size_t len = (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0) ? 2 : ((c & 0xF0) == 0xE0) ? 3 : 4;
    if (len >= outsz) len = outsz - 1;
    memcpy(out, text, len);
    out[len] = 0;
    if (len == 1 && out[0] >= 'a' && out[0] <= 'z') out[0] = (char)(out[0] - 'a' + 'A');
}

static GtkWidget *tile_icon(const char *name, int pixel)
{
    char letter[16];
    initial_of(name, letter, sizeof letter);

    GtkWidget *tile = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(tile, pixel, pixel);
    gtk_widget_set_valign(tile, GTK_ALIGN_CENTER);
    gtk_style_context_add_class(gtk_widget_get_style_context(tile), "store-tile");
    GtkWidget *l = gtk_label_new(letter);
    gtk_style_context_add_class(gtk_widget_get_style_context(l), "store-tile-letter");
    gtk_widget_set_halign(l, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(l, GTK_ALIGN_CENTER);
    gtk_widget_set_vexpand(l, TRUE);
    gtk_box_pack_start(GTK_BOX(tile), l, TRUE, TRUE, 0);
    return tile;
}

static GtkWidget *icon_from_name(const char *name, int pixel)
{
    GtkWidget *img = gtk_image_new_from_icon_name(name, GTK_ICON_SIZE_DIALOG);
    gtk_image_set_pixel_size(GTK_IMAGE(img), pixel);
    return img;
}

/* The icon of a package, at `pixel` pixels.  `deep` also reads the AppStream catalogue (slow: it is only
 * worth it for the page that is open, not for every row of a list). */
static GtkWidget *package_icon(const HdeStorePackage *p, int pixel, int deep)
{
    const char *names[6] = { NULL, NULL, NULL, NULL, NULL, NULL };
    int n = 0;
    const HdeStoreAppStream *as = deep ? appstream_for(p->name) : NULL;
    const HdeStoreDesktop *de = desktop_for(p->name);

    if (as && as->icon[0]) names[n++] = as->icon;
    if (de && de->icon[0]) names[n++] = de->icon;
    if (p->app_id[0] && strcmp(p->app_id, p->name)) names[n++] = p->app_id;
    names[n++] = p->name;

    char path[600];
    if (hde_store_icon_file(store_icon_dirs(), (const char *const *)names, path, sizeof path)) {
        GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_scale(path, pixel, pixel, TRUE, NULL);
        if (pb) {
            GtkWidget *img = gtk_image_new_from_pixbuf(pb);
            g_object_unref(pb);
            return img;
        }
    }
    GtkIconTheme *theme = gtk_icon_theme_get_default();
    for (int i = 0; i < n; i++) {
        if (names[i][0] == '/' || !names[i][0]) continue;
        if (gtk_icon_theme_has_icon(theme, names[i])) return icon_from_name(names[i], pixel);
    }
    return tile_icon((de && de->name[0]) ? de->name : p->name, pixel);
}

/* ---- the pictures the application ships, or the ones the Store fetched for it ---- */

static void shot_list_free(gpointer data)
{
    g_ptr_array_free(data, TRUE);
}

static GPtrArray *shots_for(const HdeStorePackage *p)
{
    if (!store_shot_lists)
        store_shot_lists = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, shot_list_free);
    GPtrArray *list = g_hash_table_lookup(store_shot_lists, p->name);
    if (list) return list;

    list = g_ptr_array_new_with_free_func(g_free);
    const HdeStoreAppStream *as = appstream_for(p->name);
    if (as) {
        for (int i = 0; i < as->n_shots; i++)
            if (as->shots[i].path[0] && g_file_test(as->shots[i].path, G_FILE_TEST_EXISTS))
                g_ptr_array_add(list, g_strdup(as->shots[i].path));
    }

    const char *bases[] = { "/usr/share", "/usr/local/share", "/usr/share/doc", NULL };
    const char *subs[] = { "screenshots", "screenshot", NULL };
    for (int b = 0; bases[b] && list->len < HDE_STORE_MAX_SHOTS; b++) {
        for (int u = 0; subs[u] && list->len < HDE_STORE_MAX_SHOTS; u++) {
            char *dir = g_build_filename(bases[b], p->name, subs[u], NULL);
            HdeStoreShot local[HDE_STORE_MAX_SHOTS];
            int got = hde_store_shots_in_dir(dir, local, HDE_STORE_MAX_SHOTS);
            for (int j = 0; j < got && list->len < HDE_STORE_MAX_SHOTS; j++)
                g_ptr_array_add(list, g_strdup(local[j].path));
            g_free(dir);
        }
    }
    char cache[600];
    hde_store_shots_cache_dir(g_getenv("XDG_CACHE_HOME"), p->name, cache, sizeof cache);
    if (list->len < HDE_STORE_MAX_SHOTS) {
        HdeStoreShot local[HDE_STORE_MAX_SHOTS];
        int got = hde_store_shots_in_dir(cache, local, HDE_STORE_MAX_SHOTS);
        for (int j = 0; j < got && list->len < HDE_STORE_MAX_SHOTS; j++)
            g_ptr_array_add(list, g_strdup(local[j].path));
    }
    g_hash_table_insert(store_shot_lists, g_strdup(p->name), list);
    return list;
}

static void on_shot_fetched(GPid pid, gint status, gpointer data)
{
    (void)status;
    char *package = data;
    g_spawn_close_pid(pid);

    int left = GPOINTER_TO_INT(g_hash_table_lookup(shots_fetching, package)) - 1;
    if (left > 0) {
        g_hash_table_insert(shots_fetching, g_strdup(package), GINT_TO_POINTER(left));
    } else {
        g_hash_table_remove(shots_fetching, package);
        if (store_shot_lists) g_hash_table_remove(store_shot_lists, package);   /* read the cache again */
        if (detail && !strcmp(detail->name, package)) rebuild_detail();
    }
    g_free(package);
}

/* Screenshots on a Debian machine are mostly URLs.  Fetch the first ones into the Store's cache with curl or
 * wget (the Store already runs apt-get and gzip the same way) and rebuild the page when they are here. */
static void fetch_shots(const HdeStorePackage *p)
{
    if (!store_appstreams) return;
    if (!shots_fetching) shots_fetching = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    if (g_hash_table_lookup(shots_fetching, p->name)) return;      /* already on their way */

    HdeStoreAppStream *as = appstream_for(p->name);
    if (!as || !as->n_shots) return;

    const char *tool = g_find_program_in_path("curl") ? "curl"
                     : g_find_program_in_path("wget") ? "wget" : NULL;
    if (!tool) return;

    char dir[600];
    hde_store_shots_cache_dir(g_getenv("XDG_CACHE_HOME"), p->name, dir, sizeof dir);
    if (g_mkdir_with_parents(dir, 0700) != 0) return;
    gboolean curl = !strcmp(tool, "curl");

    int started = 0;
    for (int i = 0; i < as->n_shots && started < 2; i++) {
        if (!as->shots[i].url[0]) continue;
        char *out = g_strdup_printf("%s/%d.png", dir, i + 1);
        if (g_file_test(out, G_FILE_TEST_EXISTS)) { g_free(out); continue; }

        const char *argv[10];
        int n = 0;
        argv[n++] = tool;
        if (curl) {
            argv[n++] = "-fsSL"; argv[n++] = "--max-time"; argv[n++] = "25"; argv[n++] = "-o";
        } else {
            argv[n++] = "-q"; argv[n++] = "-T"; argv[n++] = "25"; argv[n++] = "-O";
        }
        argv[n++] = out;
        argv[n++] = as->shots[i].url;
        argv[n] = NULL;

        GPid pid = 0;
        if (g_spawn_async(NULL, (gchar **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                          NULL, NULL, &pid, NULL)) {
            int running = GPOINTER_TO_INT(g_hash_table_lookup(shots_fetching, p->name)) + 1;
            g_hash_table_insert(shots_fetching, g_strdup(p->name), GINT_TO_POINTER(running));
            g_child_watch_add(pid, on_shot_fetched, g_strdup(p->name));
            started++;
        }
        g_free(out);
    }
}

static void on_shot_clicked(GtkButton *button, gpointer data)
{
    (void)data;
    const char *path = g_object_get_data(G_OBJECT(button), "store-shot-path");
    if (!path) return;
    GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_scale(path, 1200, 800, TRUE, NULL);
    if (!pb) return;

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), tr("Screenshot", "Ảnh xem trước"));
    gtk_window_set_default_size(GTK_WINDOW(win), gdk_pixbuf_get_width(pb) + 24, gdk_pixbuf_get_height(pb) + 24);
    if (main_win) gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(main_win));
    GtkWidget *img = gtk_image_new_from_pixbuf(pb);
    g_object_unref(pb);
    gtk_container_add(GTK_CONTAINER(win), img);
    gtk_widget_show_all(win);
}

/* The row of pictures of the detail page.  NULL when the application has no picture at all. */
static GtkWidget *shots_strip(HdeStorePackage *p)
{
    GPtrArray *shots = shots_for(p);
    if (!shots->len) {
        const HdeStoreAppStream *as = appstream_for(p->name);
        if (as && as->n_shots) {
            fetch_shots(p);
            return label_with_class(tr("Fetching the screenshots of this application…",
                                       "Đang tải ảnh xem trước của ứng dụng này…"), "store-row-sub");
        }
        return NULL;
    }

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    gtk_widget_set_size_request(scroll, -1, 420);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(box), 2);
    for (guint i = 0; i < shots->len; i++) {
        const char *path = g_ptr_array_index(shots, i);
        GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_scale(path, 800, 400, TRUE, NULL);
        if (!pb) continue;                     /* not a picture after all (a broken download) */
        GtkWidget *button = gtk_button_new();
        gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
        gtk_style_context_add_class(gtk_widget_get_style_context(button), "store-shot");
        gtk_widget_set_tooltip_text(button, tr("Click to see it larger", "Bấm để xem lớn hơn"));
        gtk_container_add(GTK_CONTAINER(button), gtk_image_new_from_pixbuf(pb));
        g_object_unref(pb);
        g_object_set_data_full(G_OBJECT(button), "store-shot-path", g_strdup(path), g_free);
        g_signal_connect(button, "clicked", G_CALLBACK(on_shot_clicked), NULL);
        gtk_box_pack_start(GTK_BOX(box), button, FALSE, FALSE, 0);
    }
    gtk_container_add(GTK_CONTAINER(scroll), box);
    return scroll;
}

/* ------------------------------------------------------------------ rows */

static GtkWidget *package_row(HdeStorePackage *p)
{
    GtkWidget *row = gtk_list_box_row_new();
    g_object_set_data(G_OBJECT(row), "store-pkg", p);

    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(h), 9);
    gtk_widget_set_valign(h, GTK_ALIGN_CENTER);

    GtkWidget *icon = package_icon(p, 34, 0);
    gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(h), icon, FALSE, FALSE, 0);

    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    gtk_widget_set_valign(v, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(v, TRUE);

    const HdeStoreDesktop *de = desktop_for(p->name);
    const char *title = de && de->name[0] ? de->name : p->name;
    GtkWidget *title_l = label_with_class(title, "store-row-title");
    gtk_label_set_ellipsize(GTK_LABEL(title_l), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(title_l), 58);
    gtk_box_pack_start(GTK_BOX(v), title_l, FALSE, FALSE, 0);

    char *sub_text = g_strdup_printf("%s · %s", p->name, p->version[0] ? p->version : "—");
    GtkWidget *sub = label_with_class(sub_text, "store-row-sub");
    gtk_label_set_ellipsize(GTK_LABEL(sub), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(sub), 72);
    gtk_box_pack_start(GTK_BOX(v), sub, FALSE, FALSE, 0);
    g_free(sub_text);
    gtk_box_pack_start(GTK_BOX(h), v, TRUE, TRUE, 0);

    if (p->state != HDE_STORE_STATE_NOT_INSTALLED)
        gtk_box_pack_end(GTK_BOX(h), badge(hde_store_state_title(p->state, ui_is_vietnamese()), badge_kind_for_state(p->state)),
                         FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(row), h);
    gtk_widget_show_all(row);
    return row;
}

static void list_fill(GtkWidget *list, GPtrArray *pkgs)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(list));
    for (GList *l = children; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(children);
    for (guint i = 0; pkgs && i < pkgs->len; i++)
        gtk_list_box_insert(GTK_LIST_BOX(list), package_row(g_ptr_array_index(pkgs, i)), -1);
    gtk_widget_show_all(list);
}

static GtkWidget *empty_label(const char *text)
{
    GtkWidget *l = label_with_class(text, "store-empty");
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 64);
    gtk_widget_set_margin_top(l, 18);
    gtk_widget_set_margin_bottom(l, 18);
    gtk_widget_set_halign(l, GTK_ALIGN_CENTER);
    gtk_label_set_xalign(GTK_LABEL(l), 0.5);
    return l;
}

/* ------------------------------------------------------------------ the job dialog */

typedef struct {
    GtkWidget *dialog;
    GtkWidget *text_view;
    GtkWidget *spinner;
    GString *log;
    GPid pid;
    GIOChannel *out_ch;
    GIOChannel *err_ch;
    guint timer;
    guint child_id;
    gboolean finished;
    int exit_status;
} StoreJob;

static void job_append(StoreJob *j, const char *text)
{
    if (!text || !*text || !j->log) return;
    g_string_append(j->log, text);
    if (!j->text_view) return;
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(j->text_view));
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buf, &end);
    gtk_text_buffer_insert(buf, &end, text, -1);
    GtkTextMark *mark = gtk_text_buffer_create_mark(buf, NULL, &end, FALSE);
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(j->text_view), mark);
    gtk_text_buffer_delete_mark(buf, mark);
}

static void job_drain(StoreJob *j)
{
    GIOChannel *channels[2] = { j->out_ch, j->err_ch };
    for (int k = 0; k < 2; k++) {
        GIOChannel *ch = channels[k];
        if (!ch) continue;
        for (;;) {
            gchar buf[4096];
            gsize got = 0;
            GError *e = NULL;
            GIOStatus st = g_io_channel_read_chars(ch, buf, sizeof buf - 1, &got, &e);
            if (got) {
                buf[got] = 0;
                job_append(j, buf);
            }
            g_clear_error(&e);
            if (st != G_IO_STATUS_NORMAL) break;
        }
    }
}

static gboolean job_tick(gpointer data)
{
    StoreJob *j = data;
    job_drain(j);
    return j->finished ? G_SOURCE_REMOVE : G_SOURCE_CONTINUE;
}

static void job_close_channels(StoreJob *j)
{
    if (j->out_ch) {
        g_io_channel_unref(j->out_ch);
        j->out_ch = NULL;
    }
    if (j->err_ch) {
        g_io_channel_unref(j->err_ch);
        j->err_ch = NULL;
    }
}

static void job_finish_ui(StoreJob *j)
{
    if (j->spinner) {
        gtk_spinner_stop(GTK_SPINNER(j->spinner));
        gtk_widget_hide(j->spinner);
    }
    if (!j->dialog) return;
    GtkWidget *btn = gtk_dialog_get_widget_for_response(GTK_DIALOG(j->dialog), GTK_RESPONSE_CANCEL);
    if (btn) gtk_button_set_label(GTK_BUTTON(btn), tr("Close", "Đóng"));
    gtk_widget_grab_focus(btn);
    job_append(j, j->exit_status == 0 ? tr("\n\nFinished.\n", "\n\nXong.\n")
                                      : tr("\n\nIt did not work — the reason is above.\n",
                                           "\n\nKhông thành công — lý do ở trên.\n"));
}

static void job_child_cb(GPid pid, gint status, gpointer data)
{
    StoreJob *j = data;
    j->child_id = 0;
    j->finished = TRUE;
    if (WIFEXITED(status)) j->exit_status = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) j->exit_status = 128 + WTERMSIG(status);
    else j->exit_status = -1;

    job_drain(j);
    job_close_channels(j);
    if (j->timer) {
        g_source_remove(j->timer);
        j->timer = 0;
    }
    g_spawn_close_pid(pid);
    job_finish_ui(j);
}

/* Do what a tool does, on screen, with its output.  TRUE when it exited with 0. */
static gboolean store_job_run(const char *title, const char *headline, const char *const *argv)
{
    StoreJob *j = g_new0(StoreJob, 1);
    j->log = g_string_new(NULL);

    j->dialog = gtk_dialog_new_with_buttons(title, GTK_WINDOW(main_win),
                                            GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                            tr("Cancel", "Huỷ"), GTK_RESPONSE_CANCEL, NULL);
    gtk_window_set_default_size(GTK_WINDOW(j->dialog), 640, 430);
    gtk_dialog_set_default_response(GTK_DIALOG(j->dialog), GTK_RESPONSE_CANCEL);

    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(j->dialog));
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 14);
    gtk_container_add(GTK_CONTAINER(area), box);

    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    j->spinner = gtk_spinner_new();
    gtk_spinner_start(GTK_SPINNER(j->spinner));
    gtk_box_pack_start(GTK_BOX(head), j->spinner, FALSE, FALSE, 0);
    GtkWidget *headline_l = label_with_class(headline, "store-job-headline");
    gtk_label_set_line_wrap(GTK_LABEL(headline_l), TRUE);
    gtk_box_pack_start(GTK_BOX(head), headline_l, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), head, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box),
                       label_with_class(tr("If administrator permission is needed, the system will ask outside this window.",
                                           "Nếu cần quyền quản trị, hệ thống sẽ hỏi ở ngoài cửa sổ này."),
                                        "store-row-sub"),
                       FALSE, FALSE, 0);

    j->text_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(j->text_view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(j->text_view), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(j->text_view), GTK_WRAP_WORD_CHAR);
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_vexpand(sw, TRUE);
    gtk_container_add(GTK_CONTAINER(sw), j->text_view);
    gtk_box_pack_start(GTK_BOX(box), sw, TRUE, TRUE, 0);

    gtk_widget_show_all(j->dialog);

    gchar **av = dup_argv(argv);
    gchar **env = store_env();
    gint out_fd = -1, err_fd = -1;
    GError *e = NULL;
    gboolean spawned = g_spawn_async_with_pipes(NULL, av, env,
                                                G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                                                NULL, NULL, &j->pid, NULL, &out_fd, &err_fd, &e);
    g_strfreev(av);
    g_strfreev(env);

    if (!spawned) {
        j->finished = TRUE;
        j->exit_status = -1;
        job_append(j, e ? e->message : tr("The tool could not be started.\n", "Không khởi chạy được công cụ.\n"));
        g_clear_error(&e);
        job_finish_ui(j);
        gtk_dialog_run(GTK_DIALOG(j->dialog));
        gtk_widget_destroy(j->dialog);
        g_string_free(j->log, TRUE);
        g_free(j);
        return FALSE;
    }

    j->out_ch = g_io_channel_unix_new(out_fd);
    g_io_channel_set_encoding(j->out_ch, NULL, NULL);
    g_io_channel_set_flags(j->out_ch, G_IO_FLAG_NONBLOCK, NULL);
    j->err_ch = g_io_channel_unix_new(err_fd);
    g_io_channel_set_encoding(j->err_ch, NULL, NULL);
    g_io_channel_set_flags(j->err_ch, G_IO_FLAG_NONBLOCK, NULL);

    j->timer = g_timeout_add(150, job_tick, j);
    j->child_id = g_child_watch_add(j->pid, job_child_cb, j);

    gtk_dialog_run(GTK_DIALOG(j->dialog));

    gboolean finished = j->finished;
    gboolean ok = finished && j->exit_status == 0;

    if (!finished) {
        /* Cancel: stop the tool, reap it, and let the dialog go. */
        if (j->timer) g_source_remove(j->timer);
        if (j->child_id) g_source_remove(j->child_id);
        if (j->pid > 0) {
            kill(j->pid, SIGTERM);
            g_usleep(200 * 1000);
            kill(j->pid, SIGKILL);
            waitpid(j->pid, NULL, 0);
            g_spawn_close_pid(j->pid);
        }
        job_close_channels(j);
        set_status(tr("Cancelled.", "Đã huỷ."));
    } else if (j->timer) {
        g_source_remove(j->timer);
    }

    gtk_widget_destroy(j->dialog);
    g_string_free(j->log, TRUE);
    g_free(j);
    return ok;
}

/* ------------------------------------------------------------------ pages */

static void show_page(const char *name)
{
    if (!stack) return;
    if (!strcmp(name, "detail") &&
        g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(stack)), "detail")) {
        g_free(detail_return_page);
        detail_return_page = g_strdup(gtk_stack_get_visible_child_name(GTK_STACK(stack)));
    }
    if (back_button) gtk_widget_set_sensitive(back_button, !strcmp(name, "detail"));
    gtk_stack_set_visible_child_name(GTK_STACK(stack), name);
}

static void on_back_clicked(GtkButton *button, gpointer data)
{
    (void)button;
    (void)data;
    show_page(detail_return_page ? detail_return_page : "discover");
}

static GtkWidget *page_scroller(GtkWidget **inner_out)
{
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    GtkWidget *inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_container_set_border_width(GTK_CONTAINER(inner), 22);
    gtk_container_add(GTK_CONTAINER(sw), inner);
    if (inner_out) *inner_out = inner;
    return sw;
}

static GtkWidget *card(void)
{
    GtkWidget *b = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_style_context_add_class(gtk_widget_get_style_context(b), "store-card");
    gtk_container_set_border_width(GTK_CONTAINER(b), 14);
    return b;
}

static GtkWidget *section_heading(const char *text)
{
    GtkWidget *l = label_with_class(text, "store-section");
    gtk_widget_set_margin_top(l, 6);
    return l;
}

static GtkWidget *info_row(const char *key, const char *value)
{
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_container_set_border_width(GTK_CONTAINER(h), 3);
    GtkWidget *k = label_with_class(key, "store-info-key");
    gtk_widget_set_size_request(k, 150, -1);
    gtk_widget_set_valign(k, GTK_ALIGN_START);
    GtkWidget *v = label_with_class(value && *value ? value : "—", "store-info-value");
    gtk_label_set_line_wrap(GTK_LABEL(v), TRUE);
    gtk_label_set_selectable(GTK_LABEL(v), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(v), 70);
    gtk_widget_set_hexpand(v, TRUE);
    gtk_box_pack_start(GTK_BOX(h), k, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(h), v, TRUE, TRUE, 0);
    return h;
}

/* ------------------------------------------------------------------ a .deb, checked before it is installed */

static void file_check_run(const char *path)
{
    memset(&file_check, 0, sizeof file_check);
    file_check_done = FALSE;
    if (!path || !*path) return;
    /* 0 when the file could be read (whatever was found); -1 leaves a finding that says why */
    nexsdk_pkg_check(path, &file_check);
    file_check_done = TRUE;
}

static int file_check_count(NexSdkPkgStatus status)
{
    int n = 0;
    for (int i = 0; i < file_check.count; i++)
        if (file_check.findings[i].status == status) n++;
    return n;
}

static void on_file_check_details(GtkButton *b, gpointer data)
{
    (void)b;
    (void)data;
    char *text = nexsdk_pkg_report_text(&file_check);
    GtkWidget *dlg = gtk_dialog_new_with_buttons(tr("Package check", "Kiểm tra gói"), GTK_WINDOW(main_win),
                                                 GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                 tr("_Close", "_Đóng"), GTK_RESPONSE_CLOSE, NULL);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 760, 520);
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    GtkWidget *view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)), text ? text : "", -1);
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(sw), view);
    gtk_box_pack_start(GTK_BOX(area), sw, TRUE, TRUE, 0);
    gtk_widget_show_all(dlg);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    g_free(text);
}

/* ------------------------------------------------------------------ detail page */

static void rebuild_detail(void)
{
    if (!detail_inner) return;
    GList *children = gtk_container_get_children(GTK_CONTAINER(detail_inner));
    for (GList *l = children; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(children);

    if (!detail) {
        gtk_box_pack_start(GTK_BOX(detail_inner),
                           empty_label(tr("Choose something in the list to see what it is and where it comes from.",
                                          "Chọn một mục trong danh sách để xem nó là gì và đến từ đâu.")),
                           FALSE, FALSE, 0);
        gtk_widget_show_all(detail_inner);
        return;
    }

    HdeStorePackage *p = detail;
    package_read_license(p);

    /* ---- the head of the page: the package, what it is, and the one button that matters ---- */
    GtkWidget *hero = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    gtk_style_context_add_class(gtk_widget_get_style_context(hero), "store-hero");

    GtkWidget *icon = package_icon(p, 112, 1);
    gtk_widget_set_valign(icon, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(hero), icon, FALSE, FALSE, 0);

    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_hexpand(head, TRUE);
    const HdeStoreAppStream *app = appstream_for(p->name);
    const HdeStoreDesktop *entry = desktop_for(p->name);
    const char *app_name = app && app->name[0] ? app->name
                         : entry && entry->name[0] ? entry->name : p->name;
    gtk_box_pack_start(GTK_BOX(head), label_with_class(app_name, "store-h1"), FALSE, FALSE, 0);
    GtkWidget *sum = label_with_class(p->summary, "store-lead");
    gtk_label_set_line_wrap(GTK_LABEL(sum), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(sum), 62);
    gtk_box_pack_start(GTK_BOX(head), sum, FALSE, FALSE, 0);

    GtkWidget *badges = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_top(badges, 4);
    gtk_box_pack_start(GTK_BOX(badges),
                       badge(hde_store_state_title(p->state, ui_is_vietnamese()), badge_kind_for_state(p->state)),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(badges),
                       badge(hde_store_verify_title(p->verify, ui_is_vietnamese()), badge_kind_for_verify(p->verify)),
                       FALSE, FALSE, 0);
    if (p->license[0])
        gtk_box_pack_start(GTK_BOX(badges), badge(p->license, "store-badge-plain"), FALSE, FALSE, 0);
    if (p->automatic)
        gtk_box_pack_start(GTK_BOX(badges), badge(tr("dependency", "phụ thuộc"), "store-badge-plain"),
                           FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(head), badges, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hero), head, TRUE, TRUE, 0);

    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_valign(actions, GTK_ALIGN_START);
    if (p->source == HDE_STORE_SOURCE_FLATPAK) {
        GtkWidget *b = gtk_button_new_with_label(tr("Read-only (Flatpak)", "Chỉ đọc (Flatpak)"));
        gtk_widget_set_sensitive(b, FALSE);
        gtk_widget_set_tooltip_text(b, tr("Flatpak support is being researched; this version only lists them.",
                                          "Flatpak đang được nghiên cứu; bản này chỉ liệt kê."));
        gtk_box_pack_start(GTK_BOX(actions), b, FALSE, FALSE, 0);
    } else if (p->installed || p->residual) {
        if (p->upgradable) {
            GtkWidget *up = gtk_button_new_with_label(tr("Update", "Cập nhật"));
            gtk_style_context_add_class(gtk_widget_get_style_context(up), "suggested-action");
            gtk_style_context_add_class(gtk_widget_get_style_context(up), "store-pill");
            g_signal_connect(up, "clicked", G_CALLBACK(on_install_clicked), p);
            gtk_box_pack_start(GTK_BOX(actions), up, FALSE, FALSE, 0);
        }
        GtkWidget *rm = gtk_button_new_with_label(tr("Uninstall", "Gỡ bỏ"));
        gtk_style_context_add_class(gtk_widget_get_style_context(rm), "destructive-action");
        gtk_style_context_add_class(gtk_widget_get_style_context(rm), "store-pill");
        g_signal_connect(rm, "clicked", G_CALLBACK(on_remove_clicked), p);
        gtk_box_pack_start(GTK_BOX(actions), rm, FALSE, FALSE, 0);

        GtkWidget *re = gtk_button_new_with_label(tr("Reinstall", "Cài lại"));
        gtk_style_context_add_class(gtk_widget_get_style_context(re), "store-pill");
        g_signal_connect(re, "clicked", G_CALLBACK(on_install_clicked), p);
        gtk_box_pack_start(GTK_BOX(actions), re, FALSE, FALSE, 0);
    } else {
        GtkWidget *in = gtk_button_new_with_label(p->from_file ? tr("Install this file", "Cài tệp này")
                                                               : tr("Install", "Cài đặt"));
        gtk_style_context_add_class(gtk_widget_get_style_context(in), "suggested-action");
        gtk_style_context_add_class(gtk_widget_get_style_context(in), "store-pill");
        g_signal_connect(in, "clicked", G_CALLBACK(on_install_clicked), p);
        gtk_box_pack_start(GTK_BOX(actions), in, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(hero), actions, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(detail_inner), hero, FALSE, FALSE, 0);

    /* ---- the pictures of the application: what is on disk now, or what is being fetched ---- */
    gtk_box_pack_start(GTK_BOX(detail_inner), section_heading(tr("Screenshots", "Ảnh xem trước")),
                       FALSE, FALSE, 0);
    GtkWidget *strip = shots_strip(p);
    gtk_box_pack_start(GTK_BOX(detail_inner),
                       strip ? strip
                             : label_with_class(tr("This application does not come with a screenshot.",
                                                   "Ứng dụng này không kèm ảnh xem trước."),
                                                "store-row-sub"),
                       FALSE, FALSE, 0);

    /* ---- version, source, license, verification: what a store must say about a package ---- */
    gtk_box_pack_start(GTK_BOX(detail_inner), section_heading(tr("Package", "Gói phần mềm")), FALSE, FALSE, 0);
    GtkWidget *pc = card();
    gtk_box_pack_start(GTK_BOX(pc), info_row(tr("Package name", "Tên gói"), p->name), FALSE, FALSE, 0);
    char ver[200];
    if (p->candidate[0] && strcmp(p->candidate, p->version))
        g_snprintf(ver, sizeof ver, "%s  →  %s", p->version, p->candidate);
    else
        g_snprintf(ver, sizeof ver, "%s", p->version[0] ? p->version : "—");
    gtk_box_pack_start(GTK_BOX(pc), info_row(tr("Version", "Phiên bản"), ver), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(pc), info_row(tr("Architecture", "Kiến trúc"), p->arch), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(pc), info_row(tr("Category", "Danh mục"), p->section), FALSE, FALSE, 0);
    char size[32];
    hde_store_format_size(p->size_kb, size, sizeof size);
    gtk_box_pack_start(GTK_BOX(pc), info_row(tr("Installed size", "Dung lượng cài đặt"), size), FALSE, FALSE, 0);
    if (p->maintainer[0])
        gtk_box_pack_start(GTK_BOX(pc), info_row(tr("Maintainer", "Người bảo trì"), p->maintainer),
                           FALSE, FALSE, 0);
    if (p->homepage[0])
        gtk_box_pack_start(GTK_BOX(pc), info_row(tr("Homepage", "Trang chủ"), p->homepage), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(detail_inner), pc, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(detail_inner), section_heading(tr("Source and trust", "Nguồn tải và độ tin cậy")),
                       FALSE, FALSE, 0);
    GtkWidget *sc = card();
    gtk_box_pack_start(GTK_BOX(sc), info_row(tr("Downloaded from", "Nguồn tải"),
                                             p->origin[0] ? p->origin : tr("unknown", "không rõ")),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sc), info_row(tr("Source", "Loại nguồn"),
                                             hde_store_source_title(p->source, ui_is_vietnamese())),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sc), info_row(tr("License", "Giấy phép"),
                                             p->license[0] ? hde_store_license_title(p->license, ui_is_vietnamese())
                                                           : tr("not read yet", "chưa đọc được")),
                       FALSE, FALSE, 0);

    GtkWidget *verify_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(verify_row), 3);
    GtkWidget *vk = label_with_class(tr("Verification", "Xác minh"), "store-info-key");
    gtk_widget_set_size_request(vk, 150, -1);
    gtk_box_pack_start(GTK_BOX(verify_row), vk, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(verify_row),
                       badge(hde_store_verify_title(p->verify, ui_is_vietnamese()), badge_kind_for_verify(p->verify)),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sc), verify_row, FALSE, FALSE, 0);

    if (p->from_file && file_check_done) {
        int errors = file_check_count(NEXSDK_PKG_ERROR);
        int warnings = file_check_count(NEXSDK_PKG_WARN);
        char summary[240];
        if (errors)
            g_snprintf(summary, sizeof summary, tr("%d error(s) and %d warning(s) — see the details",
                                                   "%d lỗi và %d cảnh báo — xem chi tiết"), errors, warnings);
        else if (warnings)
            g_snprintf(summary, sizeof summary, tr("%d warning(s), nothing fatal", "%d cảnh báo, không có lỗi"),
                       warnings);
        else
            g_snprintf(summary, sizeof summary, "%s", tr("Nothing wrong with this package",
                                                         "Không có gì sai với gói này"));
        GtkWidget *check_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_container_set_border_width(GTK_CONTAINER(check_row), 3);
        GtkWidget *ck = label_with_class(tr("Package check", "Kiểm tra gói"), "store-info-key");
        gtk_widget_set_size_request(ck, 150, -1);
        gtk_box_pack_start(GTK_BOX(check_row), ck, FALSE, FALSE, 0);
        GtkWidget *b = gtk_button_new_with_label(summary);
        gtk_style_context_add_class(gtk_widget_get_style_context(b), "store-link");
        g_signal_connect(b, "clicked", G_CALLBACK(on_file_check_details), NULL);
        gtk_box_pack_start(GTK_BOX(check_row), b, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(sc), check_row, FALSE, FALSE, 0);
    }

    char *verify_detail = hde_store_verify_detail(p->verify, p->origin, ui_is_vietnamese());
    if (verify_detail) {
        GtkWidget *d = label_with_class(verify_detail, "store-note");
        gtk_label_set_line_wrap(GTK_LABEL(d), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(d), 76);
        gtk_widget_set_margin_start(d, 164);
        gtk_box_pack_start(GTK_BOX(sc), d, FALSE, FALSE, 0);
        g_free(verify_detail);
    }
    gtk_box_pack_start(GTK_BOX(detail_inner), sc, FALSE, FALSE, 0);

    if (p->description[0]) {
        gtk_box_pack_start(GTK_BOX(detail_inner), section_heading(tr("About", "Giới thiệu")), FALSE, FALSE, 0);
        GtkWidget *dc = card();
        GtkWidget *d = label_with_class(p->description, "store-body");
        gtk_label_set_line_wrap(GTK_LABEL(d), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(d), 84);
        gtk_label_set_selectable(GTK_LABEL(d), TRUE);
        gtk_box_pack_start(GTK_BOX(dc), d, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(detail_inner), dc, FALSE, FALSE, 0);
    }

    gtk_box_pack_start(GTK_BOX(detail_inner), section_heading(tr("More", "Thêm")), FALSE, FALSE, 0);
    GtkWidget *tools = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    if (p->homepage[0]) {
        GtkWidget *b = gtk_button_new_with_label(tr("Open homepage", "Mở trang chủ"));
        g_object_set_data_full(G_OBJECT(b), "store-uri", g_strdup(p->homepage), g_free);
        g_signal_connect(b, "clicked", G_CALLBACK(on_open_homepage), NULL);
        gtk_box_pack_start(GTK_BOX(tools), b, FALSE, FALSE, 0);
    }
    GtkWidget *copy = gtk_button_new_with_label(tr("Copy package name", "Sao chép tên gói"));
    g_signal_connect(copy, "clicked", G_CALLBACK(on_copy_name), p);
    gtk_box_pack_start(GTK_BOX(tools), copy, FALSE, FALSE, 0);
    if (p->installed) {
        GtkWidget *show = gtk_button_new_with_label(tr("Show copyright file", "Xem tệp giấy phép"));
        g_signal_connect(show, "clicked", G_CALLBACK(on_show_copyright), p);
        gtk_box_pack_start(GTK_BOX(tools), show, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(detail_inner), tools, FALSE, FALSE, 0);

    gtk_widget_show_all(detail_inner);
}

static void open_detail(const HdeStorePackage *src)
{
    if (!src) return;
    g_free(detail);
    detail = g_new0(HdeStorePackage, 1);
    *detail = *src;
    rebuild_detail();
    show_page("detail");
}

/* Copy what the database says now over the detail page (after something was installed or removed). */
static void detail_sync_from_database(void)
{
    if (!detail) return;
    HdeStorePackage *now = g_hash_table_lookup(by_name, detail->name);
    if (now) {
        char description[sizeof detail->description];
        char homepage[sizeof detail->homepage];
        char candidate[sizeof detail->candidate];
        char license[sizeof detail->license];
        g_strlcpy(description, detail->description, sizeof description);
        g_strlcpy(homepage, detail->homepage, sizeof homepage);
        g_strlcpy(candidate, detail->candidate, sizeof candidate);
        g_strlcpy(license, detail->license, sizeof license);

        *detail = *now;
        if (!detail->description[0]) g_strlcpy(detail->description, description, sizeof detail->description);
        if (!detail->homepage[0]) g_strlcpy(detail->homepage, homepage, sizeof detail->homepage);
        if (!detail->candidate[0]) g_strlcpy(detail->candidate, candidate, sizeof detail->candidate);
        if (!detail->license[0]) g_strlcpy(detail->license, license, sizeof detail->license);
    } else if (detail->source != HDE_STORE_SOURCE_LOCAL) {
        detail->installed = 0;
        detail->upgradable = 0;
        detail->automatic = 0;
        detail->state = HDE_STORE_STATE_NOT_INSTALLED;
        if (detail->candidate[0]) g_strlcpy(detail->version, detail->candidate, sizeof detail->version);
    }
}

/* ------------------------------------------------------------------ the actions */

static void after_change(void)
{
    load_installed();
    load_updates();
    load_featured();
    rebuild_installed();
    rebuild_updates();
    rebuild_discover();
    detail_sync_from_database();
    rebuild_detail();
}

static void on_install_clicked(GtkButton *b, gpointer data)
{
    (void)b;
    HdeStorePackage *p = data;
    if (!p || p->source == HDE_STORE_SOURCE_FLATPAK) return;

    if (p->from_file && file_check_done && file_check_count(NEXSDK_PKG_ERROR) > 0) {
        /* not a refusal: the user may have built it themselves and know why.  But the Store does not install
         * a package that its own check calls broken without saying so. */
        GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(main_win), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                GTK_MESSAGE_WARNING, GTK_BUTTONS_CANCEL,
                                                "%s", tr("This package has problems", "Gói này có lỗi"));
        gtk_message_dialog_format_secondary_text(
            GTK_MESSAGE_DIALOG(dlg),
            tr("%d error(s) and %d warning(s) were found in this file.  The details are on the package page.\n\n"
               "Install it anyway?  A package with a broken dependency can leave the system half-installed.",
               "Đã tìm thấy %d lỗi và %d cảnh báo trong tệp này. Chi tiết ở trang của gói.\n\n"
               "Vẫn cài? Một gói có phụ thuộc sai có thể làm hệ thống cài dở dang."),
            file_check_count(NEXSDK_PKG_ERROR), file_check_count(NEXSDK_PKG_WARN));
        gtk_dialog_add_button(GTK_DIALOG(dlg), tr("_Install anyway", "_Vẫn cài"), GTK_RESPONSE_ACCEPT);
        int response = gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
        if (response != GTK_RESPONSE_ACCEPT) {
            set_status(tr("%s was not installed.", "Chưa cài %s."), p->name);
            return;
        }
    }

    set_busy(TRUE);
    set_status(tr("Installing %s…", "Đang cài %s…"), p->name);

    gboolean ok = FALSE;
    if (p->from_file && p->origin[0]) {
        char **argv = hde_store_argv_append(HDE_STORE_CMD_INSTALL_FILE, p->origin, NULL);
        ok = argv && store_job_run(tr("Install a .deb file", "Cài tệp .deb"),
                                   tr("Installing the package file…", "Đang cài tệp gói…"),
                                   (const char *const *)argv);
        hde_store_argv_free(argv);
    } else {
        char **argv = hde_store_argv_append(HDE_STORE_CMD_INSTALL, p->name, NULL);
        char *headline = g_strdup_printf(tr("Installing %s…", "Đang cài %s…"), p->name);
        ok = argv && store_job_run(tr("Install", "Cài đặt"), headline, (const char *const *)argv);
        g_free(headline);
        hde_store_argv_free(argv);
    }

    set_busy(FALSE);
    set_status(ok ? tr("%s is installed.", "Đã cài %s.")
                  : tr("%s could not be installed — the package manager's output is in the window that just closed.",
                       "Không cài được %s — kết quả của trình quản lý gói ở trong cửa sổ vừa đóng."),
               p->name);
    if (ok && p->from_file) {
        /* the file is now a package of the system: show that one, with its source and its license */
        char *name = g_strdup(p->name);
        g_free(detail);
        detail = NULL;
        after_change();
        HdeStorePackage *now = g_hash_table_lookup(by_name, name);
        if (now) open_detail(now);
        g_free(name);
        return;
    }
    after_change();
}

static void remove_dialog(HdeStorePackage *p)
{
    if (!p) return;
    GtkWidget *dlg = gtk_dialog_new_with_buttons(tr("Uninstall", "Gỡ bỏ"), GTK_WINDOW(main_win),
                                                 GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                 tr("Cancel", "Huỷ"), GTK_RESPONSE_CANCEL,
                                                 tr("Remove, keep settings", "Gỡ, giữ cấu hình"), 1,
                                                 tr("Remove everything", "Gỡ tất cả"), 2, NULL);
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    char *text = g_strdup_printf(tr("Remove %s?\n\nIts configuration files can be kept in case you install it again.",
                                    "Gỡ bỏ %s?\n\nCó thể giữ lại tệp cấu hình để dùng lại khi cài lại."),
                                 p->name);
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_widget_set_margin_start(l, 16);
    gtk_widget_set_margin_end(l, 16);
    gtk_widget_set_margin_top(l, 12);
    gtk_widget_set_margin_bottom(l, 12);
    gtk_container_add(GTK_CONTAINER(area), l);
    g_free(text);
    gtk_widget_show_all(dlg);

    gint r = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    if (r != 1 && r != 2) return;

    set_busy(TRUE);
    char **argv = hde_store_argv_append(r == 2 ? HDE_STORE_CMD_PURGE : HDE_STORE_CMD_REMOVE, p->name, NULL);
    char *headline = g_strdup_printf(tr("Removing %s…", "Đang gỡ %s…"), p->name);
    gboolean ok = argv && store_job_run(tr("Uninstall", "Gỡ bỏ"), headline, (const char *const *)argv);
    g_free(headline);
    hde_store_argv_free(argv);
    set_busy(FALSE);
    set_status(ok ? tr("%s was removed.", "Đã gỡ %s.") : tr("%s could not be removed.", "Không gỡ được %s."),
               p->name);
    after_change();
}

static void on_remove_clicked(GtkButton *b, gpointer data)
{
    (void)b;
    remove_dialog(data);
}

static void on_open_homepage(GtkButton *b, gpointer data)
{
    (void)data;
    const char *uri = g_object_get_data(G_OBJECT(b), "store-uri");
    if (!uri) return;
    GError *e = NULL;
    if (!gtk_show_uri_on_window(GTK_WINDOW(main_win), uri, GDK_CURRENT_TIME, &e)) {
        set_status("%s", e ? e->message : "Could not open the homepage");
        g_clear_error(&e);
    }
}

static void on_copy_name(GtkButton *b, gpointer data)
{
    (void)b;
    HdeStorePackage *p = data;
    if (!p) return;
    GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_set_text(cb, p->name, -1);
    set_status(tr("Copied %s.", "Đã sao chép %s."), p->name);
}

static void on_show_copyright(GtkButton *b, gpointer data)
{
    (void)b;
    HdeStorePackage *p = data;
    if (!p) return;
    char *path = g_strdup_printf("/usr/share/doc/%s/copyright", p->name);
    char *text = NULL;
    if (!g_file_get_contents(path, &text, NULL, NULL)) {
        char *gz = g_strdup_printf("%s.gz", path);
        const char *argv[] = { "gzip", "-dc", gz, NULL };
        text = run_capture(argv, NULL);
        g_free(gz);
    }
    g_free(path);

    GtkWidget *dlg = gtk_dialog_new_with_buttons(tr("Copyright and license", "Bản quyền và giấy phép"),
                                                 GTK_WINDOW(main_win),
                                                 GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                 tr("Close", "Đóng"), GTK_RESPONSE_CLOSE, NULL);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 700, 520);
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    GtkWidget *view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)),
                             (text && *text) ? text : tr("No copyright file was found for this package.",
                                                         "Không tìm thấy tệp giấy phép của gói này."), -1);
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(sw), view);
    gtk_container_add(GTK_CONTAINER(area), sw);
    gtk_widget_show_all(dlg);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    g_free(text);
}

/* ------------------------------------------------------------------ pages, filled */

static void rebuild_installed(void)
{
    if (!installed_inner) return;
    GList *children = gtk_container_get_children(GTK_CONTAINER(installed_inner));
    for (GList *l = children; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(children);

    guint count = pkgs_installed ? pkgs_installed->len : 0;
    char size[32];
    hde_store_format_size(installed_kb, size, sizeof size);

    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *title = label_with_class(tr("Installed software", "Phần mềm đã cài"), "store-h1");
    gtk_widget_set_hexpand(title, TRUE);
    gtk_box_pack_start(GTK_BOX(head), title, TRUE, TRUE, 0);
    char *summary = g_strdup_printf(tr("%u packages · %s", "%u gói · %s"), count, size);
    gtk_box_pack_start(GTK_BOX(head), label_with_class(summary, "store-row-sub"), FALSE, FALSE, 0);
    g_free(summary);
    gtk_box_pack_start(GTK_BOX(installed_inner), head, FALSE, FALSE, 0);

    GtkWidget *filter = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(filter), tr("Filter installed packages…", "Lọc gói đã cài…"));
    gtk_box_pack_start(GTK_BOX(installed_inner), filter, FALSE, FALSE, 0);

    GtkWidget *list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    gtk_style_context_add_class(gtk_widget_get_style_context(list), "store-card-flat");
    g_signal_connect(list, "row-activated", G_CALLBACK(on_row_activated), NULL);
    gtk_box_pack_start(GTK_BOX(installed_inner), list, FALSE, FALSE, 0);
    list_fill(list, pkgs_installed);
    g_signal_connect(filter, "search-changed", G_CALLBACK(on_filter_changed), list);

    if (!count)
        gtk_box_pack_start(GTK_BOX(installed_inner),
                           empty_label(tr("No packages were found. Is dpkg installed?",
                                          "Không tìm thấy gói nào. dpkg có được cài không?")),
                           FALSE, FALSE, 0);
    gtk_widget_show_all(installed_inner);
}

static void rebuild_updates(void)
{
    if (!updates_inner) return;
    GList *children = gtk_container_get_children(GTK_CONTAINER(updates_inner));
    for (GList *l = children; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(children);

    guint count = pkgs_updates ? pkgs_updates->len : 0;

    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *title = label_with_class(tr("Software updates", "Cập nhật phần mềm"), "store-h1");
    gtk_widget_set_hexpand(title, TRUE);
    gtk_box_pack_start(GTK_BOX(head), title, TRUE, TRUE, 0);

    GtkWidget *lists_btn = gtk_button_new_with_label(tr("Update lists", "Cập nhật danh sách"));
    gtk_widget_set_tooltip_text(lists_btn, tr("Download what the repositories offer (apt-get update)",
                                              "Tải danh sách mới từ kho phần mềm (apt-get update)"));
    g_signal_connect(lists_btn, "clicked", G_CALLBACK(on_update_lists), NULL);
    gtk_box_pack_start(GTK_BOX(head), lists_btn, FALSE, FALSE, 0);

    GtkWidget *all = gtk_button_new_with_label(tr("Update all", "Cập nhật tất cả"));
    gtk_style_context_add_class(gtk_widget_get_style_context(all), "suggested-action");
    g_signal_connect(all, "clicked", G_CALLBACK(on_upgrade_all), NULL);
    gtk_widget_set_sensitive(all, count > 0);
    gtk_box_pack_start(GTK_BOX(head), all, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(updates_inner), head, FALSE, FALSE, 0);

    char *sub = g_strdup_printf(count ? tr("%u packages can be updated.", "%u gói có thể cập nhật.")
                                      : tr("Everything is up to date.", "Mọi thứ đã là mới nhất."),
                                count);
    gtk_box_pack_start(GTK_BOX(updates_inner), label_with_class(sub, "store-row-sub"), FALSE, FALSE, 0);
    g_free(sub);

    if (count) {
        GtkWidget *list = gtk_list_box_new();
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
        gtk_style_context_add_class(gtk_widget_get_style_context(list), "store-card-flat");
        g_signal_connect(list, "row-activated", G_CALLBACK(on_row_activated), NULL);
        gtk_box_pack_start(GTK_BOX(updates_inner), list, FALSE, FALSE, 0);
        list_fill(list, pkgs_updates);
    } else {
        gtk_box_pack_start(GTK_BOX(updates_inner),
                           empty_label(tr("No updates are waiting.  Press “Update lists” to ask the repositories "
                                          "whether something new arrived since the last time.",
                                          "Không có bản cập nhật nào. Nhấn “Cập nhật danh sách” để hỏi lại kho "
                                          "phần mềm xem có gì mới từ lần trước.")),
                           FALSE, FALSE, 0);
    }
    gtk_widget_show_all(updates_inner);
}

/* One row of the Sources page. */
static GtkWidget *source_row(const HdeStoreSourceEntry *s, const char *note)
{
    char label[300];
    hde_store_source_label(s, label, sizeof label);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(row), 5);

    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    gtk_widget_set_hexpand(v, TRUE);
    GtkWidget *t = label_with_class(label, "store-row-title");
    gtk_label_set_ellipsize(GTK_LABEL(t), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_pack_start(GTK_BOX(v), t, FALSE, FALSE, 0);
    GtkWidget *n = label_with_class(note && *note ? note : tr("system keyring", "keyring của hệ thống"),
                                    "store-row-sub");
    gtk_label_set_ellipsize(GTK_LABEL(n), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_pack_start(GTK_BOX(v), n, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), v, TRUE, TRUE, 0);

    if (!s->enabled)
        gtk_box_pack_end(GTK_BOX(row), badge(tr("disabled", "đang tắt"), "store-badge-plain"), FALSE, FALSE, 0);
    HdeStoreVerify vfy = verify_for_source(s);
    gtk_box_pack_end(GTK_BOX(row), badge(hde_store_verify_title(vfy, ui_is_vietnamese()), badge_kind_for_verify(vfy)),
                     FALSE, FALSE, 0);
    return row;
}

static gboolean flathub_connected(void)
{
    if (!g_find_program_in_path("flatpak")) return FALSE;
    const char *const argv[] = { "flatpak", "remotes", "--user", "--columns=name", NULL };
    char *out = run_capture(argv, NULL);
    gboolean found = FALSE;
    char **lines = g_strsplit(out ? out : "", "\n", -1);
    for (guint i = 0; lines[i]; i++) {
        if (!g_strcmp0(g_strstrip(lines[i]), "flathub")) { found = TRUE; break; }
    }
    g_strfreev(lines);
    g_free(out);
    return found;
}

static void on_connect_flathub(GtkButton *button, gpointer data)
{
    (void)button;
    (void)data;
    const char *const argv[] = {
        "flatpak", "remote-add", "--user", "--if-not-exists",
        "flathub", "https://dl.flathub.org/repo/flathub.flatpakrepo", NULL
    };
    gboolean ok = store_job_run(tr("Connect Flathub", "Kết nối Flathub"),
                                tr("Adding Flathub for this user…", "Đang thêm Flathub cho người dùng này…"), argv);
    if (ok) {
        load_flatpak();
        set_status(tr("Flathub is connected.", "Đã kết nối Flathub."));
    }
    rebuild_sources();
}

static void rebuild_sources(void)
{
    if (!sources_inner) return;
    GList *children = gtk_container_get_children(GTK_CONTAINER(sources_inner));
    for (GList *l = children; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(children);

    gtk_box_pack_start(GTK_BOX(sources_inner), label_with_class(tr("Download sources", "Nguồn tải"), "store-h1"),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sources_inner),
                       label_with_class(tr("Where the packages of this computer come from, and how far each "
                                           "source can be trusted.",
                                           "Các gói trên máy này đến từ đâu, và mỗi nguồn đáng tin đến mức nào."),
                                        "store-row-sub"), FALSE, FALSE, 0);

    GtkWidget *box = card();
    int found = 0;

    char *text = NULL;
    if (g_file_get_contents("/etc/apt/sources.list", &text, NULL, NULL)) {
        char **lines = g_strsplit(text, "\n", -1);
        for (guint k = 0; lines[k]; k++) {
            HdeStoreSourceEntry s;
            if (!hde_store_parse_sources_line(lines[k], &s)) continue;
            gtk_box_pack_start(GTK_BOX(box), source_row(&s, s.signed_by), FALSE, FALSE, 0);
            found++;
        }
        g_strfreev(lines);
        g_free(text);
    }

    GDir *dir = g_dir_open("/etc/apt/sources.list.d", 0, NULL);
    if (dir) {
        const char *name;
        while ((name = g_dir_read_name(dir))) {
            char *path = g_build_filename("/etc/apt/sources.list.d", name, NULL);
            char *content = NULL;
            if (g_str_has_suffix(name, ".list") && g_file_get_contents(path, &content, NULL, NULL)) {
                char **lines = g_strsplit(content, "\n", -1);
                for (guint k = 0; lines[k]; k++) {
                    HdeStoreSourceEntry s;
                    if (!hde_store_parse_sources_line(lines[k], &s)) continue;
                    gtk_box_pack_start(GTK_BOX(box), source_row(&s, name), FALSE, FALSE, 0);
                    found++;
                }
                g_strfreev(lines);
            } else if (g_str_has_suffix(name, ".sources") && g_file_get_contents(path, &content, NULL, NULL)) {
                for (int idx = 0; idx < 32; idx++) {
                    HdeStoreSourceEntry s;
                    if (!hde_store_parse_sources_deb822(content, idx, &s)) break;
                    gtk_box_pack_start(GTK_BOX(box), source_row(&s, s.signed_by[0] ? s.signed_by : name),
                                       FALSE, FALSE, 0);
                    found++;
                }
            }
            g_free(content);
            g_free(path);
        }
        g_dir_close(dir);
    }

    if (!found)
        gtk_box_pack_start(GTK_BOX(box),
                           label_with_class(tr("No source file could be read — this is unusual for a Debian or "
                                               "Ubuntu system.",
                                               "Không đọc được tệp nguồn nào — điều này lạ với hệ Debian/Ubuntu."),
                                            "store-row-sub"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sources_inner), box, FALSE, FALSE, 0);

    /* Flathub is configured per-user, so connecting it needs no administrator password. */
    gtk_box_pack_start(GTK_BOX(sources_inner), section_heading(tr("Flatpak", "Flatpak")), FALSE, FALSE, 0);
    GtkWidget *fc = card();
    if (!g_find_program_in_path("flatpak")) {
        gtk_box_pack_start(GTK_BOX(fc),
                           label_with_class(tr("Flatpak is not installed. Install the flatpak package first, then return here to connect Flathub.",
                                               "Flatpak chưa được cài. Hãy cài gói flatpak trước, rồi quay lại đây để kết nối Flathub."),
                                            "store-row-sub"), FALSE, FALSE, 0);
    } else {
        gboolean connected = flathub_connected();
        GtkWidget *remote_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        GtkWidget *remote_text = label_with_class(
            connected ? tr("Flathub — connected for this user", "Flathub — đã kết nối cho người dùng này")
                      : tr("Flathub — apps and updates from flathub.org", "Flathub — ứng dụng và cập nhật từ flathub.org"),
            "store-info-value");
        gtk_widget_set_hexpand(remote_text, TRUE);
        gtk_box_pack_start(GTK_BOX(remote_row), remote_text, TRUE, TRUE, 0);
        GtkWidget *connect = gtk_button_new_with_label(
            connected ? tr("Connected", "Đã kết nối") : tr("Connect Flathub", "Kết nối Flathub"));
        gtk_widget_set_sensitive(connect, !connected);
        if (!connected) g_signal_connect(connect, "clicked", G_CALLBACK(on_connect_flathub), NULL);
        gtk_box_pack_end(GTK_BOX(remote_row), connect, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(fc), remote_row, FALSE, FALSE, 0);

        if (!pkgs_flatpak || !pkgs_flatpak->len) {
        gtk_box_pack_start(GTK_BOX(fc),
                           label_with_class(tr("No Flatpak applications are installed on this computer.",
                                               "Máy này chưa cài ứng dụng Flatpak nào."), "store-row-sub"),
                           FALSE, FALSE, 0);
    } else {
        for (guint i = 0; i < pkgs_flatpak->len; i++) {
            HdeStorePackage *f = g_ptr_array_index(pkgs_flatpak, i);
            char *line = g_strdup_printf("%s  %s  (%s)", f->name, f->version, f->origin);
            gtk_box_pack_start(GTK_BOX(fc), label_with_class(line, "store-info-value"), FALSE, FALSE, 0);
            g_free(line);
        }
        gtk_box_pack_start(GTK_BOX(fc),
                           label_with_class(tr("Read-only: this version of the Store does not install or remove "
                                               "Flatpak applications yet.",
                                               "Chỉ đọc: bản này của Cửa hàng chưa cài hay gỡ ứng dụng Flatpak."),
                                            "store-note"), FALSE, FALSE, 0);
        }
    }
    gtk_box_pack_start(GTK_BOX(sources_inner), fc, FALSE, FALSE, 0);
    gtk_widget_show_all(sources_inner);
}

static void rebuild_discover(void)
{
    if (!discover_inner) return;
    GList *children = gtk_container_get_children(GTK_CONTAINER(discover_inner));
    for (GList *l = children; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(children);

    GtkWidget *hero = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_style_context_add_class(gtk_widget_get_style_context(hero), "store-banner");
    gtk_box_pack_start(GTK_BOX(hero), label_with_class(tr("Discover", "Khám phá"), "store-banner-title"),
                       FALSE, FALSE, 0);
    GtkWidget *hero_sub = label_with_class(
        tr("Find software for your Linux desktop, see where it comes from, and install it.",
           "Tìm ứng dụng mới cho Linux của bạn, xem nó đến từ đâu, và cài đặt."),
        "store-banner-sub");
    gtk_label_set_line_wrap(GTK_LABEL(hero_sub), TRUE);
    gtk_box_pack_start(GTK_BOX(hero), hero_sub, FALSE, FALSE, 0);

    GtkWidget *hero_buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(hero_buttons, 8);
    GtkWidget *b_lists = gtk_button_new_with_label(tr("Update lists", "Cập nhật danh sách"));
    g_signal_connect(b_lists, "clicked", G_CALLBACK(on_update_lists), NULL);
    gtk_box_pack_start(GTK_BOX(hero_buttons), b_lists, FALSE, FALSE, 0);
    GtkWidget *b_file = gtk_button_new_with_label(tr("Install a .deb file…", "Cài tệp .deb…"));
    g_signal_connect(b_file, "clicked", G_CALLBACK(on_install_file), NULL);
    gtk_box_pack_start(GTK_BOX(hero_buttons), b_file, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hero), hero_buttons, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(discover_inner), hero, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(discover_inner), section_heading(tr("Categories", "Danh mục")), FALSE, FALSE, 0);
    GtkWidget *flow = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow), GTK_SELECTION_NONE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow), 4);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(flow), 2);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(flow), 6);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(flow), 6);
    for (int i = 0; categories[i].label_en; i++) {
        GtkWidget *b = gtk_button_new_with_label(tr(categories[i].label_en, categories[i].label_vi));
        gtk_style_context_add_class(gtk_widget_get_style_context(b), "store-chip");
        g_object_set_data(G_OBJECT(b), "store-query", (gpointer)categories[i].query);
        g_signal_connect(b, "clicked", G_CALLBACK(on_category_clicked), NULL);
        gtk_flow_box_insert(GTK_FLOW_BOX(flow), b, -1);
    }
    gtk_box_pack_start(GTK_BOX(discover_inner), flow, FALSE, FALSE, 0);

    guint count = pkgs_installed ? pkgs_installed->len : 0;
    char size[32];
    hde_store_format_size(installed_kb, size, sizeof size);
    GtkWidget *mini = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_style_context_add_class(gtk_widget_get_style_context(mini), "store-card");
    gtk_container_set_border_width(GTK_CONTAINER(mini), 14);
    GtkWidget *icon = gtk_image_new_from_icon_name("drive-harddisk-symbolic", GTK_ICON_SIZE_DND);
    gtk_box_pack_start(GTK_BOX(mini), icon, FALSE, FALSE, 0);
    GtkWidget *mv = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    gtk_widget_set_hexpand(mv, TRUE);
    char *installed_text = g_strdup_printf(tr("%u packages installed · %s", "%u gói đã cài · %s"), count, size);
    gtk_box_pack_start(GTK_BOX(mv), label_with_class(installed_text, "store-row-title"), FALSE, FALSE, 0);
    g_free(installed_text);
    gtk_box_pack_start(GTK_BOX(mv), label_with_class(tr("Managed by dpkg and APT", "Do dpkg và APT quản lý"),
                                                     "store-row-sub"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(mini), mv, TRUE, TRUE, 0);
    GtkWidget *manage = gtk_button_new_with_label(tr("Manage", "Quản lý"));
    g_object_set_data(G_OBJECT(manage), "store-nav", GINT_TO_POINTER(1));
    g_signal_connect(manage, "clicked", G_CALLBACK(on_nav_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(mini), manage, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(discover_inner), mini, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(discover_inner), section_heading(tr("Popular apps", "Ứng dụng phổ biến")),
                       FALSE, FALSE, 0);
    if (pkgs_featured && pkgs_featured->len) {
        GtkWidget *list = gtk_list_box_new();
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
        gtk_style_context_add_class(gtk_widget_get_style_context(list), "store-card-flat");
        g_signal_connect(list, "row-activated", G_CALLBACK(on_row_activated), NULL);
        gtk_box_pack_start(GTK_BOX(discover_inner), list, FALSE, FALSE, 0);
        list_fill(list, pkgs_featured);
    } else {
        GtkWidget *hint = card();
        gtk_box_pack_start(GTK_BOX(hint),
                           label_with_class(tr("The package lists of this computer are empty, so there is nothing to "
                                               "show yet.  Press “Update lists” (it needs your password) so that APT "
                                               "knows what the repositories offer.",
                                               "Danh sách gói trên máy đang trống nên chưa có gì để hiện. Nhấn "
                                               "“Cập nhật danh sách” (cần mật khẩu) để APT biết kho phần mềm có gì."),
                                            "store-row-sub"), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(discover_inner), hint, FALSE, FALSE, 0);
    }
    gtk_widget_show_all(discover_inner);
}

static void rebuild_results(void)
{
    if (!results_inner) return;
    GList *children = gtk_container_get_children(GTK_CONTAINER(results_inner));
    for (GList *l = children; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(children);

    char *title = (search_query && *search_query)
                      ? g_strdup_printf(tr("Results for “%s”", "Kết quả cho “%s”"), search_query)
                      : g_strdup(tr("Search", "Tìm kiếm"));
    gtk_box_pack_start(GTK_BOX(results_inner), label_with_class(title, "store-h1"), FALSE, FALSE, 0);
    g_free(title);

    guint count = pkgs_search ? pkgs_search->len : 0;
    char *sub = g_strdup_printf(count ? tr("%u packages found.", "%u gói được tìm thấy.")
                                      : tr("Nothing found.", "Không tìm thấy gì."), count);
    gtk_box_pack_start(GTK_BOX(results_inner), label_with_class(sub, "store-row-sub"), FALSE, FALSE, 0);
    g_free(sub);

    if (count) {
        GtkWidget *list = gtk_list_box_new();
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
        gtk_style_context_add_class(gtk_widget_get_style_context(list), "store-card-flat");
        g_signal_connect(list, "row-activated", G_CALLBACK(on_row_activated), NULL);
        gtk_box_pack_start(GTK_BOX(results_inner), list, FALSE, FALSE, 0);
        list_fill(list, pkgs_search);
    } else {
        gtk_box_pack_start(GTK_BOX(results_inner),
                           empty_label(tr("Type the name of an application, or what it should do.  The Store looks "
                                          "in the package names and the descriptions of the repositories.",
                                          "Hãy gõ tên ứng dụng, hoặc việc nó làm. Cửa hàng tìm trong tên gói và "
                                          "phần mô tả của kho phần mềm.")),
                           FALSE, FALSE, 0);
    }
    gtk_widget_show_all(results_inner);
}

/* ------------------------------------------------------------------ searching */

static void do_search(const char *query)
{
    /* query may point into search_query itself when refreshing. */
    char *new_query = g_strdup(query ? query : "");
    g_free(search_query);
    search_query = new_query;

    pkgs_clear(&pkgs_search);
    pkgs_search = pkgs_new();

    if (!search_query[0]) {
        rebuild_results();
        show_page("results");
        return;
    }

    set_busy(TRUE);
    set_status(tr("Searching for “%s”…", "Đang tìm “%s”…"), search_query);

    /* 1. what the repositories know */
    char **argv = hde_store_argv_append(HDE_STORE_CMD_SEARCH, search_query, NULL);
    char *out = run_capture((const char *const *)argv, NULL);
    hde_store_argv_free(argv);

    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    char **lines = g_strsplit(out, "\n", -1);
    int max = 2000; /* rank before limiting: APT output is alphabetic, not relevance ordered */
    for (guint i = 0; lines[i] && max > 0; i++) {
        HdeStorePackage tmp;
        if (!hde_store_parse_apt_search_line(lines[i], &tmp)) continue;
        if (hde_store_match(&tmp, search_query) <= 0) continue;
        HdeStorePackage *p = g_new0(HdeStorePackage, 1);
        *p = tmp;
        HdeStorePackage *installed = g_hash_table_lookup(by_name, p->name);
        if (installed) {
            g_free(p);
            p = g_new0(HdeStorePackage, 1);
            *p = *installed;            /* the installed copy has the summary, the size and the origin */
        }
        if (g_hash_table_contains(seen, p->name)) {
            g_free(p);
            continue;
        }
        g_hash_table_add(seen, g_strdup(p->name));
        g_ptr_array_add(pkgs_search, p);
        max--;
    }
    g_strfreev(lines);
    g_free(out);

    /* 2. what is installed: it has descriptions of its own, even without package lists */
    for (guint i = 0; pkgs_installed && i < pkgs_installed->len && max > 0; i++) {
        HdeStorePackage *p = g_ptr_array_index(pkgs_installed, i);
        if (hde_store_match(p, search_query) <= 0) continue;
        if (g_hash_table_contains(seen, p->name)) continue;
        HdeStorePackage *copy = g_new0(HdeStorePackage, 1);
        *copy = *p;
        g_ptr_array_add(pkgs_search, copy);
        max--;
    }
    g_hash_table_destroy(seen);

    g_ptr_array_sort(pkgs_search, cmp_by_score);

    rebuild_results();
    show_page("results");
    set_busy(FALSE);
    set_status(tr("%u results for “%s”.", "%u kết quả cho “%s”."), pkgs_search->len, search_query);
}

static void on_search_changed(GtkSearchEntry *entry, gpointer data)
{
    (void)data;
    do_search(gtk_entry_get_text(GTK_ENTRY(entry)));
}

static void on_category_clicked(GtkButton *b, gpointer data)
{
    (void)data;
    const char *query = g_object_get_data(G_OBJECT(b), "store-query");
    if (!query) return;
    if (search_entry) {
        g_signal_handlers_block_by_func(search_entry, G_CALLBACK(on_search_changed), NULL);
        gtk_entry_set_text(GTK_ENTRY(search_entry), query);
        g_signal_handlers_unblock_by_func(search_entry, G_CALLBACK(on_search_changed), NULL);
    }
    do_search(query);
}

/* A text filter over one of the lists. */
static void on_filter_changed(GtkSearchEntry *entry, gpointer data)
{
    GtkWidget *list = data;
    const char *text = gtk_entry_get_text(GTK_ENTRY(entry));
    GList *rows = gtk_container_get_children(GTK_CONTAINER(list));
    for (GList *l = rows; l; l = l->next) {
        GtkWidget *row = GTK_WIDGET(l->data);
        HdeStorePackage *p = g_object_get_data(G_OBJECT(row), "store-pkg");
        gboolean show = TRUE;
        if (text && *text && p) show = hde_store_match(p, text) > 0;
        gtk_widget_set_visible(row, show);
    }
    g_list_free(rows);
}

static void on_row_activated(GtkListBox *list, GtkListBoxRow *row, gpointer data)
{
    (void)list;
    (void)data;
    HdeStorePackage *p = row ? g_object_get_data(G_OBJECT(row), "store-pkg") : NULL;
    open_detail(p);
}

/* ------------------------------------------------------------------ updates */

static void on_update_lists(GtkButton *b, gpointer data)
{
    (void)b;
    (void)data;
    set_busy(TRUE);
    gboolean ok = store_job_run(tr("Update lists", "Cập nhật danh sách"),
                                tr("Downloading what the repositories offer (apt-get update)…",
                                   "Đang tải danh sách gói từ kho (apt-get update)…"),
                                HDE_STORE_CMD_UPDATE);
    set_busy(FALSE);
    set_status(ok ? tr("The package lists are up to date.", "Danh sách gói đã được cập nhật.")
                  : tr("The package lists could not be updated.", "Không cập nhật được danh sách gói."));
    load_installed();
    load_updates();
    load_featured();
    rebuild_installed();
    rebuild_updates();
    rebuild_discover();
    rebuild_sources();
    detail_sync_from_database();
    rebuild_detail();
    if (search_query && *search_query) do_search(search_query);
}

static void on_upgrade_all(GtkButton *b, gpointer data)
{
    (void)b;
    (void)data;
    if (!pkgs_updates || !pkgs_updates->len) return;
    const char *argv[] = { "pkexec", "apt-get", "upgrade", "-y", NULL };
    set_busy(TRUE);
    gboolean ok = store_job_run(tr("Update all", "Cập nhật tất cả"),
                                tr("Installing every available update…", "Đang cài mọi bản cập nhật…"), argv);
    set_busy(FALSE);
    set_status(ok ? tr("Everything is up to date.", "Mọi thứ đã là mới nhất.")
                  : tr("Some updates could not be installed.", "Một số bản cập nhật chưa cài được."));
    after_change();
}

/* ------------------------------------------------------------------ a .deb from disk */

static void open_local_file(const char *path)
{
    if (!path || !*path) return;
    const char *argv[] = { "dpkg-deb", "-f", path, NULL };
    char *control = run_capture(argv, NULL);
    if (!control || !strstr(control, "Package:")) {
        g_free(control);
        set_status(tr("%s is not a Debian package (or dpkg-deb is missing).",
                      "%s không phải là gói Debian (hoặc thiếu dpkg-deb)."), path);
        return;
    }
    HdeStorePackage tmp;
    if (!hde_store_from_deb_control(control, path, &tmp)) {
        g_free(control);
        set_status(tr("%s has no readable control file.", "%s không có tệp control đọc được."), path);
        return;
    }
    g_free(control);
    file_check_run(path);

    g_free(detail);
    detail = g_new0(HdeStorePackage, 1);
    *detail = tmp;
    rebuild_detail();
    show_page("detail");
    set_status(tr("%s opened from %s.", "Đã mở %s từ %s."), detail->name, path);
}

static void on_install_file(GtkButton *b, gpointer data)
{
    (void)b;
    (void)data;
    GtkWidget *dlg = gtk_file_chooser_dialog_new(tr("Open a .deb file", "Mở tệp .deb"), GTK_WINDOW(main_win),
                                                 GTK_FILE_CHOOSER_ACTION_OPEN,
                                                 tr("_Cancel", "_Huỷ"), GTK_RESPONSE_CANCEL,
                                                 tr("_Open", "_Mở"), GTK_RESPONSE_ACCEPT, NULL);
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, tr("Debian packages", "Gói Debian"));
    gtk_file_filter_add_pattern(filter, "*.deb");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), filter);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        open_local_file(path);
        g_free(path);
    }
    gtk_widget_destroy(dlg);
}

static void on_install_file_clicked(GtkButton *b, gpointer data)
{
    on_install_file(b, data);
}

/* ------------------------------------------------------------------ the window */

static void on_sidebar_changed(GtkListBox *list, GtkListBoxRow *row, gpointer data)
{
    (void)list;
    (void)data;
    if (!row) return;
    int index = gtk_list_box_row_get_index(row);
    const char *pages[] = { "discover", "installed", "updates", "sources" };
    if (index < 0 || index > 3) return;
    show_page(pages[index]);
    if (index == 1) rebuild_installed();
    if (index == 2) rebuild_updates();
    if (index == 3) rebuild_sources();
}

static void select_sidebar_row(gpointer index)
{
    if (!sidebar) return;
    GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(sidebar), GPOINTER_TO_INT(index));
    if (row) gtk_list_box_select_row(GTK_LIST_BOX(sidebar), row);
}

static void on_nav_clicked(GtkButton *b, gpointer data)
{
    (void)data;
    gpointer index = g_object_get_data(G_OBJECT(b), "store-nav");
    select_sidebar_row(index);
}

static void on_refresh_clicked(GtkButton *b, gpointer data)
{
    (void)b;
    (void)data;
    set_busy(TRUE);
    set_status(tr("Reading the package database…", "Đang đọc cơ sở dữ liệu gói…"));
    load_installed();
    load_updates();
    load_flatpak();
    load_featured();
    rebuild_installed();
    rebuild_updates();
    rebuild_sources();
    rebuild_discover();
    detail_sync_from_database();
    rebuild_detail();
    if (search_query && *search_query) do_search(search_query);
    set_busy(FALSE);
    set_status(tr("%u packages installed · %u update(s) available.",
                  "%u gói đã cài · %u bản cập nhật."), pkgs_installed ? pkgs_installed->len : 0,
               pkgs_updates ? pkgs_updates->len : 0);
}

static void show_about(void)
{
    const char *authors[] = { "Hyggshi OS Research Technology", NULL };
    gtk_show_about_dialog(GTK_WINDOW(main_win),
                          "program-name", tr("Hyggshi Store", "Cửa hàng Hyggshi"),
                          "version", HDE_VERSION,
                          "comments",
                          tr("The app store of Hyggshi OS: browse, install, update and remove .deb packages, and "
                             "see where every package comes from.",
                             "Kho ứng dụng của Hyggshi OS: xem, cài, cập nhật và gỡ gói .deb, và xem mỗi gói "
                             "đến từ đâu."),
                          "website", "https://github.com/Hyggshi-OS-Research-Technology/NexWM",
                          "website-label", "NexWM / HDE",
                          "copyright", "Copyright (c) 2024-2026 Hyggshi OS Research Technology",
                          "license-type", GTK_LICENSE_MIT_X11,
                          "authors", authors,
                          "logo-icon-name", APP_ICON,
                          NULL);
}

static gboolean on_key_press(GtkWidget *w, GdkEventKey *ev, gpointer data)
{
    (void)w;
    (void)data;
    guint mods = ev->state & gtk_accelerator_get_default_mod_mask();
    if (ev->keyval == GDK_KEY_F1) {
        show_about();
        return TRUE;
    }
    if (ev->keyval == GDK_KEY_F5) {
        on_refresh_clicked(NULL, NULL);
        return TRUE;
    }
    if (mods == GDK_CONTROL_MASK && (ev->keyval == GDK_KEY_f || ev->keyval == GDK_KEY_F)) {
        if (search_entry) gtk_widget_grab_focus(search_entry);
        return TRUE;
    }
    if (mods == GDK_CONTROL_MASK && (ev->keyval == GDK_KEY_q || ev->keyval == GDK_KEY_Q)) {
        gtk_widget_destroy(main_win);
        return TRUE;
    }
    return FALSE;
}

static void apply_store_css(void)
{
    if (!store_css) {
        store_css = gtk_css_provider_new();
        gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(store_css),
                                                  GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    HdeThemeInfo ti;
    hde_theme_info_load(&ti);
    char *accent_css = hde_theme_accent_css(&ti);
    hde_theme_info_clear(&ti);

    GString *css = g_string_new(NULL);
    g_string_append(css,
        ".store-sidebar { background: alpha(@theme_fg_color, 0.035);"
        "  border-right: 1px solid alpha(@theme_fg_color, 0.10); }\n"
        ".store-sidebar row { padding: 9px 14px; border-radius: 9px; }\n"
        ".store-sidebar row:selected { background: alpha(@theme_selected_bg_color, 0.22); }\n"
        ".store-sidebar-label { font-weight: 600; }\n"
        ".store-h1 { font-size: 21px; font-weight: 700; }\n"
        ".store-lead { font-size: 13px; }\n"
        ".store-section { font-size: 12px; font-weight: 700; letter-spacing: 0.5px;"
        "  color: alpha(@theme_fg_color, 0.62); }\n"
        ".store-row-title { font-weight: 600; }\n"
        ".store-row-sub { font-size: 12px; color: alpha(@theme_fg_color, 0.62); }\n"
        ".store-card { background: alpha(@theme_fg_color, 0.04);"
        "  border: 1px solid alpha(@theme_fg_color, 0.09); border-radius: 12px; }\n"
        ".store-card-flat { background: alpha(@theme_fg_color, 0.03);"
        "  border: 1px solid alpha(@theme_fg_color, 0.09); border-radius: 12px; }\n"
        ".store-card-flat row { border-bottom: 1px solid alpha(@theme_fg_color, 0.07); }\n"
        ".store-tile { background: alpha(@theme_fg_color, 0.12); border-radius: 9px; }\n"
        ".store-tile-letter { font-size: 15px; font-weight: 700; color: alpha(@theme_fg_color, 0.72); }\n"
        ".store-shot { padding: 0; border: 1px solid alpha(@theme_fg_color, 0.12); border-radius: 10px; }\n"
        ".store-shot:hover { border-color: alpha(@theme_fg_color, 0.28); }\n"
        ".store-hero { background-image: linear-gradient(120deg, alpha(@theme_selected_bg_color, 0.30), alpha(@theme_fg_color, 0.04));"
        "  border: 1px solid alpha(@theme_fg_color, 0.09); border-radius: 14px; padding: 18px; }\n"
        ".store-banner { background-image: linear-gradient(135deg, #3584e4, #9141ac);"
        "  border-radius: 16px; padding: 20px; }\n"
        ".store-banner-title { color: #ffffff; font-size: 24px; font-weight: 800; }\n"
        ".store-banner-sub { color: rgba(255, 255, 255, 0.88); }\n"
        ".store-badge { font-size: 11px; font-weight: 700; border-radius: 7px; padding: 1px 8px; }\n"
        ".store-badge-ok { background: alpha(#2ec27e, 0.20); color: #26a269; }\n"
        ".store-badge-warn { background: alpha(#e5a50a, 0.22); color: #8f6400; }\n"
        ".store-badge-error { background: alpha(#e01b24, 0.18); color: #c01c28; }\n"
        ".store-badge-installed { background: alpha(@theme_fg_color, 0.10); }\n"
        ".store-badge-update { background: alpha(#3584e4, 0.20); color: #1c71d8; }\n"
        ".store-badge-plain { background: alpha(@theme_fg_color, 0.08); }\n"
        ".store-pill { border-radius: 999px; padding: 5px 16px; }\n"
        ".store-chip { border-radius: 999px; padding: 4px 14px; }\n"
        ".store-info-key { font-size: 12px; color: alpha(@theme_fg_color, 0.62); }\n"
        ".store-info-value { font-size: 13px; }\n"
        ".store-note { font-size: 12px; color: alpha(@theme_fg_color, 0.70); }\n"
        /* a button that reads as a link: the package check opens its details with it */
        ".store-link { background: none; border: none; box-shadow: none; padding: 0 2px;\n"
        "  color: @theme_selected_bg_color; font-size: 12px; }\n"
        ".store-link:hover { text-decoration: underline; }\n"
        ".store-body { font-size: 13px; }\n"
        ".store-empty { color: alpha(@theme_fg_color, 0.62); }\n"
        ".store-job-headline { font-weight: 700; }\n"
        ".store-status { font-size: 12px; color: alpha(@theme_fg_color, 0.70); }\n");
    if (accent_css && *accent_css) g_string_append(css, accent_css);
    gtk_css_provider_load_from_data(store_css, css->str, -1, NULL);
    g_string_free(css, TRUE);
    g_free(accent_css);
}

static void on_theme_changed(gpointer data)
{
    (void)data;
    hde_theme_apply_process();
    apply_store_css();
    if (main_win) gtk_widget_queue_draw(main_win);
}

static void free_all(void)
{
    pkgs_clear(&pkgs_installed);
    pkgs_clear(&pkgs_updates);
    pkgs_clear(&pkgs_search);
    pkgs_clear(&pkgs_featured);
    pkgs_clear(&pkgs_flatpak);
    if (by_name) {
        g_hash_table_destroy(by_name);
        by_name = NULL;
    }
    g_free(detail);
    detail = NULL;
    g_free(detail_return_page);
    detail_return_page = NULL;
    g_free(search_query);
    search_query = NULL;
}

static void on_window_destroy(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    free_all();
    gtk_main_quit();
}

static void print_installed(void)
{
    load_installed();
    for (guint i = 0; pkgs_installed && i < pkgs_installed->len; i++) {
        HdeStorePackage *p = g_ptr_array_index(pkgs_installed, i);
        printf("%s\t%s\t%s\t%s\n", p->name, p->version, p->origin[0] ? p->origin : "-",
               hde_store_verify_title(p->verify, ui_is_vietnamese()));
    }
    free_all();
}

int main(int argc, char **argv)
{
    gboolean list_installed = FALSE;

    /* the arguments that need no display come first: they work over ssh and in the tests */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("Usage: hde-store [--search TEXT] [--updates] [--install-file FILE.deb] [--list-installed]\n"
                   "       hde-store --version\n\n"
                   "Hyggshi Store — the app store of Hyggshi OS and HDE.  Packages are .deb (APT/dpkg);\n"
                   "the Flatpak applications of the machine are listed read-only.\n");
            return 0;
        }
        if (!strcmp(argv[i], "--version")) {
            printf("hde-store %s (%s)\n", HDE_RELEASE, HDE_VERSION);
            return 0;
        }
        if (!strcmp(argv[i], "--list-installed")) list_installed = TRUE;
        else if (!strcmp(argv[i], "--search") && i + 1 < argc) pending_search = g_strdup(argv[++i]);
        else if (!strcmp(argv[i], "--updates")) pending_updates = TRUE;
        else if (!strcmp(argv[i], "--install-file") && i + 1 < argc) pending_file = g_strdup(argv[++i]);
        else if (argv[i][0] != '-') pending_file = g_strdup(argv[i]);   /* `hde-store file.deb` */
    }

    if (list_installed) {
        print_installed();
        g_free(pending_search);
        g_free(pending_file);
        return 0;
    }

    /* The session's language, so GTK's own stock labels (Cancel, Open, …) follow it as well; the Store's
     * own texts are picked by ui_is_vietnamese(), which reads the environment itself. */
    setlocale(LC_ALL, "");

    gtk_init(&argc, &argv);
    hde_theme_apply_process();
    hde_theme_watch(on_theme_changed, NULL);
    apply_store_css();

    /* ---- the window: a sidebar, the pages, and a status line ---- */
    main_win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(main_win), tr(APP_TITLE, "Cửa hàng Hyggshi"));
    gtk_window_set_default_size(GTK_WINDOW(main_win), 1120, 760);
    gtk_widget_set_size_request(main_win, 940, 600);
    gtk_window_set_icon_name(GTK_WINDOW(main_win), APP_ICON);
    g_signal_connect(main_win, "destroy", G_CALLBACK(on_window_destroy), NULL);
    g_signal_connect(main_win, "key-press-event", G_CALLBACK(on_key_press), NULL);

    GtkWidget *header = gtk_header_bar_new();
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header), TRUE);
    gtk_header_bar_set_title(GTK_HEADER_BAR(header), tr("Store", "Cửa hàng"));
    gtk_header_bar_set_subtitle(GTK_HEADER_BAR(header), tr("Install, update and remove software",
                                                          "Cài, cập nhật và gỡ phần mềm"));

    back_button = gtk_button_new_from_icon_name("go-previous-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(back_button, tr("Back to the list", "Quay lại danh sách"));
    gtk_widget_set_sensitive(back_button, FALSE);
    g_signal_connect(back_button, "clicked", G_CALLBACK(on_back_clicked), NULL);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), back_button);

    search_entry = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(search_entry), tr("Search apps…", "Tìm ứng dụng…"));
    gtk_widget_set_size_request(search_entry, 300, -1);
    g_signal_connect(search_entry, "search-changed", G_CALLBACK(on_search_changed), NULL);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), search_entry);

    GtkWidget *refresh = gtk_button_new_from_icon_name("view-refresh-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(refresh, tr("Read the package database again (F5)",
                                            "Đọc lại cơ sở dữ liệu gói (F5)"));
    g_signal_connect(refresh, "clicked", G_CALLBACK(on_refresh_clicked), NULL);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), refresh);

    GtkWidget *file_btn = gtk_button_new_from_icon_name("document-open-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(file_btn, tr("Install a .deb file", "Cài một tệp .deb"));
    g_signal_connect(file_btn, "clicked", G_CALLBACK(on_install_file_clicked), NULL);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), file_btn);

    GtkWidget *about = gtk_button_new_from_icon_name("help-about-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(about, tr("About Hyggshi Store (F1)", "Giới thiệu Cửa hàng (F1)"));
    g_signal_connect(about, "clicked", G_CALLBACK(show_about), NULL);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), about);

    gtk_window_set_titlebar(GTK_WINDOW(main_win), header);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(main_win), root);
    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(root), body, TRUE, TRUE, 0);

    sidebar = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(sidebar), GTK_SELECTION_SINGLE);
    gtk_style_context_add_class(gtk_widget_get_style_context(sidebar), "store-sidebar");
    gtk_widget_set_size_request(sidebar, 214, -1);
    g_signal_connect(sidebar, "row-selected", G_CALLBACK(on_sidebar_changed), NULL);
    gtk_box_pack_start(GTK_BOX(body), sidebar, FALSE, FALSE, 0);

    const char *nav_icons[] = { "view-grid-symbolic", "system-software-install-symbolic",
                                "software-update-available-symbolic", "network-server-symbolic" };
    const char *nav_en[] = { "Discover", "Installed", "Updates", "Sources" };
    const char *nav_vi[] = { "Khám phá", "Đã cài đặt", "Cập nhật", "Nguồn tải" };
    for (int i = 0; i < 4; i++) {
        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        GtkWidget *img = gtk_image_new_from_icon_name(nav_icons[i], GTK_ICON_SIZE_MENU);
        GtkWidget *lbl = label_with_class(tr(nav_en[i], nav_vi[i]), "store-sidebar-label");
        gtk_box_pack_start(GTK_BOX(h), img, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(h), lbl, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(row), h);
        gtk_container_add(GTK_CONTAINER(sidebar), row);
    }

    stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(GTK_STACK(stack), 120);
    gtk_widget_set_hexpand(stack, TRUE);
    gtk_widget_set_vexpand(stack, TRUE);
    gtk_box_pack_start(GTK_BOX(body), stack, TRUE, TRUE, 0);

    GtkWidget *sw;
    sw = page_scroller(&discover_inner);
    gtk_stack_add_named(GTK_STACK(stack), sw, "discover");
    sw = page_scroller(&results_inner);
    gtk_stack_add_named(GTK_STACK(stack), sw, "results");
    sw = page_scroller(&detail_inner);
    gtk_stack_add_named(GTK_STACK(stack), sw, "detail");
    sw = page_scroller(&installed_inner);
    gtk_stack_add_named(GTK_STACK(stack), sw, "installed");
    sw = page_scroller(&updates_inner);
    gtk_stack_add_named(GTK_STACK(stack), sw, "updates");
    sw = page_scroller(&sources_inner);
    gtk_stack_add_named(GTK_STACK(stack), sw, "sources");

    GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(footer), 6);
    gtk_style_context_add_class(gtk_widget_get_style_context(footer), "store-status");
    busy_spinner = gtk_spinner_new();
    gtk_widget_set_no_show_all(busy_spinner, TRUE);
    gtk_box_pack_start(GTK_BOX(footer), busy_spinner, FALSE, FALSE, 0);
    status_label = label_with_class(tr("Ready.", "Sẵn sàng."), "store-status");
    gtk_widget_set_margin_start(status_label, 6);
    gtk_box_pack_start(GTK_BOX(footer), status_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), footer, FALSE, FALSE, 0);

    gtk_widget_show_all(main_win);

    /* ---- fill it in ---- */
    set_busy(TRUE);
    set_status(tr("Reading the package database…", "Đang đọc cơ sở dữ liệu gói…"));
    load_installed();
    load_updates();
    load_flatpak();
    load_featured();
    rebuild_discover();
    rebuild_installed();
    rebuild_updates();
    rebuild_sources();
    rebuild_results();
    rebuild_detail();
    set_busy(FALSE);
    set_status(tr("%u packages installed · %u update(s) available.",
                  "%u gói đã cài · %u bản cập nhật."), pkgs_installed ? pkgs_installed->len : 0,
               pkgs_updates ? pkgs_updates->len : 0);

    select_sidebar_row(GINT_TO_POINTER(0));

    if (pending_search) {
        g_signal_handlers_block_by_func(search_entry, G_CALLBACK(on_search_changed), NULL);
        gtk_entry_set_text(GTK_ENTRY(search_entry), pending_search);
        g_signal_handlers_unblock_by_func(search_entry, G_CALLBACK(on_search_changed), NULL);
        do_search(pending_search);
        g_free(pending_search);
        pending_search = NULL;
    }
    if (pending_updates) select_sidebar_row(GINT_TO_POINTER(2));
    if (pending_file) {
        open_local_file(pending_file);
        g_free(pending_file);
        pending_file = NULL;
    }

    gtk_main();
    return 0;
}
