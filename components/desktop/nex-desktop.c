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
    snprintf(item->path, sizeof(item->path), "%s", path);

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
            snprintf(item->name, sizeof(item->name), "%s", val);
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
            snprintf(item->exec, sizeof(item->exec), "%s", cleaned);
        } else if (strcmp(key, "Icon") == 0) {
            snprintf(item->icon, sizeof(item->icon), "%s", val);
        }
    }
    fclose(f);

    if (item->name[0] == '\0') {
        const char *base = strrchr(path, '/');
        snprintf(item->name, sizeof(item->name), "%.250s", base ? base + 1 : path);
    }
}

int nex_desktop_scan(nex_desktop_item_list_t *list)
{
    if (!list) return -1;
    list->count = 0;

    const char *home = getenv("HOME");
    if (!home) home = "/root";

    /* Always populate core desktop system icons first */
    struct {
        const char *name;
        const char *exec;
    } defaults[] = {
        {"Home", "nex-fm"},
        {"Files", "nex-fm"},
        {"Settings", "nex-settings"},
        {"Terminal", "nex-terminal"}
    };

    for (size_t i = 0; i < sizeof(defaults)/sizeof(defaults[0]) && list->count < NEX_DESKTOP_MAX_FILES; i++) {
        nex_desktop_item_t *item = &list->items[list->count++];
        memset(item, 0, sizeof(*item));
        item->is_desktop_file = 1;
        snprintf(item->name, sizeof(item->name), "%s", defaults[i].name);
        snprintf(item->exec, sizeof(item->exec), "%s", defaults[i].exec);
        snprintf(item->path, sizeof(item->path), "%s", defaults[i].name);
    }

    char desktop_dir[512];
    snprintf(desktop_dir, sizeof(desktop_dir), "%s/Desktop", home);

    struct stat st;
    if (stat(desktop_dir, &st) != 0) {
        mkdir(desktop_dir, 0755);
        return list->count;
    }

    DIR *d = opendir(desktop_dir);
    if (!d) return list->count;

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
            snprintf(item->name, sizeof(item->name), "%s", ent->d_name);
            snprintf(item->path, sizeof(item->path), "%s", path);
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

/* ─── Desktop Context Menu ─────────────────────────────────────────────────── */

static void show_context_menu(xcb_connection_t *conn, xcb_screen_t *screen, int x, int y, nex_desktop_item_list_t *list)
{
    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
    uint32_t vals[3] = {
        0x1e1e2e, /* Dark palette */
        1,        /* override_redirect */
        XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS |
        XCB_EVENT_MASK_POINTER_MOTION
    };

    int menu_w = 210;
    int item_h = 28;
    int count = 5;
    int menu_h = count * item_h + 8;

    if (x + menu_w > (int)screen->width_in_pixels) x = (int)screen->width_in_pixels - menu_w;
    if (y + menu_h > (int)screen->height_in_pixels) y = (int)screen->height_in_pixels - menu_h;

    xcb_window_t menu_win = xcb_generate_id(conn);
    xcb_create_window(conn, XCB_COPY_FROM_PARENT, menu_win, screen->root,
                      (int16_t)x, (int16_t)y, (uint16_t)menu_w, (uint16_t)menu_h, 1,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, mask, vals);

    uint32_t raise_vals[1] = { XCB_STACK_MODE_ABOVE };
    xcb_configure_window(conn, menu_win, XCB_CONFIG_WINDOW_STACK_MODE, raise_vals);

    xcb_map_window(conn, menu_win);
    xcb_flush(conn);

    xcb_grab_pointer(conn, 0, menu_win, XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_POINTER_MOTION,
                     XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE, XCB_NONE, XCB_CURRENT_TIME);

    xcb_gc_t gc_bg = xcb_generate_id(conn);
    uint32_t gc_bg_vals[2] = { 0x1e1e2e, 0x1e1e2e };
    xcb_create_gc(conn, gc_bg, menu_win, XCB_GC_FOREGROUND | XCB_GC_BACKGROUND, gc_bg_vals);

    xcb_gc_t gc_fg = xcb_generate_id(conn);
    uint32_t gc_fg_vals[2] = { 0xcdd6f4, 0x1e1e2e };
    xcb_create_gc(conn, gc_fg, menu_win, XCB_GC_FOREGROUND | XCB_GC_BACKGROUND, gc_fg_vals);

    xcb_gc_t gc_hover = xcb_generate_id(conn);
    uint32_t gc_hover_vals[2] = { 0x5b8dd9, 0x5b8dd9 };
    xcb_create_gc(conn, gc_hover, menu_win, XCB_GC_FOREGROUND | XCB_GC_BACKGROUND, gc_hover_vals);

    const char *items[] = {
        "  Terminal",
        "  Files",
        "  Settings",
        "  New Folder",
        "  About NexDE"
    };

    int hovered = -1;
    int menu_running = 1;

    while (menu_running) {
        xcb_rectangle_t bg_rect = { 0, 0, (uint16_t)menu_w, (uint16_t)menu_h };
        xcb_poly_fill_rectangle(conn, menu_win, gc_bg, 1, &bg_rect);

        for (int i = 0; i < count; i++) {
            int iy = 4 + i * item_h;
            if (i == hovered) {
                xcb_rectangle_t h_rect = { 4, (int16_t)iy, (uint16_t)(menu_w - 8), (uint16_t)(item_h - 2) };
                xcb_poly_fill_rectangle(conn, menu_win, gc_hover, 1, &h_rect);
            }
            xcb_image_text_8(conn, (uint8_t)strlen(items[i]), menu_win, gc_fg,
                             12, (int16_t)(iy + 18), items[i]);
        }
        xcb_flush(conn);

        xcb_generic_event_t *ev = xcb_wait_for_event(conn);
        if (!ev) break;

        uint8_t type = ev->response_type & ~0x80;
        if (type == XCB_MOTION_NOTIFY) {
            xcb_motion_notify_event_t *mn = (xcb_motion_notify_event_t *)ev;
            int rx = mn->root_x - x;
            int ry = mn->root_y - y;
            if (rx >= 0 && rx < menu_w && ry >= 4 && ry < menu_h - 4) {
                hovered = (ry - 4) / item_h;
                if (hovered >= count) hovered = -1;
            } else {
                hovered = -1;
            }
        } else if (type == XCB_BUTTON_PRESS) {
            xcb_button_press_event_t *bp = (xcb_button_press_event_t *)ev;
            int rx = bp->root_x - x;
            int ry = bp->root_y - y;

            if (rx < 0 || rx >= menu_w || ry < 0 || ry >= menu_h) {
                menu_running = 0;
            } else if (bp->detail == XCB_BUTTON_INDEX_1) {
                int selected = (ry - 4) / item_h;
                if (selected >= 0 && selected < count) {
                    menu_running = 0;
                    pid_t pid = fork();
                    if (pid == 0) {
                        setsid();
                        for (int fd = 3; fd < 256; fd++) close(fd);
                        switch (selected) {
                            case 0: execlp("nex-terminal", "nex-terminal", NULL); break;
                            case 1: execlp("nex-fm", "nex-fm", NULL); break;
                            case 2: execlp("nex-settings", "nex-settings", NULL); break;
                            case 3: {
                                const char *home = getenv("HOME");
                                char folder_path[512];
                                snprintf(folder_path, sizeof(folder_path), "%s/Desktop/New_Folder", home ? home : "/root");
                                mkdir(folder_path, 0755);
                                break;
                            }
                            case 4: execlp("nex-notify", "nex-notify", "NexDE", "Nex Desktop Environment v0.1.0", "5000", NULL); break;
                        }
                        _exit(0);
                    }
                    if (selected == 3) {
                        nex_desktop_scan(list);
                    }
                }
            }
        }
        free(ev);
    }

    xcb_ungrab_pointer(conn, XCB_CURRENT_TIME);
    xcb_free_gc(conn, gc_bg);
    xcb_free_gc(conn, gc_fg);
    xcb_free_gc(conn, gc_hover);
    xcb_destroy_window(conn, menu_win);
    xcb_flush(conn);
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

    /* Lower desktop below all other windows including the panel */
    uint32_t lower_vals[1] = { XCB_STACK_MODE_BELOW };
    xcb_configure_window(conn, win, XCB_CONFIG_WINDOW_STACK_MODE, lower_vals);

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
            } else if (bp->detail == XCB_BUTTON_INDEX_3) {
                show_context_menu(conn, screen, bp->event_x, bp->event_y, &list);
            }
        }
        free(ev);
    }

    xcb_free_gc(conn, gc_fg);
    xcb_destroy_window(conn, win);
    xcb_disconnect(conn);
    return 0;
}
