#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

/*
 * nex-desktop.c - NexDesktop
 * Desktop Icon Manager for NexWM / Hyggshi OS
 */

#include "nex-desktop.h"
#include "../panel/nex_icon.h"

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

/* ── Colors (Catppuccin Mocha palette) ────────────────────────────────────── */
#define CLR_DESK_BG          0x181825  /* Dark background fallback */
#define CLR_TEXT_FG          0xcdd6f4  /* Text color */
#define CLR_LABEL_BG         0x1e1e2e  /* Pill bg for label */
#define CLR_SEL_BG           0x313244  /* Selection highlight bg */
#define CLR_SEL_BORDER       0x5b8dd9  /* Selection highlight border */
#define CLR_MENU_BG          0x181825  /* Context menu bg */
#define CLR_MENU_FG          0xcdd6f4  /* Context menu text */
#define CLR_MENU_HOVER       0x313244  /* Context menu hover item */

/* ── Desktop Scanning ───────────────────────────────────────────────────────── */

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

    /* System shortcuts */
    struct {
        const char *name;
        const char *exec;
        const char *icon;
    } defaults[] = {
        {"Home", "nex-fm", "user-home"},
        {"Files", "nex-fm", "system-file-manager"},
        {"Settings", "nex-settings", "preferences-system"},
        {"Terminal", "nex-terminal", "utilities-terminal"}
    };

    for (size_t i = 0; i < sizeof(defaults)/sizeof(defaults[0]) && list->count < NEX_DESKTOP_MAX_FILES; i++) {
        nex_desktop_item_t *item = &list->items[list->count++];
        memset(item, 0, sizeof(*item));
        item->is_desktop_file = 1;
        snprintf(item->name, sizeof(item->name), "%s", defaults[i].name);
        snprintf(item->exec, sizeof(item->exec), "%s", defaults[i].exec);
        snprintf(item->icon, sizeof(item->icon), "%s", defaults[i].icon);
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

            struct stat fst;
            if (stat(path, &fst) == 0 && S_ISDIR(fst.st_mode)) {
                snprintf(item->icon, sizeof(item->icon), "folder");
            } else {
                snprintf(item->icon, sizeof(item->icon), "text-x-generic");
            }
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

/*
 * Read the wallpaper pixmap published by nex-wallpaper.
 * We deliberately use XCB_ATOM_ANY here instead of assuming that every
 * wallpaper setter uses exactly the same property type.
 */
static void fill_rect(xcb_connection_t *conn, xcb_drawable_t win, uint32_t color,
                      int x, int y, int w, int h);

static xcb_pixmap_t get_root_wallpaper_pixmap(xcb_connection_t *conn,
                                               xcb_window_t root,
                                               xcb_atom_t root_pixmap_atom)
{
    if (root_pixmap_atom == XCB_ATOM_NONE)
        return XCB_NONE;

    xcb_get_property_cookie_t cookie =
        xcb_get_property(conn, 0, root, root_pixmap_atom,
                         XCB_GET_PROPERTY_TYPE_ANY, 0, 1);

    xcb_get_property_reply_t *reply =
        xcb_get_property_reply(conn, cookie, NULL);

    if (!reply)
        return XCB_NONE;

    xcb_pixmap_t pixmap = XCB_NONE;

    if (xcb_get_property_value_length(reply) >= (int)sizeof(xcb_pixmap_t)) {
        memcpy(&pixmap, xcb_get_property_value(reply), sizeof(pixmap));
    }

    free(reply);
    return pixmap;
}

/*
 * Paint the current root wallpaper into our desktop window.
 *
 * We do NOT rely on XCB_CW_BACK_PIXMAP here. The desktop window is an
 * actual fullscreen window, so explicitly copying the root wallpaper
 * pixmap makes the result deterministic and also lets us redraw it
 * immediately after _XROOTPMAP_ID changes.
 */
static int paint_root_wallpaper(xcb_connection_t *conn,
                                 xcb_window_t win,
                                 xcb_window_t root,
                                 xcb_atom_t root_pixmap_atom,
                                 uint8_t root_depth,
                                 uint32_t width,
                                 uint32_t height)
{
    xcb_pixmap_t pixmap =
        get_root_wallpaper_pixmap(conn, root, root_pixmap_atom);

    if (pixmap == XCB_NONE) {
        fill_rect(conn, win, CLR_DESK_BG, 0, 0, (int)width, (int)height);
        return 0;
    }

    /* Validate the drawable before issuing CopyArea. */
    xcb_get_geometry_cookie_t geo_cookie =
        xcb_get_geometry(conn, pixmap);
    xcb_get_geometry_reply_t *geo =
        xcb_get_geometry_reply(conn, geo_cookie, NULL);

    if (!geo || geo->depth != root_depth) {
        free(geo);
        fill_rect(conn, win, CLR_DESK_BG, 0, 0, (int)width, (int)height);
        return 0;
    }

    uint16_t copy_w = geo->width < width ? geo->width : (uint16_t)width;
    uint16_t copy_h = geo->height < height ? geo->height : (uint16_t)height;

    /* Clear/fill first so a smaller pixmap never leaves stale pixels. */
    fill_rect(conn, win, CLR_DESK_BG, 0, 0, (int)width, (int)height);

    xcb_gcontext_t gc = xcb_generate_id(conn);
    uint32_t gc_values[2] = { 0xffffff, 0 };
    xcb_create_gc(conn, gc, win, XCB_GC_FOREGROUND | XCB_GC_BACKGROUND, gc_values);

    xcb_copy_area(conn, pixmap, win, gc,
                  0, 0, 0, 0, copy_w, copy_h);

    xcb_free_gc(conn, gc);
    free(geo);
    return 1;
}

static xcb_gc_t make_gc(xcb_connection_t *conn, xcb_window_t win, uint32_t fg, uint32_t bg)
{
    xcb_gc_t gc = xcb_generate_id(conn);
    uint32_t mask = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND;
    uint32_t vals[2] = { fg, bg };
    xcb_create_gc(conn, gc, win, mask, vals);
    return gc;
}

static void fill_rect(xcb_connection_t *conn, xcb_drawable_t win, uint32_t color, int x, int y, int w, int h)
{
    xcb_gc_t gc = make_gc(conn, win, color, color);
    xcb_rectangle_t r = { (int16_t)x, (int16_t)y, (uint16_t)w, (uint16_t)h };
    xcb_poly_fill_rectangle(conn, win, gc, 1, &r);
    xcb_free_gc(conn, gc);
}

static void draw_text(xcb_connection_t *conn, xcb_drawable_t win, int x, int y, const char *text, uint32_t fg, uint32_t bg)
{
    xcb_gc_t gc = make_gc(conn, win, fg, bg);
    size_t len = strlen(text);
    if (len > 255) len = 255;
    xcb_image_text_8(conn, (uint8_t)len, win, gc, (int16_t)x, (int16_t)y, text);
    xcb_free_gc(conn, gc);
}

/* Render all desktop icons */
static void render_desktop(xcb_connection_t *conn, xcb_window_t win, uint32_t height,
                           nex_desktop_item_list_t *list, int selected_idx,
                           nex_icon_t *icon_home, nex_icon_t *icon_fm, nex_icon_t *icon_sett,
                           nex_icon_t *icon_term, nex_icon_t *icon_fold, nex_icon_t *icon_file)
{
    int start_x = 24, start_y = 56;
    int cur_x = start_x, cur_y = start_y;

    for (int i = 0; i < list->count; i++) {
        int is_sel = (i == selected_idx);

        /* Selection box */
        if (is_sel) {
            fill_rect(conn, win, CLR_SEL_BG, cur_x - 4, cur_y - 4,
                      NEX_DESKTOP_GRID_W - 8, NEX_DESKTOP_GRID_H - 8);
            xcb_gc_t gc_bdr = make_gc(conn, win, CLR_SEL_BORDER, CLR_SEL_BORDER);
            xcb_rectangle_t r = { (int16_t)(cur_x - 4), (int16_t)(cur_y - 4),
                                  (uint16_t)(NEX_DESKTOP_GRID_W - 8),
                                  (uint16_t)(NEX_DESKTOP_GRID_H - 8) };
            xcb_poly_rectangle(conn, win, gc_bdr, 1, &r);
            xcb_free_gc(conn, gc_bdr);
        }

        /* Choose PNG icon */
        nex_icon_t *ic = NULL;
        if (strcmp(list->items[i].icon, "user-home") == 0) ic = icon_home;
        else if (strcmp(list->items[i].icon, "system-file-manager") == 0) ic = icon_fm;
        else if (strcmp(list->items[i].icon, "preferences-system") == 0) ic = icon_sett;
        else if (strcmp(list->items[i].icon, "utilities-terminal") == 0) ic = icon_term;
        else if (strcmp(list->items[i].icon, "folder") == 0) ic = icon_fold;
        else ic = icon_file;

        int icon_x = cur_x + (NEX_DESKTOP_GRID_W - 16 - NEX_DESKTOP_ICON_SIZE) / 2;

        if (ic) {
            xcb_gc_t gc_dummy = make_gc(conn, win, 0xffffff, CLR_DESK_BG);
            nex_icon_draw(conn, win, gc_dummy, ic, icon_x, cur_y, is_sel ? CLR_SEL_BG : CLR_DESK_BG);
            xcb_free_gc(conn, gc_dummy);
        } else {
            fill_rect(conn, win, CLR_LABEL_BG, icon_x, cur_y, NEX_DESKTOP_ICON_SIZE, NEX_DESKTOP_ICON_SIZE);
            char initial[2] = { list->items[i].name[0], '\0' };
            draw_text(conn, win, icon_x + 18, cur_y + 30, initial, CLR_TEXT_FG, CLR_LABEL_BG);
        }

        /* Item label */
        char label[14];
        strncpy(label, list->items[i].name, sizeof(label) - 1);
        label[sizeof(label) - 1] = '\0';

        int label_len = (int)strlen(label);
        int label_x = cur_x + (NEX_DESKTOP_GRID_W - 16 - label_len * 7) / 2;
        if (label_x < cur_x) label_x = cur_x;

        uint32_t text_bg = is_sel ? CLR_SEL_BG : CLR_LABEL_BG;
        fill_rect(conn, win, text_bg, label_x - 4, cur_y + 54, label_len * 7 + 8, 18);
        draw_text(conn, win, label_x, cur_y + 67, label, CLR_TEXT_FG, text_bg);

        cur_y += NEX_DESKTOP_GRID_H;
        if (cur_y + NEX_DESKTOP_GRID_H > (int)height - 40) {
            cur_y = start_y;
            cur_x += NEX_DESKTOP_GRID_W;
        }
    }
    xcb_flush(conn);
}

/* ─── Context Menu ───────────────────────────────────────────────────────────── */

static void show_context_menu(xcb_connection_t *conn, xcb_screen_t *screen, int x, int y, nex_desktop_item_list_t *list)
{
    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
    uint32_t vals[3] = {
        CLR_MENU_BG,
        1,
        XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_POINTER_MOTION
    };

    int menu_w = 210;
    int item_h = 32;
    int count = 6;
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

    const char *items[] = {
        "  Open Terminal",
        "  Open File Manager",
        "  Desktop Settings",
        "  Create New Folder",
        "  Create New File",
        "  About NexDE"
    };

    int hovered = -1;
    int menu_running = 1;

    while (menu_running) {
        fill_rect(conn, menu_win, CLR_MENU_BG, 0, 0, menu_w, menu_h);

        for (int i = 0; i < count; i++) {
            int iy = 4 + i * item_h;
            if (i == hovered) {
                fill_rect(conn, menu_win, CLR_MENU_HOVER, 4, iy, menu_w - 8, item_h - 2);
                draw_text(conn, menu_win, 12, iy + 20, items[i], 0xffffff, CLR_MENU_HOVER);
            } else {
                draw_text(conn, menu_win, 12, iy + 20, items[i], CLR_MENU_FG, CLR_MENU_BG);
            }
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
                            case 4: {
                                const char *home = getenv("HOME");
                                char file_path[512];
                                snprintf(file_path, sizeof(file_path), "%s/Desktop/new_document.txt", home ? home : "/root");
                                int fd = open(file_path, O_CREAT | O_WRONLY, 0644);
                                if (fd >= 0) close(fd);
                                break;
                            }
                            case 5: execlp("nex-notify", "nex-notify", "NexDE", "Nex Desktop Environment v0.1.0", "5000", NULL); break;
                        }
                        _exit(0);
                    }
                    if (selected == 3 || selected == 4) {
                        nex_desktop_scan(list);
                    }
                }
            }
        }
        free(ev);
    }

    xcb_ungrab_pointer(conn, XCB_CURRENT_TIME);
    xcb_destroy_window(conn, menu_win);
    xcb_flush(conn);
}

/* ─── Main Desktop Window ────────────────────────────────────────────────────── */

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
    xcb_atom_t root_pixmap_atom = intern_atom(conn, "_XROOTPMAP_ID");

    /* Watch the root window so Apply in Settings causes an immediate redraw. */
    if (root_pixmap_atom != XCB_ATOM_NONE) {
        uint32_t root_event_mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
        xcb_change_window_attributes(conn, screen->root,
                                     XCB_CW_EVENT_MASK, &root_event_mask);
    }

    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
    uint32_t vals[3] = {
        CLR_DESK_BG,
        1,
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

    uint32_t lower_vals[1] = { XCB_STACK_MODE_BELOW };
    xcb_configure_window(conn, win, XCB_CONFIG_WINDOW_STACK_MODE, lower_vals);
    xcb_flush(conn);

    /* Paint the current wallpaper before drawing desktop icons. */
    paint_root_wallpaper(conn, win, screen->root, root_pixmap_atom,
                         screen->root_depth, width, height);
    xcb_flush(conn);

    nex_desktop_item_list_t list;
    nex_desktop_scan(&list);

    /* Load PNG icons */
    nex_icon_t *icon_home = nex_icon_load("user-home", 48);
    nex_icon_t *icon_fm   = nex_icon_load("system-file-manager", 48);
    nex_icon_t *icon_sett = nex_icon_load("preferences-system", 48);
    nex_icon_t *icon_term = nex_icon_load("utilities-terminal", 48);
    nex_icon_t *icon_fold = nex_icon_load("folder", 48);
    nex_icon_t *icon_file = nex_icon_load("text-x-generic", 48);

    int selected_idx = -1;
    uint64_t last_click_time = 0;
    int last_click_idx = -1;

    /* Render initial desktop frame immediately! */
    paint_root_wallpaper(conn, win, screen->root, root_pixmap_atom,
                         screen->root_depth, width, height);
    render_desktop(conn, win, height, &list, selected_idx,
                   icon_home, icon_fm, icon_sett, icon_term, icon_fold, icon_file);

    int running = 1;
    while (running) {
        xcb_generic_event_t *ev = xcb_wait_for_event(conn);
        if (!ev) break;

        uint8_t type = ev->response_type & ~0x80;
        if (type == XCB_PROPERTY_NOTIFY) {
            xcb_property_notify_event_t *pn =
                (xcb_property_notify_event_t *)ev;

            if (pn->window == screen->root && pn->atom == root_pixmap_atom) {
                /* Wallpaper was replaced by Settings -> copy the new pixmap. */
                paint_root_wallpaper(conn, win, screen->root, root_pixmap_atom,
                                     screen->root_depth, width, height);
                render_desktop(conn, win, height, &list, selected_idx,
                               icon_home, icon_fm, icon_sett, icon_term,
                               icon_fold, icon_file);
                xcb_flush(conn);
            }
        } else if (type == XCB_EXPOSE) {
            paint_root_wallpaper(conn, win, screen->root, root_pixmap_atom,
                                 screen->root_depth, width, height);
            render_desktop(conn, win, height, &list, selected_idx,
                           icon_home, icon_fm, icon_sett, icon_term, icon_fold, icon_file);

        } else if (type == XCB_BUTTON_PRESS) {
            xcb_button_press_event_t *bp = (xcb_button_press_event_t *)ev;
            if (bp->detail == XCB_BUTTON_INDEX_1) {
                int start_x = 24, start_y = 56;
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

                selected_idx = clicked_idx;

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

                paint_root_wallpaper(conn, win, screen->root, root_pixmap_atom,
                                     screen->root_depth, width, height);
                render_desktop(conn, win, height, &list, selected_idx,
                               icon_home, icon_fm, icon_sett, icon_term, icon_fold, icon_file);

            } else if (bp->detail == XCB_BUTTON_INDEX_3) {
                show_context_menu(conn, screen, bp->event_x, bp->event_y, &list);
            }
        }
        free(ev);
    }

    nex_icon_free(icon_home);
    nex_icon_free(icon_fm);
    nex_icon_free(icon_sett);
    nex_icon_free(icon_term);
    nex_icon_free(icon_fold);
    nex_icon_free(icon_file);

    xcb_destroy_window(conn, win);
    xcb_disconnect(conn);
    return 0;
}
