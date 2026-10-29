/* nexsdk/src/hde-system.c — the machine this program runs on, read from /etc/os-release, uname(2),
 * /proc/meminfo, /proc/uptime and the environment.  Plain C: no glib, no GTK, no display. */
#define _POSIX_C_SOURCE 200809L

#include "nexsdk/nexsdk.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

void nexsdk_system_init(NexSdkSystem *s)
{
    if (s) memset(s, 0, sizeof *s);
}

static void copy_str(char *dst, size_t size, const char *src)
{
    if (!dst || size == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= size) n = size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void trim(char *s)
{
    if (!s) return;
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
}

static int read_text_file(const char *path, char *buf, size_t size)
{
    if (!path || !buf || size == 0) return -1;
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    size_t n = fread(buf, 1, size - 1, fp);
    fclose(fp);
    buf[n] = '\0';
    return 0;
}

int nexsdk_parse_os_release(const char *text, const char *key, char *out, size_t out_size)
{
    if (!text || !key || !out || out_size == 0) return -1;
    out[0] = '\0';
    size_t klen = strlen(key);
    const char *p = text;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        const char *eq = memchr(p, '=', len);
        if (eq && (size_t)(eq - p) == klen && strncmp(p, key, klen) == 0) {
            const char *v = eq + 1;
            size_t vlen = len - (size_t)(v - p);
            /* the value may be quoted with " or ' */
            if (vlen >= 2 && (v[0] == '"' || v[0] == '\'') && v[vlen - 1] == v[0]) {
                v++;
                vlen -= 2;
            }
            if (vlen >= out_size) vlen = out_size - 1;
            memcpy(out, v, vlen);
            out[vlen] = '\0';
            trim(out);
            return 0;
        }
        if (!eol) break;
        p = eol + 1;
    }
    return -1;
}

unsigned long long nexsdk_parse_meminfo(const char *text, const char *field)
{
    if (!text || !field) return 0;
    size_t flen = strlen(field);
    const char *p = text;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if (len > flen && strncmp(p, field, flen) == 0 && p[flen] == ':') {
            const char *v = p + flen + 1;
            while (v < p + len && (*v == ' ' || *v == '\t')) v++;
            char num[32];
            size_t i = 0;
            while (v + i < p + len && i + 1 < sizeof num && isdigit((unsigned char)v[i])) {
                num[i] = v[i];
                i++;
            }
            num[i] = '\0';
            return strtoull(num, NULL, 10);
        }
        if (!eol) break;
        p = eol + 1;
    }
    return 0;
}

long nexsdk_parse_uptime(const char *text)
{
    if (!text) return 0;
    while (*text && !isdigit((unsigned char)*text)) text++;
    return (long)strtod(text, NULL);
}

int nexsdk_parse_hde_version(const char *text, char *out, size_t out_size)
{
    if (!text || !out || out_size == 0) return -1;
    out[0] = '\0';
    while (*text && isspace((unsigned char)*text)) text++;
    size_t n = strcspn(text, "\r\n");
    if (n == 0 || n >= out_size) {
        if (n == 0) return -1;
        n = out_size - 1;
    }
    memcpy(out, text, n);
    out[n] = '\0';
    trim(out);
    return out[0] ? 0 : -1;
}

NexSdkSessionType nexsdk_session_from_string(const char *value)
{
    if (!value) return NEXSDK_SESSION_UNKNOWN;
    if (!strcmp(value, "x11") || !strcmp(value, "X11")) return NEXSDK_SESSION_X11;
    if (!strcmp(value, "wayland") || !strcmp(value, "Wayland")) return NEXSDK_SESSION_WAYLAND;
    if (!strcmp(value, "tty") || !strcmp(value, "TTY")) return NEXSDK_SESSION_TTY;
    return NEXSDK_SESSION_UNKNOWN;
}

const char *nexsdk_session_name(NexSdkSessionType type)
{
    switch (type) {
    case NEXSDK_SESSION_X11: return "x11";
    case NEXSDK_SESSION_WAYLAND: return "wayland";
    case NEXSDK_SESSION_TTY: return "tty";
    case NEXSDK_SESSION_UNKNOWN:
    default: return "unknown";
    }
}

int nexsdk_hde_installed(void)
{
    if (access("/usr/share/hde/version", R_OK) == 0) return 1;
    if (access("/usr/local/share/hde/version", R_OK) == 0) return 1;
    return 0;
}

int nexsdk_system_read(NexSdkSystem *s)
{
    if (!s) return -1;
    nexsdk_system_init(s);
    char buf[4096];
    char value[256];

    if (read_text_file("/etc/os-release", buf, sizeof buf) == 0 ||
        read_text_file("/usr/lib/os-release", buf, sizeof buf) == 0) {
        if (nexsdk_parse_os_release(buf, "NAME", value, sizeof value) == 0)
            copy_str(s->os_name, sizeof s->os_name, value);
        if (nexsdk_parse_os_release(buf, "VERSION_ID", value, sizeof value) == 0)
            copy_str(s->os_version, sizeof s->os_version, value);
        if (nexsdk_parse_os_release(buf, "PRETTY_NAME", value, sizeof value) == 0)
            copy_str(s->os_pretty, sizeof s->os_pretty, value);
        if (nexsdk_parse_os_release(buf, "ID", value, sizeof value) == 0)
            copy_str(s->os_id, sizeof s->os_id, value);
        if (nexsdk_parse_os_release(buf, "ID_LIKE", value, sizeof value) == 0)
            copy_str(s->os_like, sizeof s->os_like, value);
    }
    if (!s->os_pretty[0]) {
        if (s->os_name[0] && s->os_version[0])
            snprintf(s->os_pretty, sizeof s->os_pretty, "%s %s", s->os_name, s->os_version);
        else if (s->os_name[0])
            copy_str(s->os_pretty, sizeof s->os_pretty, s->os_name);
    }

    struct utsname u;
    if (uname(&u) == 0) {
        copy_str(s->kernel, sizeof s->kernel, u.release);
        copy_str(s->machine, sizeof s->machine, u.machine);
        copy_str(s->host, sizeof s->host, u.nodename);
    } else {
        if (read_text_file("/proc/sys/kernel/osrelease", buf, sizeof buf) == 0) {
            copy_str(s->kernel, sizeof s->kernel, buf);
            trim(s->kernel);
        }
    }

    const char *session = getenv("XDG_SESSION_TYPE");
    if (!session || !*session) {
        /* a program launched from the panel or a terminal of an X11 session may only have DISPLAY */
        if (getenv("WAYLAND_DISPLAY")) session = "wayland";
        else if (getenv("DISPLAY")) session = "x11";
    }
    s->session = nexsdk_session_from_string(session);
    copy_str(s->session_raw, sizeof s->session_raw, session ? session : "");

    const char *desktop = getenv("XDG_CURRENT_DESKTOP");
    if (!desktop || !*desktop) desktop = getenv("DESKTOP_SESSION");
    copy_str(s->desktop, sizeof s->desktop, desktop ? desktop : "");

    if (read_text_file("/usr/share/hde/version", buf, sizeof buf) == 0 ||
        read_text_file("/usr/local/share/hde/version", buf, sizeof buf) == 0) {
        if (nexsdk_parse_hde_version(buf, value, sizeof value) == 0)
            copy_str(s->hde_version, sizeof s->hde_version, value);
    }

    if (read_text_file("/proc/meminfo", buf, sizeof buf) == 0) {
        s->mem_total_kb = nexsdk_parse_meminfo(buf, "MemTotal");
        s->mem_available_kb = nexsdk_parse_meminfo(buf, "MemAvailable");
        if (!s->mem_available_kb) s->mem_available_kb = nexsdk_parse_meminfo(buf, "MemFree");
    }
    if (read_text_file("/proc/uptime", buf, sizeof buf) == 0) s->uptime_seconds = nexsdk_parse_uptime(buf);

    return s->os_pretty[0] || s->kernel[0] ? 0 : -1;
}

static char *str_dup_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) return NULL;
    char *out = malloc((size_t)n + 1);
    if (!out) return NULL;
    va_start(ap, fmt);
    vsnprintf(out, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return out;
}

char *nexsdk_system_summary(const NexSdkSystem *s)
{
    if (!s) return NULL;
    const char *os = s->os_pretty[0] ? s->os_pretty : "unknown system";
    char session[192] = "";
    if (s->session != NEXSDK_SESSION_UNKNOWN) {
        snprintf(session, sizeof session, " · %s", nexsdk_session_name(s->session));
        if (s->desktop[0]) {
            size_t n = strlen(session);
            snprintf(session + n, sizeof session - n, " · %s", s->desktop);
        }
    }
    if (s->kernel[0])
        return str_dup_printf("%s · Linux %s%s", os, s->kernel, session);
    return str_dup_printf("%s%s", os, session);
}

char *nexsdk_system_report(const NexSdkSystem *s)
{
    if (!s) return NULL;
    char mem[128] = "unknown";
    if (s->mem_total_kb) {
        double total = (double)s->mem_total_kb / 1024.0 / 1024.0;
        double avail = (double)s->mem_available_kb / 1024.0 / 1024.0;
        snprintf(mem, sizeof mem, "%.1f GB (%.1f GB available)", total, avail);
    }
    char up[64] = "just started";
    if (s->uptime_seconds > 0)
        snprintf(up, sizeof up, "%ldh %ldm", s->uptime_seconds / 3600, (s->uptime_seconds / 60) % 60);

    char session[128];
    if (s->session_raw[0])
        snprintf(session, sizeof session, "%s (%s)", nexsdk_session_name(s->session), s->session_raw);
    else
        snprintf(session, sizeof session, "%s", nexsdk_session_name(s->session));

    return str_dup_printf("System   : %s\n"
                          "Kernel   : Linux %s (%s)\n"
                          "Host     : %s\n"
                          "Session  : %s\n"
                          "Desktop  : %s\n"
                          "HDE      : %s\n"
                          "Memory   : %s\n"
                          "Uptime   : %s\n",
                          s->os_pretty[0] ? s->os_pretty : "unknown",
                          s->kernel[0] ? s->kernel : "unknown",
                          s->machine[0] ? s->machine : "unknown",
                          s->host[0] ? s->host : "unknown", session,
                          s->desktop[0] ? s->desktop : "none",
                          s->hde_version[0] ? s->hde_version : "not installed", mem, up);
}

int nexsdk_in_hde_session(void)
{
    if (getenv("HDE_SESSION_PID") && *getenv("HDE_SESSION_PID")) return 1;
    const char *d = getenv("XDG_CURRENT_DESKTOP");
    if (d && (strstr(d, "HDE") || strstr(d, "Hyggshi"))) return 1;
    return nexsdk_hde_installed() && getenv("XDG_SESSION_TYPE") ? 1 : 0;
}

const char *nexsdk_os_release(void)
{
#ifdef NEXSDK_OS_RELEASE
    return NEXSDK_OS_RELEASE;
#else
    return "";
#endif
}
