/*
 * src/hde-logs-core.h — Pure-C System & Session Log Parser and Filter Engine for HDE
 *
 * Parses syslog (RFC 3164 / RFC 5424), systemd journalctl, kernel dmesg, Xorg,
 * and HDE session logs into structured HdeLogEntry records with severity and
 * category classification, tested by tests/logs-test.c.
 */
#ifndef HDE_LOGS_CORE_H
#define HDE_LOGS_CORE_H

#include <stddef.h>

typedef enum {
    HDE_LOG_SEV_EMERG = 0,
    HDE_LOG_SEV_ALERT = 1,
    HDE_LOG_SEV_CRIT = 2,
    HDE_LOG_SEV_ERR = 3,
    HDE_LOG_SEV_WARNING = 4,
    HDE_LOG_SEV_NOTICE = 5,
    HDE_LOG_SEV_INFO = 6,
    HDE_LOG_SEV_DEBUG = 7
} HdeLogSeverity;

typedef enum {
    HDE_LOG_CAT_ALL = 0,
    HDE_LOG_CAT_ERRORS = 1,  /* Virtual filter: EMERG..WARNING */
    HDE_LOG_CAT_HDE = 2,     /* Hyggshi Desktop & NexWM */
    HDE_LOG_CAT_SYSTEM = 3,  /* systemd, dbus, NetworkManager, daemon */
    HDE_LOG_CAT_KERNEL = 4,  /* kernel / dmesg / hardware */
    HDE_LOG_CAT_AUTH = 5,    /* sshd, sudo, polkit, pam, login */
    HDE_LOG_CAT_APPS = 6     /* user applications & Xorg */
} HdeLogCategory;

typedef struct {
    char timestamp[40];
    char host[48];
    char process[64];
    int pid;
    HdeLogSeverity severity;
    HdeLogCategory category;
    char source_file[96];
    char message[512];
} HdeLogEntry;

const char *hde_log_severity_name(HdeLogSeverity sev);
const char *hde_log_severity_badge(HdeLogSeverity sev);
const char *hde_log_category_name(HdeLogCategory cat);

/* Classify severity from message text and optional syslog priority (<0>..<191>) */
HdeLogSeverity hde_log_classify_severity(const char *message, int syslog_pri);

/* Classify category from process/unit name, message, and source file path */
HdeLogCategory hde_log_classify_category(const char *process, const char *message, const char *source_file);

/* Parse a single raw log line into `out`. Returns 1 if non-empty and parsed, 0 if blank. */
int hde_log_parse_line(const char *raw_line, const char *source_file, HdeLogEntry *out);

/* Returns 1 if `entry` matches category filter (`cat_filter`), severity ceiling (`max_sev`),
 * and case-insensitive search substring (`query`, may be NULL or ""). */
int hde_log_entry_matches(const HdeLogEntry *entry,
                          HdeLogCategory cat_filter,
                          HdeLogSeverity max_sev,
                          const char *query);

#endif /* HDE_LOGS_CORE_H */
