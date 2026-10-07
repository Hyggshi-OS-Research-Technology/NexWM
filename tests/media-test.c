/* tests/media-test.c — the logic of the picture viewer (hde-media/src/gallery.c) without a display: the list of a
 * folder, the natural order, the wrap-around of the arrow keys, the zoom ladder, the rotation and the slideshow clock.
 * All of it is plain C: built and run by `make check-unit` on every machine (and by tests/media-test.sh in CI).
 *
 *   cc -O2 -Wall -Wextra -std=c11 -Ihde-media/src -o build/media-test tests/media-test.c hde-media/src/gallery.c
 *
 * PASS/FAIL lines; exit status = failures.
 */
#define _POSIX_C_SOURCE 200809L

#include "media.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int fails, passes;

#define CHECK(cond, ...) do { \
        if (cond) { passes++; printf("PASS: media: "); } else { fails++; printf("FAIL: media: "); } \
        printf(__VA_ARGS__); printf("\n"); \
    } while (0)

static char root[256];

static void write_file(const char *rel, const char *data)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", root, rel);
    FILE *f = fopen(path, "w");
    if (!f) { printf("FAIL: media: cannot write %s (%s)\n", path, strerror(0)); fails++; return; }
    if (data) fputs(data, f);
    fclose(f);
}

/* the picture whose basename is `name`, or NULL: what the list says about one file */
static const char *item_named(const HdeMediaList *l, const char *name)
{
    for (size_t i = 0; i < l->n; i++)
        if (!strcmp(hde_media_basename(l->items[i].path), name)) return l->items[i].path;
    return NULL;
}

static void test_names(void)
{
    CHECK(hde_media_is_picture("holiday.png"), "holiday.png is a picture");
    CHECK(hde_media_is_picture("HOLIDAY.PNG"), "the extension is read whatever the case (HOLIDAY.PNG)");
    CHECK(hde_media_is_picture("/home/u/Pictures/a/b.jpeg"), "a path is not needed to test the name (a/b.jpeg)");
    CHECK(!hde_media_is_picture("notes.txt"), "notes.txt is not a picture");
    CHECK(!hde_media_is_picture("Makefile"), "a file without extension is not a picture");
    CHECK(!hde_media_is_picture("archive.png.zip"), "the last extension decides (archive.png.zip is not)");
    CHECK(hde_media_is_picture(".hidden.png"), " .hidden.png is a picture by name (the folder scan skips it)");
    CHECK(!strcmp(hde_media_extension("a.JPG"), "JPG"), "the extension comes back as written (JPG)");

    CHECK(hde_media_natural_cmp("img2.png", "img10.png") < 0, "img2 comes before img10 (a plain strcmp would not)");
    CHECK(hde_media_natural_cmp("img10.png", "img9.png") > 0, "img10 comes after img9");
    CHECK(hde_media_natural_cmp("a", "b") < 0, "a before b");
    CHECK(hde_media_natural_cmp("A", "a") == 0, "the order ignores the case (A == a)");
    CHECK(hde_media_natural_cmp("photo 007", "photo 7") == 0, "leading zeros do not change the number (007 == 7)");
    CHECK(hde_media_natural_cmp("v1.10", "v1.9") > 0, "v1.10 after v1.9");
    CHECK(hde_media_natural_cmp("abc", "abcd") < 0, "a shorter name comes first");

    char buf[64];
    hde_media_human_size(buf, sizeof buf, 0);
    CHECK(!strcmp(buf, "0 B"), "0 bytes is '0 B' (got %s)", buf);
    hde_media_human_size(buf, sizeof buf, 999);
    CHECK(!strcmp(buf, "999 B"), "999 bytes is '999 B' (got %s)", buf);
    hde_media_human_size(buf, sizeof buf, 1536);
    CHECK(!strcmp(buf, "1.5 KiB"), "1536 bytes is '1.5 KiB' (got %s)", buf);
    hde_media_human_size(buf, sizeof buf, 10u * 1024 * 1024);
    CHECK(!strcmp(buf, "10 MiB"), "10 MiB is '10 MiB' (got %s)", buf);

    char *d = hde_media_dirname("/a/b/c.png");
    CHECK(d && !strcmp(d, "/a/b"), "the folder of /a/b/c.png is /a/b (got %s)", d ? d : "(null)");
    free(d);
    d = hde_media_dirname("/x");
    CHECK(d && !strcmp(d, "/"), "the folder of /x is / (got %s)", d ? d : "(null)");
    free(d);
    d = hde_media_dirname("x.png");
    CHECK(d && !strcmp(d, "."), "the folder of x.png is . (got %s)", d ? d : "(null)");
    free(d);
    CHECK(!strcmp(hde_media_basename("/a/b/c.png"), "c.png"), "the name of /a/b/c.png is c.png");
}

static void test_scan(void)
{
    HdeMediaList l;
    hde_media_list_init(&l);

    long n = hde_media_list_scan(&l, root, HDE_MEDIA_SORT_NAME, 0);
    CHECK(n == 3, "a folder of 3 pictures, 1 text file and 1 hidden picture gives 3 (got %ld)", n);
    CHECK(l.n == 3, "the list has 3 entries (got %zu)", l.n);
    CHECK(item_named(&l, "img1.png") == l.items[0].path, "by name: img1.png first (got %s)",
          hde_media_basename(l.items[0].path));
    CHECK(item_named(&l, "img2.png") == l.items[1].path, "then img2.png (got %s)", hde_media_basename(l.items[1].path));
    CHECK(item_named(&l, "img10.png") == l.items[2].path, "then img10.png — not img10 before img2 (got %s)",
          hde_media_basename(l.items[2].path));

    hde_media_list_clear(&l);
    n = hde_media_list_scan(&l, root, HDE_MEDIA_SORT_NAME, 1);
    CHECK(n == 4, "the same folder with --recursive has 4 pictures (the one in sub/ as well; got %ld)", n);
    CHECK(item_named(&l, "deep.png") != NULL, "the picture of the sub-folder is in the list");

    /* the order by size and by date: the values are set here, so the test does not depend on the file system clock */
    hde_media_list_clear(&l);
    hde_media_list_add(&l, "/p/big.png");
    hde_media_list_add(&l, "/p/small.png");
    hde_media_list_add(&l, "/p/middle.png");
    l.items[0].size = 5000; l.items[0].mtime = 300;
    l.items[1].size = 100;  l.items[1].mtime = 900;
    l.items[2].size = 900;  l.items[2].mtime = 100;
    hde_media_list_sort(&l, HDE_MEDIA_SORT_SIZE);
    CHECK(!strcmp(hde_media_basename(l.items[0].path), "small.png"), "by size: the smallest first (got %s)",
          hde_media_basename(l.items[0].path));
    CHECK(!strcmp(hde_media_basename(l.items[2].path), "big.png"), "by size: the biggest last");
    hde_media_list_sort(&l, HDE_MEDIA_SORT_DATE);
    CHECK(!strcmp(hde_media_basename(l.items[0].path), "middle.png"), "by date: the oldest first (got %s)",
          hde_media_basename(l.items[0].path));
    CHECK(!strcmp(hde_media_basename(l.items[2].path), "small.png"), "by date: the newest last");
    CHECK(hde_media_list_find(&l, "/p/middle.png") == 0, "the index of a path in the sorted list");
    CHECK(hde_media_list_find(&l, "/p/nothing.png") == -1, "an unknown path has no index");
    hde_media_list_free(&l);

    /* a folder that cannot be read is not a crash (and not "0 pictures" either: -1 says it failed) */
    hde_media_list_init(&l);
    n = hde_media_list_scan(&l, "/no/such/folder/hopefully", HDE_MEDIA_SORT_NAME, 0);
    CHECK(n == -1, "a folder that does not exist gives -1 (got %ld)", n);
    hde_media_list_free(&l);

    /* an empty folder is a folder with no pictures, not an error */
    char empty[512];
    snprintf(empty, sizeof empty, "%s/empty", root);
    mkdir(empty, 0755);
    hde_media_list_init(&l);
    n = hde_media_list_scan(&l, empty, HDE_MEDIA_SORT_NAME, 0);
    CHECK(n == 0, "an empty folder gives 0 pictures, not an error (got %ld)", n);
    hde_media_list_free(&l);
}

static void test_view(void)
{
    HdeMediaView v;
    hde_media_view_init(&v, HDE_MEDIA_SORT_NAME, 0, 2.0);

    char first[512], outside[512];
    snprintf(first, sizeof first, "%s/img2.png", root);
    snprintf(outside, sizeof outside, "%s/notes.txt", root);

    CHECK(hde_media_view_open(&v, first) == 0, "opening a picture fills the list from its folder");
    CHECK(v.list.n == 3, "the list is the folder of the picture (3 pictures)");
    CHECK(!strcmp(hde_media_basename(hde_media_view_current(&v)), "img2.png"),
          "the picture asked for is the one shown (got %s)", hde_media_basename(hde_media_view_current(&v)));
    CHECK(v.shown == 1, "one picture has been shown (got %u)", v.shown);

    hde_media_view_step(&v, 1);
    CHECK(!strcmp(hde_media_basename(hde_media_view_current(&v)), "img10.png"), "Right goes to the next one");
    hde_media_view_step(&v, 1);
    CHECK(!strcmp(hde_media_basename(hde_media_view_current(&v)), "img1.png"),
          "past the last picture it wraps around to the first (got %s)",
          hde_media_basename(hde_media_view_current(&v)));
    hde_media_view_step(&v, -1);
    CHECK(!strcmp(hde_media_basename(hde_media_view_current(&v)), "img10.png"), "Left wraps around the other way");
    CHECK(v.shown == 4, "every step counts as a picture shown (got %u)", v.shown);

    /* the zoom and the rotation belong to a picture: changing it forgets them */
    hde_media_view_zoom(&v, 1);
    hde_media_view_rotate(&v, 90);
    hde_media_view_step(&v, 1);
    CHECK(v.zoom == HDE_MEDIA_ZOOM_FIT && v.rotation == 0,
          "the next picture starts fitted and straight (zoom %.2f, rotation %d)", v.zoom, v.rotation);

    char *dir = hde_media_view_dir(&v);
    CHECK(dir && !strcmp(dir, root), "the folder of the current picture is the one that was scanned (got %s)",
          dir ? dir : "(null)");
    free(dir);

    CHECK(hde_media_view_open(&v, root) == 0, "opening a folder shows its first picture");
    CHECK(!strcmp(hde_media_basename(hde_media_view_current(&v)), "img1.png"), "the first picture of the folder");
    CHECK(hde_media_view_open(&v, outside) == -1, "opening a text file is refused (-1), it is not a picture");
    CHECK(hde_media_view_open(&v, "/no/such/picture.png") == -1, "a file that does not exist is refused (-1)");

    char empty[512];
    snprintf(empty, sizeof empty, "%s/empty", root);
    CHECK(hde_media_view_open(&v, empty) == -1, "a folder with no pictures is refused (-1)");

    /* several pictures on the command line keep their order (that is what "Open With" passes) */
    char a[512], b[512];
    snprintf(a, sizeof a, "%s/img10.png", root);
    snprintf(b, sizeof b, "%s/img1.png", root);
    char *many[] = { a, outside, b };
    CHECK(hde_media_view_open_many(&v, many, 3) == 0, "opening several pictures at once");
    CHECK(v.list.n == 2, "the text file between them is skipped (2 pictures)");
    CHECK(!strcmp(hde_media_basename(hde_media_view_current(&v)), "img10.png"),
          "the first one on the command line is shown first");
    hde_media_view_step(&v, 1);
    CHECK(!strcmp(hde_media_basename(hde_media_view_current(&v)), "img1.png"), "then the second one");

    /* Home / End and the 100 % key */
    CHECK(hde_media_view_goto(&v, 1) == 0, "going to the second picture of the list");
    CHECK(!strcmp(hde_media_basename(hde_media_view_current(&v)), "img1.png"),
          "the list here is the two pictures of the command line (img1.png)");
    CHECK(hde_media_view_goto(&v, 0) == 0, "going back to the first one");
    CHECK(hde_media_view_goto(&v, 99) == -1, "a picture that is not in the list is refused (-1)");
    hde_media_view_set_zoom(&v, 1.0);
    CHECK(v.zoom == 1.0, "the 100 %% key sets the zoom to 1.0");
    hde_media_view_set_zoom(&v, 0);
    CHECK(v.zoom == HDE_MEDIA_ZOOM_FIT, "and 0 means fit again");

    /* an empty view is not a crash */
    HdeMediaView empty_view;
    hde_media_view_init(&empty_view, HDE_MEDIA_SORT_NAME, 0, 5.0);
    CHECK(hde_media_view_step(&empty_view, 1) == -1, "stepping through nothing gives -1");
    CHECK(hde_media_view_current(&empty_view) == NULL, "nothing to show: no current picture");
    CHECK(hde_media_view_rotate(&empty_view, -90) == 270, "rotating nothing still answers with the angle (270)");
    CHECK(hde_media_view_goto(&empty_view, 0) == -1, "going to a picture of an empty list gives -1");
    hde_media_view_free(&empty_view);
    hde_media_view_free(&v);
}

static void test_zoom_rotation_slideshow(void)
{
    CHECK(hde_media_zoom_step(HDE_MEDIA_ZOOM_FIT, 1) == 1.0, "from fit, + goes to 100 %%");
    CHECK(hde_media_zoom_step(HDE_MEDIA_ZOOM_FIT, -1) == 0.5, "from fit, - goes to 50 %%");
    CHECK(hde_media_zoom_step(1.0, 1) == 1.25, "+ from 100 %% is 125 %%");
    CHECK(hde_media_zoom_step(1.0, -1) == 0.75, "- from 100 %% is 75 %%");
    CHECK(hde_media_zoom_step(16.0, 1) == 16.0, "the ladder stops at 1600 %%");
    CHECK(hde_media_zoom_step(0.05, -1) == 0.05, "and at 5 %%");
    CHECK(hde_media_zoom_step(1.4, 0) == HDE_MEDIA_ZOOM_FIT, "the fit key (0) always gives fit");
    CHECK(hde_media_zoom_step(3.0, -1) == 2.0, "from 300 %% down is 200 %% (the ladder is walked, not divided)");

    HdeMediaView v;
    hde_media_view_init(&v, HDE_MEDIA_SORT_NAME, 0, 2.0);
    CHECK(hde_media_view_rotate(&v, 90) == 90, "a quarter turn to the right is 90°");
    CHECK(hde_media_view_rotate(&v, 90) == 180, "another one is 180°");
    CHECK(hde_media_view_rotate(&v, -90) == 90, "and to the left it comes back (90°)");
    CHECK(hde_media_view_rotate(&v, -180) == 270, "minus 180° from 90° is 270° (it never goes under 0)");
    CHECK(hde_media_view_rotate(&v, 360) == 270, "a full turn changes nothing");
    hde_media_view_zoom(&v, 1);                       /* fit → 100 % */
    hde_media_view_zoom(&v, -1);                      /* 100 % → 75 % (the ladder is walked, not divided) */
    CHECK(v.zoom == 0.75, "in and out again lands on the step below 100 %% (75 %%, got %.2f)", v.zoom);
    hde_media_view_zoom(&v, 0);
    CHECK(v.zoom == HDE_MEDIA_ZOOM_FIT, "the fit key goes back to fit from anywhere");

    /* the slideshow: the clock is a parameter, so the test says exactly when a picture is due */
    CHECK(hde_media_view_tick(&v, 100000) == 0, "with the slideshow off, the clock never asks for the next picture");
    hde_media_view_slideshow(&v, 1, 1000);
    CHECK(hde_media_view_tick(&v, 1500) == 0, "half an interval after switching it on: not yet (2 s per picture)");
    CHECK(hde_media_view_tick(&v, 2900) == 0, "still not at 1.9 s");
    CHECK(hde_media_view_tick(&v, 3000) == 1, "at 2 s the next picture is due");
    CHECK(hde_media_view_tick(&v, 3200) == 0, "and then the interval starts again (0.2 s later: no)");
    CHECK(hde_media_view_tick(&v, 5000) == 1, "another 2 s later: yes");
    hde_media_view_slideshow(&v, 1, 9000);
    CHECK(hde_media_view_tick(&v, 9500) == 0, "switching it on again starts the interval over (no instant skip)");
    hde_media_view_slideshow(&v, 0, 9000);
    CHECK(hde_media_view_tick(&v, 99999) == 0, "switched off: never");
    hde_media_view_free(&v);
}

int main(void)
{
    snprintf(root, sizeof root, "/tmp/hde-media-test");
    char cmd[512];
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    if (system(cmd) != 0) { /* nothing to clean */ }
    snprintf(cmd, sizeof cmd, "mkdir -p %s/sub", root);
    if (system(cmd) != 0) { printf("FAIL: media: cannot create %s\n", root); return 1; }

    write_file("img1.png", "one");
    write_file("img2.png", "two");
    write_file("img10.png", "ten");          /* written after img2: the size sort is not the name sort */
    write_file("notes.txt", "not a picture");
    write_file(".hidden.png", "hidden");
    write_file("sub/deep.png", "deep");

    test_names();
    test_scan();
    test_view();
    test_zoom_rotation_slideshow();

    printf("\nmedia-test: %d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
