/*
 * nexdiag/nexdiag-core.h — NexDiag, the diagnostics of Hyggshi OS, HDE and NexWM: the plain-C half.
 *
 * What NexDiag looks at: the session (display server, window manager, HDE components, lock screen),
 * the packages (dpkg's own audit, missing libraries, missing tools), the services (systemd units,
 * the session bus), the network, the configuration of HDE, the logs, and the system itself.
 *
 * Every check ends as OK, NOTE, WARN, FAIL or SKIP and says what to do about it, so that the report can
 * be attached to a GitHub issue as it is.  The report is redacted by default: the user name, the home
 * directory, the host name, IP and MAC addresses, e-mail addresses, UUIDs and anything that looks like
 * a password or a token are replaced before a single byte is written.
 *
 * No GTK, no GLib: tests/nexdiag-test.c drives the parsers, the redaction and the report without a
 * display, and both `nexdiag` (the command) and Settings > System Health (src/hde-settings-health.c)
 * use the same code.
 */
#ifndef NEXDIAG_CORE_H
#define NEXDIAG_CORE_H

#include <stddef.h>

typedef enum {
    NEXDIAG_OK = 0,
    NEXDIAG_NOTE = 1,   /* worth knowing, nothing wrong */
    NEXDIAG_WARN = 2,   /* something is off but the session works */
    NEXDIAG_FAIL = 3,   /* something is broken */
    NEXDIAG_SKIP = 4    /* could not be checked here (no systemd, no display, …) */
} NexdiagStatus;

#define NEXDIAG_ID_LEN 40
#define NEXDIAG_GROUP_LEN 32
#define NEXDIAG_TITLE_LEN 120
#define NEXDIAG_DETAIL_LEN 512
#define NEXDIAG_HINT_LEN 320

typedef struct {
    char id[NEXDIAG_ID_LEN];        /* "session.wm" */
    char group[NEXDIAG_GROUP_LEN];  /* "Session" */
    char title[NEXDIAG_TITLE_LEN];  /* "Window manager" */
    NexdiagStatus status;
    char detail[NEXDIAG_DETAIL_LEN];
    char hint[NEXDIAG_HINT_LEN];    /* what to do about a warning or a failure, "" when there is nothing */
} NexdiagCheck;

typedef struct {
    NexdiagCheck *items;
    int count;
    int cap;
    char *log_excerpt;              /* the last error lines of the HDE session log, "" when there are none */
} NexdiagReport;

void nexdiag_report_init(NexdiagReport *r);
void nexdiag_report_free(NexdiagReport *r);
/* Add a check (copies every string; truncates at the field sizes).  Returns its index, or -1. */
int  nexdiag_report_add(NexdiagReport *r, const char *id, const char *group, const char *title,
                        NexdiagStatus status, const char *detail, const char *hint);
/* How many checks ended in each state. */
int  nexdiag_report_count(const NexdiagReport *r, NexdiagStatus status);
/* The worst state of all the checks (NEXDIAG_OK for an empty report). */
NexdiagStatus nexdiag_report_worst(const NexdiagReport *r);
const char *nexdiag_status_name(NexdiagStatus s);    /* "OK", "WARNING", … */
const char *nexdiag_status_title(NexdiagStatus s);   /* "All good", "Needs attention", … */
const char *nexdiag_status_icon(NexdiagStatus s);    /* an icon theme name: emblem-ok-symbolic, … */

/* ---- what identifies this machine, and what a report must hide ---- */
typedef struct {
    char user[64];       /* $USER (or $LOGNAME) */
    char home[256];      /* $HOME */
    char host[256];      /* the host name */
    char os[160];        /* PRETTY_NAME of /etc/os-release */
    char kernel[96];     /* "Linux 6.12.38 x86_64" */
    char session[96];    /* "X11 (DISPLAY=:0)", "Wayland (wayland-0)", "headless" */
    char wm[64];         /* "NexWM", "Metacity", "" */
    char desktop[64];    /* "HDE", "GNOME", … */
    char hde[64];        /* the HDE build ("1.0 · 4cf724f 2026-10-09") or "" */
} NexdiagIdentity;

void nexdiag_identity_init(NexdiagIdentity *id);
void nexdiag_identity_clear(NexdiagIdentity *id);

/* A copy of `text` with the identity of the machine and the person hidden; NULL safe, never NULL. */
char *nexdiag_redact_with(const char *text, const NexdiagIdentity *id);
/* The same, in place (the buffer never grows). */
void  nexdiag_redact_buffer(char *text, const NexdiagIdentity *id);
/* Convenience: reads what it needs itself.  Free with free(). */
char *nexdiag_redact(const char *text);

/* ---- parsers: real tool output in, a sentence out (all covered by tests/nexdiag-test.c) ---- */
/* `ldd a b c` output: the names of the libraries nobody provides ("libfoo.so.1"),
 * joined with ", " in `out`.  Returns how many are missing. */
int nexdiag_parse_ldd_missing(const char *ldd_output, char *out, size_t outsz);
/* `systemctl --failed --no-legend --plain`: the failed unit names, joined with ", ".
 * Returns how many units failed. */
int nexdiag_parse_failed_units(const char *systemctl_output, char *out, size_t outsz);
/* `nmcli -t -f STATE general`: "connected", "disconnected", "unavailable", "asleep"… */
int nexdiag_parse_nmcli_state(const char *output, char *state, size_t statesz);
/* Lines that look like an error (error, failed, cannot, no such file, segfault, panic) or a warning. */
int nexdiag_count_errors(const char *text);
int nexdiag_count_warnings(const char *text);
/* `dpkg --audit`: 0 when dpkg is happy, otherwise the first line of the complaints in `out`. */
int nexdiag_parse_dpkg_audit(const char *audit_output, char *out, size_t outsz);
/* An INI file (HDE's settings.ini): 0 when it is readable, else what is wrong with the first problem. */
int nexdiag_check_ini(const char *text, char *out, size_t outsz);
/* "MemTotal: 16384000 kB" of /proc/meminfo: 0 when the field is not there. */
unsigned long long nexdiag_parse_meminfo_kb(const char *meminfo, const char *field);

/* ---- the run ---- */
/* Run every check and fill `r` (it is cleared first).  Prints nothing, needs no display.
 * Returns the number of checks added, or -1 when the report could not be grown. */
int nexdiag_collect(NexdiagReport *r);

/* ---- the report ---- */
/* The plain text of the report, ready to be pasted into a GitHub issue.  `redact` hides the identity;
 * `include_log` appends the error lines of the session log.  Free with free(). */
char *nexdiag_report_text(const NexdiagReport *r, const NexdiagIdentity *id, int redact, int include_log);
/* "nexdiag-report-20261026-140322.txt" for $HOME (or the temporary directory).  Free with free(). */
char *nexdiag_report_default_path(void);

/* What NexDiag is, in one line, for --version and the About box of the window. */
#define NEXDIAG_RELEASE "1.0"

#endif /* NEXDIAG_CORE_H */
