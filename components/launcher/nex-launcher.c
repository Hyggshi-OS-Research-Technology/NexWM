#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

/*
 * nex-launcher.c - NexLauncher
 * Application launcher for NexWM
 *
 * Phase 1: Scans .desktop files, filters by search query,
 * launches applications. Display is handled by a small XCB window
 * with keyboard-driven search (Up/Down/Enter/Esc).
 *
 * Usage:
 *   nex-launcher           (interactive mode)
 *   nex-launcher --list    (print all apps to stdout, one per line)
 */

#include "nex-launcher.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>

/* ─── .desktop file parser ──────────────────────────────────────────────────── */

static void parse_desktop_file(const char *path, nex_app_t *app)
{
    memset(app, 0, sizeof(*app));
    app->terminal = 0;

    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[1024];
    int  in_desktop_entry = 0;

    while (fgets(line, sizeof(line), f)) {
        /* Strip newline */
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') line[len-1] = '\0';

        if (line[0] == '[') {
            in_desktop_entry = (strncmp(line, "[Desktop Entry]", 15) == 0);
            continue;
        }
        if (!in_desktop_entry || line[0] == '#') continue;

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;

        /* Skip localised keys like Name[vi]= */
        if (strchr(key, '[')) continue;

        if (strcmp(key, "Name") == 0) {
            strncpy(app->name, val, sizeof(app->name) - 1);
        } else if (strcmp(key, "Exec") == 0) {
            /* Strip %u %f %U %F placeholders */
            char *p = val;
            int out = 0;
            char cleaned[512];
            while (*p && out < (int)sizeof(cleaned)-2) {
                if (*p == '%' && *(p+1)) { p += 2; continue; }
                cleaned[out++] = *p++;
            }
            cleaned[out] = '\0';
            /* Trim trailing space */
            while (out > 0 && cleaned[out-1] == ' ') cleaned[--out] = '\0';
            strncpy(app->exec, cleaned, sizeof(app->exec) - 1);
        } else if (strcmp(key, "Icon") == 0) {
            strncpy(app->icon, val, sizeof(app->icon) - 1);
        } else if (strcmp(key, "Categories") == 0) {
            strncpy(app->categories, val, sizeof(app->categories) - 1);
        } else if (strcmp(key, "Comment") == 0) {
            strncpy(app->comment, val, sizeof(app->comment) - 1);
        } else if (strcmp(key, "Terminal") == 0) {
            app->terminal = (strcasecmp(val, "true") == 0);
        } else if (strcmp(key, "NoDisplay") == 0) {
            if (strcasecmp(val, "true") == 0) {
                /* Mark as hidden by clearing name */
                app->name[0] = '\0';
            }
        } else if (strcmp(key, "Type") == 0) {
            if (strcmp(val, "Application") != 0) {
                app->name[0] = '\0';  /* Skip non-application entries */
            }
        }
    }

    fclose(f);
}

static int scan_dir(const char *dir, nex_app_list_t *list)
{
    DIR *d = opendir(dir);
    if (!d) return 0;

    struct dirent *ent;
    int added = 0;
    while ((ent = readdir(d)) != NULL && list->count < NEX_LAUNCHER_MAX_APPS) {
        if (ent->d_name[0] == '.') continue;
        size_t nlen = strlen(ent->d_name);
        if (nlen < 8 || strcmp(ent->d_name + nlen - 8, ".desktop") != 0) continue;

        char path[NEX_LAUNCHER_MAX_PATH];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);

        nex_app_t app;
        parse_desktop_file(path, &app);

        /* Only add valid, displayable apps */
        if (app.name[0] == '\0' || app.exec[0] == '\0') continue;

        /* Deduplicate by name */
        int dup = 0;
        for (int i = 0; i < list->count; i++) {
            if (strcmp(list->apps[i].name, app.name) == 0) { dup = 1; break; }
        }
        if (dup) continue;

        list->apps[list->count++] = app;
        added++;
    }
    closedir(d);
    return added;
}

int nex_launcher_scan(nex_app_list_t *list)
{
    if (!list) return -1;
    list->count = 0;

    /* System applications */
    scan_dir("/usr/share/applications", list);
    scan_dir("/usr/local/share/applications", list);

    /* User applications */
    const char *home = getenv("HOME");
    if (home) {
        char user_dir[NEX_LAUNCHER_MAX_PATH];
        snprintf(user_dir, sizeof(user_dir), "%s/.local/share/applications", home);
        scan_dir(user_dir, list);
    }

    return list->count;
}

/* ─── fuzzy filter ──────────────────────────────────────────────────────────── */

void nex_launcher_filter(const nex_app_list_t *in, const char *query,
                         nex_app_list_t *out)
{
    out->count = 0;
    if (!query || query[0] == '\0') {
        /* Return all */
        int n = in->count < NEX_LAUNCHER_MAX_APPS ? in->count : NEX_LAUNCHER_MAX_APPS;
        memcpy(out->apps, in->apps, (size_t)n * sizeof(nex_app_t));
        out->count = n;
        return;
    }

    for (int i = 0; i < in->count && out->count < NEX_LAUNCHER_MAX_APPS; i++) {
        if (strcasestr(in->apps[i].name,       query) ||
            strcasestr(in->apps[i].categories,  query) ||
            strcasestr(in->apps[i].comment,     query)) {
            out->apps[out->count++] = in->apps[i];
        }
    }
}

/* ─── launcher ──────────────────────────────────────────────────────────────── */

int nex_launcher_launch(const nex_app_t *app)
{
    if (!app || app->exec[0] == '\0') return -1;

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }
    if (pid == 0) {
        /* Child: detach from parent process group */
        setsid();
        /* Redirect stdin/stdout/stderr away from terminal */
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        char *const args[] = { "/bin/sh", "-c", (char *)app->exec, NULL };
        execv("/bin/sh", args);
        _exit(1);
    }

    /* Parent: don't wait, app runs independently */
    return 0;
}

/* ─── CLI entry point ────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    nex_app_list_t all;
    int n = nex_launcher_scan(&all);
    if (n < 0) {
        fprintf(stderr, "nex-launcher: failed to scan applications\n");
        return 1;
    }

    /* --list mode: print all apps to stdout */
    if (argc > 1 && strcmp(argv[1], "--list") == 0) {
        for (int i = 0; i < all.count; i++) {
            printf("%-40s  %s\n", all.apps[i].name, all.apps[i].exec);
        }
        return 0;
    }

    /* Interactive XCB window mode — Phase 1 stub: print and launch from CLI */
    /* Full XCB render window comes in Phase 1 polish */
    if (argc > 1) {
        /* Query provided as argument — filter and launch first match */
        nex_app_list_t filtered;
        nex_launcher_filter(&all, argv[1], &filtered);
        if (filtered.count == 0) {
            fprintf(stderr, "nex-launcher: no match for '%s'\n", argv[1]);
            return 1;
        }
        printf("Launching: %s (%s)\n", filtered.apps[0].name, filtered.apps[0].exec);
        return nex_launcher_launch(&filtered.apps[0]);
    }

    /* No args: print list and exit (full GUI in next iteration) */
    for (int i = 0; i < all.count; i++) {
        printf("[%3d] %s\n", i+1, all.apps[i].name);
    }
    fprintf(stderr, "\nNote: Interactive GUI mode not yet available — use: nex-launcher <query>\n");
    return 0;
}
