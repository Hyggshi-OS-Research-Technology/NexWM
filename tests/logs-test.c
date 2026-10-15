/*
 * tests/logs-test.c — Unit tests for Hyggshi Logs parser and filter engine
 */
#include "hde-logs-core.h"

#include <stdio.h>
#include <string.h>

static int passed = 0;
static int failed = 0;

static void check(int cond, const char *msg)
{
    if (cond) {
        printf("PASS: logs: %s\n", msg);
        passed++;
    } else {
        printf("FAIL: logs: %s\n", msg);
        failed++;
    }
}

int main(void)
{
    HdeLogEntry e;

    /* 1. RFC 3164 syslog line */
    check(hde_log_parse_line("Oct 14 14:22:05 hyggshi-pc hde-panel[1420]: Panel initialized on monitor 0",
                             "/var/log/syslog", &e) == 1,
          "parses RFC 3164 syslog line");
    check(strcmp(e.timestamp, "Oct 14 14:22:05") == 0, "extracts syslog timestamp");
    check(strcmp(e.host, "hyggshi-pc") == 0, "extracts syslog hostname");
    check(strcmp(e.process, "hde-panel") == 0 && e.pid == 1420, "extracts process hde-panel and PID 1420");
    check(e.category == HDE_LOG_CAT_HDE, "classifies hde-panel under HDE_LOG_CAT_HDE");
    check(e.severity == HDE_LOG_SEV_INFO, "classifies normal message as INFO");

    /* 2. Kernel dmesg line with error */
    check(hde_log_parse_line("[   42.198201] nvme0n1: I/O error, dev nvme0n1, sector 2048",
                             "dmesg", &e) == 1,
          "parses kernel dmesg line");
    check(e.category == HDE_LOG_CAT_KERNEL, "classifies dmesg line under HDE_LOG_CAT_KERNEL");
    check(e.severity == HDE_LOG_SEV_ERR, "classifies I/O error as HDE_LOG_SEV_ERR");

    /* 3. Auth / Security line with warning */
    check(hde_log_parse_line("2026-10-14T15:01:19+07:00 workstation sshd[3812]: Failed password for invalid user admin",
                             "/var/log/auth.log", &e) == 1,
          "parses ISO-8601 auth.log line");
    check(e.category == HDE_LOG_CAT_AUTH, "classifies sshd under HDE_LOG_CAT_AUTH");
    check(e.severity == HDE_LOG_SEV_ERR, "classifies Failed password under HDE_LOG_SEV_ERR");

    /* 4. Filtering by category, severity, and query */
    check(hde_log_entry_matches(&e, HDE_LOG_CAT_ERRORS, HDE_LOG_SEV_DEBUG, NULL) == 1,
          "error entry matches HDE_LOG_CAT_ERRORS virtual category");
    check(hde_log_entry_matches(&e, HDE_LOG_CAT_AUTH, HDE_LOG_SEV_ERR, "sshd") == 1,
          "entry matches category AUTH, max_sev ERR, and query 'sshd'");
    check(hde_log_entry_matches(&e, HDE_LOG_CAT_AUTH, HDE_LOG_SEV_CRIT, NULL) == 0,
          "ERR entry is filtered out when max_sev is CRIT");
    check(hde_log_entry_matches(&e, HDE_LOG_CAT_AUTH, HDE_LOG_SEV_DEBUG, "nonexistent") == 0,
          "entry is filtered out when query does not match");

    printf("\nlogs-test: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
