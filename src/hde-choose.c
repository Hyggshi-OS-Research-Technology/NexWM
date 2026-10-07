/* hde-choose.c — the table of features and the rules of "which program for this?" (see hde-choose.h).
 *
 * Plain C, no GTK, no GLib: the program (apps/hde-choose.c) and the unit test (tests/choose-test.c) both use this, so
 * the rules are tested without a display. Everything the user can decide lives in one place here: the order of the
 * candidates (HDE's own program first), which of them this machine has, what was remembered, and where it is written
 * down (~/.config/hde/settings.ini, key choice_<feature>, the file every other part of HDE reads).
 */
#define _POSIX_C_SOURCE 200809L

#include "hde-choose.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ------------------------------------------------------------------ the features
 *
 * The order inside a feature is the order they are offered in: HDE's own program first (that is what HDE is for), then
 * the ones most likely to be there. Names that turn out to be the same program (x-terminal-emulator is a symlink to
 * one of the others) are counted once, which is why the generic name comes last.
 */
#define HDE_OWN "HDE's own"

static const HdeChooseCandidate terminal_cands[] = {
    { "hde-cmd", NULL, HDE_OWN },          /* HDE's own terminal, when it is installed (hde-cmd) */
    { "gnome-terminal", NULL, NULL },
    { "xfce4-terminal", NULL, NULL },
    { "mate-terminal", NULL, NULL },
    { "tilix", NULL, NULL },
    { "konsole", NULL, NULL },
    { "lxterminal", NULL, NULL },
    { "qterminal", NULL, NULL },
    { "terminator", NULL, NULL },
    { "alacritty", NULL, NULL },
    { "kitty", NULL, NULL },
    { "foot", NULL, NULL },
    { "wezterm", NULL, NULL },
    { "xterm", NULL, NULL },
    { "x-terminal-emulator", NULL, NULL },  /* the system's own choice (Debian alternatives), which is one of the above */
};

static const HdeChooseCandidate files_cands[] = {
    { "hde-files", "\"$HOME\"", HDE_OWN },
    { "thunar", "\"$HOME\"", NULL },
    { "pcmanfm", "\"$HOME\"", NULL },
    { "nautilus", "\"$HOME\"", NULL },
    { "nemo", "\"$HOME\"", NULL },
    { "caja", "\"$HOME\"", NULL },
    { "dolphin", "\"$HOME\"", NULL },
    { "pcmanfm-qt", "\"$HOME\"", NULL },
};

static const HdeChooseCandidate monitor_cands[] = {
    { "gnome-system-monitor", NULL, NULL },
    { "mate-system-monitor", NULL, NULL },
    { "xfce4-taskmanager", NULL, NULL },
    { "lxtask", NULL, NULL },
    { "plasma-systemmonitor", NULL, NULL },
    { "ksysguard", NULL, NULL },
    { "qps", NULL, NULL },
    { "missioncenter", NULL, NULL },
};

static const HdeChooseCandidate pictures_cands[] = {
    { "hde-media", NULL, HDE_OWN },        /* Hyggshi Media shows pictures: zoom, rotate, one after the other */
    { "eog", NULL, NULL },
    { "gwenview", NULL, NULL },
    { "lximage-qt", NULL, NULL },
    { "ristretto", NULL, NULL },
    { "geeqie", NULL, NULL },
    { "feh", NULL, NULL },
    { "xdg-open", NULL, NULL },
};

static const HdeChooseCandidate player_cands[] = {
    { "hde-media", NULL, HDE_OWN },        /* and plays music and video (its own player window) */
    { "mpv", NULL, NULL },
    { "vlc", NULL, NULL },
    { "totem", NULL, NULL },
    { "parole", NULL, NULL },
    { "celluloid", NULL, NULL },
    { "smplayer", NULL, NULL },
    { "xplayer", NULL, NULL },
    { "xdg-open", NULL, NULL },
};

static const HdeChooseCandidate screenshot_cands[] = {
    { "hde-screenshot", NULL, HDE_OWN },   /* PrtSc is HDE's own tool: whole screen, window or area, no scrot needed */
    { "gnome-screenshot", NULL, NULL },
    { "spectacle", NULL, NULL },
    { "xfce4-screenshooter", NULL, NULL },
    { "mate-screenshot", NULL, NULL },
    { "flameshot", NULL, NULL },
    { "scrot", NULL, NULL },
    { "maim", NULL, NULL },
};

#define FEATURE(n, t, inst, list) { n, t, inst, list, (int)(sizeof list / sizeof list[0]) }

static const HdeChooseFeature features[] = {
    FEATURE("terminal", "Which program for a terminal?", "xterm", terminal_cands),
    FEATURE("files", "Which program for files and folders?", "thunar", files_cands),
    FEATURE("monitor", "Which program to see what the system is doing?", "gnome-system-monitor", monitor_cands),
    FEATURE("pictures", "Which program for pictures?", "eog", pictures_cands),
    FEATURE("player", "Which program for music and video?", "mpv", player_cands),
    FEATURE("screenshot", "Which program to take a screenshot?", "scrot", screenshot_cands),
};

const HdeChooseFeature *hde_choose_table(int *count)
{
    if (count) *count = (int)(sizeof features / sizeof features[0]);
    return features;
}

const HdeChooseFeature *hde_choose_feature(const char *feature)
{
    if (!feature || !*feature) return NULL;
    for (size_t i = 0; i < sizeof features / sizeof features[0]; i++)
        if (!strcmp(features[i].feature, feature)) return &features[i];
    return NULL;
}

/* ------------------------------------------------------------------ what is installed */

/* The full path of a program, or -1 when this machine has no such program. A name with a slash is the path itself;
 * otherwise PATH is walked. This is the one place that decides what "installed" means. */
static int which_path(const char *program, char *full, size_t len)
{
    if (!program || !*program) return -1;
    if (strchr(program, '/')) {
        if (access(program, X_OK) != 0) return -1;
        snprintf(full, len, "%s", program);
        return 0;
    }
    const char *path = getenv("PATH");
    if (!path || !*path) path = "/usr/local/bin:/usr/bin:/bin";
    for (const char *p = path; *p; ) {
        const char *end = strchr(p, ':');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        size_t need = n + 1 + strlen(program);          /* "DIR/program" without the terminating byte */
        if (n && need < len) {
            memcpy(full, p, n);                         /* built by hand: "%s/%s" of two long strings is one of the */
            full[n] = '/';                              /* ways to make gcc warn about a truncation that cannot happen */
            memcpy(full + n + 1, program, strlen(program) + 1);
            if (access(full, X_OK) == 0) return 0;
        }
        if (!end) break;
        p = end + 1;
    }
    return -1;
}

int hde_choose_have(const char *program)
{
    char full[PATH_MAX];
    return which_path(program, full, sizeof full) == 0;
}

int hde_choose_installed(const HdeChooseFeature *f, const HdeChooseCandidate **out, int max)
{
    if (!f) return 0;
    int n = 0;
    struct stat seen[32];
    int n_seen = 0;
    for (int i = 0; i < f->n && n < max; i++) {
        char full[PATH_MAX];
        if (which_path(f->candidates[i].program, full, sizeof full) != 0) continue;
        struct stat st;
        if (stat(full, &st) != 0) continue;             /* stat() follows symlinks: two names for one file are one */
        int dup = 0;
        for (int s = 0; s < n_seen; s++)
            if (seen[s].st_dev == st.st_dev && seen[s].st_ino == st.st_ino) dup = 1;
        if (dup) continue;
        if (n_seen < 32) seen[n_seen++] = st;
        out[n++] = &f->candidates[i];
    }
    return n;
}

/* ------------------------------------------------------------------ what was remembered */

char *hde_choose_settings_file(void)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    char buf[PATH_MAX];
    if (xdg && *xdg) snprintf(buf, sizeof buf, "%s/hde/settings.ini", xdg);
    else snprintf(buf, sizeof buf, "%s/.config/hde/settings.ini", home && *home ? home : "/tmp");
    return strdup(buf);
}

/* The value of choice_<feature>: the last one in the file wins, exactly as hde_settings_get() reads it (a value
 * written twice — by an older HDE, or by the Settings window appending — must not keep an old answer alive). "" when
 * there is none. Caller frees. */
char *hde_choose_choice(const char *feature)
{
    char *path = hde_choose_settings_file();
    char key[128];
    snprintf(key, sizeof key, "choice_%s=", feature);
    size_t klen = strlen(key);
    char *value = NULL;
    FILE *f = fopen(path, "r");
    if (f) {
        char line[1024];
        while (fgets(line, sizeof line, f)) {
            if (strncmp(line, key, klen)) continue;
            char *v = line + klen;
            v[strcspn(v, "\r\n")] = '\0';
            free(value);
            value = strdup(v);
        }
        fclose(f);
    }
    free(path);
    return value ? value : strdup("");
}

int hde_choose_set_choice(const char *feature, const char *value)
{
    if (!feature || !*feature) return -1;
    char *path = hde_choose_settings_file();
    char key[128];
    snprintf(key, sizeof key, "choice_%s=", feature);
    size_t klen = strlen(key);

    /* the directory of the settings file (the rest of HDE creates it too, but hde-choose may be the first to run) */
    char *slash = strrchr(path, '/');
    if (slash) {
        char dir[PATH_MAX];
        size_t n = (size_t)(slash - path);
        if (n < sizeof dir) {
            snprintf(dir, sizeof dir, "%.*s", (int)n, path);
            mkdir(dir, 0755);
        }
    }

    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s.hde-choose.tmp", path);
    FILE *in = fopen(path, "r");
    FILE *out = fopen(tmp, "w");
    if (!out) {
        if (in) fclose(in);
        free(path);
        return -1;
    }
    if (in) {
        char line[1024];
        while (fgets(line, sizeof line, in))
            if (strncmp(line, key, klen)) fputs(line, out);
        fclose(in);
    }
    if (value && *value) fprintf(out, "%s%s\n", key, value);
    fflush(out);
    int bad = ferror(out);
    fclose(out);
    if (bad || rename(tmp, path) != 0) {
        unlink(tmp);
        free(path);
        return -1;
    }
    free(path);
    return 0;
}
