/*
 * nexdiag/nexdiag-core.c — NexDiag: the checks, the parsers, the redaction and the report.
 * See nexdiag-core.h.  Plain C (POSIX), no GTK, no GLib.
 */
#define _POSIX_C_SOURCE 200809L

#include "nexdiag-core.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ small helpers */

static void put(char *dst, size_t sz, const char *src)
{
    if (!dst || !sz) return;
    if (!src) { dst[0] = 0; return; }
    size_t n = strlen(src);
    if (n >= sz) n = sz - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

static void putf(char *dst, size_t sz, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void putf(char *dst, size_t sz, const char *fmt, ...)
{
    if (!dst || !sz) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dst, sz, fmt, ap);
    va_end(ap);
}

static const char *ci_strstr(const char *hay, const char *needle)
{
    if (!hay || !needle || !*needle) return hay;
    size_t n = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < n && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == n) return p;
    }
    return NULL;
}

static char *xstrdup(const char *s)
{
    if (!s) s = "";
    size_t n = strlen(s);
    char *out = malloc(n + 1);
    if (out) memcpy(out, s, n + 1);
    return out;
}

/* A growing string, for the redaction and the report. */
typedef struct {
    char *s;
    size_t len, cap;
} Buf;

static int buf_grow(Buf *b, size_t need)
{
    if (b->len + need + 1 <= b->cap) return 1;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + need + 1) cap *= 2;
    char *s = realloc(b->s, cap);
    if (!s) return 0;
    b->s = s;
    b->cap = cap;
    return 1;
}

static int buf_add(Buf *b, const char *s, size_t n)
{
    if (!buf_grow(b, n)) return 0;
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = 0;
    return 1;
}

static int buf_str(Buf *b, const char *s) { return buf_add(b, s ? s : "", s ? strlen(s) : 0); }
static int buf_ch(Buf *b, char c) { return buf_add(b, &c, 1); }

static char *buf_take(Buf *b)
{
    if (!b->s) return xstrdup("");
    char *s = b->s;
    b->s = NULL;
    b->len = b->cap = 0;
    return s;
}

/* ------------------------------------------------------------------ the report */

void nexdiag_report_init(NexdiagReport *r)
{
    if (!r) return;
    memset(r, 0, sizeof *r);
}

void nexdiag_report_free(NexdiagReport *r)
{
    if (!r) return;
    free(r->items);
    free(r->log_excerpt);
    memset(r, 0, sizeof *r);
}

int nexdiag_report_add(NexdiagReport *r, const char *id, const char *group, const char *title,
                       NexdiagStatus status, const char *detail, const char *hint)
{
    if (!r) return -1;
    if (r->count == r->cap) {
        int cap = r->cap ? r->cap * 2 : 24;
        NexdiagCheck *items = realloc(r->items, (size_t)cap * sizeof *items);
        if (!items) return -1;
        r->items = items;
        r->cap = cap;
    }
    NexdiagCheck *c = &r->items[r->count];
    memset(c, 0, sizeof *c);
    put(c->id, sizeof c->id, id);
    put(c->group, sizeof c->group, group);
    put(c->title, sizeof c->title, title);
    c->status = status;
    put(c->detail, sizeof c->detail, detail);
    put(c->hint, sizeof c->hint, hint);
    return r->count++;
}

int nexdiag_report_count(const NexdiagReport *r, NexdiagStatus status)
{
    int n = 0;
    if (!r) return 0;
    for (int i = 0; i < r->count; i++)
        if (r->items[i].status == status) n++;
    return n;
}

NexdiagStatus nexdiag_report_worst(const NexdiagReport *r)
{
    NexdiagStatus worst = NEXDIAG_OK;
    if (!r) return worst;
    for (int i = 0; i < r->count; i++)
        if (r->items[i].status > worst && r->items[i].status != NEXDIAG_SKIP) worst = r->items[i].status;
    return worst;
}

const char *nexdiag_status_name(NexdiagStatus s)
{
    switch (s) {
    case NEXDIAG_OK: return "OK";
    case NEXDIAG_NOTE: return "NOTE";
    case NEXDIAG_WARN: return "WARNING";
    case NEXDIAG_FAIL: return "FAILED";
    case NEXDIAG_SKIP:
    default: return "SKIPPED";
    }
}

const char *nexdiag_status_title(NexdiagStatus s)
{
    switch (s) {
    case NEXDIAG_OK: return "All good";
    case NEXDIAG_NOTE: return "Worth knowing";
    case NEXDIAG_WARN: return "Needs attention";
    case NEXDIAG_FAIL: return "Broken";
    case NEXDIAG_SKIP:
    default: return "Not checked";
    }
}

const char *nexdiag_status_icon(NexdiagStatus s)
{
    switch (s) {
    case NEXDIAG_OK: return "emblem-ok-symbolic";
    case NEXDIAG_NOTE: return "dialog-information-symbolic";
    case NEXDIAG_WARN: return "dialog-warning-symbolic";
    case NEXDIAG_FAIL: return "dialog-error-symbolic";
    case NEXDIAG_SKIP:
    default: return "dialog-question-symbolic";
    }
}

/* ------------------------------------------------------------------ what identifies the machine */

static void read_os_release(char *out, size_t outsz)
{
    out[0] = 0;
    FILE *fp = fopen("/etc/os-release", "r");
    if (!fp) fp = fopen("/usr/lib/os-release", "r");
    if (!fp) return;
    char line[512];
    while (fgets(line, sizeof line, fp)) {
        if (strncmp(line, "PRETTY_NAME=", 12) != 0) continue;
        char *v = line + 12;
        v[strcspn(v, "\r\n")] = 0;
        if (*v == '"' || *v == '\'') {
            char quote = *v++;
            char *end = strrchr(v, quote);
            if (end) *end = 0;
        }
        put(out, outsz, v);
        break;
    }
    fclose(fp);
}

static void read_kernel(char *out, size_t outsz)
{
    out[0] = 0;
    FILE *fp = fopen("/proc/sys/kernel/osrelease", "r");
    char rel[96] = "";
    if (fp) {
        if (!fgets(rel, sizeof rel, fp)) rel[0] = 0;
        fclose(fp);
        rel[strcspn(rel, "\r\n")] = 0;
    }
    const char *arch = getenv("HOSTTYPE");
    if (!arch || !*arch) {
#if defined(__x86_64__)
        arch = "x86_64";
#elif defined(__aarch64__)
        arch = "aarch64";
#elif defined(__i386__)
        arch = "i686";
#else
        arch = "unknown";
#endif
    }
    if (rel[0]) snprintf(out, outsz, "Linux %s %s", rel, arch);
    else put(out, outsz, "Linux (unknown version)");
}

/* Is a process with this name (or with this name inside its command line) running? */
static int process_running(const char *name)
{
    if (!name || !*name) return 0;
    DIR *d = opendir("/proc");
    if (!d) return 0;
    int found = 0;
    struct dirent *e;
    while (!found && (e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        char path[300];
        snprintf(path, sizeof path, "/proc/%s/comm", e->d_name);
        FILE *fp = fopen(path, "r");
        if (!fp) continue;
        char comm[96] = "";
        if (!fgets(comm, sizeof comm, fp)) comm[0] = 0;
        fclose(fp);
        comm[strcspn(comm, "\r\n")] = 0;
        if (comm[0] && !strcmp(comm, name)) found = 1;
    }
    closedir(d);
    return found;
}

void nexdiag_identity_init(NexdiagIdentity *id)
{
    if (!id) return;
    memset(id, 0, sizeof *id);

    const char *user = getenv("USER");
    if (!user || !*user) user = getenv("LOGNAME");
    if (!user || !*user) user = "unknown";
    put(id->user, sizeof id->user, user);

    const char *home = getenv("HOME");
    if (!home || !*home) home = "/home/unknown";
    put(id->home, sizeof id->home, home);

    char host[256] = "";
    if (gethostname(host, sizeof host - 1) != 0) host[0] = 0;
    host[sizeof host - 1] = 0;
    if (!host[0]) put(id->host, sizeof id->host, "unknown");
    else put(id->host, sizeof id->host, host);

    read_os_release(id->os, sizeof id->os);
    read_kernel(id->kernel, sizeof id->kernel);

    const char *dpy = getenv("DISPLAY");
    const char *wld = getenv("WAYLAND_DISPLAY");
    const char *type = getenv("XDG_SESSION_TYPE");
    if (wld && *wld) {
        snprintf(id->session, sizeof id->session, "Wayland (%s)", wld);
    } else if ((type && !strcmp(type, "wayland"))) {
        put(id->session, sizeof id->session, "Wayland");
    } else if (dpy && *dpy) {
        snprintf(id->session, sizeof id->session, "X11 (DISPLAY=%s)", dpy);
    } else if (type && *type) {
        snprintf(id->session, sizeof id->session, "%s, no display", type);
    } else {
        put(id->session, sizeof id->session, "no display (headless)");
    }

    /* the window manager and the desktop, by the processes that run them */
    static const char *const wms[] = { "nexwm", "labwc", "metacity", "marco", "mutter", "muffin",
                                       "xfwm4", "openbox", "icewm", "fluxbox", NULL };
    for (int i = 0; wms[i] && !id->wm[0]; i++) {
        if (!process_running(wms[i])) continue;
        if (!strcmp(wms[i], "nexwm")) put(id->wm, sizeof id->wm, "NexWM");
        else if (!strcmp(wms[i], "labwc")) put(id->wm, sizeof id->wm, "labwc");
        else put(id->wm, sizeof id->wm, wms[i]);
    }
    if (process_running("hde-panel") || process_running("hde-session") || process_running("hde-desktop"))
        put(id->desktop, sizeof id->desktop, "HDE (Hyggshi Desktop Environment)");
    else if (process_running("gnome-shell")) put(id->desktop, sizeof id->desktop, "GNOME Shell");
    else if (process_running("plasmashell")) put(id->desktop, sizeof id->desktop, "KDE Plasma");
    else if (process_running("xfce4-panel")) put(id->desktop, sizeof id->desktop, "Xfce");
    else put(id->desktop, sizeof id->desktop, "unknown");

    /* the HDE build, when HDE is installed */
    char version[64] = "";
    FILE *fp = fopen("/usr/share/hde/version", "r");
    if (!fp) fp = fopen("/usr/local/share/hde/version", "r");
    if (fp) {
        if (!fgets(version, sizeof version, fp)) version[0] = 0;
        fclose(fp);
        version[strcspn(version, "\r\n")] = 0;
    }
    if (version[0]) snprintf(id->hde, sizeof id->hde, "%s (%s)", "1.0", version);
    else if (process_running("hde-panel")) put(id->hde, sizeof id->hde, "installed (version file not found)");
}

void nexdiag_identity_clear(NexdiagIdentity *id)
{
    if (id) memset(id, 0, sizeof *id);
}

/* ------------------------------------------------------------------ redaction */

static int is_hex(char c) { return isxdigit((unsigned char)c) != 0; }

/* "aa:bb:cc:dd:ee:ff" or "aa-bb-cc-dd-ee-ff" */
static int match_mac(const char *p)
{
    for (int i = 0; i < 6; i++) {
        if (!is_hex(p[0]) || !is_hex(p[1])) return 0;
        p += 2;
        if (i < 5) {
            if (*p != ':' && *p != '-') return 0;
            p++;
        }
    }
    if (*p && (isalnum((unsigned char)*p) || *p == ':' || *p == '-')) return 0;
    return 1;
}

/* "Version: 1.2.3.4" is a version, not an address. */
static int looks_like_version(const char *text, size_t i)
{
    size_t start = i > 24 ? i - 24 : 0;
    char ctx[32];
    size_t n = i - start;
    memcpy(ctx, text + start, n);
    ctx[n] = 0;
    return ci_strstr(ctx, "version") != NULL || ci_strstr(ctx, "release") != NULL ||
           ci_strstr(ctx, "build") != NULL;
}

/* 192.168.1.10, with sane numbers */
static int match_ipv4(const char *p, size_t *len)
{
    size_t used = 0;
    for (int part = 0; part < 4; part++) {
        int digits = 0, value = 0;
        while (isdigit((unsigned char)p[used]) && digits < 3) {
            value = value * 10 + (p[used] - '0');
            used++;
            digits++;
        }
        if (!digits || value > 255) return 0;
        if (part < 3) {
            if (p[used] != '.') return 0;
            used++;
        }
    }
    *len = used;
    return 1;
}

/* 2a00:1450:4001:82f::200e — only when it is clearly an address (at least three groups and a "::"). */
static int match_ipv6(const char *p, size_t *len)
{
    if (!is_hex(*p)) return 0;
    size_t used = 0;
    int groups = 0, doubles = 0;
    while (p[used]) {
        if (is_hex(p[used])) {
            int n = 0;
            while (is_hex(p[used]) && n < 4) { used++; n++; }
            groups++;
            if (p[used] == ':') {
                used++;
                if (p[used] == ':') { doubles++; used++; }
                continue;
            }
            break;
        }
        break;
    }
    if (groups < 3) return 0;
    if (!doubles && groups < 8) return 0;
    *len = used;
    return 1;
}

static int match_uuid(const char *p)
{
    static const int groups[5] = { 8, 4, 4, 4, 12 };
    size_t used = 0;
    for (int g = 0; g < 5; g++) {
        for (int i = 0; i < groups[g]; i++)
            if (!is_hex(p[used + i])) return 0;
        used += (size_t)groups[g];
        if (g < 4) {
            if (p[used] != '-') return 0;
            used++;
        }
    }
    return 1;
}

/* someone@example.com */
static int match_email(const char *p, size_t *len)
{
    size_t i = 0;
    while (p[i] && (isalnum((unsigned char)p[i]) || strchr("._%+-", p[i]))) i++;
    if (i == 0 || p[i] != '@') return 0;
    size_t at = i++;
    size_t start_domain = i;
    while (isalnum((unsigned char)p[i]) || p[i] == '-' || p[i] == '.') i++;
    if (i <= start_domain || p[i - 1] == '.') return 0;
    /* there has to be a dot inside the domain, with something after it */
    size_t last_dot = 0;
    int has_dot = 0;
    for (size_t k = start_domain; k < i; k++)
        if (p[k] == '.') { last_dot = k; has_dot = 1; }
    if (!has_dot || last_dot == start_domain || last_dot + 1 >= i) return 0;
    (void)at;
    *len = i;
    return 1;
}

static const char *secret_keywords[] = {
    "password", "passwd", "passphrase", "token", "secret", "api_key", "apikey",
    "access_key", "private_key", "bearer", NULL
};

/* If a keyword starts here, how long is it, and is a value behind it?  Returns the offset of the value. */
static size_t secret_value_offset(const char *p, size_t *keyword_len)
{
    for (int k = 0; secret_keywords[k]; k++) {
        size_t n = strlen(secret_keywords[k]);
        if (strncasecmp(p, secret_keywords[k], n) != 0) continue;
        size_t j = n;
        while (p[j] == ' ' || p[j] == '\t') j++;
        if (p[j] == '=' || p[j] == ':') {
            j++;
            while (p[j] == ' ' || p[j] == '\t' || p[j] == '"' || p[j] == '\'') j++;
        } else if (j == n) {
            continue;                          /* "passwordless" is not a password */
        }
        if (!p[j] || !isgraph((unsigned char)p[j])) continue;
        *keyword_len = n;
        return j;
    }
    return 0;
}

char *nexdiag_redact_with(const char *text, const NexdiagIdentity *id)
{
    if (!text) return xstrdup("");
    Buf out = { 0 };
    size_t user_len = id ? strlen(id->user) : 0;
    size_t home_len = id ? strlen(id->home) : 0;
    size_t host_len = id ? strlen(id->host) : 0;

    for (size_t i = 0; text[i];) {
        const char *p = text + i;

        /* the user name, the home directory and the host name of this machine */
        if (id && user_len && user_len > 2 && strncmp(p, id->user, user_len) == 0) {
            buf_str(&out, "<user>");
            i += user_len;
            continue;
        }
        if (id && home_len > 1 && strncmp(p, id->home, home_len) == 0) {
            buf_str(&out, "<home>");
            i += home_len;
            continue;
        }
        if (id && host_len > 2 && strncasecmp(p, id->host, host_len) == 0) {
            buf_str(&out, "<host>");
            i += host_len;
            continue;
        }

        /* "password = hunter2", "token: abc", "Authorization: Bearer …" */
        size_t keyword_len = 0;
        size_t value_at = secret_value_offset(p, &keyword_len);
        if (value_at > 0) {
            buf_add(&out, p, value_at);
            buf_str(&out, "<redacted>");
            i += value_at;
            while (text[i] && !isspace((unsigned char)text[i]) && text[i] != '"' && text[i] != '\'' &&
                   text[i] != ',' && text[i] != ';' && text[i] != '&' && text[i] != ']')
                i++;
            continue;
        }

        if (is_hex(*p) && match_mac(p)) {
            buf_str(&out, "<mac>");
            i += 17;
            continue;
        }
        if (is_hex(*p) && match_uuid(p)) {
            buf_str(&out, "<uuid>");
            i += 36;
            continue;
        }
        size_t span = 0;
        if (isalnum((unsigned char)*p) && match_email(p, &span)) {
            buf_str(&out, "<email>");
            i += span;
            continue;
        }
        if (isdigit((unsigned char)*p) && match_ipv4(p, &span)) {
            /* only when the address is a word of its own, so that "1.2.3.4-p1" of a version stays */
            int after_ok = !text[i + span] || !isalnum((unsigned char)text[i + span]);
            int before_ok = i == 0 || !(isalnum((unsigned char)text[i - 1]) || text[i - 1] == '.' ||
                                        text[i - 1] == '-');
            if (after_ok && before_ok && !looks_like_version(text, i)) {
                buf_str(&out, "<ip>");
                i += span;
                continue;
            }
        }
        if (is_hex(*p) && match_ipv6(p, &span)) {
            buf_str(&out, "<ip>");
            i += span;
            continue;
        }

        buf_ch(&out, text[i]);
        i++;
    }
    return buf_take(&out);
}

void nexdiag_redact_buffer(char *text, const NexdiagIdentity *id)
{
    if (!text) return;
    char *red = nexdiag_redact_with(text, id);
    if (red) {
        strcpy(text, red);      /* the redacted text is never longer than the original */
        free(red);
    }
}

char *nexdiag_redact(const char *text)
{
    NexdiagIdentity id;
    nexdiag_identity_init(&id);
    char *out = nexdiag_redact_with(text, &id);
    nexdiag_identity_clear(&id);
    return out;
}

/* ------------------------------------------------------------------ parsers */

int nexdiag_parse_ldd_missing(const char *ldd_output, char *out, size_t outsz)
{
    if (out && outsz) out[0] = 0;
    if (!ldd_output) return 0;
    char seen[16][96];
    int n = 0;

    const char *line = ldd_output;
    while (line && *line && n < 16) {
        const char *eol = strchr(line, '\n');
        size_t len = eol ? (size_t)(eol - line) : strlen(line);
        char buf[512];
        size_t copy = len < sizeof buf - 1 ? len : sizeof buf - 1;
        memcpy(buf, line, copy);
        buf[copy] = 0;

        if (ci_strstr(buf, "not found")) {
            /* "libfoo.so.1 => not found" or "ldd: /usr/bin/x: No such file or directory" */
            char lib[96] = "";
            const char *arrow = strstr(buf, "=>");
            if (arrow) {
                const char *s = buf;
                while (*s == ' ' || *s == '\t') s++;
                size_t l = (size_t)(arrow - s);
                while (l && (s[l - 1] == ' ' || s[l - 1] == '\t')) l--;
                if (l && l < sizeof lib) {
                    memcpy(lib, s, l);
                    lib[l] = 0;
                }
            } else {
                const char *colon = strrchr(buf, ':');
                if (colon) {
                    const char *s = buf;
                    while (*s == ' ' || *s == '\t') s++;
                    size_t l = (size_t)(colon - s);
                    if (l > 16 && l < sizeof lib) {           /* a path, not "ldd" */
                        memcpy(lib, s, l);
                        lib[l] = 0;
                    }
                }
            }
            if (!lib[0]) put(lib, sizeof lib, "unknown library");
            int dup = 0;
            for (int i = 0; i < n; i++)
                if (!strcmp(seen[i], lib)) dup = 1;
            if (!dup) {
                put(seen[n], sizeof seen[n], lib);
                if (out && outsz) {
                    if (n) put(out + strlen(out), outsz - strlen(out), ", ");
                    put(out + strlen(out), outsz - strlen(out), lib);
                }
                n++;
            }
        }
        line = eol ? eol + 1 : NULL;
    }
    return n;
}

int nexdiag_parse_failed_units(const char *systemctl_output, char *out, size_t outsz)
{
    if (out && outsz) out[0] = 0;
    if (!systemctl_output) return 0;
    int n = 0;
    const char *line = systemctl_output;
    while (line && *line) {
        const char *eol = strchr(line, '\n');
        size_t len = eol ? (size_t)(eol - line) : strlen(line);
        char buf[512];
        size_t copy = len < sizeof buf - 1 ? len : sizeof buf - 1;
        memcpy(buf, line, copy);
        buf[copy] = 0;
        line = eol ? eol + 1 : NULL;

        if (!strstr(buf, "failed")) continue;
        const char *s = buf;
        while (*s == ' ' || *s == '\t' || *s == '\xe2' || *s == '\x97' || *s == '\x8f') s++;
        const char *e = s;
        while (*e && *e != ' ' && *e != '\t') e++;
        if (e == s) continue;
        char unit[96];
        size_t l = (size_t)(e - s);
        if (l >= sizeof unit) continue;
        memcpy(unit, s, l);
        unit[l] = 0;
        if (!strchr(unit, '.')) continue;               /* the description lines do not name a unit */
        if (out && outsz) {
            if (n) put(out + strlen(out), outsz - strlen(out), ", ");
            put(out + strlen(out), outsz - strlen(out), unit);
        }
        n++;
        if (n >= 8) break;
    }
    return n;
}

int nexdiag_parse_nmcli_state(const char *output, char *state, size_t statesz)
{
    if (state && statesz) state[0] = 0;
    if (!output) return 0;
    const char *s = output;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    size_t n = 0;
    while (s[n] && s[n] != '\n' && s[n] != '\r' && s[n] != ' ' && n + 1 < statesz) n++;
    if (!n) return 0;
    memcpy(state, s, n);
    state[n] = 0;
    return 1;
}

static int line_has_error(const char *line)
{
    return ci_strstr(line, "error") || ci_strstr(line, "failed") || ci_strstr(line, "failure") ||
           ci_strstr(line, "cannot ") || ci_strstr(line, "can't ") || ci_strstr(line, "no such file") ||
           ci_strstr(line, "segmentation fault") || ci_strstr(line, "traceback") ||
           ci_strstr(line, "unable to") || ci_strstr(line, "core dumped") || ci_strstr(line, "panic");
}

static int line_has_warning(const char *line)
{
    return ci_strstr(line, "warning") || ci_strstr(line, "warn:") || ci_strstr(line, "deprecated") ||
           ci_strstr(line, "missing") || ci_strstr(line, "not supported");
}

static int count_lines_matching(const char *text, int want_errors)
{
    if (!text) return 0;
    int n = 0;
    const char *line = text;
    while (line && *line) {
        const char *eol = strchr(line, '\n');
        size_t len = eol ? (size_t)(eol - line) : strlen(line);
        char buf[1024];
        size_t copy = len < sizeof buf - 1 ? len : sizeof buf - 1;
        memcpy(buf, line, copy);
        buf[copy] = 0;
        if (want_errors ? line_has_error(buf) : (line_has_warning(buf) || line_has_error(buf))) n++;
        line = eol ? eol + 1 : NULL;
    }
    return n;
}

int nexdiag_count_errors(const char *text) { return count_lines_matching(text, 1); }
int nexdiag_count_warnings(const char *text) { return count_lines_matching(text, 0); }

int nexdiag_parse_dpkg_audit(const char *audit_output, char *out, size_t outsz)
{
    if (out && outsz) out[0] = 0;
    if (!audit_output) return 0;
    const char *s = audit_output;
    while (*s == '\n' || *s == '\r' || *s == ' ' || *s == '\t') s++;
    if (!*s) return 0;
    const char *eol = strchr(s, '\n');
    size_t len = eol ? (size_t)(eol - s) : strlen(s);
    if (out && outsz) {
        size_t copy = len < outsz - 1 ? len : outsz - 1;
        memcpy(out, s, copy);
        out[copy] = 0;
    }
    return 1;
}

int nexdiag_check_ini(const char *text, char *out, size_t outsz)
{
    if (out && outsz) out[0] = 0;
    if (!text) return 1;
    int lineno = 0;
    const char *line = text;
    while (line && *line) {
        const char *eol = strchr(line, '\n');
        size_t len = eol ? (size_t)(eol - line) : strlen(line);
        char buf[1024];
        size_t copy = len < sizeof buf - 1 ? len : sizeof buf - 1;
        memcpy(buf, line, copy);
        buf[copy] = 0;
        buf[strcspn(buf, "\r")] = 0;
        line = eol ? eol + 1 : NULL;
        lineno++;

        char *s = buf;
        while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == '#' || *s == ';') continue;
        if (*s == '[') {
            char *close = strchr(s, ']');
            if (!close) {
                putf(out, outsz, "line %d: a section that is never closed", lineno);
                return 1;
            }
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq) {
            putf(out, outsz, "line %d: \"%s\" is neither a section nor a key = value line", lineno,
                 s);
            return 1;
        }
        if (eq == s) {
            putf(out, outsz, "line %d: a key with no name", lineno);
            return 1;
        }
    }
    return 0;
}

unsigned long long nexdiag_parse_meminfo_kb(const char *meminfo, const char *field)
{
    if (!meminfo || !field) return 0;
    size_t n = strlen(field);
    const char *line = meminfo;
    while (line && *line) {
        if (strncmp(line, field, n) == 0 && line[n] == ':') {
            const char *v = line + n + 1;
            while (*v == ' ' || *v == '\t') v++;
            return strtoull(v, NULL, 10);
        }
        const char *eol = strchr(line, '\n');
        line = eol ? eol + 1 : NULL;
    }
    return 0;
}

/* ------------------------------------------------------------------ running things */

static char *read_file(const char *path, size_t max_len)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return NULL;
    Buf b = { 0 };
    char chunk[4096];
    size_t got;
    while ((got = fread(chunk, 1, sizeof chunk, fp)) > 0) {
        if (max_len && b.len + got > max_len) got = max_len - b.len;
        if (!buf_add(&b, chunk, got)) break;
        if (max_len && b.len >= max_len) break;
    }
    fclose(fp);
    return buf_take(&b);
}

/* The output of a shell command, or NULL when it could not run.  stderr is dropped. */
static char *run_shell(const char *cmd)
{
    if (!cmd) return NULL;
    size_t n = strlen(cmd) + 20;
    char *full = malloc(n);
    if (!full) return NULL;
    snprintf(full, n, "%s 2>/dev/null", cmd);
    FILE *fp = popen(full, "r");
    free(full);
    if (!fp) return NULL;

    Buf b = { 0 };
    char chunk[4096];
    size_t got;
    while ((got = fread(chunk, 1, sizeof chunk, fp)) > 0) {
        if (!buf_add(&b, chunk, got)) break;
        if (b.len > 256 * 1024) break;             /* never let one command blow up a report */
    }
    int status = pclose(fp);
    char *out = buf_take(&b);
    if (status != 0 && (!out || !out[0])) {
        /* keep what was printed, but say that it failed by returning it anyway */
    }
    return out;
}

static int command_exists(const char *name)
{
    if (!name || !*name) return 0;
    const char *path = getenv("PATH");
    if (!path) path = "/usr/local/bin:/usr/bin:/bin";
    char *copy = xstrdup(path);
    if (!copy) return 0;
    int found = 0;
    char *save = NULL;
    for (char *dir = strtok_r(copy, ":", &save); dir && !found; dir = strtok_r(NULL, ":", &save)) {
        char buf[1024];
        snprintf(buf, sizeof buf, "%s/%s", dir, name);
        if (access(buf, X_OK) == 0) found = 1;
    }
    free(copy);
    return found;
}

/* ------------------------------------------------------------------ the checks */

typedef struct {
    NexdiagReport *r;
} Collect;

static void check_system(Collect *c, const NexdiagIdentity *id)
{
    char detail[NEXDIAG_DETAIL_LEN];
    snprintf(detail, sizeof detail, "%s · %s", id->os[0] ? id->os : "unknown system", id->kernel);
    nexdiag_report_add(c->r, "system.os", "System", "Operating system and kernel", NEXDIAG_OK, detail,
                       "If this is not the system you expect, the report was made on another machine.");

    if (id->hde[0]) {
        nexdiag_report_add(c->r, "system.hde", "System", "Hyggshi Desktop Environment (HDE)", NEXDIAG_OK,
                           id->hde, "");
    } else {
        nexdiag_report_add(c->r, "system.hde", "System", "Hyggshi Desktop Environment (HDE)", NEXDIAG_SKIP,
                           "HDE is not installed on this machine (no version file, no HDE program running).",
                           "HDE is what NexDiag is for; install it to check the desktop.");
    }

    char *mem = read_file("/proc/meminfo", 64 * 1024);
    unsigned long long total = nexdiag_parse_meminfo_kb(mem, "MemTotal");
    unsigned long long avail = nexdiag_parse_meminfo_kb(mem, "MemAvailable");
    free(mem);
    if (total) {
        char d[256];
        snprintf(d, sizeof d, "%.1f GB in total, %.1f GB free right now", (double)total / 1048576.0,
                 (double)avail / 1048576.0);
        NexdiagStatus st = (avail && avail < 200 * 1024) ? NEXDIAG_WARN : NEXDIAG_NOTE;
        nexdiag_report_add(c->r, "system.memory", "System", "Memory", st, d,
                           st == NEXDIAG_WARN ? "Close some programs, or add memory." : "");
    } else {
        nexdiag_report_add(c->r, "system.memory", "System", "Memory", NEXDIAG_SKIP,
                           "/proc/meminfo is not readable.", "");
    }

    char *uptime = read_file("/proc/uptime", 256);
    if (uptime) {
        double seconds = strtod(uptime, NULL);
        char d[128];
        snprintf(d, sizeof d, "up for %.1f hours", seconds / 3600.0);
        nexdiag_report_add(c->r, "system.uptime", "System", "Uptime", NEXDIAG_NOTE, d, "");
    }
    free(uptime);
}

static void check_session(Collect *c, const NexdiagIdentity *id)
{
    NexdiagStatus st = (strstr(id->session, "no display") || strstr(id->session, "no display"))
                           ? NEXDIAG_WARN
                           : NEXDIAG_OK;
    nexdiag_report_add(c->r, "session.type", "Session", "Display server", st, id->session,
                       st == NEXDIAG_WARN ? "A desktop session is expected to have a display; start HDE from "
                                            "the login screen or check DISPLAY / WAYLAND_DISPLAY."
                                          : "");

    if (id->wm[0]) {
        nexdiag_report_add(c->r, "session.wm", "Session", "Window manager", NEXDIAG_OK,
                           id->wm, "");
    } else {
        nexdiag_report_add(c->r, "session.wm", "Session", "Window manager", NEXDIAG_WARN,
                           "No window manager is running (Metacity, Marco, Mutter, Muffin, NexWM, labwc …).",
                           "Windows will have no decorations and cannot be moved.  Choose one in Settings → "
                           "Window Management, or start nexwm.");
    }

    nexdiag_report_add(c->r, "session.desktop", "Session", "Desktop environment", NEXDIAG_NOTE,
                       id->desktop, "");

    /* the HDE programs a session is made of */
    static const char *const parts[] = { "hde-panel", "hde-desktop", "hde-settings", "hde-session",
                                         "hde-hotkeys", "hde-xsettings", "hde-idle", "hde-lock", NULL };
    char missing[256] = "";
    int missing_count = 0, running_count = 0;
    for (int i = 0; parts[i]; i++) {
        if (process_running(parts[i])) {
            running_count++;
        } else {
            missing_count++;
            if (strlen(missing) < sizeof missing - 2) {
                if (missing[0]) strncat(missing, ", ", sizeof missing - strlen(missing) - 1);
                strncat(missing, parts[i], sizeof missing - strlen(missing) - 1);
            }
        }
    }
    if (running_count == 0) {
        nexdiag_report_add(c->r, "session.components", "Session", "HDE programs", NEXDIAG_SKIP,
                           "No HDE program is running: this is not an HDE session.", "");
    } else if (missing_count) {
        char d[512];
        snprintf(d, sizeof d, "%d of %d HDE programs are running; not running: %s", running_count,
                 running_count + missing_count, missing);
        nexdiag_report_add(c->r, "session.components", "Session", "HDE programs", NEXDIAG_WARN, d,
                           "Start the missing program, or restart the session: hde-session restart");
    } else {
        nexdiag_report_add(c->r, "session.components", "Session", "HDE programs", NEXDIAG_OK,
                           "All of the panel, desktop, hotkeys, settings, idle and lock programs are running.",
                           "");
    }

    /* can this session be locked at all? */
    if (command_exists("hde-lock")) {
        char *out = run_shell("hde-lock --check");
        int st_lock = out ? (int)strtol(out, NULL, 10) : -1;
        free(out);
        if (st_lock == 0) {
            nexdiag_report_add(c->r, "session.lock", "Session", "Lock screen", NEXDIAG_OK,
                               "This session can be locked (hde-lock --check says yes).", "");
        } else {
            nexdiag_report_add(c->r, "session.lock", "Session", "Lock screen", NEXDIAG_WARN,
                               "hde-lock --check does not say this session can be locked.",
                               "Install the lock support (PAM, libxcb for X11 or the session-lock protocol on "
                               "Wayland) so that Super+L protects the machine.");
        }
    } else {
        nexdiag_report_add(c->r, "session.lock", "Session", "Lock screen", NEXDIAG_SKIP,
                           "hde-lock is not installed.", "");
    }
}

static void check_packages(Collect *c)
{
    /* dpkg's own audit: half-installed packages, packages with missing dependencies, … */
    if (command_exists("dpkg")) {
        char *out = run_shell("dpkg --audit");
        char first[256] = "";
        int bad = out ? nexdiag_parse_dpkg_audit(out, first, sizeof first) : 0;
        if (!bad) {
            nexdiag_report_add(c->r, "packages.audit", "Packages", "Package database (dpkg --audit)", NEXDIAG_OK,
                               "dpkg sees no broken or half-installed package.", "");
        } else {
            char d[512];
            snprintf(d, sizeof d, "dpkg reports a problem: %s", first);
            nexdiag_report_add(c->r, "packages.audit", "Packages", "Package database (dpkg --audit)",
                               NEXDIAG_FAIL, d,
                               "Run `sudo dpkg --configure -a` and `sudo apt-get install -f`, then run the "
                               "diagnostics again.");
        }
        free(out);
    } else {
        nexdiag_report_add(c->r, "packages.audit", "Packages", "Package database (dpkg --audit)", NEXDIAG_SKIP,
                           "dpkg is not installed.", "");
    }

    /* the tools HDE itself needs */
    static const char *const tools[] = { "dpkg", "apt-get", "pkexec", "dbus-daemon", "notify-send",
                                         "gsettings", NULL };
    char missing[256] = "";
    int missing_count = 0;
    for (int i = 0; tools[i]; i++) {
        if (command_exists(tools[i])) continue;
        missing_count++;
        if (missing[0]) strncat(missing, ", ", sizeof missing - strlen(missing) - 1);
        strncat(missing, tools[i], sizeof missing - strlen(missing) - 1);
    }
    if (!missing_count) {
        nexdiag_report_add(c->r, "packages.tools", "Packages", "Required tools", NEXDIAG_OK,
                           "dpkg, apt-get, pkexec, dbus-daemon, notify-send and gsettings are installed.", "");
    } else {
        char d[512];
        snprintf(d, sizeof d, "Missing: %s", missing);
        nexdiag_report_add(c->r, "packages.tools", "Packages", "Required tools", NEXDIAG_WARN, d,
                           "Install what is missing (pkexec is in policykit-1, notify-send in libnotify-bin); "
                           "without them parts of the desktop cannot work.");
    }

    /* missing shared libraries of the HDE programs */
    static const char *const programs[] = { "hde-panel", "hde-settings", "hde-desktop", "hde-files",
                                            "hde-lock", "hde-idle", "nexwm", NULL };
    char cmd[1024] = "for b in";
    for (int i = 0; programs[i]; i++) {
        strncat(cmd, " ", sizeof cmd - strlen(cmd) - 1);
        strncat(cmd, programs[i], sizeof cmd - strlen(cmd) - 1);
    }
    strncat(cmd, "; do p=$(command -v $b) || continue; echo \"== $b\"; ldd \"$p\"; done",
            sizeof cmd - strlen(cmd) - 1);

    char *ldd_out = run_shell(cmd);
    if (ldd_out && strstr(ldd_out, "==")) {
        char names[512] = "";
        int missing_libs = nexdiag_parse_ldd_missing(ldd_out, names, sizeof names);
        if (missing_libs) {
            char d[1024];
            snprintf(d, sizeof d, "%d shared library(ies) are missing: %s", missing_libs, names);
            nexdiag_report_add(c->r, "packages.libraries", "Packages", "Shared libraries of the HDE programs",
                               NEXDIAG_FAIL, d,
                               "A package was removed or an upgrade did not finish.  "
                               "`sudo apt-get install -f` usually puts the libraries back.");
        } else {
            nexdiag_report_add(c->r, "packages.libraries", "Packages", "Shared libraries of the HDE programs",
                               NEXDIAG_OK, "Every HDE program finds all of its shared libraries.", "");
        }
    } else {
        nexdiag_report_add(c->r, "packages.libraries", "Packages", "Shared libraries of the HDE programs",
                           NEXDIAG_SKIP, "No HDE program was found on this machine.", "");
    }
    free(ldd_out);
}

static void check_services(Collect *c)
{
    if (command_exists("systemctl")) {
        char *out = run_shell("systemctl --failed --no-legend --plain");
        char units[512] = "";
        int failed = out ? nexdiag_parse_failed_units(out, units, sizeof units) : 0;
        if (failed) {
            char d[768];
            snprintf(d, sizeof d, "%d systemd unit(s) failed: %s", failed, units);
            nexdiag_report_add(c->r, "services.systemd", "Services", "Failed systemd units", NEXDIAG_WARN, d,
                               "`systemctl status <unit>` and `journalctl -u <unit> -b` say why; the desktop "
                               "may still work without them.");
        } else {
            nexdiag_report_add(c->r, "services.systemd", "Services", "Failed systemd units", NEXDIAG_OK,
                               "No systemd unit has failed.", "");
        }
        free(out);
    } else {
        nexdiag_report_add(c->r, "services.systemd", "Services", "Failed systemd units", NEXDIAG_SKIP,
                           "systemd is not the init system here.", "");
    }

    const char *bus = getenv("DBUS_SESSION_BUS_ADDRESS");
    if (bus && *bus)
        nexdiag_report_add(c->r, "services.dbus", "Services", "Session bus", NEXDIAG_OK,
                           "The D-Bus session bus is reachable (notifications, settings, udisks2).", "");
    else
        nexdiag_report_add(c->r, "services.dbus", "Services", "Session bus", NEXDIAG_WARN,
                           "DBUS_SESSION_BUS_ADDRESS is not set: programs cannot talk to each other.",
                           "Start the session through dbus-run-session (hde-start does this) instead of "
                           "running programs one by one.");
}

static void check_network(Collect *c)
{
    if (command_exists("nmcli")) {
        char *out = run_shell("nmcli -t -f STATE general");
        char state[64] = "";
        if (out && nexdiag_parse_nmcli_state(out, state, sizeof state)) {
            int connected = !strcmp(state, "connected");
            char d[256];
            snprintf(d, sizeof d, "NetworkManager says: %s", state);
            nexdiag_report_add(c->r, "network.state", "Network", "Network connection",
                               connected ? NEXDIAG_OK : NEXDIAG_NOTE, d,
                               connected ? "" : "Open the network menu of the panel, or run `nmcli device` to "
                                               "see what is not connected.");
        } else {
            nexdiag_report_add(c->r, "network.state", "Network", "Network connection", NEXDIAG_NOTE,
                               "NetworkManager is installed but did not answer (`nmcli` failed).",
                               "`systemctl status NetworkManager` shows whether the service runs.");
        }
        free(out);
    } else {
        char *out = run_shell("ip route show default");
        int has_default = out && strstr(out, "default");
        char d[256];
        if (out) snprintf(d, sizeof d, has_default ? "A default route exists." : "There is no default route.");
        else snprintf(d, sizeof d, "Could not ask the kernel for the routing table.");
        nexdiag_report_add(c->r, "network.state", "Network", "Network connection",
                           has_default ? NEXDIAG_OK : NEXDIAG_NOTE, d,
                           has_default ? "" : "Without a default route nothing on the network is reachable.");
        free(out);
    }
}

static void check_display(Collect *c, const NexdiagIdentity *id)
{
    if (strstr(id->session, "no display")) {
        nexdiag_report_add(c->r, "display.screens", "Display", "Screens", NEXDIAG_SKIP,
                           "There is no display in this session, so there are no screens to read.", "");
        return;
    }
    if (!strstr(id->session, "X11")) {
        nexdiag_report_add(c->r, "display.screens", "Display", "Screens", NEXDIAG_SKIP,
                           "The screens of a Wayland session are not read by NexDiag yet.", "");
        return;
    }
    if (!command_exists("xrandr")) {
        nexdiag_report_add(c->r, "display.screens", "Display", "Screens", NEXDIAG_SKIP,
                           "xrandr is not installed.", "");
        return;
    }
    char *out = run_shell("xrandr --query");
    int screens = 0, modes = 0;
    if (out) {
        const char *line = out;
        while (line && *line) {
            if (strstr(line, " connected") || !strncmp(line, "connected", 9)) screens++;
            if (strstr(line, "x") && strstr(line, "+") && strstr(line, "*")) modes++;
            const char *eol = strchr(line, '\n');
            line = eol ? eol + 1 : NULL;
        }
    }
    char d[256];
    snprintf(d, sizeof d, "%d screen(s) connected", screens);
    nexdiag_report_add(c->r, "display.screens", "Display", "Screens",
                       screens > 0 ? NEXDIAG_OK : NEXDIAG_WARN, d,
                       screens > 0 ? "" : "No screen is connected according to XRandR; the panel cannot lay "
                                          "itself out.");
    free(out);
}

static void check_config(Collect *c, const NexdiagIdentity *id)
{
    char path[512];
    snprintf(path, sizeof path, "%s/.config/hde/settings.ini", id->home);
    char *text = read_file(path, 512 * 1024);
    if (!text) {
        nexdiag_report_add(c->r, "config.settings", "Configuration", "HDE settings", NEXDIAG_NOTE,
                           "No ~/.config/hde/settings.ini yet: HDE uses its defaults.", "");
    } else {
        char problem[256] = "";
        if (nexdiag_check_ini(text, problem, sizeof problem)) {
            char d[512];
            snprintf(d, sizeof d, "settings.ini cannot be read: %s", problem);
            nexdiag_report_add(c->r, "config.settings", "Configuration", "HDE settings", NEXDIAG_WARN, d,
                               "Rename the file (`mv ~/.config/hde/settings.ini ~/.config/hde/settings.ini.old`) "
                               "and change one setting in Settings to write a fresh one.");
        } else {
            nexdiag_report_add(c->r, "config.settings", "Configuration", "HDE settings", NEXDIAG_OK,
                               "settings.ini is readable and well formed.", "");
        }
        free(text);
    }

    struct statvfs vfs;
    if (statvfs("/", &vfs) == 0) {
        unsigned long long free_bytes = (unsigned long long)vfs.f_bavail * vfs.f_frsize;
        unsigned long long total = (unsigned long long)vfs.f_blocks * vfs.f_frsize;
        char d[256];
        snprintf(d, sizeof d, "%.1f GB free of %.1f GB on /", (double)free_bytes / 1073741824.0,
                 (double)total / 1073741824.0);
        NexdiagStatus st = NEXDIAG_OK;
        if (free_bytes < 200ULL * 1024 * 1024) st = NEXDIAG_FAIL;
        else if (free_bytes < 1024ULL * 1024 * 1024) st = NEXDIAG_WARN;
        nexdiag_report_add(c->r, "config.disk", "Configuration", "Free space", st, d,
                           st == NEXDIAG_OK ? ""
                                            : "A full disk stops the desktop from writing its settings and the "
                                              "package manager from installing anything.");
    }

    snprintf(path, sizeof path, "%s/.cache/hde", id->home);
    if (access(path, F_OK) != 0) {
        char d[768];
        snprintf(d, sizeof d, "%s does not exist yet.", path);
        nexdiag_report_add(c->r, "config.cache", "Configuration", "HDE cache directory", NEXDIAG_NOTE, d, "");
    } else if (access(path, W_OK) != 0) {
        char d[768];
        snprintf(d, sizeof d, "%s is not writable by this user.", path);
        nexdiag_report_add(c->r, "config.cache", "Configuration", "HDE cache directory", NEXDIAG_FAIL, d,
                           "Fix the ownership: `sudo chown -R $USER: ~/.cache/hde`.");
    } else {
        nexdiag_report_add(c->r, "config.cache", "Configuration", "HDE cache directory", NEXDIAG_OK,
                           "~/.cache/hde is writable (logs and thumbnails go there).", "");
    }
}

static void check_logs(Collect *c, const NexdiagIdentity *id)
{
    char path[512];
    snprintf(path, sizeof path, "%s/.cache/hde/session.log", id->home);
    char *log = read_file(path, 256 * 1024);
    if (!log) {
        nexdiag_report_add(c->r, "logs.session", "Logs", "HDE session log", NEXDIAG_SKIP,
                           "There is no ~/.cache/hde/session.log on this machine.",
                           "Start a session with hde-start; the log is written from then on.");
        return;
    }
    int errors = nexdiag_count_errors(log);
    int warnings = nexdiag_count_warnings(log);
    char d[256];
    snprintf(d, sizeof d, "%d error line(s) and %d warning line(s) in the last 256 KB", errors, warnings);
    nexdiag_report_add(c->r, "logs.session", "Logs", "HDE session log",
                       errors ? NEXDIAG_WARN : NEXDIAG_OK, d,
                       errors ? "The error lines are at the end of this report; they often name the program that "
                                "failed."
                              : "");

    /* the excerpt that goes into the report: the last error lines */
    size_t log_len = strlen(log);
    size_t start = log_len > 8000 ? log_len - 8000 : 0;
    if (start && log[start - 1] != '\n') {
        char *nl = strchr(log + start, '\n');
        if (nl) start = (size_t)(nl - log) + 1;
    }
    Buf b = { 0 };
    const char *line = log + start;
    int taken = 0;
    while (line && *line && taken < 40) {
        const char *eol = strchr(line, '\n');
        size_t len = eol ? (size_t)(eol - line) : strlen(line);
        char buf[1024];
        size_t copy = len < sizeof buf - 1 ? len : sizeof buf - 1;
        memcpy(buf, line, copy);
        buf[copy] = 0;
        if (line_has_error(buf) || line_has_warning(buf)) {
            buf_add(&b, buf, strlen(buf));
            buf_ch(&b, '\n');
            taken++;
        }
        line = eol ? eol + 1 : NULL;
    }
    free(c->r->log_excerpt);
    c->r->log_excerpt = buf_take(&b);
    free(log);
}

/* ------------------------------------------------------------------ the run */

int nexdiag_collect(NexdiagReport *r)
{
    if (!r) return -1;
    nexdiag_report_free(r);
    nexdiag_report_init(r);
    r->log_excerpt = xstrdup("");

    NexdiagIdentity id;
    nexdiag_identity_init(&id);
    Collect c = { r };

    check_system(&c, &id);
    check_session(&c, &id);
    check_packages(&c);
    check_services(&c);
    check_network(&c);
    check_display(&c, &id);
    check_config(&c, &id);
    check_logs(&c, &id);

    nexdiag_identity_clear(&id);
    return r->count;
}

/* ------------------------------------------------------------------ the report */

static void append_check(Buf *b, const NexdiagCheck *c)
{
    char line[1200];
    snprintf(line, sizeof line, "[%-8s] %-22s %s\n", nexdiag_status_name(c->status), c->id, c->title);
    buf_str(b, line);
    if (c->detail[0]) {
        snprintf(line, sizeof line, "           %s\n", c->detail);
        buf_str(b, line);
    }
    if (c->hint[0] && c->status != NEXDIAG_OK) {
        snprintf(line, sizeof line, "           → %s\n", c->hint);
        buf_str(b, line);
    }
}

char *nexdiag_report_text(const NexdiagReport *r, const NexdiagIdentity *id, int redact, int include_log)
{
    Buf b = { 0 };
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    char when[64];
    strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S %Z", &tm_now);

    buf_str(&b, "Hyggshi NexDiag report\n");
    buf_str(&b, "=====================\n\n");
    if (id) {
        char line[1024];
        snprintf(line, sizeof line, "Generated : %s (nexdiag %s)\n", when, NEXDIAG_RELEASE);
        buf_str(&b, line);
        snprintf(line, sizeof line, "System    : %s\n", id->os[0] ? id->os : "unknown system");
        buf_str(&b, line);
        snprintf(line, sizeof line, "Kernel    : %s\n", id->kernel);
        buf_str(&b, line);
        snprintf(line, sizeof line, "Session   : %s\n", id->session);
        buf_str(&b, line);
        snprintf(line, sizeof line, "Desktop   : %s · window manager: %s\n", id->desktop,
                 id->wm[0] ? id->wm : "none");
        buf_str(&b, line);
        snprintf(line, sizeof line, "HDE       : %s\n", id->hde[0] ? id->hde : "not installed");
        buf_str(&b, line);
        snprintf(line, sizeof line, "User      : %s\n", id->user);
        buf_str(&b, line);
        snprintf(line, sizeof line, "Home      : %s\n", id->home);
        buf_str(&b, line);
        snprintf(line, sizeof line, "Host      : %s\n\n", id->host);
        buf_str(&b, line);
    } else {
        char line[256];
        snprintf(line, sizeof line, "Generated : %s (nexdiag %s)\n\n", when, NEXDIAG_RELEASE);
        buf_str(&b, line);
    }

    int ok = nexdiag_report_count(r, NEXDIAG_OK);
    int note = nexdiag_report_count(r, NEXDIAG_NOTE);
    int warn = nexdiag_report_count(r, NEXDIAG_WARN);
    int fail = nexdiag_report_count(r, NEXDIAG_FAIL);
    int skip = nexdiag_report_count(r, NEXDIAG_SKIP);
    char line[512];
    snprintf(line, sizeof line, "Result    : %d failed, %d warning(s), %d note(s), %d ok, %d not checked\n",
             fail, warn, note, ok, skip);
    buf_str(&b, line);
    snprintf(line, sizeof line, "Redaction : %s\n\n", redact ? "on (names, paths, addresses and secrets hidden)"
                                                            : "OFF — this report says everything");
    buf_str(&b, line);
    buf_str(&b, "How to read it: FAILED is something that does not work, WARNING something that is off and "
                "may be the cause, NOTE is only information, SKIPPED could not be checked here.\n\n");

    const char *current_group = NULL;
    for (int i = 0; r && i < r->count; i++) {
        const NexdiagCheck *c = &r->items[i];
        if (!current_group || strcmp(current_group, c->group) != 0) {
            current_group = c->group;
            snprintf(line, sizeof line, "\n%s\n%s\n", c->group, "--------------------------------------------------"
                                                                "------------------");
            buf_str(&b, line);
        }
        append_check(&b, c);
    }

    if (include_log && r && r->log_excerpt && r->log_excerpt[0]) {
        buf_str(&b, "\nLast error lines of the HDE session log\n"
                    "---------------------------------------------------------------\n");
        buf_str(&b, r->log_excerpt);
    }

    buf_str(&b, "\n-- \nnexdiag " NEXDIAG_RELEASE " · Hyggshi OS · "
                "https://github.com/Hyggshi-OS-Research-Technology/NexWM\n");

    char *raw = buf_take(&b);
    if (!redact) return raw;
    char *safe = nexdiag_redact_with(raw, id);
    free(raw);
    return safe;
}

char *nexdiag_report_default_path(void)
{
    const char *home = getenv("HOME");
    if (!home || !*home) home = getenv("TMPDIR");
    if (!home || !*home) home = "/tmp";
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm_now);
    size_t n = strlen(home) + 64;
    char *path = malloc(n);
    if (!path) return NULL;
    snprintf(path, n, "%s/nexdiag-report-%s.txt", home, stamp);
    return path;
}
