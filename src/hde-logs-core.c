/*
 * src/hde-logs-core.c — Pure-C System & Session Log Parser and Filter Engine for HDE
 */
#include "hde-logs-core.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int str_contains_ci(const char *haystack, const char *needle)
{
    if (!haystack || !needle || !*needle) return 0;
    size_t nlen = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        size_t i = 0;
        while (i < nlen && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == nlen) return 1;
    }
    return 0;
}

const char *hde_log_severity_name(HdeLogSeverity sev)
{
    switch (sev) {
    case HDE_LOG_SEV_EMERG:   return "Emergency";
    case HDE_LOG_SEV_ALERT:   return "Alert";
    case HDE_LOG_SEV_CRIT:    return "Critical";
    case HDE_LOG_SEV_ERR:     return "Error";
    case HDE_LOG_SEV_WARNING: return "Warning";
    case HDE_LOG_SEV_NOTICE:  return "Notice";
    case HDE_LOG_SEV_INFO:    return "Info";
    case HDE_LOG_SEV_DEBUG:   return "Debug";
    default:                  return "Info";
    }
}

const char *hde_log_severity_badge(HdeLogSeverity sev)
{
    switch (sev) {
    case HDE_LOG_SEV_EMERG:   return "EMERG";
    case HDE_LOG_SEV_ALERT:   return "ALERT";
    case HDE_LOG_SEV_CRIT:    return "CRIT";
    case HDE_LOG_SEV_ERR:     return "ERROR";
    case HDE_LOG_SEV_WARNING: return "WARN";
    case HDE_LOG_SEV_NOTICE:  return "NOTICE";
    case HDE_LOG_SEV_INFO:    return "INFO";
    case HDE_LOG_SEV_DEBUG:   return "DEBUG";
    default:                  return "INFO";
    }
}

const char *hde_log_category_name(HdeLogCategory cat)
{
    switch (cat) {
    case HDE_LOG_CAT_ALL:    return "All Logs";
    case HDE_LOG_CAT_ERRORS: return "Errors & Warnings";
    case HDE_LOG_CAT_HDE:    return "HDE Session";
    case HDE_LOG_CAT_SYSTEM: return "System Services";
    case HDE_LOG_CAT_KERNEL: return "Kernel & Hardware";
    case HDE_LOG_CAT_AUTH:   return "Security & Auth";
    case HDE_LOG_CAT_APPS:   return "Applications";
    default:                 return "System Services";
    }
}

HdeLogSeverity hde_log_classify_severity(const char *message, int syslog_pri)
{
    if (syslog_pri >= 0) {
        int sev = syslog_pri & 0x07;
        return (HdeLogSeverity)sev;
    }
    if (!message) return HDE_LOG_SEV_INFO;
    if (str_contains_ci(message, "panic") || str_contains_ci(message, "emerg"))
        return HDE_LOG_SEV_EMERG;
    if (str_contains_ci(message, "segfault") || str_contains_ci(message, "critical") ||
        str_contains_ci(message, "fatal") || str_contains_ci(message, "crit:"))
        return HDE_LOG_SEV_CRIT;
    if (str_contains_ci(message, "error") || str_contains_ci(message, "failed") ||
        str_contains_ci(message, "failure") || str_contains_ci(message, "err:") ||
        str_contains_ci(message, "[ee]"))
        return HDE_LOG_SEV_ERR;
    if (str_contains_ci(message, "warn") || str_contains_ci(message, "[ww]") ||
        str_contains_ci(message, "deprecated") || str_contains_ci(message, "timeout"))
        return HDE_LOG_SEV_WARNING;
    if (str_contains_ci(message, "notice"))
        return HDE_LOG_SEV_NOTICE;
    if (str_contains_ci(message, "debug") || str_contains_ci(message, "trace"))
        return HDE_LOG_SEV_DEBUG;
    return HDE_LOG_SEV_INFO;
}

HdeLogCategory hde_log_classify_category(const char *process, const char *message, const char *source_file)
{
    const char *p = process ? process : "";
    const char *m = message ? message : "";
    const char *s = source_file ? source_file : "";

    if (str_contains_ci(p, "hde-") || str_contains_ci(p, "nexwm") ||
        str_contains_ci(s, "hde") || str_contains_ci(m, "hde-") || str_contains_ci(m, "nexwm"))
        return HDE_LOG_CAT_HDE;

    if (str_contains_ci(p, "kernel") || str_contains_ci(p, "dmesg") ||
        str_contains_ci(s, "dmesg") || str_contains_ci(s, "kern.log"))
        return HDE_LOG_CAT_KERNEL;

    if (str_contains_ci(p, "sshd") || str_contains_ci(p, "sudo") ||
        str_contains_ci(p, "polkit") || str_contains_ci(p, "pam") ||
        str_contains_ci(p, "login") || str_contains_ci(p, "su") ||
        str_contains_ci(p, "keyring") || str_contains_ci(s, "auth.log") ||
        str_contains_ci(s, "secure"))
        return HDE_LOG_CAT_AUTH;

    if (str_contains_ci(p, "systemd") || str_contains_ci(p, "dbus") ||
        str_contains_ci(p, "networkmanager") || str_contains_ci(p, "cron") ||
        str_contains_ci(p, "udev") || str_contains_ci(p, "rsyslog") ||
        str_contains_ci(p, "avahi") || str_contains_ci(p, "cups"))
        return HDE_LOG_CAT_SYSTEM;

    return HDE_LOG_CAT_APPS;
}

static int is_month_prefix(const char *s)
{
    static const char *const months[12] = {
        "Jan ", "Feb ", "Mar ", "Apr ", "May ", "Jun ",
        "Jul ", "Aug ", "Sep ", "Oct ", "Nov ", "Dec "
    };
    for (int i = 0; i < 12; i++) {
        if (strncmp(s, months[i], 4) == 0)
            return 1;
    }
    return 0;
}

static void parse_tag_and_pid(const char *tag_in, char *proc_out, size_t proc_cap, int *pid_out)
{
    *pid_out = 0;
    const char *lb = strchr(tag_in, '[');
    const char *rb = lb ? strchr(lb, ']') : NULL;
    if (lb && rb && rb > lb + 1) {
        size_t plen = (size_t)(lb - tag_in);
        if (plen >= proc_cap) plen = proc_cap - 1;
        memcpy(proc_out, tag_in, plen);
        proc_out[plen] = '\0';
        *pid_out = atoi(lb + 1);
    } else {
        snprintf(proc_out, proc_cap, "%s", tag_in);
    }
}

int hde_log_parse_line(const char *raw_line, const char *source_file, HdeLogEntry *out)
{
    if (!raw_line || !out) return 0;
    while (*raw_line == ' ' || *raw_line == '\t')
        raw_line++;

    size_t len = strlen(raw_line);
    while (len > 0 && (raw_line[len - 1] == '\n' || raw_line[len - 1] == '\r'))
        len--;
    if (len == 0) return 0;

    memset(out, 0, sizeof(*out));
    snprintf(out->source_file, sizeof(out->source_file), "%s", source_file ? source_file : "system");
    snprintf(out->host, sizeof(out->host), "localhost");
    snprintf(out->process, sizeof(out->process), "system");

    char line[1024];
    if (len >= sizeof(line)) len = sizeof(line) - 1;
    memcpy(line, raw_line, len);
    line[len] = '\0';

    int syslog_pri = -1;
    const char *p = line;
    if (*p == '<' && isdigit((unsigned char)p[1])) {
        syslog_pri = atoi(p + 1);
        const char *gt = strchr(p, '>');
        if (gt) p = gt + 1;
    }

    /* Format 1: Traditional RFC 3164 syslog: "Oct 14 14:22:05 host proc[123]: message" */
    if (strlen(p) > 16 && is_month_prefix(p) && p[6] == ' ' && p[9] == ':' && p[12] == ':') {
        memcpy(out->timestamp, p, 15);
        out->timestamp[15] = '\0';
        p += 15;
        while (*p == ' ') p++;

        /* Host token */
        const char *sp = strchr(p, ' ');
        if (sp) {
            size_t hlen = (size_t)(sp - p);
            if (hlen < sizeof(out->host)) {
                memcpy(out->host, p, hlen);
                out->host[hlen] = '\0';
            }
            p = sp + 1;
            while (*p == ' ') p++;
        }

        /* Process[pid]: message */
        const char *colon = strstr(p, ": ");
        if (colon && colon - p < 60) {
            char tag[64];
            size_t tlen = (size_t)(colon - p);
            if (tlen >= sizeof(tag)) tlen = sizeof(tag) - 1;
            memcpy(tag, p, tlen);
            tag[tlen] = '\0';
            parse_tag_and_pid(tag, out->process, sizeof(out->process), &out->pid);
            p = colon + 2;
        }
        snprintf(out->message, sizeof(out->message), "%s", p);
    }
    /* Format 2: ISO-8601 timestamp: "2026-10-14T14:22:05..." or "2026-10-14 14:22:05..." */
    else if (strlen(p) > 20 && isdigit((unsigned char)p[0]) && isdigit((unsigned char)p[1]) &&
             isdigit((unsigned char)p[2]) && isdigit((unsigned char)p[3]) && p[4] == '-' &&
             (p[10] == 'T' || p[10] == ' ')) {
        const char *sp = strchr(p, ' ');
        if (sp && p[10] == ' ') {
            sp = strchr(sp + 1, ' ');
        }
        if (sp) {
            size_t tslen = (size_t)(sp - p);
            if (tslen >= sizeof(out->timestamp)) tslen = sizeof(out->timestamp) - 1;
            memcpy(out->timestamp, p, tslen);
            out->timestamp[tslen] = '\0';
            p = sp + 1;
            while (*p == ' ') p++;

            const char *sp2 = strchr(p, ' ');
            const char *colon = strstr(p, ": ");
            if (sp2 && (!colon || sp2 < colon)) {
                size_t hlen = (size_t)(sp2 - p);
                if (hlen < sizeof(out->host)) {
                    memcpy(out->host, p, hlen);
                    out->host[hlen] = '\0';
                }
                p = sp2 + 1;
                while (*p == ' ') p++;
            }
            colon = strstr(p, ": ");
            if (colon && colon - p < 60) {
                char tag[64];
                size_t tlen = (size_t)(colon - p);
                if (tlen >= sizeof(tag)) tlen = sizeof(tag) - 1;
                memcpy(tag, p, tlen);
                tag[tlen] = '\0';
                parse_tag_and_pid(tag, out->process, sizeof(out->process), &out->pid);
                p = colon + 2;
            }
        }
        snprintf(out->message, sizeof(out->message), "%s", p);
    }
    /* Format 3: Kernel dmesg: "[   12.345678] subsys: message" */
    else if (*p == '[' && (isdigit((unsigned char)p[1]) || p[1] == ' ')) {
        const char *rb = strchr(p, ']');
        if (rb && rb - p < 32) {
            size_t tslen = (size_t)(rb - p + 1);
            memcpy(out->timestamp, p, tslen);
            out->timestamp[tslen] = '\0';
            snprintf(out->process, sizeof(out->process), "kernel");
            p = rb + 1;
            while (*p == ' ') p++;
            const char *colon = strstr(p, ": ");
            if (colon && colon - p < 32) {
                size_t slen = (size_t)(colon - p);
                char sub[40];
                memcpy(sub, p, slen);
                sub[slen] = '\0';
                snprintf(out->process, sizeof(out->process), "kernel (%s)", sub);
                p = colon + 2;
            }
            snprintf(out->message, sizeof(out->message), "%s", p);
        } else {
            snprintf(out->timestamp, sizeof(out->timestamp), "session");
            snprintf(out->message, sizeof(out->message), "%s", p);
        }
    }
    /* Format 4: Generic "process: message" or "[process] message" */
    else {
        snprintf(out->timestamp, sizeof(out->timestamp), "session");
        if (*p == '[') {
            const char *rb = strchr(p, ']');
            if (rb && rb - p < 40) {
                size_t plen = (size_t)(rb - p - 1);
                memcpy(out->process, p + 1, plen);
                out->process[plen] = '\0';
                p = rb + 1;
                while (*p == ' ' || *p == ':') p++;
            }
        } else {
            const char *colon = strstr(p, ": ");
            if (colon && colon - p < 40 && !strchr(p, ' ')) {
                char tag[48];
                size_t tlen = (size_t)(colon - p);
                memcpy(tag, p, tlen);
                tag[tlen] = '\0';
                parse_tag_and_pid(tag, out->process, sizeof(out->process), &out->pid);
                p = colon + 2;
            }
        }
        snprintf(out->message, sizeof(out->message), "%s", p);
    }

    out->severity = hde_log_classify_severity(out->message, syslog_pri);
    out->category = hde_log_classify_category(out->process, out->message, out->source_file);
    return 1;
}

int hde_log_entry_matches(const HdeLogEntry *entry,
                          HdeLogCategory cat_filter,
                          HdeLogSeverity max_sev,
                          const char *query)
{
    if (!entry) return 0;
    if (entry->severity > max_sev) return 0;

    if (cat_filter == HDE_LOG_CAT_ERRORS) {
        if (entry->severity > HDE_LOG_SEV_WARNING)
            return 0;
    } else if (cat_filter != HDE_LOG_CAT_ALL) {
        if (entry->category != cat_filter)
            return 0;
    }

    if (query && *query) {
        if (!str_contains_ci(entry->message, query) &&
            !str_contains_ci(entry->process, query) &&
            !str_contains_ci(entry->source_file, query) &&
            !str_contains_ci(entry->timestamp, query))
            return 0;
    }
    return 1;
}
