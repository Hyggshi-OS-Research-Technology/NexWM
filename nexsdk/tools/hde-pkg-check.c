/* nexsdk/tools/hde-pkg-check.c — check a .deb before it is published.
 *
 *   hde-pkg-check myapp_1.0-1_amd64.deb        one package
 *   hde-pkg-check build/…                     every package that was just built
 *   hde-pkg-check --quiet myapp_*.deb          only what is wrong (for a Makefile or a CI script)
 *   hde-pkg-check --strict myapp_*.deb         warnings are errors: the exit status says so
 *
 * Exit status: 0 nothing wrong, 1 warnings (errors with --strict), 2 errors, 3 the file cannot be read.
 * That is what makes it usable at the end of `debian/rules`:
 *
 *   override_dh_builddeb:
 *       dh_builddeb
 *       hde-pkg-check --strict ../…deb
 */
#define _POSIX_C_SOURCE 200809L

#include <nexsdk/nexsdk.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *out)
{
    fprintf(out,
            "Usage: hde-pkg-check [options] PACKAGE.deb...\n"
            "\n"
            "Checks a Debian package before it is published: the control fields (name, version,\n"
            "architecture, maintainer, a description with more than a synopsis, Depends), the files\n"
            "(no /usr/local, no setuid, /usr/share/doc/<package>/copyright), and every .desktop file\n"
            "(Type, Name, and whether Exec and Icon name something the package actually installs).\n"
            "\n"
            "  -q, --quiet    print only warnings and errors\n"
            "      --strict   treat warnings as errors (exit status 1)\n"
            "      --json     the findings as JSON, for a CI job\n"
            "  -h, --help     this text\n"
            "  -v, --version  the NexSDK release\n"
            "\n"
            "Exit status: 0 all good, 1 warnings (or errors with --strict), 2 errors, 3 unreadable file.\n");
}

/* JSON strings: quotes, backslashes and control characters escaped. */
static void json_string(const char *s)
{
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        switch (*p) {
        case '"': fputs("\\\"", stdout); break;
        case '\\': fputs("\\\\", stdout); break;
        case '\n': fputs("\\n", stdout); break;
        case '\t': fputs("\\t", stdout); break;
        case '\r': break;
        default:
            if (*p < 0x20) printf("\\u%04x", *p);
            else putchar(*p);
        }
    }
    putchar('"');
}

static const char *json_status(NexSdkPkgStatus s)
{
    switch (s) {
    case NEXSDK_PKG_OK: return "ok";
    case NEXSDK_PKG_NOTE: return "note";
    case NEXSDK_PKG_WARN: return "warning";
    case NEXSDK_PKG_ERROR:
    default: return "error";
    }
}

int main(int argc, char **argv)
{
    int quiet = 0, strict = 0, json = 0;
    int first_file = argc;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage(stdout);
            return 0;
        }
        if (!strcmp(a, "-v") || !strcmp(a, "--version")) {
            printf("hde-pkg-check %s (NexSDK)\n", nexsdk_version());
            return 0;
        }
        if (!strcmp(a, "-q") || !strcmp(a, "--quiet")) {
            quiet = 1;
            continue;
        }
        if (!strcmp(a, "--strict")) {
            strict = 1;
            continue;
        }
        if (!strcmp(a, "--json")) {
            json = 1;
            continue;
        }
        if (a[0] == '-' && a[1]) {
            fprintf(stderr, "hde-pkg-check: unknown option %s (see --help)\n", a);
            return 3;
        }
        first_file = i;
        break;
    }
    if (first_file >= argc) {
        usage(stderr);
        return 3;
    }

    int worst_exit = 0;
    int total = 0, errors = 0, warnings = 0, files_checked = 0;
    if (json) fputs("{\n  \"packages\": [\n", stdout);

    for (int i = first_file; i < argc; i++) {
        NexSdkPkgReport report;
        int rc = nexsdk_pkg_check(argv[i], &report);
        NexSdkPkgStatus worst = nexsdk_pkg_worst(&report);

        for (int k = 0; k < report.count; k++) {
            if (report.findings[k].status == NEXSDK_PKG_ERROR) errors++;
            else if (report.findings[k].status == NEXSDK_PKG_WARN) warnings++;
        }

        if (json) {
            if (files_checked) fputs(",\n", stdout);
            fputs("    {\n      \"file\": ", stdout);
            json_string(report.path);
            fputs(",\n      \"package\": ", stdout);
            json_string(report.control.package);
            fputs(",\n      \"version\": ", stdout);
            json_string(report.control.version);
            printf(",\n      \"status\": \"%s\",\n      \"findings\": [\n", json_status(worst));
            for (int k = 0; k < report.count; k++) {
                const NexSdkPkgFinding *f = &report.findings[k];
                fputs("        {\"status\": ", stdout);
                json_string(json_status(f->status));
                fputs(", \"id\": ", stdout);
                json_string(f->id);
                fputs(", \"message\": ", stdout);
                json_string(f->message);
                fputs(", \"hint\": ", stdout);
                json_string(f->hint);
                fputs(k + 1 < report.count ? "},\n" : "}\n", stdout);
            }
            fputs("      ]\n    }", stdout);
        } else {
            if (files_checked) putchar('\n');
            for (int k = 0; k < report.count; k++) {
                const NexSdkPkgFinding *f = &report.findings[k];
                if (quiet && (f->status == NEXSDK_PKG_OK || f->status == NEXSDK_PKG_NOTE)) continue;
                total++;
                printf("%s: [%-7s] %s\n    %s\n", report.path, nexsdk_pkg_status_name(f->status), f->id, f->message);
                if (f->hint[0]) printf("    → %s\n", f->hint);
            }
            if (!quiet) {
                const char *name = report.control.package[0] ? report.control.package : argv[i];
                if (rc != 0)
                    printf("%s: cannot be checked (see above).\n", report.path);
                else if (worst == NEXSDK_PKG_OK)
                    printf("%s: %s %s — ready to publish.\n", report.path, name, report.control.version);
                else if (worst == NEXSDK_PKG_NOTE)
                    printf("%s: %s %s — no problem, some notes above.\n", report.path, name,
                           report.control.version);
                else
                    printf("%s: %s %s — %d finding(s).\n", report.path, name, report.control.version,
                           report.count);
            } else if (worst >= NEXSDK_PKG_WARN) {
                printf("%s: %d finding(s) to fix.\n", report.path, report.count);
            }
        }

        files_checked++;
        if (rc != 0) {
            worst_exit = 3;
        } else if (worst == NEXSDK_PKG_ERROR) {
            if (worst_exit < 2) worst_exit = 2;
        } else if (worst == NEXSDK_PKG_WARN && worst_exit < 1) {
            worst_exit = 1;
        }
    }

    if (json) {
        printf("\n  ],\n  \"packages_checked\": %d,\n  \"errors\": %d,\n  \"warnings\": %d\n}\n", files_checked,
               errors, warnings);
        return worst_exit;
    }
    if (!files_checked) return 3;
    if (!quiet)
        printf("\n%d package(s) checked · %d error(s) · %d warning(s) · %s\n", files_checked, errors, warnings,
               worst_exit == 0 ? "ready to publish"
                               : worst_exit == 1 ? "fix the warnings, or publish knowingly"
                                                 : worst_exit == 2 ? "do not publish yet" : "some files failed");
    if (strict && worst_exit == 1) worst_exit = 2;
    return worst_exit;
}
