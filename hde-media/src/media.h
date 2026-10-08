/* media.h — hde-media: the pictures, the music and the video of HDE (its own folder, hde-media/).
 *
 * The viewer is the first part (multimedia step 1); the media player and the recorder follow in this same folder, and
 * they share what is here: the folder scans, the order, the "what is shown now" state and the keyboard steps.
 *
 * Everything in this header is plain C (POSIX only — no GTK, no glib): the whole of it is built and run by
 * `make check-unit` (tests/media-test.c) on any machine, with a fake clock for the slideshow, so the logic of the
 * viewer is tested without a display.
 *
 * Rules the rest of the folder follows (and what the tests check):
 *   - the list of a folder is filtered by extension and sorted the way the user asked (name, date, size);
 *   - stepping walks the list and wraps around (the last picture's Right key shows the first one);
 *   - changing the picture forgets the zoom and the rotation of the previous one (they belong to a picture);
 *   - the slideshow advances only when it is on, and never instantly when it is switched on.
 */
#ifndef HDE_MEDIA_H
#define HDE_MEDIA_H

#include <stddef.h>
#include <stdint.h>

/* the name of the program, in one place: it is on the window title, in --version and in the logs */
#define MEDIA_TITLE "Hyggshi Media"

/* the zoom of a picture that fits the window (the value -1 was the old "no zoom yet"; 0 means fit) */
#define HDE_MEDIA_ZOOM_FIT 0.0

/* the order of the list: by name (the natural one: img2 before img10), by date or by size — oldest/smallest first */
typedef enum { HDE_MEDIA_SORT_NAME = 0, HDE_MEDIA_SORT_DATE, HDE_MEDIA_SORT_SIZE } HdeMediaSort;

/* ------------------------------------------------------------------ the list of pictures of a folder
 * Each entry remembers what the sorts need (size, date), so sorting a folder of 10 000 pictures does not stat() every
 * file again for every comparison. */
typedef struct {
    char    *path;
    uint64_t size;
    int64_t  mtime;
} HdeMediaItem;

typedef struct {
    HdeMediaItem *items;
    size_t        n, cap;
} HdeMediaList;

void  hde_media_list_init(HdeMediaList *l);
void  hde_media_list_clear(HdeMediaList *l);
void  hde_media_list_free(HdeMediaList *l);
int   hde_media_list_add(HdeMediaList *l, const char *path);      /* 0 ok, -1 out of memory */
/* the pictures of `dir` (POSIX readdir), hidden files skipped, `recursive` also walks the sub-folders (8 deep at
 * most: a shortcut loop cannot hang the caller). Returns how many were added, or -1 when the folder cannot be read. */
long  hde_media_list_scan(HdeMediaList *l, const char *dir, HdeMediaSort how, int recursive);
void  hde_media_list_sort(HdeMediaList *l, HdeMediaSort how);
long  hde_media_list_find(const HdeMediaList *l, const char *path);   /* index of a path, or -1 */

/* ------------------------------------------------------------------ names, extensions, sizes */
int         hde_media_is_picture(const char *name);        /* by extension: .png, .jpg, ... (case-insensitive) */
const char *hde_media_extension(const char *name);         /* "jpg" (lower case) or "" */
int         hde_media_natural_cmp(const char *a, const char *b);   /* -1/0/1: img2 < img10 ("natural" order) */
const char *hde_media_basename(const char *path);
char       *hde_media_dirname(const char *path);           /* malloc'ed, "" for "/x" → "/", "." for "x" */
char       *hde_media_strdup(const char *s);               /* our own: strdup() is not declared under -std=c11 */
size_t      hde_media_human_size(char *buf, size_t n, uint64_t bytes);   /* "1.4 MiB"; returns what it wrote */
const char *hde_media_sort_name(HdeMediaSort how);         /* "name" / "date" / "size", for the messages */

/* ------------------------------------------------------------------ what the viewer shows */
typedef struct {
    HdeMediaList  list;
    long          index;          /* which picture of the list */
    double        zoom;           /* 1.0 = 100 %, HDE_MEDIA_ZOOM_FIT = fit the window */
    int           rotation;       /* 0, 90, 180, 270 (clockwise) */
    int           slideshow;      /* is the slideshow running? */
    double        interval;       /* seconds per picture */
    HdeMediaSort  sort;
    int           recursive;
    int64_t       last_tick_ms;   /* the clock of the last advance: the caller passes the time, the test fakes it */
    unsigned      shown;          /* how many pictures were shown since the program started */
} HdeMediaView;

void  hde_media_view_init(HdeMediaView *v, HdeMediaSort sort, int recursive, double interval);
void  hde_media_view_free(HdeMediaView *v);
/* show one path: a picture (the list becomes the pictures of its folder, with it current) or a folder (its first
 * picture). -1 when there is nothing to show (no such file, not a picture, an empty folder). */
int   hde_media_view_open(HdeMediaView *v, const char *path);
/* the pictures named on the command line, in that order (the first one current); -1 when none of them is a picture */
int   hde_media_view_open_many(HdeMediaView *v, char *const *paths, int n);
/* next (delta > 0) or previous (delta < 0) picture, wrapping around: 0 ok, -1 empty list */
int   hde_media_view_step(HdeMediaView *v, int delta);
/* show the picture at `index` (the Home / End keys): 0 ok, -1 empty list or out of range */
int   hde_media_view_goto(HdeMediaView *v, long index);
void  hde_media_view_set_zoom(HdeMediaView *v, double zoom);   /* 0 = fit; 1.0 = 100 % (the "1" key) */
const char *hde_media_view_current(const HdeMediaView *v);   /* NULL when the list is empty */
char       *hde_media_view_dir(const HdeMediaView *v);       /* malloc'ed folder of the current picture */
/* direction: +1 in, -1 out, 0 = fit the window. Returns the new zoom (the ladder of hde_media_zoom_step). */
double hde_media_view_zoom(HdeMediaView *v, int direction);
int    hde_media_view_rotate(HdeMediaView *v, int degrees);  /* +90 / -90 → the new angle (0..270) */
/* the slideshow: 1 when it is time for the next picture. Only advances when the slideshow is on, and the interval
 * starts again when it is switched on (so the first picture is not skipped). */
int    hde_media_view_tick(HdeMediaView *v, int64_t now_ms);
void   hde_media_view_slideshow(HdeMediaView *v, int on, int64_t now_ms);

/* the steps of the +/- keys: fit → 100 %, then the ladder; the ends stop */
double hde_media_zoom_step(double zoom, int direction);

#endif /* HDE_MEDIA_H */
