/* hde-distro.h — which system HDE runs on and how to install something on it.
 *
 * Header-only and free of GTK/GLib: the session manager, the panel, Settings, the screenshot tool and the test
 * programs all include it, so a missing program is never reported as "sudo apt install ..." on a Fedora machine.
 * The messages say what the system in front of the user needs:
 *
 *   Debian / Ubuntu / Linux Mint / Hyggshi OS   sudo apt install metacity
 *   Fedora / RHEL / Nobara / Ultramarine        sudo dnf install metacity
 *   openSUSE                                    sudo zypper install metacity
 *   Arch / Manjaro                              sudo pacman -S metacity
 *
 * The name of the package is translated too (Debian's libxi-dev is Fedora's libXi-devel, Debian's network-manager is
 * Fedora's NetworkManager, ...): hde_install_hint("network-manager") -> "sudo dnf install NetworkManager".
 *
 * HDE_OS_RELEASE=<file> reads another os-release file (tests, screenshots of other systems), like src/hde-osinfo.c.
 */
#ifndef HDE_DISTRO_H
#define HDE_DISTRO_H

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Package families: who installs packages from where (and what the packages are called). */
enum { HDE_FAMILY_UNKNOWN = 0, HDE_FAMILY_DEB, HDE_FAMILY_RPM, HDE_FAMILY_SUSE, HDE_FAMILY_ARCH };

static inline const char *hde_distro_family_name(int family)
{
    switch (family) {
    case HDE_FAMILY_DEB:   return "deb";
    case HDE_FAMILY_RPM:   return "rpm";
    case HDE_FAMILY_SUSE:  return "suse";
    case HDE_FAMILY_ARCH:  return "arch";
    default:               return "unknown";
    }
}

/* os-release to read: HDE_OS_RELEASE (tests), /etc/os-release, /usr/lib/os-release. */
static inline const char *hde_distro_os_release(void)
{
    const char *env = getenv("HDE_OS_RELEASE");
    if (env && *env) return env;              /* never cached: the tests switch the file between calls */
    static const char *cached;
    if (cached) return cached;
    FILE *f = fopen("/etc/os-release", "r");
    if (f) { fclose(f); cached = "/etc/os-release"; return cached; }
    cached = "/usr/lib/os-release";
    return cached;
}

/* Our own string copy: strdup() is a POSIX extension glibc hides when the compiler is asked for strict ISO C
 * (-std=c11), and there a missing declaration is not just a warning: with `int` as the return type every string this
 * file hands out becomes a truncated pointer (which is exactly how the distro unit test crashed on CI). malloc +
 * memcpy depends on nothing but the C standard. */
static inline char *hde_distro_dup(const char *s)
{
    size_t n = strlen(s ? s : "") + 1;
    char *p = malloc(n);
    if (!p) return NULL;
    memcpy(p, s ? s : "", n);
    return p;
}

/* The value of one key of the os-release file (unquoted), or NULL. The caller frees it.
 * It is read on every call: the tests (and the ISO build) switch HDE_OS_RELEASE between files. */
static inline char *hde_distro_value(const char *key)
{
    const char *path = hde_distro_os_release();
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char line[1024];
    char *res = NULL;
    size_t klen = strlen(key);
    while (fgets(line, sizeof line, f)) {
        char *l = line;
        while (*l == ' ' || *l == '\t') l++;
        if (*l == '#' || strncmp(l, key, klen) || l[klen] != '=') continue;
        char *v = l + klen + 1;
        char *nl = strchr(v, '\n');
        if (nl) *nl = '\0';
        while (*v == ' ' || *v == '\t') v++;
        size_t n = strlen(v);
        while (n && (v[n - 1] == ' ' || v[n - 1] == '\t')) v[--n] = '\0';
        if (n >= 2 && ((v[0] == '"' && v[n - 1] == '"') || (v[0] == '\'' && v[n - 1] == '\''))) {
            v[n - 1] = '\0';
            v++;
        }
        free(res);
        res = hde_distro_dup(v);
    }
    fclose(f);
    return res;
}

/* "fedora", "debian", "hyggshios", ... (ID, lower case) — "" when there is no os-release at all. */
static inline char *hde_distro_id(void)
{
    char *id = hde_distro_value("ID");
    if (id && *id) {
        for (char *p = id; *p; p++) *p = (char)tolower((unsigned char)*p);
        return id;
    }
    free(id);
    char *pretty = hde_distro_value("NAME");
    if (pretty && *pretty) return pretty;
    free(pretty);
    return hde_distro_dup("");
}

/* NAME / PRETTY_NAME, for messages ("Fedora Linux 42"): never NULL. */
static inline char *hde_distro_name(void)
{
    char *n = hde_distro_value("PRETTY_NAME");
    if (n && *n) return n;
    free(n);
    n = hde_distro_value("NAME");
    if (n && *n) return n;
    free(n);
    return hde_distro_dup("this system");
}

static inline char *hde_distro_version(void)
{
    char *v = hde_distro_value("VERSION_ID");
    return v && *v ? v : (free(v), hde_distro_dup(""));
}

/* Does the id (or one of the ids in ID_LIKE) contain `want`? "debian" matches Ubuntu, Mint, Hyggshi OS. */
static inline int hde_distro_is(const char *want)
{
    char *id = hde_distro_id();
    int hit = id && *id && strstr(id, want) != NULL;
    free(id);
    if (hit) return 1;
    char *like = hde_distro_value("ID_LIKE");
    if (!like) return 0;
    /* ID_LIKE is a space separated list of ids */
    for (char *p = like; *p; p++) *p = (char)tolower((unsigned char)*p);
    hit = strstr(like, want) != NULL;
    free(like);
    return hit;
}

/* The family of the system: HDE_FAMILY_DEB / _RPM / _SUSE / _ARCH / _UNKNOWN. */
static inline int hde_distro_family(void)
{
    static const char *const deb[]  = { "debian", "ubuntu", "linuxmint", "raspbian", "hyggshios", "pop", "elementary",
                                        "zorin", "kali", "devuan", "neon", "mx", "antix", "deepin", "uos", NULL };
    static const char *const rpm[]  = { "fedora", "rhel", "redhat", "centos", "rocky", "almalinux", "ol", "amzn",
                                        "nobara", "ultramarine", "bazzite", "qubes", "oracle", "clear-linux-os",
                                        "azurelinux", "mariner", NULL };
    static const char *const suse[] = { "suse", "sles", "opensuse", NULL };
    static const char *const arch[] = { "arch", "manjaro", "endeavouros", "garuda", "cachyos", "artix", NULL };

    for (int i = 0; deb[i]; i++)  if (hde_distro_is(deb[i]))  return HDE_FAMILY_DEB;
    for (int i = 0; rpm[i]; i++)  if (hde_distro_is(rpm[i]))  return HDE_FAMILY_RPM;
    for (int i = 0; suse[i]; i++) if (hde_distro_is(suse[i])) return HDE_FAMILY_SUSE;
    for (int i = 0; arch[i]; i++) if (hde_distro_is(arch[i])) return HDE_FAMILY_ARCH;
    return HDE_FAMILY_UNKNOWN;
}

/* "apt", "dnf", "yum", "zypper", "pacman" — the program that installs packages here ("" when unknown). */
static inline const char *hde_distro_pkgcmd(void)
{
    switch (hde_distro_family()) {
    case HDE_FAMILY_DEB:  return "apt";
    case HDE_FAMILY_RPM:  return "dnf";
    case HDE_FAMILY_SUSE: return "zypper";
    case HDE_FAMILY_ARCH: return "pacman";
    default:              return "";
    }
}

/* The name of a package on this system, translated from the Debian name used in HDE's sources and documentation. */
static inline const char *hde_distro_pkg(const char *pkg)
{
    static const struct { const char *deb, *rpm, *arch, *suse; } alias[] = {
        /* library / build packages */
        { "libgtk-3-dev",            "gtk3-devel",              "gtk3",                 "gtk3-devel" },
        { "libwnck-3-dev",           "libwnck3-devel",          "libwnck3",             "libwnck3-devel" },
        { "libgtk-layer-shell-dev",  "gtk-layer-shell-devel",   "gtk-layer-shell",      "gtk-layer-shell-devel" },
        { "libxi-dev",               "libXi-devel",             "libxi",                "libXi-devel" },
        { "libxrandr-dev",           "libXrandr-devel",         "libxrandr",            "libXrandr-devel" },
        { "libx11-dev",              "libX11-devel",            "libx11",               "libX11-devel" },
        { "libxfixes-dev",           "libXfixes-devel",         "libxfixes",            "libXfixes-devel" },
        { "libxcomposite-dev",       "libXcomposite-devel",     "libxcomposite",        "libXcomposite-devel" },
        { "libxcursor-dev",          "libXcursor-devel",        "libxcursor",           "libXcursor-devel" },
        { "libwayland-dev",          "wayland-devel",           "wayland",              "wayland-devel" },
        { "libxkbcommon-dev",        "libxkbcommon-devel",      "libxkbcommon",         "libxkbcommon-devel" },
        { "libxkbcommon-x11-dev",    "libxkbcommon-x11-devel",  "libxkbcommon-x11",     "libxkbcommon-x11-devel" },
        { "libinput-dev",            "libinput-devel",          "libinput",             "libinput-devel" },
        { "libpixman-1-dev",         "pixman-devel",            "pixman",               "pixman-devel" },
        { "libcairo2-dev",           "cairo-devel",             "cairo",                "cairo-devel" },
        { "libvte-2.91-dev",         "vte291-devel",            "vte3",                 "vte-devel" },
        { "libwlroots-dev",          "wlroots-devel",           "wlroots",              "wlroots-devel" },
        { "libpam0g-dev",            "pam-devel",               "pam",                  "pam-devel" },
        { "qtbase5-dev",             "qt5-qtbase-devel",        "qt5-base",             "qt5-base-devel" },
        { "qtdeclarative5-dev",      "qt5-qtdeclarative-devel", "qt5-declarative",      "qt5-declarative-devel" },
        { "build-essential",         "gcc gcc-c++ make",        "base-devel",           "gcc gcc-c++ make" },
        { "pkg-config",              "pkgconf-pkg-config",      "pkgconf",              "pkgconf-pkg-config" },
        /* runtime programs */
        { "network-manager",         "NetworkManager",          "networkmanager",       "NetworkManager" },
        { "policykit-1-gnome",       "polkit-gnome",            "polkit-gnome",         "polkit-gnome" },
        { "pulseaudio-utils",        "pulseaudio-utils",        "libpulse",             "pulseaudio-utils" },
        { "libnotify-bin",           "libnotify",               "libnotify",            "libnotify-tools" },
        { "xserver-xephyr",          "xorg-x11-server-Xephyr",  "xorg-server-xephyr",   "xorg-x11-server" },
        { "xserver-xorg-core",       "xorg-x11-server-Xorg",    "xorg-server",          "xorg-x11-server" },
        { "xserver-xorg-video-dummy","xorg-x11-drv-dummy",      "xf86-video-dummy",     "xf86-video-dummy" },
        { "xvfb",                    "xorg-x11-server-Xvfb",    "xorg-server-xvfb",     "xorg-x11-server" },
        { "x11-xserver-utils",       "xset xsetroot xrandr",    "xorg-xset xorg-xsetroot", "xset xsetroot" },
        { "x11-utils",               "xprop xwininfo",          "xorg-xprop xorg-xwininfo", "xprop xwininfo" },
        { "xwayland",                "xorg-x11-server-Xwayland", "xorg-xwayland",       "xwayland" },
        { "dbus-x11",                "dbus-tools",              "dbus",                 "dbus-1-tools" },
        { "python3-dbusmock",        "python3-dbusmock",        "python-dbusmock",      "python3-dbusmock" },
        { "imagemagick",             "ImageMagick",             "imagemagick",          "ImageMagick" },
        { "adwaita-icon-theme",      "adwaita-icon-theme",      "adwaita-icon-theme",   "adwaita-icon-theme" },
        { "librsvg2-common",         "librsvg2",                "librsvg",              "librsvg2" },
        { NULL, NULL, NULL, NULL }
    };
    int fam = hde_distro_family();
    for (int i = 0; alias[i].deb; i++) {
        if (strcmp(alias[i].deb, pkg)) continue;
        switch (fam) {
        case HDE_FAMILY_RPM:  return alias[i].rpm;
        case HDE_FAMILY_ARCH: return alias[i].arch;
        case HDE_FAMILY_SUSE: return alias[i].suse;
        default:              return alias[i].deb;
        }
    }
    return pkg;
}

/* "sudo dnf install metacity": the install command of this system for already translated package names
 * (apt/dnf/zypper take "install", pacman "-S"). */
static inline char *hde_install_hint_names(const char *names)
{
    const char *cmd = hde_distro_pkgcmd();
    int pacman = !strcmp(cmd, "pacman");
    size_t n = strlen(names) + strlen(cmd) + 32;
    char *r = malloc(n);
    if (!r) return NULL;
    if (!*cmd) snprintf(r, n, "install %s", names);
    else snprintf(r, n, "sudo %s %s %s", cmd, pacman ? "-S" : "install", names);
    return r;
}

/* "sudo dnf install metacity": how to install `pkg` here (pkg given with its Debian name, translated for the system). */
static inline char *hde_install_hint(const char *pkg)
{
    const char *name = hde_distro_pkg(pkg);
    return hde_install_hint_names(name);
}

/* The packages the Debian instructions list, for the family of the system. Returns one command ("sudo dnf install
 * gtk3-devel libwnck3-devel ..."). NULL when there is nothing to say. What = "build" (compiling HDE), "runtime" (the
 * recommended programs) or "test" (the test suite). */
static inline char *hde_deps_hint(const char *what)
{
    static const char *const build_deb[] = { "build-essential", "pkg-config", "libgtk-3-dev", "libwnck-3-dev",
                                             "libxi-dev", "libxrandr-dev", "libx11-dev", "libgtk-layer-shell-dev",
                                             "libwayland-dev", NULL };
    static const char *const runtime_deb[] = { "metacity", "network-manager", "bluez", "pulseaudio-utils",
                                               "policykit-1-gnome", "gnome-themes-extra", "libnotify-bin", "playerctl",
                                               "labwc", "grim", "slurp", "swaylock", NULL };
    static const char *const test_deb[] = { "xvfb", "xdotool", "dbus-x11", "python3", "imagemagick", "xsltproc", NULL };
    const char *const *list = NULL;
    if (!strcmp(what, "build")) list = build_deb;
    else if (!strcmp(what, "runtime")) list = runtime_deb;
    else if (!strcmp(what, "test")) list = test_deb;
    if (!list) return NULL;
    const char *cmd = hde_distro_pkgcmd();
    int pacman = !strcmp(cmd, "pacman");
    size_t n = 64;
    char *r = malloc(n);
    if (!r) return NULL;
    if (!*cmd) snprintf(r, n, "install");
    else snprintf(r, n, "sudo %s %s", cmd, pacman ? "-S" : "install");
    for (int i = 0; list[i]; i++) {
        const char *name = hde_distro_pkg(list[i]);
        size_t need = strlen(r) + strlen(name) + 2;
        if (need > n) {
            n = need * 2;
            char *grown = realloc(r, n);
            if (!grown) { free(r); return NULL; }
            r = grown;
        }
        strcat(r, " ");
        strcat(r, name);
    }
    return r;
}

/* The note shown when HDE was built without libXInput2 (Debian's libxi-dev, Fedora's libXi-devel): the touchpad, the
 * mouse and the Super key need it. Returns a string the caller frees. */
static inline char *hde_xi_missing_note(void)
{
    char *hint = hde_install_hint("libxi-dev");
    size_t n = strlen(hint) + 128;
    char *r = malloc(n);
    if (r) snprintf(r, n, "HDE was built without libXInput2, so the touchpad and the mouse cannot be changed. "
                          "Install it (%s) and rebuild HDE.", hint);
    free(hint);
    return r;
}

/* What the desktop of this system needs in RAM, as a sentence for Settings > About (the page used to say Debian
 * everywhere). */
static inline char *hde_distro_ram_advice(void)
{
    int fam = hde_distro_family();
    if (fam == HDE_FAMILY_RPM)
        return hde_distro_dup("Fedora asks for 2 GB of RAM for a Workstation install and recommends 4 GB (a Fedora Live image "
                      "needs at least 2 GB, like every live system it runs from RAM).");
    if (fam == HDE_FAMILY_ARCH)
        return hde_distro_dup("Arch needs no more than 512 MB to install, but a desktop with a browser wants 2 GB or more.");
    if (fam == HDE_FAMILY_SUSE)
        return hde_distro_dup("openSUSE recommends 2 GB of RAM for a desktop and 4 GB for comfortable work.");
    if (hde_distro_is("ubuntu"))
        return hde_distro_dup("Ubuntu asks for at least 4 GB of RAM for its desktop (2 GB is the absolute minimum with swap).");
    if (fam == HDE_FAMILY_DEB)
        return hde_distro_dup("Debian asks for at least 1 GB of RAM for a desktop and recommends 2 GB (with swap; a live USB "
                      "stick needs more, it runs from RAM).");
    return hde_distro_dup("A desktop system with a browser wants at least 2 GB of RAM (4 GB is comfortable).");
}

#endif
