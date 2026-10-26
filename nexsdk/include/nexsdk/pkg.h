/* nexsdk/pkg.h — checking a .deb before it is published (the engine behind hde-pkg-check).
 *
 * A Hyggshi OS application is a Debian package.  The mistakes that make one unusable are always the same:
 * a missing Depends, a description that is one line, a .desktop file whose Exec names a program that is not
 * in the package, an icon that is a path to nothing, a /usr/local path in an install into /usr, setuid bits
 * that nobody wanted, or a maintainer script that does something surprising.  This finds those, and it says
 * what to do about each one.
 *
 * The parsers (`nexsdk_pkg_parse_control`, `nexsdk_pkg_scan_files`) take text, so they can be tested without
 * dpkg; `nexsdk_pkg_check()` is what runs `dpkg-deb -f` and `dpkg-deb -c` for a real file.
 */
#ifndef NEXSDK_PKG_H
#define NEXSDK_PKG_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char package[128];
    char version[128];
    char architecture[64];
    char maintainer[256];
    char section[64];
    char priority[32];
    char description[512];     /* the synopsis: the first line */
    char homepage[256];
    char depends[1024];
    int installed_kb;
} NexSdkDebControl;

typedef enum {
    NEXSDK_PKG_OK = 0,         /* nothing wrong */
    NEXSDK_PKG_NOTE,           /* worth knowing, not wrong */
    NEXSDK_PKG_WARN,           /* should be fixed before publishing */
    NEXSDK_PKG_ERROR           /* the package is broken */
} NexSdkPkgStatus;

typedef struct {
    NexSdkPkgStatus status;
    char id[48];               /* e.g. "control.missing-depends" */
    char message[512];
    char hint[512];            /* what to do; "" when there is nothing to do */
} NexSdkPkgFinding;

#define NEXSDK_PKG_MAX_FINDINGS 64

typedef struct {
    char path[1024];
    NexSdkDebControl control;
    NexSdkPkgFinding findings[NEXSDK_PKG_MAX_FINDINGS];
    int count;
    int files;                 /* how many files the data archive has */
} NexSdkPkgReport;

/* ---- the parsers (no files are opened) ---- */

/* Debian control text of the binary package -> control.  Returns 0 when Package, Version and Architecture
 * were all there, -1 otherwise (the fields that were there are still filled in). */
int nexsdk_pkg_parse_control(const char *text, NexSdkDebControl *control);

/* The output of `dpkg-deb -c` -> the checks that need the file list: absolute paths, /usr/local,
 * setuid/setgid, a .desktop file whose Exec or Icon points at nothing, a program without documentation.
 * The findings are appended to report (count is updated).  Returns the number of files seen. */
int nexsdk_pkg_scan_files(const char *listing, const NexSdkDebControl *control, NexSdkPkgReport *report);

/* Check the control block alone: required fields, the Debian rules for names and versions, a Maintainer
 * with an address, a description with more than a synopsis, and Depends for a program. */
void nexsdk_pkg_scan_control(const NexSdkDebControl *control, NexSdkPkgReport *report);

/* One key of a .desktop file ("Exec", "Icon", "Name"), with the group respected: [Desktop Entry] only.
 * Returns 0 when it was there. */
int nexsdk_pkg_parse_desktop_key(const char *text, const char *key, char *out, size_t out_size);

/* Check one .desktop file of the package.  `exists` answers whether a path of the package is in it (the
 * check gives it "/usr/bin/hello" or "/usr/share/icons/hicolor/48x48/apps/hello.png"); when it is NULL,
 * the paths are not looked at.  This is what catches "Exec names a program that is not in the package". */
void nexsdk_pkg_scan_desktop(const char *desktop_text, const char *desktop_path,
                             int (*exists)(const char *path, void *data), void *data, NexSdkPkgReport *report);

/* Add a finding (the parsers do this themselves; programs may add their own). */
void nexsdk_pkg_add(NexSdkPkgReport *report, NexSdkPkgStatus status, const char *id, const char *message,
                    const char *hint);

/* The worst status in the report. */
NexSdkPkgStatus nexsdk_pkg_worst(const NexSdkPkgReport *report);

/* Check a real .deb with dpkg-deb.  Returns 0 when the file could be read (whatever was found), -1 when it
 * is not a package or dpkg-deb is not installed (then report->path says why in the first finding). */
int nexsdk_pkg_check(const char *deb_path, NexSdkPkgReport *report);

/* The report as text for a terminal or a CI log (malloc'd, free with free()). */
char *nexsdk_pkg_report_text(const NexSdkPkgReport *report);

const char *nexsdk_pkg_status_name(NexSdkPkgStatus status);   /* "OK" / "NOTE" / "WARNING" / "ERROR" */

#ifdef __cplusplus
}
#endif
#endif
