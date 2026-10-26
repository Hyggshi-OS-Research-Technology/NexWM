/*
 * nexdiag/cli.c — `nexdiag`: the diagnostics of Hyggshi OS, HDE and NexWM on the command line.
 *
 *   nexdiag                     run every check and print them, coloured
 *   nexdiag --report [FILE]     write the report for a GitHub issue (redacted by default)
 *   nexdiag --no-redact         the same, with everything in it (it says so, and so does the file)
 *   nexdiag --group Session     only one group of checks
 *   nexdiag --quiet             only what is wrong
 *
 * Exit status: 0 nothing is wrong (warnings are fine when --quiet is not asked for: see 1),
 *              1 there are warnings, 2 something is broken, 3 NexDiag itself could not run.
 * In other words the shell can do `nexdiag --quiet || nexdiag --report` and get a file to attach.
 */
#define _POSIX_C_SOURCE 200809L

#include "nexdiag-core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static const char *colour(NexdiagStatus s)
{
    switch (s) {
    case NEXDIAG_OK: return "\033[32m";
    case NEXDIAG_NOTE: return "\033[36m";
    case NEXDIAG_WARN: return "\033[33m";
    case NEXDIAG_FAIL: return "\033[31m";
    case NEXDIAG_SKIP:
    default: return "\033[90m";
    }
}

static void usage(FILE *out)
{
    fprintf(out,
            "Usage: nexdiag [--report [FILE]] [--no-redact] [--group GROUP] [--check ID] [--quiet]\n"
            "       nexdiag --version\n"
            "\n"
            "NexDiag checks the session, the packages, the services, the network, the configuration and\n"
            "the logs of Hyggshi OS, HDE and NexWM, and can write a report to attach to a GitHub issue.\n"
            "\n"
            "  --report [FILE]  write the report (default: ~/nexdiag-report-<date>.txt) and say where\n"
            "  --no-redact      keep user name, host name, paths, addresses and secrets in the report\n"
            "  --group GROUP    only the checks of this group (Session, Packages, Services, Network,\n"
            "                   Display, Configuration, Logs, System)\n"
            "  --check ID       only this check (e.g. packages.audit)\n"
            "  --quiet          print only warnings and failures\n"
            "  --version        the NexDiag release\n"
            "\n"
            "Exit status: 0 all good, 1 warnings, 2 failures, 3 NexDiag could not run.\n");
}

int main(int argc, char **argv)
{
    int want_report = 0;
    int no_redact = 0;
    int quiet = 0;
    const char *group = NULL;
    const char *only_check = NULL;
    const char *report_path = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage(stdout);
            return 0;
        }
        if (!strcmp(a, "--version") || !strcmp(a, "-v")) {
            printf("nexdiag %s (NexDiag, Hyggshi OS)\n", NEXDIAG_RELEASE);
            return 0;
        }
        if (!strcmp(a, "--report")) {
            want_report = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') report_path = argv[++i];
            continue;
        }
        if (!strncmp(a, "--report=", 9)) {
            want_report = 1;
            report_path = a + 9;
            continue;
        }
        if (!strcmp(a, "--no-redact")) { no_redact = 1; continue; }
        if (!strcmp(a, "--quiet") || !strcmp(a, "-q")) { quiet = 1; continue; }
        if (!strcmp(a, "--group") && i + 1 < argc) { group = argv[++i]; continue; }
        if (!strncmp(a, "--group=", 8)) { group = a + 8; continue; }
        if (!strcmp(a, "--check") && i + 1 < argc) { only_check = argv[++i]; continue; }
        if (!strncmp(a, "--check=", 8)) { only_check = a + 8; continue; }
        fprintf(stderr, "nexdiag: unknown option %s (see --help)\n", a);
        return 3;
    }

    NexdiagReport report;
    nexdiag_report_init(&report);
    if (nexdiag_collect(&report) < 0) {
        fprintf(stderr, "nexdiag: could not collect the checks (out of memory?)\n");
        nexdiag_report_free(&report);
        return 3;
    }

    NexdiagIdentity id;
    nexdiag_identity_init(&id);

    if (want_report) {
        char *path = report_path ? strdup(report_path) : nexdiag_report_default_path();
        if (!path) {
            fprintf(stderr, "nexdiag: out of memory\n");
            nexdiag_identity_clear(&id);
            nexdiag_report_free(&report);
            return 3;
        }
        char *text = nexdiag_report_text(&report, &id, !no_redact, 1);
        if (!text) {
            fprintf(stderr, "nexdiag: out of memory\n");
            free(path);
            nexdiag_identity_clear(&id);
            nexdiag_report_free(&report);
            return 3;
        }
        /* "-" writes the report to standard output, for a pipeline */
        if (!strcmp(path, "-")) {
            fputs(text, stdout);
        } else {
            FILE *fp = fopen(path, "w");
            if (!fp) {
                fprintf(stderr, "nexdiag: could not write %s\n", path);
                free(text);
                free(path);
                nexdiag_identity_clear(&id);
                nexdiag_report_free(&report);
                return 3;
            }
            fputs(text, fp);
            fclose(fp);
            if (!no_redact)
                printf("Report written to %s (redacted: the user name, the home directory, the host name,\n"
                       "addresses, e-mail addresses, UUIDs and passwords are hidden).\n", path);
            else
                printf("Report written to %s — WITHOUT redaction: check it before you attach it.\n", path);
        }
        fflush(stdout);
        free(text);
        free(path);
    }

    if (!want_report || report_path) {
        int use_colour = isatty(fileno(stdout));
        NexdiagStatus worst = NEXDIAG_OK;
        const char *last_group = NULL;
        int shown = 0;

        for (int i = 0; i < report.count; i++) {
            const NexdiagCheck *c = &report.items[i];
            if (group && strcasecmp(group, c->group) != 0) continue;
            if (only_check && strcmp(only_check, c->id) != 0) continue;
            if (c->status != NEXDIAG_SKIP && c->status > worst) worst = c->status;
            if (quiet && (c->status == NEXDIAG_OK || c->status == NEXDIAG_NOTE)) continue;
            if (!last_group || strcmp(last_group, c->group) != 0) {
                last_group = c->group;
                printf("\n%s\n", c->group);
            }
            if (use_colour) printf("%s", colour(c->status));
            printf("  %-8s %-22s %s\n", nexdiag_status_name(c->status), c->id, c->title);
            if (use_colour) printf("\033[0m");
            if (c->detail[0]) printf("           %s\n", c->detail);
            if (c->hint[0] && c->status != NEXDIAG_OK) printf("           → %s\n", c->hint);
            shown++;
        }

        printf("\n%d check(s) shown · %d failed · %d warning(s) · %d ok · %d not checked\n", shown,
               nexdiag_report_count(&report, NEXDIAG_FAIL), nexdiag_report_count(&report, NEXDIAG_WARN),
               nexdiag_report_count(&report, NEXDIAG_OK), nexdiag_report_count(&report, NEXDIAG_SKIP));
        if (worst >= NEXDIAG_FAIL || worst == NEXDIAG_WARN) {
            printf("To make a report for a bug report:\n    nexdiag --report\n");
        }
        nexdiag_identity_clear(&id);
        nexdiag_report_free(&report);
        return worst >= NEXDIAG_FAIL ? 2 : worst == NEXDIAG_WARN ? 1 : 0;
    }

    NexdiagStatus worst = nexdiag_report_worst(&report);
    nexdiag_identity_clear(&id);
    nexdiag_report_free(&report);
    return worst >= NEXDIAG_FAIL ? 2 : worst == NEXDIAG_WARN ? 1 : 0;
}
