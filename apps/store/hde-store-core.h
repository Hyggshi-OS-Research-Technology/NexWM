/*
 * apps/store/hde-store-core.h — Hyggshi Store, the plain-C half.
 *
 * Everything the Store knows about a package lives here: what `dpkg-query`, `apt list`,
 * `apt-cache search`, `apt-cache show`, a .deb's control file and a copyright file say about it,
 * plus the Debian version comparison that decides what is newer.  No GTK, no GLib, no processes:
 * tests/store-test.c drives all of it without a package manager and without a display, and
 * apps/store/hde-store.c only puts the results on the screen.
 */
#ifndef HDE_STORE_CORE_H
#define HDE_STORE_CORE_H

#include <stddef.h>

/* Where a package comes from.  .deb (APT/dpkg) is what the Store installs; Flatpak is read-only for now
 * (see the Sources page: "research"), local .deb files are what "Install a file…" takes. */
typedef enum {
    HDE_STORE_SOURCE_DEB = 0,       /* a repository known to APT */
    HDE_STORE_SOURCE_LOCAL = 1,     /* a .deb file on disk, not from a repository */
    HDE_STORE_SOURCE_FLATPAK = 2,   /* a Flatpak remote (read-only in this version) */
    HDE_STORE_SOURCE_DPKG_STATUS = 3, /* only known to dpkg, no repository behind it anymore */
    HDE_STORE_SOURCE_UNKNOWN = 4
} HdeStoreSource;

/* How much the origin can be trusted.  Shown with every package: the Store never installs anything
 * without saying where it comes from. */
typedef enum {
    HDE_STORE_VERIFY_UNKNOWN = 0,     /* cannot tell */
    HDE_STORE_VERIFY_VERIFIED = 1,    /* a signed repository of the distribution (main/stable/…) */
    HDE_STORE_VERIFY_THIRD_PARTY = 2, /* a repository added on top (PPA, vendor repo) */
    HDE_STORE_VERIFY_UNVERIFIED = 3   /* a local file or an origin nobody signed */
} HdeStoreVerify;

typedef enum {
    HDE_STORE_STATE_NOT_INSTALLED = 0,
    HDE_STORE_STATE_INSTALLED = 1,
    HDE_STORE_STATE_UPGRADABLE = 2,
    HDE_STORE_STATE_RESIDUAL = 3,   /* removed, configuration files left behind */
    HDE_STORE_STATE_BROKEN = 4      /* half-installed / half-configured */
} HdeStoreState;

typedef struct {
    char name[80];          /* package name ("gnome-weather") */
    char version[80];       /* installed version, or the version that would be installed */
    char candidate[80];     /* the version a repository offers ("" when unknown) */
    char summary[240];      /* one line ("Weather app for the GNOME desktop") */
    char description[1024]; /* the long description, first paragraph */
    char section[48];       /* "gnome", "utils", "libs" */
    char origin[80];        /* "trixie/main", "now/local", "flatpak: flathub" */
    char homepage[192];
    char license[48];       /* "GPL-3+", "MIT", "Multiple licenses (4)" */
    char arch[24];
    char branch[24];        /* Flatpak: "stable", "beta", … */
    char maintainer[120];
    char app_id[160];       /* the app this package provides (a .desktop id), "" when none */
    unsigned long long size_kb;  /* Installed-Size */
    int installed;          /* installed now */
    int upgradable;         /* a newer version is available */
    int automatic;          /* installed as a dependency, not asked for */
    int residual;           /* configuration files left behind */
    int broken;             /* half-installed / half-configured */
    int from_file;          /* read from a local .deb */
    HdeStoreSource source;
    HdeStoreVerify verify;
    HdeStoreState state;
} HdeStorePackage;

/* `dpkg-query -W -f=…` format that hde_store_parse_dpkg_line() reads: the tabs and the order matter. */
#define HDE_STORE_DPKG_FORMAT \
    "${binary:Package}\t${Version}\t${Status}\t${Section}\t${Installed-Size}\t${binary:Summary}\t${Homepage}\t${Architecture}"

/* ---- versions (dpkg's rules: epoch, upstream version, revision; '~' sorts before nothing) ---- */
/* -1 when a is older than b, 0 when they are the same version, 1 when a is newer. */
int  hde_store_version_compare(const char *a, const char *b);
/* TRUE if `v` looks like a Debian version (it must start with a digit, after an optional epoch). */
int  hde_store_version_valid(const char *v);
/* The version without its epoch ("2:1.2-3" -> "1.2-3"). */
void hde_store_version_upstream(const char *v, char *out, size_t outsz);

/* ---- parsing: one line at a time (returns 1 when the line was understood) ---- */
int  hde_store_parse_dpkg_line(const char *line, HdeStorePackage *out);
int  hde_store_parse_apt_list_line(const char *line, HdeStorePackage *out);
int  hde_store_parse_apt_search_line(const char *line, HdeStorePackage *out);
/* "package - summary" as `apt-cache search` prints it, or the indented "  pkg - summary" of `apt search` */
int  hde_store_parse_flatpak_line(const char *line, HdeStorePackage *out);

/* A whole control stanza (`apt-cache show`, /var/lib/dpkg/status, `dpkg-deb -f`), possibly with
 * continuation lines and several stanzas (the first one is used). */
int  hde_store_parse_control_stanza(const char *text, HdeStorePackage *out);
/* A local .deb: the control fields of `dpkg-deb -f file` and the path it came from. */
int  hde_store_from_deb_control(const char *control_text, const char *path, HdeStorePackage *out);
/* `apt-cache policy <pkg>`: the installed version, the version the repositories offer and where that
 * version comes from ("trixie/main", "now", "/var/lib/dpkg/status").  Returns 1 when a candidate was found. */
int  hde_store_parse_apt_policy(const char *text, char *installed, size_t isz,
                                char *candidate, size_t csz, char *origin, size_t osz);

/* "License: GPL-3+" / the prose of an older /usr/share/doc/<pkg>/copyright, reduced to a short name
 * ("GPL-3+", "MIT", "Apache-2.0", "Multiple licenses (5)").  Returns 1 when something was recognised. */
int  hde_store_license_from_copyright(const char *text, char *out, size_t outsz);
/* The friendly name of a license ("GNU GPL 3 or later").  vi != 0 gives the Vietnamese name: the Store
 * speaks Vietnamese unless the session asks for English (hde_store_speak_vietnamese). */
const char *hde_store_license_title(const char *license, int vi);

/* ---- the sources of the machine (/etc/apt/sources.list and .d) ---- */
typedef struct {
    char types[40];        /* "deb", "deb-src", "deb deb-src" (deb822: "deb") */
    char uri[220];         /* "http://deb.debian.org/debian" */
    char suite[96];        /* "trixie", "trixie-updates" */
    char components[160];  /* "main contrib non-free-firmware" */
    char signed_by[200];   /* the key file, "" when the source trusts the system keyring */
    int  enabled;          /* 0 for a commented-out line or Enabled: no */
    int  flatpak;          /* a Flatpak remote (sources.list.d/flatpak.list) */
} HdeStoreSourceEntry;

/* One line of a classic sources.list: "deb [signed-by=…] http://host/debian trixie main".
 * Returns 1 when the line describes a repository (enabled or commented out). */
int hde_store_parse_sources_line(const char *line, HdeStoreSourceEntry *out);
/* Stanza `index` of a deb822 .sources file (the format used since Ubuntu 24.04 and Debian 13). */
int hde_store_parse_sources_deb822(const char *text, int index, HdeStoreSourceEntry *out);
/* How the Sources page shows one: "deb.debian.org/debian trixie main", "flatpak: flathub". */
void hde_store_source_label(const HdeStoreSourceEntry *s, char *out, size_t outsz);

/* ---- where it comes from, and whether that can be trusted ---- */
HdeStoreVerify hde_store_verify_for(const char *origin, HdeStoreSource source);
const char *hde_store_verify_title(HdeStoreVerify v, int vi);
/* One sentence for the detail page; `origin` may be NULL.  Free with free(). */
char *hde_store_verify_detail(HdeStoreVerify v, const char *origin, int vi);
const char *hde_store_source_title(HdeStoreSource s, int vi);

/* ---- which language the Store speaks ---- */
/* 1 = Vietnamese, 0 = English.  `language` is the session's preference (LANGUAGE, LC_ALL, LC_MESSAGES or
 * LANG — whichever is set first, "vi_VN.UTF-8", "vi:en", "en_GB" …), `forced` the HDE_STORE_LANG override.
 * Vietnamese is the language of the Store and of Hyggshi OS, so it is what is shown unless the session asks
 * for English by name: any other locale (fr_FR, ja_JP, C, an empty environment) gets Vietnamese. */
int hde_store_speak_vietnamese(const char *language, const char *forced);

/* ---- states, sizes, searching ----
 * The functions that return text the user reads take `vi`: 0 is English, anything else Vietnamese
 * (hde_store_speak_vietnamese() answers which one this session gets). */
const char *hde_store_state_title(HdeStoreState s, int vi);
/* "12.4 MB" from Installed-Size (KiB). */
void        hde_store_format_size(unsigned long long kb, char *out, size_t outsz);
/* Case-insensitive, accent-tolerant search.  0 = no match; higher = a better match
 * (name starts with the query 4, name contains it 3, an app id/summary word starts with it 2,
 *  the summary contains it 1). */
int         hde_store_match(const HdeStorePackage *p, const char *query);
/* Ordering for a list: the better match first, then installed/upgradable, then the name.
 * Returns TRUE when `a` should come before `b`. */
int         hde_store_package_before(const HdeStorePackage *a, const HdeStorePackage *b);
/* Lower-case, accents removed: "Cài đặt" -> "cai dat".  For comparing what the user typed with names. */
void        hde_store_fold(const char *in, char *out, size_t outsz);

/* ---- what the Store would run (the GUI and the tests agree on these) ---- */
/* Update the lists: "apt-get update" (as root, through pkexec). */
extern const char *const HDE_STORE_CMD_UPDATE[];
/* Install: "apt-get install -y --no-install-recommends <pkg>" */
extern const char *const HDE_STORE_CMD_INSTALL[];
/* Remove: "apt-get remove -y <pkg>" */
extern const char *const HDE_STORE_CMD_REMOVE[];
/* Remove with the configuration: "apt-get purge -y <pkg>" */
extern const char *const HDE_STORE_CMD_PURGE[];
/* Install a local file: "apt-get install -y ./file.deb" (its dependencies come from the repositories) */
extern const char *const HDE_STORE_CMD_INSTALL_FILE[];
/* The package list of the machine: `dpkg-query -W -f=<HDE_STORE_DPKG_FORMAT>` */
extern const char *const HDE_STORE_CMD_DPKG_QUERY[];
/* What a repository offers: `apt list --upgradable` */
extern const char *const HDE_STORE_CMD_UPGRADABLE[];
/* Search: `apt-cache search --names-only <query>` */
extern const char *const HDE_STORE_CMD_SEARCH[];
/* What a package offers right now: `apt-cache policy <pkg>` */
extern const char *const HDE_STORE_CMD_POLICY[];
/* Flatpak, read-only: `flatpak list --columns=application,version,origin,branch` (empty without flatpak) */
extern const char *const HDE_STORE_CMD_FLATPAK[];

/* `base` (NULL-terminated) copied with `a` and `b` appended (either may be NULL).  Free with
 * hde_store_argv_free().  Returns NULL when out of memory. */
char **hde_store_argv_append(const char *const *base, const char *a, const char *b);
void   hde_store_argv_free(char **argv);

#endif /* HDE_STORE_CORE_H */
