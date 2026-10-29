/*
 * tests/nexsdk-test.c — Unit tests for NexSDK (nexsdk/src/):
 *   the parsers of the system files, the argv of a notification (no shell, nothing lost, nothing run),
 *   the settings file (comments kept, one key changed, new groups, atomic writes) and the checks of a .deb.
 *
 * Everything is text in, decisions out: no display, no D-Bus, no package manager to install anything.
 */
#define _POSIX_C_SOURCE 200809L

#include "nexsdk/nexsdk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int passed, failed;

static void check(int cond, const char *msg)
{
    if (cond) {
        printf("PASS: nexsdk: %s\n", msg);
        passed++;
    } else {
        printf("FAIL: nexsdk: %s\n", msg);
        failed++;
    }
}

static void check_str(const char *got, const char *want, const char *msg)
{
    if (got && want && !strcmp(got, want)) {
        printf("PASS: nexsdk: %s\n", msg);
        passed++;
    } else {
        printf("FAIL: nexsdk: %s (got \"%s\", want \"%s\")\n", msg, got ? got : "(null)", want ? want : "(null)");
        failed++;
    }
}

static void check_contains(const char *hay, const char *needle, const char *msg)
{
    if (hay && strstr(hay, needle)) {
        printf("PASS: nexsdk: %s\n", msg);
        passed++;
    } else {
        printf("FAIL: nexsdk: %s (\"%s\" not in \"%s\")\n", msg, needle, hay ? hay : "(null)");
        failed++;
    }
}

static void check_int(int got, int want, const char *msg)
{
    if (got == want) {
        printf("PASS: nexsdk: %s\n", msg);
        passed++;
    } else {
        printf("FAIL: nexsdk: %s (got %d, want %d)\n", msg, got, want);
        failed++;
    }
}

/* ------------------------------------------------------------------ 1. the system */

static void test_system(void)
{
    const char *os_release =
        "NAME=\"Hyggshi OS\"\n"
        "VERSION=\"1.0 (Nex)\"\n"
        "VERSION_ID=\"1.0\"\n"
        "ID=hyggshi\n"
        "ID_LIKE=\"debian ubuntu\"\n"
        "PRETTY_NAME=\"Hyggshi OS 1.0\"\n"
        "HOME_URL=\"https://hyggshi.example/\"\n";
    char value[128];

    check_int(nexsdk_parse_os_release(os_release, "NAME", value, sizeof value), 0, "NAME is read");
    check_str(value, "Hyggshi OS", "the quotes around a value are removed");
    check_int(nexsdk_parse_os_release(os_release, "PRETTY_NAME", value, sizeof value), 0, "PRETTY_NAME is read");
    check_str(value, "Hyggshi OS 1.0", "PRETTY_NAME has the version in it");
    check_int(nexsdk_parse_os_release(os_release, "ID_LIKE", value, sizeof value), 0, "ID_LIKE is read");
    check_str(value, "debian ubuntu", "ID_LIKE keeps its spaces");
    check_int(nexsdk_parse_os_release(os_release, "HOME_URL", value, sizeof value), 0, "HOME_URL is read");
    check_str(value, "https://hyggshi.example/", "a URL keeps its slashes");
    check_int(nexsdk_parse_os_release(os_release, "VERSION_ID", value, sizeof value), 0, "VERSION_ID is read");
    check_str(value, "1.0", "VERSION_ID is the version alone");
    check(nexsdk_parse_os_release(os_release, "CODENAME", value, sizeof value) != 0, "a missing key is not found");
    check_str(value, "", "and leaves the buffer empty");
    check(nexsdk_parse_os_release(os_release, "NAME", value, 4) == 0, "a short buffer is filled as far as it goes");
    check_str(value, "Hyg", "the value is truncated, not overrun");

    const char *meminfo =
        "MemTotal:       16384000 kB\n"
        "MemFree:         2000000 kB\n"
        "MemAvailable:    8000000 kB\n"
        "SwapTotal:             0 kB\n";
    check(nexsdk_parse_meminfo(meminfo, "MemTotal") == 16384000ULL, "MemTotal is read");
    check(nexsdk_parse_meminfo(meminfo, "MemAvailable") == 8000000ULL, "MemAvailable is read");
    check(nexsdk_parse_meminfo(meminfo, "SwapTotal") == 0ULL, "a zero field is zero");
    check(nexsdk_parse_meminfo(meminfo, "MemFree") == 2000000ULL, "MemFree is not confused with MemFreeTotal");
    check(nexsdk_parse_meminfo(meminfo, "HugePages") == 0ULL, "a field that is not there is 0");

    check(nexsdk_parse_uptime("12345.67 45678.90\n") == 12345L, "uptime is read from /proc/uptime");
    check(nexsdk_parse_uptime("") == 0L, "an empty uptime is 0");

    char version[64];
    check_int(nexsdk_parse_hde_version("1.0 (4cf724f 2026-10-09)\n", version, sizeof version), 0, "the HDE version");
    check_str(version, "1.0 (4cf724f 2026-10-09)", "the newline is not part of it");
    check_int(nexsdk_parse_hde_version("\n", version, sizeof version), -1, "an empty version file is not a version");

    check_int(nexsdk_session_from_string("x11"), NEXSDK_SESSION_X11, "x11 is an X11 session");
    check_int(nexsdk_session_from_string("wayland"), NEXSDK_SESSION_WAYLAND, "wayland is a Wayland session");
    check_int(nexsdk_session_from_string("tty"), NEXSDK_SESSION_TTY, "tty is a text console");
    check_int(nexsdk_session_from_string("something-new"), NEXSDK_SESSION_UNKNOWN, "an unknown session is unknown");
    check_str(nexsdk_session_name(NEXSDK_SESSION_WAYLAND), "wayland", "the name of a session type");

    NexSdkSystem s;
    nexsdk_system_init(&s);
    check(s.os_name[0] == '\0' && s.mem_total_kb == 0, "init empties every field");
    check_int(nexsdk_system_read(&s), 0, "this machine can be read");
    check(s.kernel[0] != '\0', "the kernel is read");

    NexSdkSystem fake;
    nexsdk_system_init(&fake);
    snprintf(fake.os_pretty, sizeof fake.os_pretty, "Hyggshi OS 1.0");
    snprintf(fake.kernel, sizeof fake.kernel, "6.12.38");
    fake.session = NEXSDK_SESSION_WAYLAND;
    snprintf(fake.desktop, sizeof fake.desktop, "HDE");
    char *summary = nexsdk_system_summary(&fake);
    check_str(summary, "Hyggshi OS 1.0 · Linux 6.12.38 · wayland · HDE", "the summary of a system");
    free(summary);

    char *report = nexsdk_system_report(&fake);
    check_contains(report, "Hyggshi OS 1.0", "the report names the system");
    check_contains(report, "Session  : wayland", "the report names the session");
    check_contains(report, "HDE      : not installed", "an empty HDE version is said to be not installed");
    free(report);
    check(nexsdk_system_summary(NULL) == NULL, "a NULL system has no summary");
}

/* ------------------------------------------------------------------ 2. the notification */

static void test_notify(void)
{
    NexSdkNotification n = NEXSDK_NOTIFICATION_INIT;
    char buf[NEXSDK_NOTIFY_ARGV_SIZE];
    char *argv[32];
    int argc;

    /* nothing to say: no notification, not a notification with an empty summary */
    argc = nexsdk_notify_argv(&n, buf, sizeof buf, argv, 32);
    check_int(argc, -1, "a notification without a summary is refused");

    n.summary = "Backup finished";
    argc = nexsdk_notify_argv(&n, buf, sizeof buf, argv, 32);
    check_int(argc, 3, "the shortest notification is notify-send -- SUMMARY");
    check_str(argv[0], "notify-send", "the program is notify-send");
    check_str(argv[1], "--", "the arguments after -- are text, never options");
    check_str(argv[2], "Backup finished", "the summary arrives unchanged");
    check(argv[3] == NULL, "the argv ends with NULL");

    n.body = "1,204 files copied";
    n.icon = "drive-harddisk-symbolic";
    n.app_name = "Hyggshi Backup";
    n.urgency = NEXSDK_URGENCY_CRITICAL;
    n.timeout_ms = 8000;
    n.replace_id = "backup-42";
    argc = nexsdk_notify_argv(&n, buf, sizeof buf, argv, 32);
    check(argc > 3, "a full notification has more arguments");
    int found_summary = 0, found_urgency = 0, found_icon = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "Backup finished")) found_summary = 1;
        if (!strcmp(argv[i], "critical")) found_urgency = 1;
        if (!strcmp(argv[i], "drive-harddisk-symbolic")) found_icon = 1;
        check_int(argv[i][0] == '-' && i + 1 < argc ? 1 : 0, argv[i][0] == '-' && i + 1 < argc ? 1 : 0,
                  "an option is followed by its value");
    }
    check(found_summary, "the summary is there with every option in use");
    check(found_urgency, "the urgency is passed");
    check(found_icon, "the icon is passed");

    /* the reason this function exists: a summary is data, not a command */
    NexSdkNotification evil = NEXSDK_NOTIFICATION_INIT;
    evil.summary = "It's fine; rm -rf ~/ -- $(whoami) `id`";
    evil.body = "\"; touch /tmp/should-not-exist; \"";
    argc = nexsdk_notify_argv(&evil, buf, sizeof buf, argv, 32);
    check_int(argc, 4, "the summary and the body are two arguments");
    check_str(argv[2], evil.summary, "the summary arrives exactly as it was written");
    check_str(argv[3], evil.body, "the body arrives exactly as it was written");
    check(access("/tmp/should-not-exist", F_OK) != 0, "nothing was executed");

    evil.summary = "-dangerous";
    argc = nexsdk_notify_argv(&evil, buf, sizeof buf, argv, 32);
    check_int(argc, 4, "a summary that starts with a dash is still accepted");
    check_str(argv[2], "-dangerous", "and is still the summary, after --");

    char tiny[16];
    evil.summary = "a summary that does not fit into a small buffer at all";
    check_int(nexsdk_notify_argv(&evil, tiny, sizeof tiny, argv, 32), -1, "a buffer that is too small is refused");

    evil.summary = "x";
    check_int(nexsdk_notify_argv(&evil, buf, sizeof buf, argv, 3), -1, "too few argv slots are refused");

    check_str(nexsdk_urgency_name(NEXSDK_URGENCY_LOW), "low", "the name of an urgency");
    check_str(nexsdk_urgency_name(NEXSDK_URGENCY_CRITICAL), "critical", "the name of a critical urgency");
    check_int(nexsdk_notify_available(), 0, "a sandbox without a session bus cannot notify");
}

/* ------------------------------------------------------------------ 3. the settings */

static void write_file(const char *path, const char *text)
{
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    fputs(text, fp);
    fclose(fp);
}

static char *read_file(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return NULL;
    char *buf = malloc(8192);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    size_t n = fread(buf, 1, 8191, fp);
    fclose(fp);
    buf[n] = '\0';
    return buf;
}

static void test_settings(void)
{
    char dir[] = "/tmp/nexsdk-test-XXXXXX";
    if (!mkdtemp(dir)) {
        printf("FAIL: nexsdk: cannot make a temporary directory\n");
        failed++;
        return;
    }
    setenv("XDG_CONFIG_HOME", dir, 1);
    char *expected = malloc(strlen(dir) + sizeof "/hde/settings.ini");
    sprintf(expected, "%s/hde/settings.ini", dir);
    char *path = nexsdk_settings_path();
    check_str(path, expected, "the settings live in $XDG_CONFIG_HOME/hde/settings.ini");
    char conf_dir[600];
    snprintf(conf_dir, sizeof conf_dir, "%s/hde", dir);
    mkdir(conf_dir, 0755);

    /* a file with comments, spacing, an inline comment and two groups */
    const char *ini =
        "# HDE settings — hde-settings writes this file\n"
        "\n"
        "[settings]\n"
        "theme_index=2          # 1 = Light, 2 = Dark\n"
        "accent = \"#9141ac\"\n"
        "animations=true\n"
        "\n"
        "[panel]\n"
        "position=bottom\n"
        "height=40\n";
    write_file(path, ini);

    NexSdkSettings *s = nexsdk_settings_open();
    check(s != NULL, "the settings file opens");
    check_str(nexsdk_settings_get(s, "settings", "theme_index", "0"), "2", "a value with an inline comment");
    check_str(nexsdk_settings_get(s, "settings", "accent", "?"), "#9141ac", "the quotes are not part of the value");
    check_int(nexsdk_settings_get_int(s, "settings", "theme_index", 0), 2, "an integer is an integer");
    check_int(nexsdk_settings_get_bool(s, "settings", "animations", 0), 1, "true is true");
    check_int(nexsdk_settings_get_int(s, "panel", "height", 0), 40, "a key of another group");
    check_str(nexsdk_settings_get(s, "panel", "theme_index", "none"), "none", "a key is not found in another group");
    check_int(nexsdk_settings_get_bool(s, "settings", "missing", 0), 0, "a missing boolean falls back to false");
    check_int(nexsdk_settings_get_int(s, "settings", "missing", 7), 7, "a missing integer falls back");
    check_int(nexsdk_settings_dirty(s), 0, "nothing has changed yet");

    check_int(nexsdk_settings_set(s, "settings", "theme_index", "1"), 0, "a value can be changed");
    check_int(nexsdk_settings_dirty(s), 1, "changing a value makes the file dirty");
    check_int(nexsdk_settings_set(s, "settings", "theme_index", "1"), 0, "setting the same value again is fine");
    check_int(nexsdk_settings_set(s, "panel", "autohide", "true"), 0, "a key can be added to an existing group");
    check_int(nexsdk_settings_set(s, "myapp", "last_folder", "/home/me/Pictures"), 0, "a group can be added");
    check_int(nexsdk_settings_set_int(s, "myapp", "count", 3), 0, "an integer can be written");
    check_int(nexsdk_settings_set_bool(s, "myapp", "enabled", 1), 0, "a boolean is written as true");
    check_int(nexsdk_settings_remove(s, "settings", "animations"), 1, "a key can be removed");
    check_int(nexsdk_settings_remove(s, "settings", "animations"), 0, "removing it twice reports nothing");

    char *text = nexsdk_settings_to_text(s);
    check_contains(text, "# HDE settings — hde-settings writes this file", "the comment at the top survives");
    check_contains(text, "theme_index=1", "the changed key has its new value");
    check_contains(text, "accent = \"#9141ac\"", "an unchanged line is untouched, spacing and quotes included");
    check_contains(text, "autohide=true", "the new key of the existing group");
    check_contains(text, "height=40", "the rest of the group is still there");
    check_contains(text, "[myapp]", "the new group");
    check_contains(text, "last_folder=/home/me/Pictures", "the new key of the new group");
    check(strstr(text, "animations") == NULL, "the removed key is gone");
    check(strstr(text, "[panel]\nposition=bottom\nheight=40\nautohide=true") != NULL,
          "the new key lands at the end of its group, not after the next one");
    free(text);

    check_int(nexsdk_settings_save(s), 0, "the file is written");
    check_int(nexsdk_settings_dirty(s), 0, "and is no longer dirty");
    char *on_disk = read_file(path);
    check_contains(on_disk, "theme_index=1", "the value reached the disk");
    check_contains(on_disk, "# HDE settings", "so did the comment");
    free(on_disk);
    check(access(path, F_OK) == 0, "the file exists");
    char *leftover = malloc(strlen(path) + 8);
    sprintf(leftover, "%s.new", path);
    check(access(leftover, F_OK) != 0, "no temporary file is left behind");
    free(leftover);

    /* a second program reads what the first wrote */
    NexSdkSettings *again = nexsdk_settings_open();
    check_str(nexsdk_settings_get(again, "myapp", "last_folder", ""), "/home/me/Pictures",
              "another reader sees the new key");
    check_str(nexsdk_settings_get(again, "settings", "theme_index", ""), "1", "and the changed one");
    nexsdk_settings_free(again);

    /* free() writes what was not saved: a program cannot lose a setting by forgetting save() */
    nexsdk_settings_set(s, "settings", "font", "Sans 10");
    nexsdk_settings_free(s);
    NexSdkSettings *third = nexsdk_settings_open();
    check_str(nexsdk_settings_get(third, "settings", "font", ""), "Sans 10", "free() saved the pending change");
    nexsdk_settings_free(third);

    /* the file does not exist: a new one is made when something is written */
    char fresh[512];
    snprintf(fresh, sizeof fresh, "%s/hde/settings-new.ini", dir);
    NexSdkSettings *empty = nexsdk_settings_open_path(fresh);
    check(empty != NULL, "a settings file that does not exist yet can be opened");
    check_str(nexsdk_settings_get(empty, "settings", "theme_index", "0"), "0", "and is empty");
    nexsdk_settings_free(empty);
    check(access(fresh, F_OK) != 0, "nothing is written when nothing was changed");
    empty = nexsdk_settings_open_path(fresh);
    nexsdk_settings_set(empty, "settings", "theme_index", "2");
    nexsdk_settings_free(empty);
    char *fresh_text = read_file(fresh);
    check_contains(fresh_text, "[settings]\ntheme_index=2", "a new file gets its group and its key");
    free(fresh_text);

    /* broken input is not a crash */
    NexSdkSettings *broken = nexsdk_settings_open_path(fresh);
    check_int(nexsdk_settings_parse(broken, "[settings\nkey=value\n[panel]\n"), 0, "an unclosed group line is kept as text");
    check_str(nexsdk_settings_get(broken, "", "key", ""), "value", "a key before any group has no group");
    nexsdk_settings_free(broken);

    unlink(fresh);
    unlink(path);
    snprintf(fresh, sizeof fresh, "%s/hde", dir);
    rmdir(fresh);
    rmdir(dir);
    free(expected);
    free(path);
}

/* ------------------------------------------------------------------ 4. the .deb */

static int path_in_list(const char *path, void *data)
{
    const char *list = data;
    return list && strstr(list, path) != NULL;
}

static void test_pkg(void)
{
    const char *control =
        "Package: hyggshi-hello\n"
        "Version: 1.2-1\n"
        "Architecture: amd64\n"
        "Maintainer: A Person <person@example.com>\n"
        "Installed-Size: 128\n"
        "Section: utils\n"
        "Priority: optional\n"
        "Homepage: https://github.com/Hyggshi-OS-Research-Technology/NexWM\n"
        "Depends: libgtk-3-0 (>= 3.24), libc6 (>= 2.34)\n"
        "Description: Hello for Hyggshi OS\n"
        " A small program that says hello, used as the example of the templates.\n"
        " .\n"
        " It uses NexSDK for the notification.\n";
    NexSdkDebControl c;
    check_int(nexsdk_pkg_parse_control(control, &c), 0, "a good control block parses");
    check_str(c.package, "hyggshi-hello", "the package name");
    check_str(c.version, "1.2-1", "the version");
    check_str(c.architecture, "amd64", "the architecture");
    check_str(c.maintainer, "A Person <person@example.com>", "the maintainer");
    check_contains(c.description, "Hello for Hyggshi OS", "the description starts with its synopsis");
    check_contains(c.description, "A small program that says hello", "and keeps the long text");
    check(strchr(c.description, '\n') != NULL, "the long text is on lines of its own");
    check_contains(c.depends, "libgtk-3-0", "the dependencies are read");
    check_int(c.installed_kb, 128, "Installed-Size is a number");

    NexSdkPkgReport r;
    memset(&r, 0, sizeof r);
    nexsdk_pkg_scan_control(&c, &r);
    check_int(nexsdk_pkg_worst(&r), NEXSDK_PKG_OK, "a control block that follows the rules has nothing to report");

    NexSdkDebControl bad;
    check_int(nexsdk_pkg_parse_control(
                  "Package: BadName\nVersion: v1.0\nArchitecture: amd64\nMaintainer: nobody\n"
                  "Description: one line\n",
                  &bad),
              0, "every field of the control block is read, however wrong its value is");
    NexSdkPkgReport rb;
    memset(&rb, 0, sizeof rb);
    nexsdk_pkg_scan_control(&bad, &rb);
    check_int(nexsdk_pkg_worst(&rb), NEXSDK_PKG_ERROR, "the mistakes make it an error");
    int has_name = 0, has_version = 0, has_maintainer = 0, has_desc = 0;
    for (int i = 0; i < rb.count; i++) {
        if (!strcmp(rb.findings[i].id, "control.package")) has_name = 1;
        if (!strcmp(rb.findings[i].id, "control.version")) has_version = 1;
        if (!strcmp(rb.findings[i].id, "control.maintainer-email")) has_maintainer = 1;
        if (!strcmp(rb.findings[i].id, "control.description-long")) has_desc = 1;
    }
    check(has_name, "an uppercase name is reported");
    check(has_version, "a version that does not start with a digit is reported");
    check(has_maintainer, "a maintainer without an address is reported");
    check(has_desc, "a one-line description is reported");

    const char *listing =
        "drwxr-xr-x root/root         0 2026-01-01 10:00 ./\n"
        "drwxr-xr-x root/root         0 2026-01-01 10:00 ./usr/\n"
        "drwxr-xr-x root/root         0 2026-01-01 10:00 ./usr/bin/\n"
        "-rwxr-xr-x root/root     45000 2026-01-01 10:00 ./usr/bin/hyggshi-hello\n"
        "drwxr-xr-x root/root         0 2026-01-01 10:00 ./usr/share/\n"
        "drwxr-xr-x root/root         0 2026-01-01 10:00 ./usr/share/applications/\n"
        "-rw-r--r-- root/root       412 2026-01-01 10:00 ./usr/share/applications/hyggshi-hello.desktop\n"
        "drwxr-xr-x root/root         0 2026-01-01 10:00 ./usr/share/icons/\n"
        "drwxr-xr-x root/root         0 2026-01-01 10:00 ./usr/share/icons/hicolor/\n"
        "drwxr-xr-x root/root         0 2026-01-01 10:00 ./usr/share/icons/hicolor/scalable/\n"
        "drwxr-xr-x root/root         0 2026-01-01 10:00 ./usr/share/icons/hicolor/scalable/apps/\n"
        "-rw-r--r-- root/root      1200 2026-01-01 10:00 ./usr/share/icons/hicolor/scalable/apps/hyggshi-hello.svg\n"
        "drwxr-xr-x root/root         0 2026-01-01 10:00 ./usr/share/doc/\n"
        "drwxr-xr-x root/root         0 2026-01-01 10:00 ./usr/share/doc/hyggshi-hello/\n"
        "-rw-r--r-- root/root      1000 2026-01-01 10:00 ./usr/share/doc/hyggshi-hello/copyright\n"
        "lrwxrwxrwx root/root         0 2026-01-01 10:00 ./usr/bin/hello -> hyggshi-hello\n";
    NexSdkPkgReport rf;
    memset(&rf, 0, sizeof rf);
    check_int(nexsdk_pkg_scan_files(listing, &c, &rf), 16, "every file of the listing is counted");
    check_int(nexsdk_pkg_worst(&rf), NEXSDK_PKG_NOTE, "a good package only has the note about its .desktop file");
    int notes = 0;
    for (int i = 0; i < rf.count; i++) if (rf.findings[i].status == NEXSDK_PKG_NOTE) notes++;
    check_int(notes, 1, "one note, for the .desktop file");
    check_contains(rf.findings[0].message, "1 .desktop", "the note says how many .desktop files there are");

    const char *bad_listing =
        "-rwsr-xr-x root/root     45000 2026-01-01 10:00 ./usr/local/bin/thing\n"
        "-rw-r--r-- root/root       100 2026-01-01 10:00 ./usr/bin/readme\n";
    NexSdkPkgReport rg;
    memset(&rg, 0, sizeof rg);
    nexsdk_pkg_scan_files(bad_listing, &c, &rg);
    int usr_local = 0, setuid = 0, no_copyright = 0;
    for (int i = 0; i < rg.count; i++) {
        if (!strcmp(rg.findings[i].id, "files.usr-local")) usr_local = 1;
        if (!strcmp(rg.findings[i].id, "files.setuid")) setuid = 1;
        if (!strcmp(rg.findings[i].id, "files.no-copyright")) no_copyright = 1;
    }
    check(usr_local, "/usr/local is reported");
    check(setuid, "a setuid file is reported");
    check(no_copyright, "a missing copyright file is reported");
    check_int(nexsdk_pkg_worst(&rg), NEXSDK_PKG_ERROR, "a package that installs into /usr/local is broken");

    /* the .desktop file, checked against what the package installs */
    const char *desktop =
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=Hyggshi Hello\n"
        "Exec=hyggshi-hello %U\n"
        "Icon=hyggshi-hello\n"
        "Categories=Utility;\n"
        "\n"
        "[Desktop Action new-window]\n"
        "Name=New window\n"
        "Exec=/usr/bin/does-not-exist\n";
    char value[256];
    check_int(nexsdk_pkg_parse_desktop_key(desktop, "Exec", value, sizeof value), 0, "Exec is read");
    check_str(value, "hyggshi-hello %U", "the whole Exec line is read");
    check_int(nexsdk_pkg_parse_desktop_key(desktop, "Name", value, sizeof value), 0, "Name is read");
    check_str(value, "Hyggshi Hello", "a Name of a Desktop Action does not win");
    check(nexsdk_pkg_parse_desktop_key(desktop, "NoDisplay", value, sizeof value) != 0, "a missing key is not found");

    NexSdkPkgReport rd;
    memset(&rd, 0, sizeof rd);
    nexsdk_pkg_scan_desktop(desktop, "/usr/share/applications/hyggshi-hello.desktop", path_in_list, (void *)listing,
                            &rd);
    check_int(rd.count, 0, "a .desktop file that everything is in place for has nothing to report");

    const char *broken_desktop =
        "[Desktop Entry]\n"
        "Name=Broken\n"
        "Exec=/usr/bin/not-installed\n"
        "Icon=nothing-here\n";
    NexSdkPkgReport re;
    memset(&re, 0, sizeof re);
    nexsdk_pkg_scan_desktop(broken_desktop, "/usr/share/applications/broken.desktop", path_in_list, (void *)listing,
                            &re);
    int no_type = 0, exec_missing = 0, icon_missing = 0;
    for (int i = 0; i < re.count; i++) {
        if (!strcmp(re.findings[i].id, "desktop.no-type")) no_type = 1;
        if (!strcmp(re.findings[i].id, "desktop.exec-missing")) exec_missing = 1;
        if (!strcmp(re.findings[i].id, "desktop.icon-missing")) icon_missing = 1;
    }
    check(no_type, "a .desktop file without Type is reported");
    check(exec_missing, "an Exec that is not in the package is reported");
    check(icon_missing, "an icon that is not in the package is reported");
    check_int(nexsdk_pkg_worst(&re), NEXSDK_PKG_ERROR, "a .desktop file that starts nothing is an error");

    NexSdkPkgReport rn;
    memset(&rn, 0, sizeof rn);
    nexsdk_pkg_scan_desktop("[Desktop Entry]\nType=Application\nName=No Exec\n", "/x.desktop", NULL, NULL, &rn);
    check_int(rn.count, 1, "a .desktop file without Exec is one finding");
    check_str(rn.findings[0].id, "desktop.no-exec", "and it says the Exec is missing");

    check_str(nexsdk_pkg_status_name(NEXSDK_PKG_WARN), "WARNING", "the name of a warning");
    check_int(nexsdk_pkg_worst(NULL), NEXSDK_PKG_OK, "an empty report is fine");

    NexSdkPkgReport rt;
    memset(&rt, 0, sizeof rt);
    snprintf(rt.path, sizeof rt.path, "/tmp/hyggshi-hello_1.2-1_amd64.deb");
    rt.control = c;
    rt.files = 16;
    nexsdk_pkg_add(&rt, NEXSDK_PKG_WARN, "files.setuid", "The package has a setuid file.", "Use pkexec.");
    char *text = nexsdk_pkg_report_text(&rt);
    check_contains(text, "hyggshi-hello 1.2-1", "the report names the package");
    check_contains(text, "WARNING", "the report has the warning");
    check_contains(text, "→ Use pkexec.", "the report has the hint");
    check_contains(text, "1 finding(s)", "the report counts the findings");
    free(text);

    /* a real file, when this machine has dpkg-deb: not a package, and not even a file */
    check_int(nexsdk_pkg_check("/nonexistent/file.deb", &rt), -1, "a file that is not there cannot be checked");
    check_int(nexsdk_pkg_worst(&rt), NEXSDK_PKG_ERROR, "and is an error");
    char junk[] = "/tmp/nexsdk-junk-XXXXXX";
    int fd = mkstemp(junk);
    if (fd >= 0) {
        write(fd, "this is not a package\n", 22);
        close(fd);
        NexSdkPkgReport rj;
        check_int(nexsdk_pkg_check(junk, &rj), -1, "a file that is not an archive is refused");
        check_str(rj.findings[0].id, "file.not-a-deb", "and is said not to be a Debian package");
        check_contains(rj.findings[0].hint, "dpkg-deb", "with the command that makes one");
        unlink(junk);
    }
}

int main(void)
{
    test_system();
    test_notify();
    test_settings();
    test_pkg();

    printf("\nnexsdk-test: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
