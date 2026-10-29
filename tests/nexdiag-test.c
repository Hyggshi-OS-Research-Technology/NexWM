/*
 * tests/nexdiag-test.c — Unit tests for NexDiag (nexdiag/nexdiag-core.c):
 * the checks bookkeeping, the redaction of everything private, the parsers of real tool output and the
 * text of the report.  No display, no systemd, no package manager.
 */
#define _POSIX_C_SOURCE 200809L

#include "nexdiag-core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int passed = 0;
static int failed = 0;

static void check(int cond, const char *msg)
{
    if (cond) {
        printf("PASS: nexdiag: %s\n", msg);
        passed++;
    } else {
        printf("FAIL: nexdiag: %s\n", msg);
        failed++;
    }
}

static void check_int(int got, int want, const char *msg)
{
    if (got == want) {
        printf("PASS: nexdiag: %s\n", msg);
        passed++;
    } else {
        printf("FAIL: nexdiag: %s (got %d, want %d)\n", msg, got, want);
        failed++;
    }
}

static void check_str(const char *got, const char *want, const char *msg)
{
    if (got && want && !strcmp(got, want)) {
        printf("PASS: nexdiag: %s\n", msg);
        passed++;
    } else {
        printf("FAIL: nexdiag: %s (got \"%s\", want \"%s\")\n", msg, got ? got : "(null)", want);
        failed++;
    }
}

static void check_contains(const char *hay, const char *needle, const char *msg)
{
    if (hay && strstr(hay, needle)) {
        printf("PASS: nexdiag: %s\n", msg);
        passed++;
    } else {
        printf("FAIL: nexdiag: %s (\"%s\" not in \"%s\")\n", msg, needle, hay ? hay : "(null)");
        failed++;
    }
}

static void check_not_contains(const char *hay, const char *needle, const char *msg)
{
    if (hay && !strstr(hay, needle)) {
        printf("PASS: nexdiag: %s\n", msg);
        passed++;
    } else {
        printf("FAIL: nexdiag: %s (\"%s\" is still in \"%s\")\n", msg, needle, hay ? hay : "(null)");
        failed++;
    }
}

static void identity(NexdiagIdentity *id)
{
    memset(id, 0, sizeof *id);
    strcpy(id->user, "tester");
    strcpy(id->home, "/home/tester");
    strcpy(id->host, "hyggshi-pc");
    strcpy(id->os, "Hyggshi OS 1.0");
    strcpy(id->kernel, "Linux 6.12.38 x86_64");
    strcpy(id->session, "X11 (DISPLAY=:0)");
    strcpy(id->wm, "NexWM");
    strcpy(id->desktop, "HDE");
    strcpy(id->hde, "1.0 (4cf724f 2026-10-09)");
}

int main(void)
{
    /* ---- 1. the bookkeeping of a report ---- */
    NexdiagReport r;
    nexdiag_report_init(&r);
    check_int(nexdiag_report_add(&r, "session.wm", "Session", "Window manager", NEXDIAG_OK, "NexWM", ""), 0,
              "the first check is at index 0");
    nexdiag_report_add(&r, "packages.audit", "Packages", "Package database", NEXDIAG_FAIL, "half-installed",
                       "run dpkg --configure -a");
    nexdiag_report_add(&r, "display.screens", "Display", "Screens", NEXDIAG_SKIP, "Wayland", "");
    nexdiag_report_add(&r, "config.disk", "Configuration", "Free space", NEXDIAG_WARN, "300 MB", "free some");
    check_int(r.count, 4, "four checks were added");
    check_int(nexdiag_report_count(&r, NEXDIAG_OK), 1, "one OK");
    check_int(nexdiag_report_count(&r, NEXDIAG_FAIL), 1, "one FAILED");
    check_int(nexdiag_report_count(&r, NEXDIAG_SKIP), 1, "one SKIPPED");
    check_int(nexdiag_report_worst(&r), NEXDIAG_FAIL, "the worst state is FAILED");
    check_str(nexdiag_status_name(NEXDIAG_WARN), "WARNING", "the name of a warning");
    check_str(nexdiag_status_title(NEXDIAG_FAIL), "Broken", "the title of a failure");
    check_str(nexdiag_status_icon(NEXDIAG_OK), "emblem-ok-symbolic", "the icon of an OK check");

    NexdiagReport empty;
    nexdiag_report_init(&empty);
    check_int(nexdiag_report_worst(&empty), NEXDIAG_OK, "an empty report is not a failure");

    /* ---- 2. the redaction: nothing private leaves the machine ---- */
    NexdiagIdentity id;
    identity(&id);

    char *red = nexdiag_redact_with("/home/tester/.config/hde/settings.ini belongs to tester on hyggshi-pc", &id);
    check_str(red, "<home>/.config/hde/settings.ini belongs to <user> on <host>",
              "the home directory, the user name and the host name are hidden");
    free(red);

    red = nexdiag_redact_with("inet 192.168.1.10/24 brd 192.168.1.255", &id);
    check_str(red, "inet <ip>/24 brd <ip>", "IPv4 addresses are hidden");
    free(red);

    red = nexdiag_redact_with("server 2a00:1450:4001:82f::200e answered", &id);
    check_str(red, "server <ip> answered", "IPv6 addresses are hidden");
    free(red);

    red = nexdiag_redact_with("link/ether a4:5e:60:2b:11:7c brd ff:ff:ff:ff:ff:ff", &id);
    check_contains(red, "<mac>", "MAC addresses are hidden");
    check_not_contains(red, "a4:5e:60", "the MAC address itself is gone");
    free(red);

    red = nexdiag_redact_with("From: hyggshi.dev@example.com to support@hyggshi.org", &id);
    check_str(red, "From: <email> to <email>", "e-mail addresses are hidden");
    free(red);

    red = nexdiag_redact_with("uuid 3f2504e0-4f89-11d3-9a0c-0305e82c3301", &id);
    check_str(red, "uuid <uuid>", "UUIDs are hidden");
    free(red);

    red = nexdiag_redact_with("password=hunter2 token: abc123 secret = s3cr3t", &id);
    check_str(red, "password=<redacted> token: <redacted> secret = <redacted>",
              "passwords, tokens and secrets are hidden");
    free(red);

    red = nexdiag_redact_with("Authorization: Bearer eyJhbGciOiJIUzI1NiJ9", &id);
    check_contains(red, "<redacted>", "a bearer token is hidden");
    check_not_contains(red, "eyJhbGci", "the token itself is gone");
    free(red);

    red = nexdiag_redact_with("Version: 1.2.3.4 and passwordless login", &id);
    check_contains(red, "1.2.3.4", "a version that looks like an address is kept");
    check_contains(red, "passwordless", "a word that merely starts with 'password' is kept");
    free(red);

    red = nexdiag_redact_with(NULL, &id);
    check_str(red, "", "redacting nothing is safe");
    free(red);

    char buf[128];
    strcpy(buf, "tester ran this on hyggshi-pc");
    nexdiag_redact_buffer(buf, &id);
    check_str(buf, "<user> ran this on <host>", "the in-place redaction works too");

    /* ---- 3. the parsers ---- */
    const char *ldd_output =
        "\tlinux-vdso.so.1 (0x00007ffd4b7f5000)\n"
        "\tlibgtk-3.so.0 => /usr/lib/x86_64-linux-gnu/libgtk-3.so.0 (0x00007f1c2a000000)\n"
        "\tlibgdk-3.so.0 => not found\n"
        "\tlibpango-1.0.so.0 => not found\n"
        "\tlibgdk-3.so.0 => not found\n";
    char missing[256];
    check_int(nexdiag_parse_ldd_missing(ldd_output, missing, sizeof missing), 2,
              "two libraries are missing, the repeated one counted once");
    check_str(missing, "libgdk-3.so.0, libpango-1.0.so.0", "the missing libraries are named");
    check_int(nexdiag_parse_ldd_missing("libgtk-3.so.0 => /usr/lib/libgtk-3.so.0\n", missing, sizeof missing), 0,
              "a program with all of its libraries is fine");

    const char *systemctl_output =
        "hde-panel.service loaded failed failed Hyggshi Desktop Panel\n"
        "bluetooth.service loaded failed failed Bluetooth service\n";
    char units[256];
    check_int(nexdiag_parse_failed_units(systemctl_output, units, sizeof units), 2, "two units failed");
    check_str(units, "hde-panel.service, bluetooth.service", "the failed units are named");
    check_int(nexdiag_parse_failed_units("", units, sizeof units), 0, "no failed units is no problem");

    char state[64];
    check_int(nexdiag_parse_nmcli_state("connected\n", state, sizeof state), 1, "reads the NetworkManager state");
    check_str(state, "connected", "the state is 'connected'");
    check_int(nexdiag_parse_nmcli_state("", state, sizeof state), 0, "an empty answer is no state");

    const char *log =
        "hde-panel: Error: cannot open display\n"
        "hde-panel: panel ready\n"
        "hde-settings: warning: no icon theme\n"
        "nexwm: segmentation fault at 0x0\n";
    check_int(nexdiag_count_errors(log), 2, "two lines are errors");
    check_int(nexdiag_count_warnings(log), 3, "the warnings, the errors and the failure count as warnings too");

    char first[256];
    check_int(nexdiag_parse_dpkg_audit("", first, sizeof first), 0, "dpkg --audit with no output is healthy");
    check_int(nexdiag_parse_dpkg_audit(
                  "The following packages have been unpacked but not yet configured:\n  hde-weather\n",
                  first, sizeof first),
              1, "dpkg --audit with output is a problem");
    check_str(first, "The following packages have been unpacked but not yet configured:",
              "the first complaint is kept");

    check_int(nexdiag_check_ini("[settings]\ntheme_index=2\nfont = Sans 10\n# a comment\n", first, sizeof first),
              0, "a well formed settings.ini");
    check_int(nexdiag_check_ini("[settings]\nthis line has no equals sign\n", first, sizeof first), 1,
              "a broken line is reported");
    check_contains(first, "line 2", "the line number of the problem is given");
    check_int(nexdiag_check_ini("[settings\n", first, sizeof first), 1, "a section that is never closed");

    const char *meminfo = "MemTotal:       16384000 kB\nMemFree:         2000000 kB\n"
                          "MemAvailable:    8000000 kB\n";
    check(nexdiag_parse_meminfo_kb(meminfo, "MemTotal") == 16384000ULL, "MemTotal is read from /proc/meminfo");
    check(nexdiag_parse_meminfo_kb(meminfo, "MemAvailable") == 8000000ULL, "MemAvailable is read");
    check(nexdiag_parse_meminfo_kb(meminfo, "SwapTotal") == 0ULL, "a field that is not there is 0");

    /* ---- 4. the report ---- */
    nexdiag_report_add(&r, "session.lock", "Session", "Lock screen", NEXDIAG_WARN, "cannot lock",
                       "install the PAM support");
    r.log_excerpt = strdup("hde-panel: Error: cannot open display\n");
    char *text = nexdiag_report_text(&r, &id, 1, 1);
    check_contains(text, "Hyggshi NexDiag report", "the report has a title");
    check_contains(text, "System    : Hyggshi OS 1.0", "the report names the system");
    check_contains(text, "Redaction : on", "the report says that it is redacted");
    check_contains(text, "FAILED", "a failure is in the report");
    check_contains(text, "[WARNING ]", "a warning is marked");
    check_contains(text, "→ install the PAM support", "the hint of a warning is in the report");
    check_contains(text, "Packages", "the groups are in the report");
    check_contains(text, "Last error lines of the HDE session log", "the log excerpt is there");
    check_contains(text, "<user>", "the user name is redacted in the report");
    check_not_contains(text, "tester", "the user name itself is not in the report");
    check_not_contains(text, "hyggshi-pc", "the host name is not in the report");
    free(text);

    text = nexdiag_report_text(&r, &id, 0, 0);
    check_contains(text, "Redaction : OFF", "a report without redaction says so");
    check_contains(text, "tester", "and then it does contain the user name");
    check_not_contains(text, "Last error lines", "without logs there is no log section");
    free(text);

    char *path = nexdiag_report_default_path();
    check_contains(path, "nexdiag-report-", "the default report path has the name of the tool");
    check_contains(path, ".txt", "the default report is a text file");
    free(path);

    nexdiag_report_free(&r);
    check_int(r.count, 0, "freeing the report empties it");
    nexdiag_report_free(&empty);

    printf("\nnexdiag-test: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
