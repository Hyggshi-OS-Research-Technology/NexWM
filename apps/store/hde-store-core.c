/*
 * apps/store/hde-store-core.c — Hyggshi Store, the plain-C half.  See hde-store-core.h.
 *
 * The parsers here match what the tools on a Debian / Ubuntu / Hyggshi OS machine actually print:
 *   dpkg-query -W -f=…      one line per package, Tab separated (HDE_STORE_DPKG_FORMAT)
 *   apt list --upgradable   "bash/trixie 5.2.15-2+b4 amd64 [upgradable from: 5.2.15-2+b3]"
 *   apt-cache search        "gnome-weather - Weather app for the GNOME desktop"
 *   apt-cache show          control stanzas, with the long description on continuation lines
 *   apt-cache policy        "Installed:", "Candidate:" and the repository each version comes from
 *   flatpak list            Tab separated application, version, origin, branch
 *   /usr/share/doc/<pkg>/copyright  the license (DEP-5 "License:" fields, or plain prose)
 * tests/store-test.c checks all of them with the real output of these tools where it can, and the
 * Debian version comparison against `dpkg --compare-versions`.
 */
#define _POSIX_C_SOURCE 200809L

#include "hde-store-core.h"
#include "hde-store-fold.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/* ---------------------------------------------------------------- small helpers */

/* The two texts of one message: English, Vietnamese. */
static const char *pick(int vi, const char *en, const char *vi_text)
{
    return vi ? vi_text : en;
}

static void put(char *dst, size_t sz, const char *src)
{
    if (!dst || !sz) return;
    if (!src) { dst[0] = 0; return; }
    size_t n = strlen(src);
    if (n >= sz) n = sz - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

/* copy [begin, end) without the spaces, tabs and line ends around it */
static void put_range(char *dst, size_t sz, const char *begin, const char *end)
{
    if (!dst || !sz) return;
    while (begin < end && (*begin == ' ' || *begin == '\t')) begin++;
    while (end > begin && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) end--;
    size_t n = (size_t)(end - begin);
    if (n >= sz) n = sz - 1;
    if (n) memcpy(dst, begin, n);
    dst[n] = 0;
}

static int starts_with_ci(const char *s, const char *prefix)
{
    if (!s || !prefix) return 0;
    for (; *prefix; s++, prefix++) {
        if (!*s) return 0;
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) return 0;
    }
    return 1;
}

static const char *skip_spaces(const char *s)
{
    while (s && (*s == ' ' || *s == '\t')) s++;
    return s;
}

/* the value of "Field: value" on one line, trimmed; NULL when the line is not that field */
static const char *control_value(const char *line, const char *field)
{
    if (!line || !field) return NULL;
    size_t n = strlen(field);
    if (strncasecmp(line, field, n) != 0) return NULL;
    const char *p = line + n;
    if (*p != ':' && *p != ' ' && *p != '\t') return NULL;
    while (*p && *p != ':') p++;
    if (*p != ':') return NULL;
    return skip_spaces(p + 1);
}

void hde_store_fold(const char *in, char *out, size_t outsz)
{
    if (!out || !outsz) return;
    size_t o = 0;
    for (size_t i = 0; in && in[i];) {
        const unsigned char c = (unsigned char)in[i];
        if (c < 0x80) {
            if (o + 1 < outsz) out[o++] = (char)tolower(c);
            i++;
            continue;
        }
        const char *to = NULL;
        size_t used = 0;
        for (size_t k = 0; k < sizeof hde_store_fold_table / sizeof hde_store_fold_table[0]; k++) {
            const char *from = hde_store_fold_table[k].from;
            size_t l = strlen(from);
            if (strncmp(in + i, from, l) == 0) { to = hde_store_fold_table[k].to; used = l; break; }
        }
        if (to) {
            for (const char *t = to; *t && o + 1 < outsz; t++) out[o++] = (char)tolower((unsigned char)*t);
            i += used;
        } else {
            /* not a letter we know: keep the byte so that nothing silently disappears */
            if (o + 1 < outsz) out[o++] = in[i];
            i++;
        }
    }
    out[o] = 0;
}

/* ---------------------------------------------------------------- versions */

/* dpkg's order(): digits sort as one class, '~' before everything, letters by their ASCII value,
 * other characters after the letters. */
static int version_order(int c)
{
    if (isdigit(c)) return 0;
    if (isalpha(c)) return c;
    if (c == '~') return -1;
    if (c) return c + 256;
    return 0;
}

/* dpkg's verrevcmp(): letter by letter, then the numbers run by run, compared as numbers. */
static int verrevcmp(const char *a, const char *b)
{
    while (*a || *b) {
        int first_diff = 0;
        while ((*a && !isdigit((unsigned char)*a)) || (*b && !isdigit((unsigned char)*b))) {
            const int ac = version_order((unsigned char)*a);
            const int bc = version_order((unsigned char)*b);
            if (ac != bc) return ac - bc;
            if (*a) a++;
            if (*b) b++;
        }
        while (*a == '0') a++;
        while (*b == '0') b++;
        while (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            if (!first_diff) first_diff = *a - *b;
            a++;
            b++;
        }
        if (isdigit((unsigned char)*a)) return 1;
        if (isdigit((unsigned char)*b)) return -1;
        if (first_diff) return first_diff;
    }
    return 0;
}

static void version_split(const char *v, long *epoch, const char **up_begin, const char **up_end,
                          const char **rev_begin, const char **rev_end)
{
    *epoch = 0;
    const char *start = v ? v : "";
    const char *colon = strchr(start, ':');
    if (colon && colon != start) {
        int digits = 1;
        for (const char *p = start; p < colon; p++)
            if (!isdigit((unsigned char)*p)) { digits = 0; break; }
        if (digits) {
            *epoch = strtol(start, NULL, 10);
            start = colon + 1;
        }
    }
    const char *dash = strrchr(start, '-');
    *up_begin = start;
    if (dash) {
        *up_end = dash;
        *rev_begin = dash + 1;
        *rev_end = start + strlen(start);
    } else {
        *up_end = start + strlen(start);
        *rev_begin = *up_end;
        *rev_end = *up_end;
    }
}

int hde_store_version_compare(const char *a, const char *b)
{
    long ea = 0, eb = 0;
    const char *ua, *uae, *ra, *rae, *ub, *ube, *rb, *rbe;
    version_split(a, &ea, &ua, &uae, &ra, &rae);
    version_split(b, &eb, &ub, &ube, &rb, &rbe);
    if (ea != eb) return ea < eb ? -1 : 1;

    char bufa[256], bufb[256];
    put_range(bufa, sizeof bufa, ua, uae);
    put_range(bufb, sizeof bufb, ub, ube);
    int r = verrevcmp(bufa, bufb);
    if (r) return r < 0 ? -1 : 1;

    put_range(bufa, sizeof bufa, ra, rae);
    put_range(bufb, sizeof bufb, rb, rbe);
    r = verrevcmp(bufa, bufb);
    return r < 0 ? -1 : r > 0 ? 1 : 0;
}

int hde_store_version_valid(const char *v)
{
    if (!v || !*v) return 0;
    const char *colon = strchr(v, ':');
    const char *p = v;
    if (colon && colon != v) {
        int digits = 1;
        for (const char *q = v; q < colon; q++)
            if (!isdigit((unsigned char)*q)) { digits = 0; break; }
        if (digits) p = colon + 1;
    }
    return isdigit((unsigned char)*p) != 0;
}

void hde_store_version_upstream(const char *v, char *out, size_t outsz)
{
    const char *colon = v ? strchr(v, ':') : NULL;
    if (colon && colon != v) {
        int digits = 1;
        for (const char *q = v; q < colon; q++)
            if (!isdigit((unsigned char)*q)) { digits = 0; break; }
        if (digits) v = colon + 1;
    }
    put(out, outsz, v ? v : "");
}

/* ---------------------------------------------------------------- states and sizes */

static void apply_status(const char *status, HdeStorePackage *p)
{
    if (!status) return;
    if (strstr(status, "installed") && strncmp(status, "install ok", 10) == 0) p->installed = 1;
    if (strstr(status, "config-files")) p->residual = 1;
    if (strstr(status, "half-installed") || strstr(status, "half-configured") ||
        strstr(status, "unpacked") || strstr(status, "triggers-awaited") || strstr(status, "triggers-pending"))
        p->broken = 1;
}

static void derive_state(HdeStorePackage *p)
{
    if (p->broken) p->state = HDE_STORE_STATE_BROKEN;
    else if (p->residual && !p->installed) p->state = HDE_STORE_STATE_RESIDUAL;
    else if (p->installed && p->upgradable) p->state = HDE_STORE_STATE_UPGRADABLE;
    else if (p->installed) p->state = HDE_STORE_STATE_INSTALLED;
    else p->state = HDE_STORE_STATE_NOT_INSTALLED;
}

/* ---------------------------------------------------------------- dpkg-query */

int hde_store_parse_dpkg_line(const char *line, HdeStorePackage *out)
{
    if (!line || !out || !*line) return 0;
    memset(out, 0, sizeof *out);

    char buf[4096];
    put(buf, sizeof buf, line);

    char *field[8] = { NULL };
    int n = 0;
    char *p = buf;
    field[n++] = p;
    while (n < 8) {
        char *tab = strchr(p, '\t');
        if (!tab) break;
        *tab = 0;
        p = tab + 1;
        field[n++] = p;
    }
    if (n < 3 || !field[0][0]) return 0;

    put(out->name, sizeof out->name, field[0]);
    put(out->version, sizeof out->version, field[1]);
    apply_status(field[2], out);
    if (n > 3) put(out->section, sizeof out->section, field[3]);
    if (n > 4 && field[4][0]) out->size_kb = strtoull(field[4], NULL, 10);
    if (n > 5) put(out->summary, sizeof out->summary, field[5]);
    if (n > 6) put(out->homepage, sizeof out->homepage, field[6]);
    if (n > 7) put(out->arch, sizeof out->arch, field[7]);
    if (!out->summary[0]) put(out->summary, sizeof out->summary, out->name);
    if (!out->section[0]) put(out->section, sizeof out->section, "unknown");

    out->source = out->installed ? HDE_STORE_SOURCE_UNKNOWN : HDE_STORE_SOURCE_DEB;
    out->verify = HDE_STORE_VERIFY_UNKNOWN;
    derive_state(out);
    return 1;
}

/* ---------------------------------------------------------------- apt list */

/* "[installed,automatic]", "[upgradable from: 1.2]", "[installed,upgradable to: 1.3]" */
static void apply_apt_flags(const char *flags, HdeStorePackage *p)
{
    if (!flags) return;
    if (strstr(flags, "installed")) p->installed = 1;
    if (strstr(flags, "automatic")) p->automatic = 1;
    if (strstr(flags, "residual-config")) p->residual = 1;
    if (strstr(flags, "local")) put(p->origin, sizeof p->origin, "now/local");

    const char *up = strstr(flags, "upgradable");
    if (!up) return;
    p->upgradable = 1;
    const char *to = strstr(up, "upgradable to:");
    if (to) {
        /* "[installed,upgradable to: 1.3]": field 2 is the installed version */
        put(p->candidate, sizeof p->candidate, skip_spaces(to + strlen("upgradable to:")));
        char *close = strchr(p->candidate, ']');
        if (close) *close = 0;
    }
    const char *from = strstr(up, "upgradable from:");
    if (from) {
        /* `apt list --upgradable`: field 2 is the candidate, "from" is what is installed */
        char installed[80];
        put(installed, sizeof installed, skip_spaces(from + strlen("upgradable from:")));
        char *close = strchr(installed, ']');
        if (close) *close = 0;
        put(p->candidate, sizeof p->candidate, p->version);
        put(p->version, sizeof p->version, installed);
        p->installed = 1;
    }
    if (!p->candidate[0]) put(p->candidate, sizeof p->candidate, p->version);
}

int hde_store_parse_apt_list_line(const char *line, HdeStorePackage *out)
{
    if (!line || !out || !*line) return 0;
    if (starts_with_ci(line, "Listing...")) return 0;
    memset(out, 0, sizeof *out);

    const char *slash = strchr(line, '/');
    if (!slash || slash == line) return 0;

    put_range(out->name, sizeof out->name, line, slash);

    const char *p = slash + 1;
    const char *suite_begin = p;
    while (*p && *p != ' ') p++;
    char suites[96];
    put_range(suites, sizeof suites, suite_begin, p);

    p = skip_spaces(p);
    const char *ver_begin = p;
    while (*p && *p != ' ') p++;
    put_range(out->version, sizeof out->version, ver_begin, p);
    if (!out->version[0]) return 0;

    p = skip_spaces(p);
    const char *arch_begin = p;
    while (*p && *p != ' ') p++;
    put_range(out->arch, sizeof out->arch, arch_begin, p);

    const char *flags = strchr(p, '[');

    /* the suite: "trixie" or "trixie,now" (the second one is where it is installed from) */
    char suite[64];
    put(suite, sizeof suite, suites);
    char *comma = strchr(suite, ',');
    if (comma) *comma = 0;
    if (!strcmp(suite, "now")) {
        out->installed = 1;
        put(out->origin, sizeof out->origin, "now");
    } else if (suite[0]) {
        put(out->origin, sizeof out->origin, suites);
        /* "trixie,now": installed from a repository, present on the machine */
        if (strstr(suites, ",now")) out->installed = 1;
    }

    apply_apt_flags(flags, out);
    out->source = (!strcmp(out->origin, "now") || !strcmp(out->origin, "now/local"))
                      ? HDE_STORE_SOURCE_DPKG_STATUS
                      : HDE_STORE_SOURCE_DEB;
    out->verify = hde_store_verify_for(out->origin, out->source);
    if (!out->summary[0]) put(out->summary, sizeof out->summary, out->name);
    if (!out->section[0]) put(out->section, sizeof out->section, "unknown");
    derive_state(out);
    return 1;
}

/* ---------------------------------------------------------------- apt-cache search */

int hde_store_parse_apt_search_line(const char *line, HdeStorePackage *out)
{
    if (!line || !out) return 0;
    const char *text = skip_spaces(line);
    if (!*text) return 0;
    memset(out, 0, sizeof *out);

    /* "name - summary"; older apt-cache may print "name1, name2 - summary" */
    const char *dash = strstr(text, " - ");
    const char *name_end = dash ? dash : text + strlen(text);
    while (name_end > text && (name_end[-1] == ' ' || name_end[-1] == ',')) name_end--;
    const char *name_begin = text;
    for (const char *p = text; p < name_end; p++)
        if (*p == ' ') { name_begin = p + 1; break; }   /* the last name is the one we show */
    put_range(out->name, sizeof out->name, name_begin, name_end);
    if (!out->name[0]) return 0;
    if (dash) {
        put_range(out->summary, sizeof out->summary, dash + 3, text + strlen(text));
    }
    if (!out->summary[0]) put(out->summary, sizeof out->summary, out->name);
    put(out->section, sizeof out->section, "unknown");
    out->source = HDE_STORE_SOURCE_DEB;
    out->verify = HDE_STORE_VERIFY_UNKNOWN;
    derive_state(out);
    return 1;
}

/* ---------------------------------------------------------------- flatpak (read-only) */

int hde_store_parse_flatpak_line(const char *line, HdeStorePackage *out)
{
    if (!line || !out || !*line) return 0;
    if (starts_with_ci(line, "application") || starts_with_ci(line, "no installed"))
        return 0;
    memset(out, 0, sizeof *out);

    char buf[512];
    put(buf, sizeof buf, line);
    char *tab[4] = { NULL };
    int n = 0;
    char *p = buf;
    tab[n++] = p;
    while (n < 4) {
        char *t = strchr(p, '\t');
        if (!t) break;
        *t = 0;
        p = t + 1;
        tab[n++] = p;
    }
    if (!tab[0][0]) return 0;
    put(out->name, sizeof out->name, tab[0]);
    if (n > 1) put(out->version, sizeof out->version, tab[1]);
    if (n > 2) put(out->origin, sizeof out->origin, tab[2]);
    if (n > 3) put(out->branch, sizeof out->branch, tab[3]);
    put(out->summary, sizeof out->summary, out->name);
    put(out->section, sizeof out->section, "flatpak");
    put(out->app_id, sizeof out->app_id, out->name);
    if (out->origin[0]) {
        char with_prefix[96];
        snprintf(with_prefix, sizeof with_prefix, "flatpak: %s", out->origin);
        put(out->origin, sizeof out->origin, with_prefix);
    }
    out->source = HDE_STORE_SOURCE_FLATPAK;
    out->verify = HDE_STORE_VERIFY_THIRD_PARTY;
    out->installed = 1;
    out->state = HDE_STORE_STATE_INSTALLED;
    return 1;
}

/* ---------------------------------------------------------------- control stanzas */

static void append_text(char *dst, size_t sz, const char *add)
{
    if (!dst || !sz || !add) return;
    size_t have = strlen(dst);
    if (have + 1 >= sz) return;
    put(dst + have, sz - have, add);
}

int hde_store_parse_control_stanza(const char *text, HdeStorePackage *out)
{
    if (!text || !out) return 0;
    memset(out, 0, sizeof *out);

    int fields = 0, in_description = 0;
    const char *line = text;
    while (line && *line) {
        const char *eol = strchr(line, '\n');
        size_t len = eol ? (size_t)(eol - line) : strlen(line);

        char buf[2048];
        size_t n = len < sizeof buf - 1 ? len : sizeof buf - 1;
        memcpy(buf, line, n);
        buf[n] = 0;
        while (n && (buf[n - 1] == '\r' || buf[n - 1] == '\n')) buf[--n] = 0;

        if (!buf[0]) {
            if (fields) break;               /* the end of the first stanza */
        } else if (buf[0] == ' ' || buf[0] == '\t') {
            if (in_description) {
                append_text(out->description, sizeof out->description, "\n");
                append_text(out->description, sizeof out->description, skip_spaces(buf));
            }
        } else {
            in_description = 0;
            char *colon = strchr(buf, ':');
            if (colon) {
                *colon = 0;
                const char *value = skip_spaces(colon + 1);
                fields++;
                if (!strcasecmp(buf, "Package")) put(out->name, sizeof out->name, value);
                else if (!strcasecmp(buf, "Version")) put(out->version, sizeof out->version, value);
                else if (!strcasecmp(buf, "Status")) apply_status(value, out);
                else if (!strcasecmp(buf, "Section")) put(out->section, sizeof out->section, value);
                else if (!strcasecmp(buf, "Installed-Size")) out->size_kb = strtoull(value, NULL, 10);
                else if (!strcasecmp(buf, "Homepage")) put(out->homepage, sizeof out->homepage, value);
                else if (!strcasecmp(buf, "Architecture")) put(out->arch, sizeof out->arch, value);
                else if (!strcasecmp(buf, "Maintainer")) put(out->maintainer, sizeof out->maintainer, value);
                else if (!strcasecmp(buf, "License")) {
                    char lic[48];
                    if (hde_store_license_from_copyright(value, lic, sizeof lic))
                        put(out->license, sizeof out->license, lic);
                } else if (!strcasecmp(buf, "Description")) {
                    put(out->summary, sizeof out->summary, value);
                    in_description = 1;
                }
            }
        }
        line = eol ? eol + 1 : NULL;
    }

    if (!out->name[0]) return 0;
    if (!out->summary[0]) put(out->summary, sizeof out->summary, out->name);
    if (!out->section[0]) put(out->section, sizeof out->section, "unknown");
    out->source = HDE_STORE_SOURCE_DEB;
    out->verify = HDE_STORE_VERIFY_UNKNOWN;
    derive_state(out);
    return 1;
}

int hde_store_from_deb_control(const char *control_text, const char *path, HdeStorePackage *out)
{
    if (!control_text || !out) return 0;
    if (!hde_store_parse_control_stanza(control_text, out)) return 0;

    out->source = HDE_STORE_SOURCE_LOCAL;
    out->from_file = 1;
    out->verify = HDE_STORE_VERIFY_UNVERIFIED;
    out->installed = 0;
    out->upgradable = 0;
    out->residual = 0;
    out->broken = 0;
    out->state = HDE_STORE_STATE_NOT_INSTALLED;
    put(out->origin, sizeof out->origin, path ? path : "");
    if (!out->license[0]) put(out->license, sizeof out->license, "");
    return 1;
}

/* ---------------------------------------------------------------- apt-cache policy */

int hde_store_parse_apt_policy(const char *text, char *installed, size_t isz,
                               char *candidate, size_t csz, char *origin, size_t osz)
{
    if (installed && isz) installed[0] = 0;
    if (candidate && csz) candidate[0] = 0;
    if (origin && osz) origin[0] = 0;
    if (!text) return 0;

    char cand[80] = "";
    int in_table = 0;
    char current[80] = "";
    int found_origin = 0;

    const char *line = text;
    while (line && *line) {
        const char *eol = strchr(line, '\n');
        size_t len = eol ? (size_t)(eol - line) : strlen(line);
        char buf[1024];
        size_t n = len < sizeof buf - 1 ? len : sizeof buf - 1;
        memcpy(buf, line, n);
        buf[n] = 0;
        line = eol ? eol + 1 : NULL;

        const char *v = control_value(skip_spaces(buf), "Installed");
        if (v) { if (installed && isz) put(installed, isz, strcmp(v, "(none)") ? v : ""); continue; }
        v = control_value(skip_spaces(buf), "Candidate");
        if (v) {
            put(cand, sizeof cand, strcmp(v, "(none)") ? v : "");
            if (candidate && csz) put(candidate, csz, cand);
            continue;
        }
        if (starts_with_ci(skip_spaces(buf), "Version table:")) { in_table = 1; continue; }
        if (!in_table) continue;

        /* Two kinds of line in the version table:
         *   " *** 2.6.1 100"                         a version and its pin
         *   "     500 http://… trixie/main amd64"    where that version comes from
         *   "     100 /var/lib/dpkg/status"          installed by hand
         * The first is two numbers, the second starts with the pin and names a place. */
        char tok[5][256];
        int ntok = 0;
        const char *t = skip_spaces(buf);
        while (*t == '*') t++;
        t = skip_spaces(t);
        while (*t && ntok < 5) {
            const char *e = t;
            while (*e && *e != ' ' && *e != '\t') e++;
            put_range(tok[ntok], sizeof tok[ntok], t, e);
            if (!tok[ntok][0]) break;
            ntok++;
            t = skip_spaces(e);
        }
        int first_is_number = ntok >= 1 && tok[0][0] &&
                              strspn(tok[0], "0123456789") == strlen(tok[0]);
        int second_is_number = ntok >= 2 && tok[1][0] &&
                               strspn(tok[1], "0123456789") == strlen(tok[1]);
        if (ntok == 2 && second_is_number && hde_store_version_valid(tok[0])) {
            put(current, sizeof current, tok[0]);
            continue;
        }
        if (first_is_number && ntok >= 2 && !found_origin && cand[0] && !strcmp(current, cand)) {
            const char *where = tok[1];
            if (strstr(where, "dpkg/status")) {
                put(origin, osz, "now");
                found_origin = 1;
            } else if (strstr(where, "://")) {
                /* the token after the URL is the suite and the component: "trixie/main" */
                if (ntok >= 3 && tok[2][0]) put(origin, osz, tok[2]);
                else put(origin, osz, where);
                found_origin = 1;
            } else if (where[0]) {
                put(origin, osz, where);
                found_origin = 1;
            }
        }
    }
    return cand[0] != 0;
}

/* ---------------------------------------------------------------- the sources of the machine */

int hde_store_parse_sources_line(const char *line, HdeStoreSourceEntry *out)
{
    if (!line || !out) return 0;
    memset(out, 0, sizeof *out);

    const char *p = skip_spaces(line);
    if (*p == '#') {
        out->enabled = 0;
        p = skip_spaces(p + 1);
    } else {
        out->enabled = 1;
    }

    const char *types_end = p;
    while (*types_end && *types_end != ' ' && *types_end != '\t') types_end++;
    char types[40];
    put_range(types, sizeof types, p, types_end);
    if (strncmp(types, "deb", 3) != 0) return 0;          /* deb, deb-src, and nothing else */
    if (types[3] && types[3] != '-' && types[3] != '\0') {
        /* "deb-src" is fine, "debwhatever" is not */
        if (strncmp(types, "deb-src", 7) != 0) return 0;
    }
    put(out->types, sizeof out->types, types);

    p = skip_spaces(types_end);
    if (*p == '[') {                                       /* [signed-by=/usr/share/keyrings/x.gpg arch=amd64] */
        const char *close = strchr(p, ']');
        if (close) {
            const char *key = strstr(p, "signed-by=");
            if (key && key < close) {
                const char *v = key + strlen("signed-by=");
                const char *ve = v;
                while (ve < close && *ve != ' ' && *ve != '\t') ve++;
                put_range(out->signed_by, sizeof out->signed_by, v, ve);
            }
            p = skip_spaces(close + 1);
        }
    }

    const char *uri_end = p;
    while (*uri_end && *uri_end != ' ' && *uri_end != '\t') uri_end++;
    put_range(out->uri, sizeof out->uri, p, uri_end);

    p = skip_spaces(uri_end);
    const char *suite_end = p;
    while (*suite_end && *suite_end != ' ' && *suite_end != '\t') suite_end++;
    put_range(out->suite, sizeof out->suite, p, suite_end);

    p = skip_spaces(suite_end);
    put_range(out->components, sizeof out->components, p, p + strlen(p));

    if (!out->uri[0] || !out->suite[0]) return 0;
    if (strstr(out->uri, "flatpak")) out->flatpak = 1;
    return 1;
}

int hde_store_parse_sources_deb822(const char *text, int index, HdeStoreSourceEntry *out)
{
    if (!text || !out || index < 0) return 0;
    memset(out, 0, sizeof *out);
    out->enabled = 1;

    int stanza = 0;
    int in_wanted = 0;
    const char *line = text;
    while (line) {
        const char *eol = strchr(line, '\n');
        size_t len = eol ? (size_t)(eol - line) : strlen(line);
        char buf[1024];
        size_t n = len < sizeof buf - 1 ? len : sizeof buf - 1;
        memcpy(buf, line, n);
        buf[n] = 0;
        while (n && (buf[n - 1] == '\r' || buf[n - 1] == '\n')) buf[--n] = 0;
        line = eol ? eol + 1 : NULL;

        if (!buf[0]) {
            if (in_wanted) break;
            stanza++;
            continue;
        }
        if (stanza != index) continue;
        in_wanted = 1;

        if (buf[0] == '#') {                       /* a comment inside the stanza */
            if (!strncmp(skip_spaces(buf + 1), "Enabled:", 8)) {
                const char *v = skip_spaces(skip_spaces(buf + 1) + 8);
                out->enabled = !(starts_with_ci(v, "no") || starts_with_ci(v, "false"));
            }
            continue;
        }
        if (buf[0] == ' ' || buf[0] == '\t') continue;   /* a continuation line: the fields we read are one line */

        char *colon = strchr(buf, ':');
        if (!colon) continue;
        *colon = 0;
        const char *value = skip_spaces(colon + 1);
        if (!strcasecmp(buf, "Types")) put(out->types, sizeof out->types, value);
        else if (!strcasecmp(buf, "URIs")) put(out->uri, sizeof out->uri, value);
        else if (!strcasecmp(buf, "Suites")) put(out->suite, sizeof out->suite, value);
        else if (!strcasecmp(buf, "Components")) put(out->components, sizeof out->components, value);
        else if (!strcasecmp(buf, "Signed-By")) put(out->signed_by, sizeof out->signed_by, value);
        else if (!strcasecmp(buf, "Enabled")) {
            out->enabled = !(starts_with_ci(value, "no") || starts_with_ci(value, "false"));
        }
    }

    if (!in_wanted) return 0;
    if (!out->uri[0] || !out->suite[0]) return 0;
    if (!out->types[0]) put(out->types, sizeof out->types, "deb");
    if (strstr(out->uri, "flatpak")) out->flatpak = 1;
    return 1;
}

void hde_store_source_label(const HdeStoreSourceEntry *s, char *out, size_t outsz)
{
    if (!out || !outsz) return;
    if (!s) { out[0] = 0; return; }
    if (s->flatpak) {
        snprintf(out, outsz, "flatpak: %s", s->suite[0] ? s->suite : s->uri);
        return;
    }
    const char *uri = s->uri;
    const char *sep = strstr(uri, "://");
    if (sep) uri = sep + 3;
    snprintf(out, outsz, "%s %s%s%s", uri, s->suite,
             s->components[0] ? " " : "", s->components);
}

/* ---------------------------------------------------------------- verification */

static const char *const known_suites[] = {
    "main", "stable", "oldstable", "testing", "unstable", "sid", "experimental", "release",
    "stable-updates", "stable-security", "stable-backports", "stable-proposed-updates",
    "bullseye", "bullseye-updates", "bullseye-security", "bullseye-backports",
    "bookworm", "bookworm-updates", "bookworm-security", "bookworm-backports", "bookworm-proposed-updates",
    "trixie", "trixie-updates", "trixie-security", "trixie-backports", "trixie-proposed-updates",
    "forky", "forky-updates", "forky-security", "forky-backports",
    "bionic", "bionic-updates", "bionic-security", "bionic-backports",
    "focal", "focal-updates", "focal-security", "focal-backports",
    "jammy", "jammy-updates", "jammy-security", "jammy-backports", "jammy-proposed",
    "noble", "noble-updates", "noble-security", "noble-backports", "noble-proposed",
    "oracular", "oracular-updates", "oracular-security", "oracular-backports",
    "plucky", "plucky-updates", "plucky-security", "plucky-backports",
    "hyggshi", "hyggshi-updates", "hyggshi-security",
    NULL
};

/* "trixie/main" -> "trixie"; "trixie" -> "trixie" */
static void suite_of(const char *origin, char *out, size_t outsz)
{
    const char *slash = origin ? strchr(origin, '/') : NULL;
    if (slash) put_range(out, outsz, origin, slash);
    else put(out, outsz, origin ? origin : "");
}

HdeStoreVerify hde_store_verify_for(const char *origin, HdeStoreSource source)
{
    if (source == HDE_STORE_SOURCE_LOCAL || source == HDE_STORE_SOURCE_FLATPAK)
        return HDE_STORE_VERIFY_UNVERIFIED;
    if (source == HDE_STORE_SOURCE_DPKG_STATUS)
        return HDE_STORE_VERIFY_UNVERIFIED;
    if (!origin || !*origin) return HDE_STORE_VERIFY_UNKNOWN;
    if (!strcmp(origin, "now") || !strncmp(origin, "now/", 4) || !strcmp(origin, "now/local"))
        return HDE_STORE_VERIFY_UNVERIFIED;
    if (strstr(origin, "ppa:") || strstr(origin, "ppa.") || strstr(origin, "third-party"))
        return HDE_STORE_VERIFY_THIRD_PARTY;

    char suite[96];
    suite_of(origin, suite, sizeof suite);
    for (int i = 0; known_suites[i]; i++)
        if (!strcmp(suite, known_suites[i])) return HDE_STORE_VERIFY_VERIFIED;
    /* anything else: a repository somebody added on top of the distribution */
    return HDE_STORE_VERIFY_THIRD_PARTY;
}

const char *hde_store_verify_title(HdeStoreVerify v, int vi)
{
    switch (v) {
    case HDE_STORE_VERIFY_VERIFIED: return pick(vi, "Verified", "Đã xác minh");
    case HDE_STORE_VERIFY_THIRD_PARTY: return pick(vi, "Third party", "Bên thứ ba");
    case HDE_STORE_VERIFY_UNVERIFIED: return pick(vi, "Unverified", "Chưa xác minh");
    case HDE_STORE_VERIFY_UNKNOWN:
    default: return pick(vi, "Unknown origin", "Không rõ nguồn");
    }
}

char *hde_store_verify_detail(HdeStoreVerify v, const char *origin, int vi)
{
    const char *where = (origin && *origin) ? origin : pick(vi, "an unknown place", "một nơi không rõ");
    char buf[480];
    switch (v) {
    case HDE_STORE_VERIFY_VERIFIED:
        if (vi)
            snprintf(buf, sizeof buf,
                     "Đến từ kho gói đã ký của hệ thống (%s) và được kiểm tra chữ ký khi cài.", where);
        else
            snprintf(buf, sizeof buf,
                     "Comes from the signed package repository of your system (%s) and is checked against "
                     "its signature when it is installed.", where);
        break;
    case HDE_STORE_VERIFY_THIRD_PARTY:
        if (vi)
            snprintf(buf, sizeof buf,
                     "Đến từ %s, một kho được thêm vào hệ thống.  Chữ ký được kiểm tra, nhưng không ai của "
                     "bản phân phối xem xét nội dung bên trong.", where);
        else
            snprintf(buf, sizeof buf,
                     "Comes from %s, a repository added on top of your system.  Its signature is checked, but "
                     "nobody from the distribution looked at what is in it.", where);
        break;
    case HDE_STORE_VERIFY_UNVERIFIED:
        if (vi)
            snprintf(buf, sizeof buf,
                     "Đến từ %s, không phải kho đã ký của hệ thống (tệp cục bộ, gói cài bằng tay, hoặc gói "
                     "mà kho của nó đã mất).  Chỉ cài khi bạn tin nơi nó đến từ.", where);
        else
            snprintf(buf, sizeof buf,
                     "Comes from %s, which is not a signed repository of your system (a local file, a package "
                     "installed by hand, or one whose repository is gone).  Only install it if you trust where "
                     "it came from.", where);
        break;
    case HDE_STORE_VERIFY_UNKNOWN:
    default:
        if (vi)
            snprintf(buf, sizeof buf, "Không xác định được gói này đến từ đâu (%s).", where);
        else
            snprintf(buf, sizeof buf, "Where this package comes from could not be determined (%s).", where);
        break;
    }
    char *out = malloc(strlen(buf) + 1);
    if (out) strcpy(out, buf);
    return out;
}

const char *hde_store_source_title(HdeStoreSource s, int vi)
{
    switch (s) {
    case HDE_STORE_SOURCE_DEB: return pick(vi, "APT repository (.deb)", "Kho APT (.deb)");
    case HDE_STORE_SOURCE_LOCAL: return pick(vi, "Local .deb file", "Tệp .deb cục bộ");
    case HDE_STORE_SOURCE_FLATPAK: return pick(vi, "Flatpak (read-only)", "Flatpak (chỉ đọc)");
    case HDE_STORE_SOURCE_DPKG_STATUS: return pick(vi, "dpkg database only", "Chỉ có trong dpkg");
    case HDE_STORE_SOURCE_UNKNOWN:
    default: return pick(vi, "Unknown", "Không rõ");
    }
}

const char *hde_store_state_title(HdeStoreState s, int vi)
{
    switch (s) {
    case HDE_STORE_STATE_INSTALLED: return pick(vi, "Installed", "Đã cài đặt");
    case HDE_STORE_STATE_UPGRADABLE: return pick(vi, "Update available", "Có bản cập nhật");
    case HDE_STORE_STATE_RESIDUAL: return pick(vi, "Removed, configuration kept", "Đã gỡ, còn tệp cấu hình");
    case HDE_STORE_STATE_BROKEN: return pick(vi, "Broken installation", "Cài đặt lỗi");
    case HDE_STORE_STATE_NOT_INSTALLED:
    default: return pick(vi, "Not installed", "Chưa cài đặt");
    }
}

void hde_store_format_size(unsigned long long kb, char *out, size_t outsz)
{
    if (!out || !outsz) return;
    if (!kb) { put(out, outsz, "—"); return; }
    if (kb < 1024) snprintf(out, outsz, "%llu KB", kb);
    else if (kb < 1024ULL * 1024ULL) snprintf(out, outsz, "%.1f MB", (double)kb / 1024.0);
    else snprintf(out, outsz, "%.2f GB", (double)kb / (1024.0 * 1024.0));
}

/* ---------------------------------------------------------------- search and order */

static int word_starts_with(const char *hay, const char *needle)
{
    if (!*needle) return 0;
    size_t n = strlen(needle);
    for (const char *p = hay; *p; p++) {
        if (p == hay || !isalnum((unsigned char)p[-1]) ) {
            if (!strncmp(p, needle, n)) return 1;
        }
    }
    return 0;
}

int hde_store_match(const HdeStorePackage *p, const char *query)
{
    if (!p) return 0;
    char q[160];
    hde_store_fold(query ? query : "", q, sizeof q);
    if (!q[0]) return 1;

    char name[160], summary[480], appid[340], section[96];
    hde_store_fold(p->name, name, sizeof name);
    hde_store_fold(p->summary, summary, sizeof summary);
    hde_store_fold(p->app_id, appid, sizeof appid);
    hde_store_fold(p->section, section, sizeof section);

    if (!strncmp(name, q, strlen(q))) return 4;
    if (strstr(name, q)) return 3;
    if (appid[0] && strstr(appid, q)) return 2;
    if (word_starts_with(summary, q)) return 2;
    if (strstr(summary, q)) return 1;
    if (strstr(section, q)) return 1;
    return 0;
}

int hde_store_package_before(const HdeStorePackage *a, const HdeStorePackage *b)
{
    if (!a || !b) return a != NULL;
    if (a->upgradable != b->upgradable) return a->upgradable > b->upgradable;
    if (a->installed != b->installed) return a->installed > b->installed;
    char fa[160], fb[160];
    hde_store_fold(a->name, fa, sizeof fa);
    hde_store_fold(b->name, fb, sizeof fb);
    return strcmp(fa, fb) < 0;
}

/* ---------------------------------------------------------------- license */

static void add_license(char list[][24], int *count, int max, const char *id)
{
    if (!id || !*id || *count >= max) return;
    for (int i = 0; i < *count; i++)
        if (!strcmp(list[i], id)) return;
    put(list[*count], sizeof list[*count], id);
    (*count)++;
}

/* "GPL-3+", "GPLv3+", "GPL-3.0-or-later", "LGPL-2.1" -> the short name the Store shows */
static const char *canonical_license(const char *raw, char *buf, size_t bufsz)
{
    char f[64];
    hde_store_fold(raw, f, sizeof f);
    if (strstr(f, "agpl") || strstr(f, "affero")) {
        if (strstr(f, "3")) return "AGPL-3";
        return "AGPL";
    }
    if (strstr(f, "lgpl") || strstr(f, "lesser general public")) {
        if (strstr(f, "2.1")) return "LGPL-2.1+";
        if (strstr(f, "3")) return "LGPL-3+";
        return "LGPL";
    }
    if (strstr(f, "gpl") || strstr(f, "general public license")) {
        if (strstr(f, "3")) return "GPL-3+";
        if (strstr(f, "2")) return "GPL-2+";
        return "GPL";
    }
    if (strstr(f, "apache")) return "Apache-2.0";
    if (strstr(f, "mpl") || strstr(f, "mozilla public")) return "MPL-2.0";
    if (strstr(f, "bsd-3") || strstr(f, "bsd 3") || strstr(f, "3-clause")) return "BSD-3-Clause";
    if (strstr(f, "bsd-2") || strstr(f, "bsd 2") || strstr(f, "2-clause")) return "BSD-2-Clause";
    if (strstr(f, "bsd")) return "BSD";
    if (strstr(f, "mit") || strstr(f, "expat")) return "MIT";
    if (strstr(f, "isc")) return "ISC";
    if (strstr(f, "cc0") || strstr(f, "public domain") || strstr(f, "unlicense")) return "Public domain";
    if (strstr(f, "zlib")) return "Zlib";
    if (strstr(f, "artistic")) return "Artistic-2.0";
    if (strstr(f, "proprietary") || strstr(f, "commercial")) return "Proprietary";

    /* an SPDX-looking id we do not know: keep it, it is still better than nothing */
    put(buf, bufsz, skip_spaces(raw));
    char *cut = strpbrk(buf, ",;(");
    if (cut) *cut = 0;
    while (*buf && (buf[strlen(buf) - 1] == ' ')) buf[strlen(buf) - 1] = 0;
    return buf[0] ? buf : NULL;
}

int hde_store_license_from_copyright(const char *text, char *out, size_t outsz)
{
    if (!out || !outsz) return 0;
    out[0] = 0;
    if (!text) return 0;

    char list[8][24];
    int count = 0;
    char scratch[64];

    /* DEP-5 and the classic files: "License: GPL-3+" */
    const char *line = text;
    while (line && *line) {
        const char *eol = strchr(line, '\n');
        char buf[512];
        size_t n = eol ? (size_t)(eol - line) : strlen(line);
        if (n >= sizeof buf) n = sizeof buf - 1;
        memcpy(buf, line, n);
        buf[n] = 0;
        line = eol ? eol + 1 : NULL;

        const char *v = control_value(skip_spaces(buf), "License");
        if (v && *v) {
            const char *c = canonical_license(v, scratch, sizeof scratch);
            add_license(list, &count, 8, c);
        }
    }

    /* prose, for the copyright files written before DEP-5 */
    if (strstr(text, "GNU Lesser General Public License") || strstr(text, "GNU Library General Public License")) {
        if (strstr(text, "version 2.1") || strstr(text, "Version 2.1")) add_license(list, &count, 8, "LGPL-2.1+");
        else if (strstr(text, "version 3") || strstr(text, "Version 3")) add_license(list, &count, 8, "LGPL-3+");
        else add_license(list, &count, 8, "LGPL");
    } else if (strstr(text, "GNU General Public License") || strstr(text, "GNU GPL")) {
        if (strstr(text, "version 3") || strstr(text, "Version 3")) add_license(list, &count, 8, "GPL-3+");
        else if (strstr(text, "version 2") || strstr(text, "Version 2")) add_license(list, &count, 8, "GPL-2+");
        else add_license(list, &count, 8, "GPL");
    }
    if (strstr(text, "Permission is hereby granted, free of charge")) add_license(list, &count, 8, "MIT");
    if (strstr(text, "Apache License") &&
        (strstr(text, "Version 2.0") || strstr(text, "version 2.0"))) add_license(list, &count, 8, "Apache-2.0");
    if (strstr(text, "Mozilla Public License")) add_license(list, &count, 8, "MPL-2.0");
    if (strstr(text, "Redistribution and use in source and binary forms")) add_license(list, &count, 8, "BSD");
    if (strstr(text, "Permission to use, copy, modify, and distribute") ||
        strstr(text, "Permission to use, copy, modify, and/or distribute")) add_license(list, &count, 8, "ISC");

    if (!count) return 0;
    if (count == 1) {
        put(out, outsz, list[0]);
        return 1;
    }
    if (count <= 3) {
        size_t used = 0;
        for (int i = 0; i < count && used + 1 < outsz; i++) {
            if (i) {
                put(out + used, outsz - used, " + ");
                used = strlen(out);
            }
            put(out + used, outsz - used, list[i]);
            used = strlen(out);
        }
        return 1;
    }
    snprintf(out, outsz, "Multiple licenses (%d)", count);
    return 1;
}

const char *hde_store_license_title(const char *license, int vi)
{
    if (!license || !*license) return pick(vi, "Not stated", "Không ghi rõ");
    if (strstr(license, "Multiple"))
        return pick(vi, "Several licenses (see the copyright file)", "Nhiều giấy phép (xem tệp copyright)");
    if (strstr(license, "GPL-3")) return pick(vi, "GNU GPL 3 or later", "GNU GPL 3 trở lên");
    if (strstr(license, "GPL-2")) return pick(vi, "GNU GPL 2 or later", "GNU GPL 2 trở lên");
    if (strstr(license, "AGPL")) return pick(vi, "GNU AGPL 3", "GNU AGPL 3");
    if (strstr(license, "LGPL-3")) return pick(vi, "GNU LGPL 3 or later", "GNU LGPL 3 trở lên");
    if (strstr(license, "LGPL-2.1")) return pick(vi, "GNU LGPL 2.1 or later", "GNU LGPL 2.1 trở lên");
    if (!strcmp(license, "MIT")) return "MIT (Expat)";
    if (strstr(license, "Apache")) return pick(vi, "Apache License 2.0", "Giấy phép Apache 2.0");
    if (strstr(license, "BSD-3")) return pick(vi, "BSD 3-Clause", "BSD 3 điều khoản");
    if (strstr(license, "BSD-2")) return pick(vi, "BSD 2-Clause", "BSD 2 điều khoản");
    if (!strcmp(license, "BSD")) return "BSD";
    if (strstr(license, "MPL")) return pick(vi, "Mozilla Public License 2.0", "Giấy phép Mozilla Public 2.0");
    if (!strcmp(license, "ISC")) return "ISC";
    if (strstr(license, "Public domain")) return pick(vi, "Public domain", "Phạm vi công cộng");
    if (strstr(license, "Zlib")) return "zlib";
    if (strstr(license, "Artistic")) return pick(vi, "Artistic License 2.0", "Giấy phép Artistic 2.0");
    if (strstr(license, "Proprietary")) return pick(vi, "Proprietary", "Độc quyền");
    return license;
}

/* ---------------------------------------------------------------- the commands */

const char *const HDE_STORE_CMD_UPDATE[] = { "pkexec", "apt-get", "update", NULL };
const char *const HDE_STORE_CMD_INSTALL[] = { "pkexec", "apt-get", "install", "-y",
                                              "--no-install-recommends", NULL };
const char *const HDE_STORE_CMD_REMOVE[] = { "pkexec", "apt-get", "remove", "-y", NULL };
const char *const HDE_STORE_CMD_PURGE[] = { "pkexec", "apt-get", "purge", "-y", NULL };
const char *const HDE_STORE_CMD_INSTALL_FILE[] = { "pkexec", "apt-get", "install", "-y", NULL };
const char *const HDE_STORE_CMD_DPKG_QUERY[] = { "dpkg-query", "-W", "-f=" HDE_STORE_DPKG_FORMAT, NULL };
const char *const HDE_STORE_CMD_UPGRADABLE[] = { "apt", "list", "--upgradable", NULL };
const char *const HDE_STORE_CMD_SEARCH[] = { "apt-cache", "search", "--names-only", NULL };
const char *const HDE_STORE_CMD_POLICY[] = { "apt-cache", "policy", NULL };
const char *const HDE_STORE_CMD_FLATPAK[] = { "flatpak", "list", "--columns=application,version,origin,branch", NULL };

char **hde_store_argv_append(const char *const *base, const char *a, const char *b)
{
    size_t n = 0;
    while (base && base[n]) n++;
    size_t extra = (a ? 1 : 0) + (b ? 1 : 0);
    char **argv = calloc(n + extra + 1, sizeof *argv);
    if (!argv) return NULL;
    for (size_t i = 0; i < n; i++) {
        argv[i] = malloc(strlen(base[i]) + 1);
        if (!argv[i]) { hde_store_argv_free(argv); return NULL; }
        strcpy(argv[i], base[i]);
    }
    size_t i = n;
    if (a) {
        argv[i] = malloc(strlen(a) + 1);
        if (!argv[i]) { hde_store_argv_free(argv); return NULL; }
        strcpy(argv[i], a);
        i++;
    }
    if (b) {
        argv[i] = malloc(strlen(b) + 1);
        if (!argv[i]) { hde_store_argv_free(argv); return NULL; }
        strcpy(argv[i], b);
    }
    return argv;
}

void hde_store_argv_free(char **argv)
{
    if (!argv) return;
    for (size_t i = 0; argv[i]; i++) free(argv[i]);
    free(argv);
}

/* ------------------------------------------------------------------ the language of the Store */

int hde_store_speak_vietnamese(const char *language, const char *forced)
{
    /* an explicit request wins over the session: somebody whose desktop is in English may still want the
     * Store in Vietnamese, and a script that takes screenshots has to say which one it wants */
    const char *choice = (forced && *forced) ? forced : language;
    if (!choice || !*choice) return 1;                       /* nothing set: Vietnamese */
    return strncasecmp(choice, "en", 2) == 0 ? 0 : 1;        /* English when asked for by name */
}

/* ------------------------------------------------------------------ artwork: icons and pictures */

/* [a-z0-9] only, so "org.gnome.Weather" and "gnome-weather" can be compared (see the matcher below). */
static void squash(const char *in, char *out, size_t outsz)
{
    size_t n = 0;
    if (!out || !outsz) return;
    out[0] = 0;
    for (const char *p = in ? in : ""; *p && n + 1 < outsz; p++) {
        unsigned char c = (unsigned char)*p;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out[n++] = (char)c;
    }
    out[n] = 0;
}

/* The program of an Exec= line: "Exec=gnome-weather %u" -> "gnome-weather". */
static void exec_program(const char *exec, char *out, size_t outsz)
{
    char buf[200];
    put(buf, sizeof buf, exec);
    char *sp = buf;
    while (*sp == ' ' || *sp == '\t') sp++;
    char *end = sp;
    while (*end && *end != ' ' && *end != '\t') end++;
    *end = 0;
    put(out, outsz, sp);
}

/* "Key=value" of a desktop entry ("=" and not the ":" of a control file; the key of Name[vi] is not Name). */
static const char *desktop_value(const char *line, const char *key)
{
    if (!line || !key) return NULL;
    size_t n = strlen(key);
    if (strncmp(line, key, n) != 0) return NULL;
    if (line[n] != '=') return NULL;
    return skip_spaces(line + n + 1);
}

int hde_store_parse_desktop(const char *text, HdeStoreDesktop *out)
{
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!text) return 0;

    const char *line = text;
    int in_entry = 0;
    while (*line) {
        const char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line) : strlen(line);
        char buf[512];
        put_range(buf, sizeof buf, line, line + len);

        if (buf[0] == '[') {
            in_entry = strcmp(buf, "[Desktop Entry]") == 0;
        } else if (in_entry && buf[0] && buf[0] != '#') {
            const char *value = desktop_value(buf, "Name");
            if (value) put(out->name, sizeof out->name, value);
            else if ((value = desktop_value(buf, "Icon"))) put(out->icon, sizeof out->icon, value);
            else if ((value = desktop_value(buf, "Exec"))) exec_program(value, out->exec, sizeof out->exec);
            else if ((value = desktop_value(buf, "X-HDE-Package"))) put(out->package, sizeof out->package, value);
            else if ((value = desktop_value(buf, "X-AppStream-Id"))) put(out->app_id, sizeof out->app_id, value);
        }
        if (!nl) break;
        line = nl + 1;
    }
    return out->icon[0] ? 1 : 0;
}

/* Fills id and app_id from the file name of a .desktop entry ("/x/org.gnome.Weather.desktop"). */
void hde_store_desktop_id(const char *path, HdeStoreDesktop *d)
{
    if (!d) return;
    const char *base = path ? strrchr(path, '/') : NULL;
    base = base ? base + 1 : path;
    put(d->id, sizeof d->id, base);
    put(d->app_id, sizeof d->app_id, base);
    char *dot = strstr(d->app_id, ".desktop");
    if (dot && !dot[8]) *dot = 0;
}

int hde_store_desktop_matches_package(const HdeStoreDesktop *d, const char *package)
{
    if (!d || !package || !*package) return 0;

    if (d->package[0]) return strcmp(d->package, package) == 0;

    char pkg[96], app[96];
    squash(package, pkg, sizeof pkg);
    squash(d->app_id[0] ? d->app_id : d->id, app, sizeof app);
    if (pkg[0] && app[0]) {
        if (strcmp(pkg, app) == 0) return 1;
        size_t lp = strlen(pkg), la = strlen(app);
        /* org.gnome.Weather against gnome-weather: the package name is the tail of the app id. */
        if (la > lp && strcmp(app + la - lp, pkg) == 0) return 1;
        if (lp > la && strcmp(pkg + lp - la, app) == 0) return 1;
    }
    if (d->exec[0]) {
        char prog[96];
        squash(d->exec, prog, sizeof prog);
        if (prog[0] && strcmp(prog, pkg) == 0) return 1;
    }
    return 0;
}

static int is_a_file(const char *path)
{
    struct stat st;
    return path && *path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

int hde_store_icon_file(const char *const *dirs, const char *const *names, char *out, size_t outsz)
{
    static const char *const ext[] = { ".png", ".svg", ".xpm", NULL };
    if (out && outsz) out[0] = 0;
    if (!names || !out || !outsz) return 0;

    for (int i = 0; names[i]; i++) {
        const char *name = names[i];
        if (!name || !*name) continue;
        if (name[0] == '/') {                      /* Icon= already names a file */
            if (is_a_file(name)) { put(out, outsz, name); return 1; }
            continue;
        }
        if (strchr(name, '/')) continue;
        for (int d = 0; dirs && dirs[d]; d++) {
            if (!*dirs[d]) continue;
            for (int e = 0; ext[e]; e++) {
                char path[512];
                snprintf(path, sizeof path, "%s/%s%s", dirs[d], name, ext[e]);
                if (is_a_file(path)) { put(out, outsz, path); return 1; }
            }
        }
    }
    return 0;
}

static int has_image_extension(const char *name)
{
    static const char *const ext[] = { ".png", ".jpg", ".jpeg", ".webp", NULL };
    size_t n = strlen(name);
    for (int i = 0; ext[i]; i++) {
        size_t e = strlen(ext[i]);
        if (n > e && strcasecmp(name + n - e, ext[i]) == 0) return 1;
    }
    return 0;
}

int hde_store_shots_in_dir(const char *dir, HdeStoreShot *shots, int max_shots)
{
    if (!dir || !*dir || !shots || max_shots <= 0) return 0;
    DIR *d = opendir(dir);
    if (!d) return 0;

    char **names = NULL;
    int count = 0, capacity = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (!has_image_extension(ent->d_name)) continue;
        if (count == capacity) {
            capacity = capacity ? capacity * 2 : 8;
            char **grown = realloc(names, (size_t)capacity * sizeof *names);
            if (!grown) break;
            names = grown;
        }
        names[count] = strdup(ent->d_name);
        if (!names[count]) break;
        count++;
    }
    closedir(d);

    /* sorted by name, so the strip does not shuffle between two visits */
    for (int i = 0; i < count; i++)
        for (int j = i + 1; j < count; j++)
            if (strcmp(names[i], names[j]) > 0) {
                char *t = names[i];
                names[i] = names[j];
                names[j] = t;
            }

    int n = 0;
    for (int i = 0; i < count && n < max_shots; i++) {
        char path[400];
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        if (!is_a_file(path)) continue;
        memset(&shots[n], 0, sizeof shots[n]);
        put(shots[n].path, sizeof shots[n].path, path);
        n++;
    }
    for (int i = 0; i < count; i++) free(names[i]);
    free(names);
    return n;
}

void hde_store_shots_cache_dir(const char *cache_home, const char *package, char *out, size_t outsz)
{
    const char *base = (cache_home && *cache_home) ? cache_home : NULL;
    char fallback[400];
    if (!base) {
        const char *home = getenv("HOME");
        snprintf(fallback, sizeof fallback, "%s/.cache", (home && *home) ? home : "/tmp");
        base = fallback;
    }
    snprintf(out, outsz, "%s/hde/store/shots/%s", base, (package && *package) ? package : "unknown");
}

/* The text inside the first <tag …>…</tag> at or after `from`.  `plain` asks for a tag without attributes
 * (`<name>`), which is the untranslated one.  Returns NULL when the tag is not there. */
static const char *tag_body(const char *from, const char *tag, int plain, const char **body_end)
{
    if (!from || !tag) return NULL;
    char open[64], close[64];
    snprintf(open, sizeof open, "<%s", tag);
    snprintf(close, sizeof close, "</%s>", tag);

    const char *p = from;
    while ((p = strstr(p, open)) != NULL) {
        const char *after = p + strlen(open);
        int is_plain = (*after == '>');
        if (plain ? is_plain : (*after == '>' || *after == ' ' || *after == '\t' || *after == '\n')) {
            const char *gt = strchr(after, '>');
            const char *end = gt ? strstr(gt + 1, close) : NULL;
            if (end) {
                if (body_end) *body_end = end;
                return gt + 1;
            }
        }
        if (plain) break;
        p = after;
    }
    return NULL;
}

/* The attributes of the first <tag …> at or after `from` (" type=\"cached\" width=\"64\""). */
static const char *tag_attrs(const char *from, const char *tag, size_t *len)
{
    if (len) *len = 0;
    if (!from || !tag) return NULL;
    char open[64];
    snprintf(open, sizeof open, "<%s", tag);
    const char *p = strstr(from, open);
    if (!p) return NULL;
    const char *begin = p + strlen(open);
    const char *gt = strchr(begin, '>');
    if (!gt) return NULL;
    if (len) *len = (size_t)(gt - begin);
    return begin;
}

/* Copies the text inside <tag> into out, spaces squeezed.  1 when the tag was there. */
static int tag_value(const char *from, const char *tag, int plain, char *out, size_t outsz)
{
    const char *end = NULL;
    const char *begin = tag_body(from, tag, plain, &end);
    if (!begin) return 0;
    put_range(out, outsz, begin, end);
    return 1;
}

int hde_store_parse_appstream(const char *xml, const char *package, HdeStoreAppStream *out)
{
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!xml) return 0;

    /* The component for this package, else the first one in the file. */
    const char *first = NULL, *chosen = NULL;
    const char *p = xml;
    while ((p = strstr(p, "<component")) != NULL) {
        const char *end = strstr(p, "</component>");
        if (!end) break;
        char name[128] = "";
        tag_value(p, "pkgname", 0, name, sizeof name);
        if (!first) first = p;
        if (package && *package && name[0] && strcmp(name, package) == 0) { chosen = p; break; }
        p = end + 1;
    }
    if (!chosen) chosen = first;
    if (!chosen) chosen = xml;

    tag_value(chosen, "pkgname", 0, out->package, sizeof out->package);
    if (!tag_value(chosen, "id", 0, out->app_id, sizeof out->app_id))
        tag_value(chosen, "id", 1, out->app_id, sizeof out->app_id);
    if (!tag_value(chosen, "name", 1, out->name, sizeof out->name))
        tag_value(chosen, "name", 0, out->name, sizeof out->name);
    tag_value(chosen, "summary", 1, out->summary, sizeof out->summary);

    /* <icon type="cached">/var/cache/…</icon>, <icon type="stock">name</icon>. */
    const char *icon_end = NULL;
    const char *icon = tag_body(chosen, "icon", 0, &icon_end);
    if (icon) {
        size_t alen = 0;
        const char *attrs = tag_attrs(chosen, "icon", &alen);
        char atext[160] = "";
        if (attrs) put_range(atext, sizeof atext, attrs, attrs + alen);
        int is_file = strstr(atext, "cached") != NULL || strstr(atext, "local") != NULL;
        int is_stock = strstr(atext, "stock") != NULL;
        char text[400];
        put_range(text, sizeof text, icon, icon_end);
        if (text[0]) {
            if (!is_stock && (is_file || text[0] == '/')) {
                put(out->icon, sizeof out->icon, text);
                out->icon_is_file = 1;
            } else {
                put(out->icon, sizeof out->icon, text);
                out->icon_is_file = 0;
            }
        }
    }

    /* <screenshots><screenshot><image type="source">…</image>… */
    const char *shots_begin = strstr(chosen, "<screenshots");
    const char *shots_end = shots_begin ? strstr(shots_begin, "</screenshots>") : NULL;
    const char *s = shots_begin;
    while (shots_begin && shots_end && s && s < shots_end && out->n_shots < HDE_STORE_MAX_SHOTS) {
        s = strstr(s, "<screenshot");
        if (!s || s > shots_end) break;
        const char *closing = strstr(s, "</screenshot>");
        if (!closing) break;
        char url[600] = "", plain[600] = "";
        int have_url = 0, have_plain = 0;
        const char *img = s;
        while ((img = strstr(img, "<image")) != NULL && img < closing) {
            const char *img_end = NULL;
            const char *text_begin = tag_body(img, "image", 0, &img_end);
            if (!text_begin || text_begin > closing) break;
            char text[600];
            put_range(text, sizeof text, text_begin, img_end);
            int is_url = (strncmp(text, "http://", 7) == 0 || strncmp(text, "https://", 8) == 0);
            if (is_url && !have_url) { put(url, sizeof url, text); have_url = 1; }
            else if (!is_url && !have_plain) { put(plain, sizeof plain, text); have_plain = 1; }
            /* a source image wins over a thumbnail: take the first, do not look further */
            break;
        }
        if (have_plain || have_url) {
            HdeStoreShot *shot = &out->shots[out->n_shots];
            memset(shot, 0, sizeof *shot);
            put(shot->path, sizeof shot->path, plain);
            put(shot->url, sizeof shot->url, url);
            out->n_shots++;
        }
        s = closing + 1;
    }

    return (out->app_id[0] || out->name[0] || out->icon[0] || out->n_shots) ? 1 : 0;
}
