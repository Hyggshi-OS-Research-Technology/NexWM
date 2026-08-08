#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

/*
 * nex-desktop.c - NexDesktop
 * Desktop Icon Manager for NexWM / Hyggshi OS
 *
 * Creates a desktop-level window (_NET_WM_WINDOW_TYPE_DESKTOP) spanning the screen,
 * scans ~/Desktop for files and .desktop shortcuts, and renders an interactive icon grid.
 */

#include "nex-desktop.h"

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
#include <time.h>

#include <xcb/xcb.h>

/* ─── Desktop Scanning ───────────────────────────────────────────────────────── */

static void parse_desktop_entry(const char *path, nex_desktop_item_t *item)
{
    memset(item, 0, sizeof(*item));
    item->is_desktop_file = 1;
    strncpy(item->path, path, sizeof(item->path) - 1);

    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[1024];
    int in_entry = 0;

    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';

        if (line[0] == '[') {
            in_entry = (strncmp(line, "[Desktop Entry]", 15) == 0);
            continue;
        }
        if (!in_entry || line[0] == '#') continue;

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;

        if (strchr(key, '[')) continue;

        if (strcmp(key, "Name") == 0) {
            strncpy(item->name, val, sizeof(item->name) - 1);
        } else if (strcmp(key, "Exec") == 0) {
            /* Strip %u %f placeholders */
            char cleaned[512];
            int out = 0;
            char *p = val;
            while (*p && out < (int)sizeof(cleaned) - 2) {
                if (*p == '%' && *(p + 1)) { p += 2; continue; }
                cleaned[out++] = *p++;
            }
            cleaned[out] = '\0';
            while (out > 0 && cleaned[out - 1] == ' ') cleaned[--out] = '\0';
            strncpy(item->exec, cleaned, sizeof(item->exec) - 1);
        } else if (strcmp(key, "Icon") == 0) {
            strncpy(item->icon, val, sizeof(item->icon) - 1);
        }
    }
    fclose(f);

    if (item->name[0] == '\0') {
        const char *base = strrchr(path, '/');
        strncpy(item->name, base ? base + 1 : path, sizeof(item->name) - 1);
    }
}

int nex_desktop_scan(nex_desktop_item_list_t *list)
{
    if (!list) return -1;
    list->count = 0;

    const char *home = getenv("HOME");
    if (!home) home = "/root";

    char desktop_dir[512];
    snprintf(desktop_dir, sizeof(desktop_dir), "%s/Desktop", home);

    /* Create ~/Desktop if it does not exist */
    struct stat st;
    if (stat(desktop_dir, &st) != 0) {
        mkdir(desktop_dir, 0755);
        return 0;
    }

    DIR *d = opendir(desktop_dir);
    if (!d) return 0;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && list->count < NEX_DESKTOP_MAX_FILES) {
        if (ent->d_name[0] == '.') continue;

        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", desktop_dir, ent->d_name);

        nex_desktop_item_t *item = &list->items[list->count];
        size_t nlen = strlen(ent->d_name);

        if (nlen > 8 && strcmp(ent->d_name + nlen - 8, ".desktop") == 0) {
            parse_desktop_entry(path, item);
        } else {
            memset(item, 0, sizeof(*item));
            item->is_desktop_file = 0;
            strncpy(item->name, ent->d_name, sizeof(item->name) - 1);
            strncpy(item->path, path, sizeof(item->path) - 1);
        }
        list->count++;
    }
    closedir(d);
    return list->count;
}

int nex_desktop_launch(const nex_desktop_item_t *item)
{
    if (!item) return -1;

    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        setsid();
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        if (item->is_desktop_file && item->exec[0] != '\0') {
            execl("/bin/sh", "/bin/sh", "-c", item->exec, (char *)NULL);
        } else {
            execlp("xdg-open", "xdg-open", item->path, (char *)NULL);
        }
        _exit(1);
    }
    return 0;
}

/* ─── XCB Window & Rendering ─────────────────────────────────────────────────── */

static xcb_atom_t intern_atom(xcb_connection_t *conn, const char *name)
{
    xcb_intern_atom_cookie_t c = xcb_intern_atom(conn, 0, (uint16_t)strlen(name), name);
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(conn, c, NULL);
    xcb_atom_t a = r ? r->atom : XCB_ATOM_NONE;
    free(r);
    return a;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    xcb_connection_t *conn = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(conn)) {
        fprintf(stderr, "nex-desktop: cannot connect to X server\n");
        return 1;
    }

    const xcb_setup_t *setup = xcb_get_setup(conn);
    xcb_screen_iterator_t iter = xcb_setup_roots_iterator(setup);
    xcb_screen_t *screen = iter.data;

    uint32_t width = screen->width_in_pixels;
    uint32_t height = screen->height_in_pixels;

    xcb_atom_t type_atom = intern_atom(conn, "_NET_WM_WINDOW_TYPE");
    xcb_atom_t desktop_atom = intern_atom(conn, "_NET_WM_WINDOW_TYPE_DESKTOP");

    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
    uint32_t vals[3] = {
        0x000000, /* transparent/black background */
        1,        /* override_redirect */
        XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS
    };

    xcb_window_t win = xcb_generate_id(conn);
    xcb_create_window(conn, XCB_COPY_FROM_PARENT, win, screen->root,
                      0, 0, (uint16_t)width, (uint16_t)height, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, mask, vals);

    if (type_atom != XCB_ATOM_NONE && desktop_atom != XCB_ATOM_NONE) {
        xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win,
                            type_atom, XCB_ATOM_ATOM, 32, 1, &desktop_atom);
    }

    xcb_map_window(conn, win);
    xcb_flush(conn);

    nex_desktop_item_list_t list;
    nex_desktop_scan(&list);

    xcb_gc_t gc_fg = xcb_generate_id(conn);
    uint32_t gc_vals[2] = { 0xffffff, 0x000000 };
    xcb_create_gc(conn, gc_fg, win, XCB_GC_FOREGROUND | XCB_GC_BACKGROUND, gc_vals);

    uint64_t last_click_time = 0;
    int last_click_idx = -1;

    int running = 1;
    while (running) {
        xcb_generic_event_t *ev = xcb_wait_for_event(conn);
        if (!ev) break;

        uint8_t type = ev->response_type & ~0x80;
        if (type == XCB_EXPOSE) {
            /* Render icon grid */
            int start_x = 24, start_y = 48;
            int cur_x = start_x, cur_y = start_y;

            for (int i = 0; i < list.count; i++) {
                /* Draw icon box placeholder */
                xcb_rectangle_t rect = { (int16_t)cur_x, (int16_t)cur_y, 48, 48 };
                xcb_poly_fill_rectangle(conn, win, gc_fg, 1, &rect);

                /* Draw item name below icon */
                char label[16];
                strncpy(label, list.items[i].name, sizeof(label) - 1);
                label[sizeof(label) - 1] = '\0';

                xcb_image_text_8(conn, (uint8_t)strlen(label), win, gc_fg,
                                 (int16_t)cur_x, (int16_t)(cur_y + 64), label);

                cur_y += NEX_DESKTOP_GRID_H;
                if (cur_y + NEX_DESKTOP_GRID_H > (int)height - 40) {
                    cur_y = start_y;
                    cur_x += NEX_DESKTOP_GRID_W;
                }
            }
            xcb_flush(conn);

        } else if (type == XCB_BUTTON_PRESS) {
            xcb_button_press_event_t *bp = (xcb_button_press_event_t *)ev;
            if (bp->detail == XCB_BUTTON_INDEX_1) {
                int start_x = 24, start_y = 48;
                int cur_x = start_x, cur_y = start_y;
                int clicked_idx = -1;

                for (int i = 0; i < list.count; i++) {
                    if (bp->event_x >= cur_x && bp->event_x < cur_x + NEX_DESKTOP_GRID_W &&
                        bp->event_y >= cur_y && bp->event_y < cur_y + NEX_DESKTOP_GRID_H) {
                        clicked_idx = i;
                        break;
                    }
                    cur_y += NEX_DESKTOP_GRID_H;
                    if (cur_y + NEX_DESKTOP_GRID_H > (int)height - 40) {
                        cur_y = start_y;
                        cur_x += NEX_DESKTOP_GRID_W;
                    }
                }

                if (clicked_idx >= 0) {
                    uint64_t now_ms = (uint64_t)time(NULL) * 1000;
                    if (clicked_idx == last_click_idx && (now_ms - last_click_time < 500)) {
                        nex_desktop_launch(&list.items[clicked_idx]);
                        last_click_idx = -1;
                    } else {
                        last_click_idx = clicked_idx;
                        last_click_time = now_ms;
                    }
                }
            }
        }
        free(ev);
    }

    xcb_free_gc(conn, gc_fg);
    xcb_destroy_window(conn, win);
    xcb_disconnect(conn);
    return 0;
}
