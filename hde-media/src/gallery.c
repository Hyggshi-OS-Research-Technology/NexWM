/* gallery.c — the list of pictures of a folder, the order, and the state of the viewer: plain C + POSIX, no GTK and no
 * glib, because all of it is tested without a display by tests/media-test.c (`make check-unit`). The window that draws
 * the picture is viewer.c; this file is the part that is easy to get wrong (the order, the wrap-around, the zoom
 * steps, the slideshow clock).
 */
#define _POSIX_C_SOURCE 200809L    /* opendir/readdir/stat/strcasecmp: HDE is built with -std=c11, POSIX is not implied */

#include "media.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

/* the extensions the viewer opens. gdk-pixbuf decides in the end (an SVG needs librsvg, a JPEG its loader): this list
 * only says "this file could be a picture", so that a folder listing can be filtered without opening every file. */
static const char *const PICTURES[] = {
    "png", "jpg", "jpeg", "jpe", "jfif", "gif", "bmp", "webp", "tif", "tiff", "svg", "svgz", "ico",
    "avif", "jxl", "heic", "heif", "ppm", "pgm", "pbm", "pnm", "tga", "xpm", "qoi", "dds", "wbmp",
    NULL
};

/* the ladder of the +/- keys (each step is roughly a third bigger than the last one) */
static const double ZOOM_LADDER[] = {
    0.05, 0.10, 0.15, 0.25, 0.33, 0.50, 0.67, 0.75, 1.00, 1.25, 1.50, 2.00, 3.00, 4.00, 6.00, 8.00, 12.00, 16.00
};
#define ZOOM_N (int)(sizeof ZOOM_LADDER / sizeof ZOOM_LADDER[0])

/* how deep hde_media_list_scan follows sub-folders (a shortcut loop would otherwise never end) */
#define SCAN_MAX_DEPTH 8

/* ------------------------------------------------------------------ strings */

char *hde_media_strdup(const char *s)
{
    if (!s) s = "";
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

const char *hde_media_basename(const char *path)
{
    if (!path) return "";
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

char *hde_media_dirname(const char *path)
{
    if (!path || !*path) return hde_media_strdup(".");
    const char *slash = strrchr(path, '/');
    if (!slash) return hde_media_strdup(".");
    if (slash == path) return hde_media_strdup("/");
    size_t n = (size_t)(slash - path);
    char *p = malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, path, n);
    p[n] = '\0';
    return p;
}

const char *hde_media_extension(const char *name)
{
    if (!name) return "";
    const char *dot = strrchr(hde_media_basename(name), '.');
    return dot && dot[1] ? dot + 1 : "";
}

int hde_media_is_picture(const char *name)
{
    const char *ext = hde_media_extension(name);
    if (!*ext) return 0;
    for (int i = 0; PICTURES[i]; i++) {
        const char *p = PICTURES[i];
        const char *e = ext;
        while (*p && *e && tolower((unsigned char)*p) == tolower((unsigned char)*e)) { p++; e++; }
        if (!*p && !*e) return 1;
    }
    return 0;
}

/* the natural order: "photo 2.png" comes before "photo 10.png" (a plain strcmp would put 10 first) */
int hde_media_natural_cmp(const char *a, const char *b)
{
    if (!a) a = "";
    if (!b) b = "";
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            const char *pa = a, *pb = b;
            while (*pa == '0') pa++;
            while (*pb == '0') pb++;
            const char *ea = pa, *eb = pb;
            while (isdigit((unsigned char)*ea)) ea++;
            while (isdigit((unsigned char)*eb)) eb++;
            size_t la = (size_t)(ea - pa), lb = (size_t)(eb - pb);
            if (la != lb) return la < lb ? -1 : 1;        /* more digits: the bigger number */
            int c = strncmp(pa, pb, la);
            if (c) return c < 0 ? -1 : 1;
            a = ea;
            b = eb;
            continue;
        }
        int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
        if (ca != cb) return ca < cb ? -1 : 1;
        a++;
        b++;
    }
    if (*a) return 1;
    if (*b) return -1;
    return 0;
}

size_t hde_media_human_size(char *buf, size_t n, uint64_t bytes)
{
    static const char *const units[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; u++; }
    int wrote;
    if (u == 0) wrote = snprintf(buf, n, "%llu B", (unsigned long long)bytes);
    else if (v < 10.0) wrote = snprintf(buf, n, "%.1f %s", v, units[u]);
    else wrote = snprintf(buf, n, "%.0f %s", v, units[u]);
    return wrote < 0 ? 0 : (size_t)wrote;
}

const char *hde_media_sort_name(HdeMediaSort how)
{
    switch (how) {
    case HDE_MEDIA_SORT_DATE: return "date";
    case HDE_MEDIA_SORT_SIZE: return "size";
    default:                  return "name";
    }
}

/* ------------------------------------------------------------------ the list */

void hde_media_list_init(HdeMediaList *l)
{
    l->items = NULL;
    l->n = l->cap = 0;
}

void hde_media_list_clear(HdeMediaList *l)
{
    for (size_t i = 0; i < l->n; i++) free(l->items[i].path);
    l->n = 0;
}

void hde_media_list_free(HdeMediaList *l)
{
    hde_media_list_clear(l);
    free(l->items);
    l->items = NULL;
    l->cap = 0;
}

int hde_media_list_add(HdeMediaList *l, const char *path)
{
    if (l->n == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 16;
        HdeMediaItem *items = realloc(l->items, cap * sizeof *items);
        if (!items) return -1;
        l->items = items;
        l->cap = cap;
    }
    char *copy = hde_media_strdup(path);
    if (!copy) return -1;
    struct stat st;
    int64_t mtime = 0;
    uint64_t size = 0;
    if (stat(path, &st) == 0) {
        mtime = (int64_t)st.st_mtime;
        size = (uint64_t)st.st_size;
    }
    l->items[l->n].path = copy;
    l->items[l->n].size = size;
    l->items[l->n].mtime = mtime;
    l->n++;
    return 0;
}

long hde_media_list_find(const HdeMediaList *l, const char *path)
{
    for (size_t i = 0; i < l->n; i++)
        if (!strcmp(l->items[i].path, path)) return (long)i;
    return -1;
}

static HdeMediaSort g_sort;    /* qsort cannot take an argument: the sorts below read this (single-threaded) */

static int cmp_name(const void *A, const void *B)
{
    const HdeMediaItem *a = A, *b = B;
    int c = hde_media_natural_cmp(hde_media_basename(a->path), hde_media_basename(b->path));
    if (c) return c;
    return hde_media_natural_cmp(a->path, b->path);      /* two folders, same file name: keep the order stable */
}

static int cmp_date(const void *A, const void *B)
{
    const HdeMediaItem *a = A, *b = B;
    if (a->mtime != b->mtime) return a->mtime < b->mtime ? -1 : 1;      /* oldest first: a timeline */
    return cmp_name(A, B);
}

static int cmp_size(const void *A, const void *B)
{
    const HdeMediaItem *a = A, *b = B;
    if (a->size != b->size) return a->size < b->size ? -1 : 1;          /* smallest first */
    return cmp_name(A, B);
}

void hde_media_list_sort(HdeMediaList *l, HdeMediaSort how)
{
    g_sort = how;
    int (*cmp)(const void *, const void *) = cmp_name;
    if (g_sort == HDE_MEDIA_SORT_DATE) cmp = cmp_date;
    else if (g_sort == HDE_MEDIA_SORT_SIZE) cmp = cmp_size;
    if (l->n > 1) qsort(l->items, l->n, sizeof *l->items, cmp);
}

static long scan_into(HdeMediaList *l, const char *dir, HdeMediaSort how, int depth, int *unreadable)
{
    DIR *d = opendir(dir);
    if (!d) { *unreadable = 1; return 0; }
    long added = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (e->d_name[0] == '.') continue;               /* hidden files and folders are not pictures */
        size_t need = strlen(dir) + strlen(e->d_name) + 2;
        char *path = malloc(need);
        if (!path) break;
        snprintf(path, need, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) { free(path); continue; }
        if (S_ISDIR(st.st_mode)) {
            if (depth < SCAN_MAX_DEPTH) added += scan_into(l, path, how, depth + 1, unreadable);
            free(path);
            continue;
        }
        if (hde_media_is_picture(path)) {
            if (hde_media_list_add(l, path) == 0) added++;
        }
        free(path);
    }
    closedir(d);
    return added;
}

long hde_media_list_scan(HdeMediaList *l, const char *dir, HdeMediaSort how, int recursive)
{
    int unreadable = 0;
    long added;
    if (recursive) {
        added = scan_into(l, dir, how, 1, &unreadable);
    } else {
        DIR *d = opendir(dir);
        if (!d) return -1;
        closedir(d);
        added = scan_into(l, dir, how, SCAN_MAX_DEPTH, &unreadable);   /* depth at the cap: no sub-folders */
    }
    if (unreadable && added == 0) return -1;
    hde_media_list_sort(l, how);
    return added;
}

/* ------------------------------------------------------------------ the state of the viewer */

void hde_media_view_init(HdeMediaView *v, HdeMediaSort sort, int recursive, double interval)
{
    hde_media_list_init(&v->list);
    v->index = 0;
    v->zoom = HDE_MEDIA_ZOOM_FIT;
    v->rotation = 0;
    v->slideshow = 0;
    v->interval = interval > 0.5 ? interval : 5.0;
    v->sort = sort;
    v->recursive = recursive;
    v->last_tick_ms = 0;
    v->shown = 0;
}

void hde_media_view_free(HdeMediaView *v)
{
    hde_media_list_free(&v->list);
}

static void view_use(HdeMediaView *v, HdeMediaList *list, long index)
{
    hde_media_list_free(&v->list);
    v->list = *list;
    hde_media_list_init(list);      /* the list now belongs to the view */
    v->index = index < 0 ? 0 : index;
    v->zoom = HDE_MEDIA_ZOOM_FIT;
    v->rotation = 0;
    v->shown++;
}

int hde_media_view_open(HdeMediaView *v, const char *path)
{
    if (!path || !*path) return -1;
    struct stat st;
    if (stat(path, &st) != 0) return -1;

    HdeMediaList list;
    hde_media_list_init(&list);
    long index = 0;

    if (S_ISDIR(st.st_mode)) {
        if (hde_media_list_scan(&list, path, v->sort, v->recursive) <= 0) {
            hde_media_list_free(&list);
            return -1;
        }
    } else {
        if (!hde_media_is_picture(path)) {
            hde_media_list_free(&list);
            return -1;
        }
        char *dir = hde_media_dirname(path);
        if (dir) hde_media_list_scan(&list, dir, v->sort, v->recursive);
        long at = hde_media_list_find(&list, path);
        if (at < 0) {
            /* a folder that could not be read, or a file whose extension is a picture but which the scan skipped:
             * this one picture is still worth showing */
            if (hde_media_list_add(&list, path) != 0) {
                free(dir);
                hde_media_list_free(&list);
                return -1;
            }
            hde_media_list_sort(&list, v->sort);
            at = hde_media_list_find(&list, path);
        }
        free(dir);
        index = at < 0 ? 0 : at;
    }
    if (list.n == 0) {
        hde_media_list_free(&list);
        return -1;
    }
    view_use(v, &list, index);
    return 0;
}

int hde_media_view_open_many(HdeMediaView *v, char *const *paths, int n)
{
    HdeMediaList list;
    hde_media_list_init(&list);
    for (int i = 0; i < n; i++) {
        if (!paths[i]) continue;
        if (!hde_media_is_picture(paths[i])) continue;
        hde_media_list_add(&list, paths[i]);
    }
    if (list.n == 0) {
        hde_media_list_free(&list);
        return -1;
    }
    view_use(v, &list, 0);
    return 0;
}

int hde_media_view_step(HdeMediaView *v, int delta)
{
    if (v->list.n == 0) return -1;
    long n = (long)v->list.n;
    v->index = ((v->index + delta) % n + n) % n;      /* wraps around, both ways */
    v->zoom = HDE_MEDIA_ZOOM_FIT;
    v->rotation = 0;
    v->shown++;
    return 0;
}

int hde_media_view_goto(HdeMediaView *v, long index)
{
    if (v->list.n == 0 || index < 0 || (size_t)index >= v->list.n) return -1;
    v->index = index;
    v->zoom = HDE_MEDIA_ZOOM_FIT;
    v->rotation = 0;
    v->shown++;
    return 0;
}

void hde_media_view_set_zoom(HdeMediaView *v, double zoom)
{
    v->zoom = zoom > 0.0 ? zoom : HDE_MEDIA_ZOOM_FIT;
}

const char *hde_media_view_current(const HdeMediaView *v)
{
    if (v->list.n == 0 || v->index < 0 || (size_t)v->index >= v->list.n) return NULL;
    return v->list.items[v->index].path;
}

char *hde_media_view_dir(const HdeMediaView *v)
{
    const char *cur = hde_media_view_current(v);
    return cur ? hde_media_dirname(cur) : NULL;
}

double hde_media_zoom_step(double zoom, int direction)
{
    if (direction == 0) return HDE_MEDIA_ZOOM_FIT;                       /* 0 (the "fit" key) */
    if (zoom <= 0.0) return direction > 0 ? 1.00 : 0.50;                 /* from fit: 100 % or 50 % */
    int at = 0;
    for (int i = 0; i < ZOOM_N; i++)
        if (ZOOM_LADDER[i] <= zoom * 1.0001) at = i;
    at += direction > 0 ? 1 : -1;
    if (at < 0) at = 0;
    if (at >= ZOOM_N) at = ZOOM_N - 1;
    return ZOOM_LADDER[at];
}

double hde_media_view_zoom(HdeMediaView *v, int direction)
{
    v->zoom = hde_media_zoom_step(v->zoom, direction);
    return v->zoom;
}

int hde_media_view_rotate(HdeMediaView *v, int degrees)
{
    int angle = (v->rotation + degrees) % 360;
    if (angle < 0) angle += 360;
    v->rotation = angle;
    return v->rotation;
}

void hde_media_view_slideshow(HdeMediaView *v, int on, int64_t now_ms)
{
    v->slideshow = on ? 1 : 0;
    v->last_tick_ms = now_ms;      /* switching it on (or off) starts the interval again */
}

int hde_media_view_tick(HdeMediaView *v, int64_t now_ms)
{
    if (!v->slideshow) return 0;
    int64_t interval_ms = (int64_t)(v->interval * 1000.0 + 0.5);
    if (interval_ms < 500) interval_ms = 500;
    if (now_ms - v->last_tick_ms < interval_ms) return 0;
    v->last_tick_ms = now_ms;
    return 1;
}
