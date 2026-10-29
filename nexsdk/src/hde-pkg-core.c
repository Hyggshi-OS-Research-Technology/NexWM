/* nexsdk/src/hde-pkg-core.c — the checks behind hde-pkg-check: is this .deb ready to be published?
 *
 * The parsers here take text, so the unit test can check them without dpkg:

 *   nexsdk_pkg_parse_control()   the control block of `dpkg-deb -f`
 *   nexsdk_pkg_scan_control()    the Debian rules a binary package has to follow
 *   nexsdk_pkg_scan_files()      the file list of `dpkg-deb -c`
 *   nexsdk_pkg_scan_desktop()    one .desktop file, with the paths of the package
 *
 * and nexsdk_pkg_check() is the whole thing for a real file: control, file list, then the package extracted
 * into a temporary directory so that the .desktop files can be read and their Exec/Icon checked against what
 * the package actually installs.
 */
#define _POSIX_C_SOURCE 200809L

#include "nexsdk/nexsdk.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* ------------------------------------------------------------------ findings */

const char *nexsdk_pkg_status_name(NexSdkPkgStatus status)
{
    switch (status) {
    case NEXSDK_PKG_OK: return "OK";
    case NEXSDK_PKG_NOTE: return "NOTE";
    case NEXSDK_PKG_WARN: return "WARNING";
    case NEXSDK_PKG_ERROR:
    default: return "ERROR";
    }
}

void nexsdk_pkg_add(NexSdkPkgReport *report, NexSdkPkgStatus status, const char *id, const char *message,
                    const char *hint)
{
    if (!report || report->count >= NEXSDK_PKG_MAX_FINDINGS) return;
    NexSdkPkgFinding *f = &report->findings[report->count++];
    f->status = status;
    snprintf(f->id, sizeof f->id, "%s", id ? id : "");
    snprintf(f->message, sizeof f->message, "%s", message ? message : "");
    snprintf(f->hint, sizeof f->hint, "%s", hint ? hint : "");
}

NexSdkPkgStatus nexsdk_pkg_worst(const NexSdkPkgReport *report)
{
    NexSdkPkgStatus worst = NEXSDK_PKG_OK;
    if (!report) return worst;
    for (int i = 0; i < report->count; i++)
        if (report->findings[i].status > worst) worst = report->findings[i].status;
    return worst;
}

/* ------------------------------------------------------------------ control */

static void copy_trimmed(char *dst, size_t size, const char *src, size_t len)
{
    while (len > 0 && isspace((unsigned char)*src)) {
        src++;
        len--;
    }
    while (len > 0 && isspace((unsigned char)src[len - 1])) len--;
    if (len >= size) len = size - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

/* Every field of the control block, one per call: `field` is compared case-insensitively.  A field continues
 * on the next line when that line starts with a space or a tab (that is how Description gets its long text). */
static int control_field(const char *text, const char *field, char *out, size_t out_size)
{
    if (!text || !field || !out || out_size == 0) return -1;
    size_t flen = strlen(field);
    const char *p = text;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        const char *colon = memchr(p, ':', len);
        if (colon && (size_t)(colon - p) == flen && strncasecmp(p, field, flen) == 0) {
            /* first line */
            size_t used = 0;
            const char *v = colon + 1;
            size_t vlen = len - (size_t)(v - p);
            char first[512];
            copy_trimmed(first, sizeof first, v, vlen);
            size_t n = strlen(first);
            if (n >= out_size) n = out_size - 1;
            memcpy(out, first, n);
            used = n;
            /* its continuation lines */
            const char *next = eol ? eol + 1 : NULL;
            while (next && *next && (*next == ' ' || *next == '\t')) {
                const char *nl = strchr(next, '\n');
                size_t llen = nl ? (size_t)(nl - next) : strlen(next);
                if (used + 1 < out_size) {
                    out[used++] = '\n';
                    size_t room = out_size - used - 1;
                    size_t take = llen < room ? llen : room;
                    memcpy(out + used, next, take);
                    used += take;
                }
                next = nl ? nl + 1 : NULL;
            }
            out[used] = '\0';
            while (used > 0 && isspace((unsigned char)out[used - 1])) out[--used] = '\0';
            return 0;
        }
        if (!eol) break;
        p = eol + 1;
    }
    out[0] = '\0';
    return -1;
}

int nexsdk_pkg_parse_control(const char *text, NexSdkDebControl *control)
{
    if (!text || !control) return -1;
    memset(control, 0, sizeof *control);
    control_field(text, "Package", control->package, sizeof control->package);
    control_field(text, "Version", control->version, sizeof control->version);
    control_field(text, "Architecture", control->architecture, sizeof control->architecture);
    control_field(text, "Maintainer", control->maintainer, sizeof control->maintainer);
    control_field(text, "Section", control->section, sizeof control->section);
    control_field(text, "Priority", control->priority, sizeof control->priority);
    control_field(text, "Homepage", control->homepage, sizeof control->homepage);
    control_field(text, "Depends", control->depends, sizeof control->depends);

    char desc[2048];
    if (control_field(text, "Description", desc, sizeof desc) == 0) {
        size_t n = strcspn(desc, "\n");
        if (n >= sizeof control->description) n = sizeof control->description - 1;
        memcpy(control->description, desc, n);
        control->description[n] = '\0';
        /* the description with its long text, for the checks below */
        size_t dlen = strlen(control->description);
        if (dlen + 1 < sizeof control->description && desc[dlen] == '\n')
            snprintf(control->description + dlen, sizeof control->description - dlen, "\n%s", desc + dlen + 1);
    }
    char size[32];
    if (control_field(text, "Installed-Size", size, sizeof size) == 0) control->installed_kb = atoi(size);

    return control->package[0] && control->version[0] && control->architecture[0] ? 0 : -1;
}

/* Is this a package name Debian accepts: lowercase letters, digits, +, -, . with at least two characters and
 * starting with a letter or a digit. */
static int valid_package_name(const char *name)
{
    if (!name || strlen(name) < 2) return 0;
    if (!isalnum((unsigned char)name[0])) return 0;
    for (const char *p = name; *p; p++)
        if (!islower((unsigned char)*p) && !isdigit((unsigned char)*p) && *p != '+' && *p != '-' && *p != '.')
            return 0;
    return 1;
}

/* Debian versions: [epoch:]upstream_version[-debian_revision], upstream_version starting with a digit. */
static int valid_version(const char *v)
{
    if (!v || !*v) return 0;
    const char *colon = strchr(v, ':');
    const char *up = colon ? colon + 1 : v;
    if (colon) {
        if (colon == v) return 0;
        for (const char *p = v; p < colon; p++)
            if (!isdigit((unsigned char)*p)) return 0;
    }
    if (!isdigit((unsigned char)*up)) return 0;
    const char *dash = strrchr(up, '-');
    const char *body = up;
    size_t body_len = dash ? (size_t)(dash - up) : strlen(up);
    if (body_len == 0) return 0;
    for (size_t i = 0; i < strlen(body); i++) {
        char c = body[i];
        if (isalnum((unsigned char)c) || c == '.' || c == '+' || c == '~' || c == ':' || c == '-') continue;
        return 0;
    }
    if (dash) {
        if (!dash[1]) return 0;
        for (const char *p = dash + 1; *p; p++)
            if (!isalnum((unsigned char)*p) && *p != '.' && *p != '+' && *p != '~') return 0;
    }
    return 1;
}

void nexsdk_pkg_scan_control(const NexSdkDebControl *control, NexSdkPkgReport *report)
{
    if (!control || !report) return;

    if (!control->package[0]) {
        nexsdk_pkg_add(report, NEXSDK_PKG_ERROR, "control.package", "The control file has no Package field.",
                       "Add `Package: myapp` to debian/control.");
    } else if (!valid_package_name(control->package)) {
        nexsdk_pkg_add(report, NEXSDK_PKG_ERROR, "control.package",
                       "The package name is not one Debian accepts (lowercase letters, digits, + - . only).",
                       "Rename it, e.g. `hyggshi-myapp`.");
    }
    if (!control->version[0])
        nexsdk_pkg_add(report, NEXSDK_PKG_ERROR, "control.version", "The control file has no Version field.",
                       "Add `Version: 1.0-1` (an upstream version and a Debian revision).");
    else if (!valid_version(control->version))
        nexsdk_pkg_add(report, NEXSDK_PKG_ERROR, "control.version",
                       "The version is not one Debian accepts (it has to start with a digit).",
                       "Use e.g. `1.0-1`, not `v1.0`.");
    else if (!strchr(control->version, '-'))
        nexsdk_pkg_add(report, NEXSDK_PKG_WARN, "control.version-revision",
                       "The version has no Debian revision (1.0 instead of 1.0-1).",
                       "Add a revision so that a packaging fix can be told apart from an upstream release.");

    if (!control->architecture[0])
        nexsdk_pkg_add(report, NEXSDK_PKG_ERROR, "control.architecture", "The control file has no Architecture field.",
                       "Use `all` for a program without binaries, or `amd64 arm64 ...` through the build.");
    if (!control->maintainer[0])
        nexsdk_pkg_add(report, NEXSDK_PKG_ERROR, "control.maintainer", "The control file has no Maintainer field.",
                       "Add `Maintainer: Name <you@example.com>`.");
    else if (!strchr(control->maintainer, '@'))
        nexsdk_pkg_add(report, NEXSDK_PKG_WARN, "control.maintainer-email",
                       "The Maintainer field has no e-mail address.", "Write `Name <you@example.com>`.");
    if (!control->description[0])
        nexsdk_pkg_add(report, NEXSDK_PKG_ERROR, "control.description", "The control file has no Description field.",
                       "Write a synopsis line and, under it, a paragraph that says what the program is for.");
    else if (!strchr(control->description, '\n'))
        nexsdk_pkg_add(report, NEXSDK_PKG_WARN, "control.description-long",
                       "The description is one line: apt and the software centers show only it.",
                       "Add a longer text on the following lines, each starting with a space.");
    if (!control->section[0])
        nexsdk_pkg_add(report, NEXSDK_PKG_NOTE, "control.section", "No Section: the package lands in \"misc\".",
                       "Use e.g. `Section: utils` or `Section: gnome`.");
    if (!control->installed_kb)
        nexsdk_pkg_add(report, NEXSDK_PKG_NOTE, "control.installed-size", "No Installed-Size.",
                       "dpkg-deb writes it itself; a handwritten control file should have it.");
    if (!control->depends[0] && control->architecture[0] && strcmp(control->architecture, "all") != 0)
        nexsdk_pkg_add(report, NEXSDK_PKG_WARN, "control.depends",
                       "The package is architecture dependent but has no Depends.",
                       "A binary almost always needs its libraries: `Depends: libgtk-3-0` and so on.");
    if (control->homepage[0] && !strstr(control->homepage, "://"))
        nexsdk_pkg_add(report, NEXSDK_PKG_WARN, "control.homepage", "The Homepage is not a URL.",
                       "Write the whole address, `https://github.com/...`.");
}

/* ------------------------------------------------------------------ the file list */

typedef struct {
    char path[1024];
    int setuid;
    int setgid;
    int is_dir;
    int is_link;
} FileEntry;

static const char *mode_of(const char *line)
{
    return line;
}

static void path_of(const char *line, char *out, size_t out_size)
{
    /* "-rw-r--r-- root/root  1234 2024-01-01 00:00 ./usr/bin/hello -> target" */
    const char *p = line;
    int spaces = 0;
    while (*p && spaces < 5) {
        if (*p == ' ' && p[1] != ' ') spaces++;
        p++;
    }
    while (*p == ' ') p++;
    size_t n = strcspn(p, "\n");
    /* The " -> target" of a symlink is not part of the path.  It is searched for inside this line only: a
     * strstr() would jump to the " -> " of a later line and swallow everything in between. */
    for (size_t i = 0; i + 4 <= n; i++) {
        if (!memcmp(p + i, " -> ", 4)) {
            n = i;
            break;
        }
    }
    if (n >= out_size) n = out_size - 1;
    memcpy(out, p, n);
    out[n] = '\0';
}

/* The path as the package installs it: "./usr/bin/x" -> "/usr/bin/x" */
static const char *normalized(const char *path)
{
    return strncmp(path, "./", 2) == 0 ? path + 1 : path;
}

static FileEntry *list_files(const char *listing, int *count_out)
{
    int cap = 128, count = 0;
    FileEntry *files = malloc((size_t)cap * sizeof *files);
    if (!files) return NULL;
    const char *p = listing;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if (len > 10) {
            const char *mode = mode_of(p);
            if (count >= cap) {
                cap *= 2;
                FileEntry *nf = realloc(files, (size_t)cap * sizeof *files);
                if (!nf) break;
                files = nf;
            }
            FileEntry *f = &files[count];
            memset(f, 0, sizeof *f);
            f->is_dir = mode[0] == 'd';
            f->is_link = mode[0] == 'l';
            f->setuid = mode[3] == 's' || mode[3] == 'S';
            f->setgid = mode[6] == 's' || mode[6] == 'S';
            path_of(p, f->path, sizeof f->path);
            if (f->path[0]) count++;
        }
        if (!eol) break;
        p = eol + 1;
    }
    *count_out = count;
    return files;
}

static int has_suffix(const char *s, const char *suffix)
{
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && !strcmp(s + n - m, suffix);
}

int nexsdk_pkg_scan_files(const char *listing, const NexSdkDebControl *control, NexSdkPkgReport *report)
{
    if (!listing || !report) return -1;
    int count = 0;
    FileEntry *files = list_files(listing, &count);
    if (!files) return -1;

    int docs = 0, desktops = 0, local_paths = 0, setuid = 0, relative = 0;
    char doc_dir[256] = "";
    if (control && control->package[0]) snprintf(doc_dir, sizeof doc_dir, "/usr/share/doc/%s/", control->package);

    for (int i = 0; i < count; i++) {
        const FileEntry *f = &files[i];
        const char *path = normalized(f->path);
        if (path[0] != '/') {
            relative++;
            continue;
        }
        if (!strncmp(path, "/usr/local/", 11) || !strcmp(path, "/usr/local")) local_paths++;
        if (f->setuid || f->setgid) setuid++;
        if (doc_dir[0] && !strncmp(path, doc_dir, strlen(doc_dir))) docs++;
        if (has_suffix(path, ".desktop") && strstr(path, "/applications/")) desktops++;
    }

    if (relative)
        nexsdk_pkg_add(report, NEXSDK_PKG_ERROR, "files.relative-path",
                       "The data archive has a path outside the filesystem root.",
                       "Every file has to be installed under /usr (or /etc, /opt, /var).");
    if (local_paths)
        nexsdk_pkg_add(report, NEXSDK_PKG_ERROR, "files.usr-local",
                       "The package installs into /usr/local, which belongs to the local administrator.",
                       "Install into /usr; /usr/local is written by `make install`, not by a package.");
    if (setuid)
        nexsdk_pkg_add(report, NEXSDK_PKG_WARN, "files.setuid",
                       "The package has a setuid or setgid file.",
                       "Only do that if it is the point of the program (pkexec or sudo is the usual answer).");
    if (doc_dir[0] && !docs)
        nexsdk_pkg_add(report, NEXSDK_PKG_WARN, "files.no-copyright",
                       "There is no /usr/share/doc/<package>/copyright (Debian policy requires it).",
                       "Install debian/copyright with `dh_installdocs`.");

    if (desktops) {
        char msg[256];
        snprintf(msg, sizeof msg, "The package installs %d .desktop file(s): they are checked one by one.", desktops);
        nexsdk_pkg_add(report, NEXSDK_PKG_NOTE, "files.desktop", msg, "");
    }
    free(files);
    return count;
}

/* ------------------------------------------------------------------ .desktop files */

int nexsdk_pkg_parse_desktop_key(const char *text, const char *key, char *out, size_t out_size)
{
    if (!text || !key || !out || out_size == 0) return -1;
    out[0] = '\0';
    size_t klen = strlen(key);
    int in_entry = 0;
    const char *p = text;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if (len > 0 && p[0] == '[') {
            /* only [Desktop Entry] counts: [Desktop Action ...] has a Name and an Exec of its own */
            in_entry = len >= 15 && !strncmp(p, "[Desktop Entry]", 15);
        } else if (in_entry && len > klen && !strncmp(p, key, klen) && p[klen] == '=') {
            copy_trimmed(out, out_size, p + klen + 1, len - klen - 1);
            return out[0] ? 0 : -1;
        }
        if (!eol) break;
        p = eol + 1;
    }
    return -1;
}

/* "gedit %U" -> "gedit"; "sh -c 'x'" -> "sh"; "/usr/bin/x --flag" -> "/usr/bin/x"; "env A=1 prog" -> "prog" */
static void exec_program(const char *exec, char *out, size_t out_size)
{
    out[0] = '\0';
    while (*exec && isspace((unsigned char)*exec)) exec++;
    size_t n = 0;
    while (exec[n] && !isspace((unsigned char)exec[n]) && n + 1 < out_size) {
        out[n] = exec[n];
        n++;
    }
    out[n] = '\0';
    if (!*out) return;
    /* a leading "env VAR=value" wrapper is not the program */
    if (!strcmp(out, "env")) {
        const char *p = exec + 3;
        while (*p) {
            while (*p && isspace((unsigned char)*p)) p++;
            const char *word = p;
            size_t wlen = 0;
            while (p[wlen] && !isspace((unsigned char)p[wlen])) wlen++;
            if (!wlen) break;
            if (!memchr(word, '=', wlen)) {
                size_t take = wlen < out_size - 1 ? wlen : out_size - 1;
                memcpy(out, word, take);
                out[take] = '\0';
                return;
            }
            p += wlen;
        }
    }
}

void nexsdk_pkg_scan_desktop(const char *desktop_text, const char *desktop_path,
                             int (*exists)(const char *path, void *data), void *data, NexSdkPkgReport *report)
{
    if (!desktop_text || !report) return;
    const char *name = desktop_path ? desktop_path : "(a .desktop file)";
    char exec[512], icon[512], type[32], text_key[256];

    if (nexsdk_pkg_parse_desktop_key(desktop_text, "Type", type, sizeof type) != 0) {
        char msg[600];
        snprintf(msg, sizeof msg, "%s has no Type: line, so no application menu shows it.", name);
        nexsdk_pkg_add(report, NEXSDK_PKG_WARN, "desktop.no-type", msg, "Add `Type=Application`.");
    } else if (strcmp(type, "Application") != 0) {
        char msg[600];
        snprintf(msg, sizeof msg, "%s is of Type=%s, not Application.", name, type);
        nexsdk_pkg_add(report, NEXSDK_PKG_NOTE, "desktop.type", msg, "");
    }
    if (nexsdk_pkg_parse_desktop_key(desktop_text, "Name", text_key, sizeof text_key) != 0) {
        char msg[600];
        snprintf(msg, sizeof msg, "%s has no Name: line (the menu would show the file name).", name);
        nexsdk_pkg_add(report, NEXSDK_PKG_WARN, "desktop.no-name", msg, "Add `Name=My Application`.");
    }
    if (nexsdk_pkg_parse_desktop_key(desktop_text, "Exec", exec, sizeof exec) != 0) {
        char msg[600];
        snprintf(msg, sizeof msg, "%s has no Exec: line, so it cannot start anything.", name);
        nexsdk_pkg_add(report, NEXSDK_PKG_ERROR, "desktop.no-exec", msg, "Add `Exec=/usr/bin/myapp`.");
        return;
    }

    char program[512];
    exec_program(exec, program, sizeof program);
    if (!program[0]) {
        char msg[600];
        snprintf(msg, sizeof msg, "%s has an empty Exec.", name);
        nexsdk_pkg_add(report, NEXSDK_PKG_ERROR, "desktop.no-exec", msg, "Add `Exec=/usr/bin/myapp`.");
    } else if (exists) {
        if (program[0] == '/') {
            if (!exists(program, data)) {
                char msg[600];
                snprintf(msg, sizeof msg, "%s starts %s, which the package does not install.", name, program);
                nexsdk_pkg_add(report, NEXSDK_PKG_ERROR, "desktop.exec-missing", msg,
                               "Install the program there, or fix Exec=.");
            }
        } else {
            char candidate[600];
            snprintf(candidate, sizeof candidate, "/usr/bin/%s", program);
            if (!exists(candidate, data)) {
                snprintf(candidate, sizeof candidate, "/usr/games/%s", program);
                if (!exists(candidate, data)) {
                    snprintf(candidate, sizeof candidate, "/opt/%s", program);
                    if (!exists(candidate, data)) {
                        char msg[600];
                        snprintf(msg, sizeof msg, "%s starts \"%s\", which the package does not install.", name,
                                 program);
                        nexsdk_pkg_add(report, NEXSDK_PKG_ERROR, "desktop.exec-missing", msg,
                                       "Use the absolute path of the program, or install it into /usr/bin.");
                    }
                }
            }
        }
    }

    if (nexsdk_pkg_parse_desktop_key(desktop_text, "Icon", icon, sizeof icon) == 0 && exists) {
        /* an absolute path has to be there; a name has to be in the icon theme directories */
        if (icon[0] == '/') {
            if (!exists(icon, data)) {
                char msg[600];
                snprintf(msg, sizeof msg, "%s uses the icon %s, which the package does not install.", name, icon);
                nexsdk_pkg_add(report, NEXSDK_PKG_WARN, "desktop.icon-missing", msg,
                               "Install the icon under /usr/share/icons/hicolor/.../apps/ or use an icon name.");
            }
        } else {
            char candidate[700];
            int found = 0;
            static const char *const sizes[] = { "48x48", "64x64", "128x128", "256x256", "scalable", NULL };
            static const char *const exts[] = { "png", "svg", "xpm", NULL };
            for (int si = 0; sizes[si] && !found; si++)
                for (int ei = 0; exts[ei] && !found; ei++) {
                    snprintf(candidate, sizeof candidate, "/usr/share/icons/hicolor/%s/apps/%s.%s", sizes[si],
                             icon, exts[ei]);
                    if (exists(candidate, data)) found = 1;
                }
            if (!found) {
                snprintf(candidate, sizeof candidate, "/usr/share/pixmaps/%s.png", icon);
                if (exists(candidate, data)) found = 1;
                snprintf(candidate, sizeof candidate, "/usr/share/pixmaps/%s.svg", icon);
                if (exists(candidate, data)) found = 1;
            }
            if (!found) {
                char msg[700];
                snprintf(msg, sizeof msg, "%s uses the icon \"%s\", which is not in the package (a name is only "
                                          "right when the icon comes from the icon theme).", name, icon);
                nexsdk_pkg_add(report, NEXSDK_PKG_WARN, "desktop.icon-missing", msg,
                               "Install the icon, or use the name of an icon of hicolor or of the system theme.");
            }
        }
    }
}

/* ------------------------------------------------------------------ running dpkg-deb */

static int run_capture(const char *const argv[], char **out)
{
    int fds[2];
    if (pipe(fds) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) dup2(devnull, STDERR_FILENO);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    close(fds[1]);
    size_t cap = 65536, used = 0;
    char *buf = malloc(cap);
    if (!buf) {
        close(fds[0]);
        waitpid(pid, NULL, 0);
        return -1;
    }
    ssize_t got;
    while ((got = read(fds[0], buf + used, cap - used - 1)) > 0) {
        used += (size_t)got;
        if (used + 1 >= cap) {
            char *nb = realloc(buf, cap * 2);
            if (!nb) break;
            buf = nb;
            cap *= 2;
        }
    }
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    buf[used] = '\0';
    *out = buf;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* Does the package install this path?  The extracted tree is the answer. */
typedef struct {
    const char *root;
} Tree;

static int tree_exists(const char *path, void *data)
{
    Tree *t = data;
    if (!t || !t->root || !path || path[0] != '/') return 0;
    char candidate[2048];
    snprintf(candidate, sizeof candidate, "%s%s", t->root, path);
    struct stat st;
    return stat(candidate, &st) == 0;
}

static void remove_tree(const char *path)
{
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char child[2048];
        snprintf(child, sizeof child, "%s/%s", path, e->d_name);
        struct stat st;
        if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode))
            remove_tree(child);
        else
            unlink(child);
    }
    closedir(d);
    rmdir(path);
}

/* Every .desktop file of the extracted tree, checked with the tree as the list of what exists. */
static void check_desktop_files(const char *root, NexSdkPkgReport *report)
{
    char dirpath[2048];
    snprintf(dirpath, sizeof dirpath, "%s/usr/share/applications", root);
    DIR *d = opendir(dirpath);
    if (!d) return;
    Tree tree = { root };
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (!has_suffix(e->d_name, ".desktop")) continue;
        char file[3000];
        snprintf(file, sizeof file, "%s/%s", dirpath, e->d_name);
        FILE *fp = fopen(file, "r");
        if (!fp) continue;
        char text[16384];
        size_t n = fread(text, 1, sizeof text - 1, fp);
        fclose(fp);
        text[n] = '\0';
        char shown[512];
        snprintf(shown, sizeof shown, "/usr/share/applications/%s", e->d_name);
        nexsdk_pkg_scan_desktop(text, shown, tree_exists, &tree, report);
    }
    closedir(d);
}

int nexsdk_pkg_check(const char *deb_path, NexSdkPkgReport *report)
{
    if (!deb_path || !report) return -1;
    memset(report, 0, sizeof *report);
    snprintf(report->path, sizeof report->path, "%s", deb_path);

    struct stat st;
    if (stat(deb_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        nexsdk_pkg_add(report, NEXSDK_PKG_ERROR, "file.missing", "This file does not exist or is not a regular file.",
                       "Give the path of a .deb, e.g. ../myapp_1.0-1_amd64.deb.");
        return -1;
    }
    FILE *fp = fopen(deb_path, "rb");
    if (!fp) {
        nexsdk_pkg_add(report, NEXSDK_PKG_ERROR, "file.unreadable", "This file cannot be read.", "");
        return -1;
    }
    char magic[8] = "";
    size_t got = fread(magic, 1, sizeof magic, fp);
    fclose(fp);
    if (got < 8 || memcmp(magic, "!<arch>\n", 8) != 0) {
        nexsdk_pkg_add(report, NEXSDK_PKG_ERROR, "file.not-a-deb",
                       "This is not a Debian package (a .deb starts with the ar magic \"!<arch>\").",
                       "Build one with `dpkg-deb --build` or `debuild -us -uc`.");
        return -1;
    }

    const char *control_argv[] = { "dpkg-deb", "-f", deb_path, NULL };
    const char *files_argv[] = { "dpkg-deb", "-c", deb_path, NULL };
    char *control_text = NULL;
    char *file_list = NULL;
    if (run_capture(control_argv, &control_text) != 0 || !control_text || !control_text[0]) {
        nexsdk_pkg_add(report, NEXSDK_PKG_ERROR, "file.no-control",
                       "The control block of this package cannot be read (is dpkg installed?).",
                       "Install dpkg / dpkg-deb, or rebuild the package: `dpkg-deb --build dir`.");
        free(control_text);
        return -1;
    }
    nexsdk_pkg_parse_control(control_text, &report->control);
    nexsdk_pkg_scan_control(&report->control, report);
    free(control_text);

    if (run_capture(files_argv, &file_list) == 0 && file_list && file_list[0])
        report->files = nexsdk_pkg_scan_files(file_list, &report->control, report);
    else
        nexsdk_pkg_add(report, NEXSDK_PKG_ERROR, "file.no-list", "The files of this package cannot be listed.", "");
    free(file_list);

    /* the .desktop files need the contents, so the package is unpacked into a temporary directory */
    char template[] = "/tmp/hde-pkg-check-XXXXXX";
    char *root = mkdtemp(template);
    if (root) {
        const char *extract_argv[] = { "dpkg-deb", "-x", deb_path, root, NULL };
        char *ignored = NULL;
        if (run_capture(extract_argv, &ignored) == 0) check_desktop_files(root, report);
        free(ignored);
        remove_tree(root);
    }
    return 0;
}

char *nexsdk_pkg_report_text(const NexSdkPkgReport *report)
{
    if (!report) return NULL;
    size_t cap = 65536, used = 0;
    char *out = malloc(cap);
    if (!out) return NULL;

#define APPEND(...)                                                                  \
    do {                                                                             \
        int n = snprintf(out + used, cap - used, __VA_ARGS__);                       \
        if (n > 0) used += (size_t)n < cap - used ? (size_t)n : cap - used - 1;      \
    } while (0)

    NexSdkPkgStatus worst = nexsdk_pkg_worst(report);
    APPEND("Hyggshi NexSDK package check\n============================\n\n");
    APPEND("File     : %s\n", report->path);
    if (report->control.package[0])
        APPEND("Package  : %s %s (%s)\n", report->control.package, report->control.version,
               report->control.architecture);
    if (report->control.maintainer[0]) APPEND("Maintainer: %s\n", report->control.maintainer);
    if (report->control.description[0]) {
        char synopsis[256];
        size_t n = strcspn(report->control.description, "\n");
        if (n >= sizeof synopsis) n = sizeof synopsis - 1;
        memcpy(synopsis, report->control.description, n);
        synopsis[n] = '\0';
        APPEND("Summary  : %s\n", synopsis);
    }
    if (report->files) APPEND("Files    : %d\n", report->files);
    APPEND("Result   : %s", nexsdk_pkg_status_name(worst));
    if (worst == NEXSDK_PKG_OK) APPEND(" — ready to publish\n\n");
    else if (worst == NEXSDK_PKG_NOTE) APPEND(" — only notes\n\n");
    else APPEND("\n\n");

    if (!report->count) {
        APPEND("Nothing to report: every check passed.\n");
    } else {
        for (int i = 0; i < report->count; i++) {
            const NexSdkPkgFinding *f = &report->findings[i];
            APPEND("[%-7s] %-26s %s\n", nexsdk_pkg_status_name(f->status), f->id, f->message);
            if (f->hint[0]) APPEND("          → %s\n", f->hint);
        }
    }
    int errors = 0, warnings = 0;
    for (int i = 0; i < report->count; i++) {
        if (report->findings[i].status == NEXSDK_PKG_ERROR) errors++;
        else if (report->findings[i].status == NEXSDK_PKG_WARN) warnings++;
    }
    APPEND("\n%d finding(s) · %d error(s) · %d warning(s)\n", report->count, errors, warnings);
#undef APPEND
    return out;
}
