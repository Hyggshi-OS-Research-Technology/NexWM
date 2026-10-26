/* nexsdk/system.h — what this machine is, without glib, GTK or a display.
 *
 * Everything comes from files and the environment: /etc/os-release, /proc/sys/kernel/osrelease, uname(2),
 * /proc/meminfo, /proc/uptime, XDG_SESSION_TYPE, XDG_CURRENT_DESKTOP, /usr/share/hde/version.  That makes it
 * work in a session, over SSH, in a container and in a unit test.
 */
#ifndef NEXSDK_SYSTEM_H
#define NEXSDK_SYSTEM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NEXSDK_SESSION_UNKNOWN = 0,
    NEXSDK_SESSION_X11,
    NEXSDK_SESSION_WAYLAND,
    NEXSDK_SESSION_TTY
} NexSdkSessionType;

typedef struct {
    /* /etc/os-release */
    char os_name[64];       /* NAME */
    char os_version[64];    /* VERSION_ID */
    char os_pretty[128];    /* PRETTY_NAME */
    char os_id[32];         /* ID, e.g. "debian" */
    char os_like[64];       /* ID_LIKE, e.g. "debian ubuntu" */
    /* the kernel and the machine */
    char kernel[64];        /* 6.12.38-x86_64 */
    char machine[64];       /* x86_64 */
    char host[128];         /* host name (never sent anywhere by NexSDK itself) */
    /* the session */
    NexSdkSessionType session;
    char session_raw[32];   /* "x11", "wayland", "tty", "" */
    char desktop[64];       /* XDG_CURRENT_DESKTOP, e.g. "HDE" */
    char hde_version[64];   /* /usr/share/hde/version, e.g. "1.0 (4cf724f 2026-10-09)" */
    /* the numbers */
    unsigned long long mem_total_kb;
    unsigned long long mem_available_kb;
    long uptime_seconds;
} NexSdkSystem;

/* Zero the structure (every field becomes an empty string / 0). */
void nexsdk_system_init(NexSdkSystem *s);

/* Read everything.  Returns 0 when at least the operating system could be read, -1 when s is NULL or the
 * machine is so unlike a Linux system that nothing could be filled in.  Missing pieces are left empty, they
 * are never guesses. */
int nexsdk_system_read(NexSdkSystem *s);

/* "Hyggshi OS 1.0 · Linux 6.12.38-x86_64 · Wayland · HDE" — malloc'd, free with free(). */
char *nexsdk_system_summary(const NexSdkSystem *s);

/* The same, one line per fact, for a log or a bug report: malloc'd. */
char *nexsdk_system_report(const NexSdkSystem *s);

/* "x11" / "wayland" / "tty" / "unknown" */
const char *nexsdk_session_name(NexSdkSessionType type);

/* 1 when the HDE desktop is installed (its version file exists), 0 otherwise. */
int nexsdk_hde_installed(void);

/* ---- the parts, for programs that read one file themselves (and for the unit tests) ----
 * All of them are plain parsers over text: no files are opened. */
/* KEY=value of os-release; quotes are removed.  Returns 0 when found. */
int nexsdk_parse_os_release(const char *text, const char *key, char *out, size_t out_size);
/* "MemTotal: 16384000 kB" -> 16384000; 0 when the field is not there. */
unsigned long long nexsdk_parse_meminfo(const char *text, const char *field);
/* "12345.67 45678.90" (of /proc/uptime) -> 12345 */
long nexsdk_parse_uptime(const char *text);
/* "1.0 (4cf724f 2026-10-09)" of /usr/share/hde/version, trailing newline removed. */
int nexsdk_parse_hde_version(const char *text, char *out, size_t out_size);
/* "x11" / "wayland" / "tty" -> the enum; anything else is UNKNOWN. */
NexSdkSessionType nexsdk_session_from_string(const char *value);

#ifdef __cplusplus
}
#endif
#endif
