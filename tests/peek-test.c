/* tests/peek-test.c — the sums of the taskbar preview (src/hde-peek-core.c) without a screen, a window manager or
 * a window: how big the thumbnail of a window is drawn, how the windows of one application are laid out, where the
 * popup goes, and when they become one button. Plain C, so `make check-unit` runs it on any machine.
 *   cc -O2 -Wall -Wextra -Wpedantic -std=c11 -Isrc -o build/peek-test tests/peek-test.c src/hde-peek-core.c
 * Prints PASS/FAIL lines; exit status = number of failures.
 */
#include "hde-peek-core.h"

#include <stdio.h>
#include <string.h>

static int fails, passes;

#define CHECK(cond, ...) do { \
        if (cond) { passes++; printf("PASS: peek: "); } else { fails++; printf("FAIL: peek: "); } \
        printf(__VA_ARGS__); printf("\n"); \
    } while (0)

/* "WxH" of one call, so a check reads like what the user sees */
static char *size_text(int win_w, int win_h, int max_w, int max_h, char *out, size_t n)
{
    int w = -1, h = -1;
    hde_peek_thumb_size(win_w, win_h, max_w, max_h, &w, &h);
    snprintf(out, n, "%dx%d", w, h);
    return out;
}

static char *grid_text(int count, int per_row, char *out, size_t n)
{
    int c = -1, r = -1;
    hde_peek_grid(count, per_row, &c, &r);
    snprintf(out, n, "%dx%d", c, r);
    return out;
}

/* ---------------------------------------------------------------- the thumbnail */

static void test_thumb_size(void)
{
    char a[64];

    /* a window of the usual size, and the box the preview allows */
    CHECK(!strcmp(size_text(1920, 1080, 320, 180, a, sizeof a), "320x180"),
          "a 1920x1080 window fills the box: %s", size_text(1920, 1080, 320, 180, a, sizeof a));
    /* 1280x800: the height is what runs out first (1280/320 = 4, 800/180 = 4.44) */
    CHECK(!strcmp(size_text(1280, 800, 320, 180, a, sizeof a), "288x180"),
          "a 1280x800 window is capped by its height: %s", size_text(1280, 800, 320, 180, a, sizeof a));
    /* a narrow window: the width is what runs out */
    CHECK(!strcmp(size_text(400, 2000, 320, 180, a, sizeof a), "36x180"),
          "a tall narrow window is capped by its width: %s", size_text(400, 2000, 320, 180, a, sizeof a));

    /* smaller than the box: drawn as it is, not blown up to look bigger than it is */
    CHECK(!strcmp(size_text(200, 100, 320, 180, a, sizeof a), "200x100"),
          "a window smaller than the box keeps its own size: %s", size_text(200, 100, 320, 180, a, sizeof a));
    CHECK(!strcmp(size_text(320, 180, 320, 180, a, sizeof a), "320x180"),
          "a window exactly the size of the box: %s", size_text(320, 180, 320, 180, a, sizeof a));
    CHECK(!strcmp(size_text(1, 1, 320, 180, a, sizeof a), "1x1"),
          "the smallest window there can be: %s", size_text(1, 1, 320, 180, a, sizeof a));

    /* the shape is kept: a square window stays square, a wide one stays as wide as it is */
    CHECK(!strcmp(size_text(1000, 1000, 320, 180, a, sizeof a), "180x180"),
          "a square window stays square: %s", size_text(1000, 1000, 320, 180, a, sizeof a));
    CHECK(!strcmp(size_text(3000, 1000, 320, 180, a, sizeof a), "320x106"),
          "a very wide window keeps its shape: %s", size_text(3000, 1000, 320, 180, a, sizeof a));

    /* nothing to draw */
    CHECK(!strcmp(size_text(0, 600, 320, 180, a, sizeof a), "0x0"),
          "a window that has not said how wide it is: %s", size_text(0, 600, 320, 180, a, sizeof a));
    CHECK(!strcmp(size_text(800, 0, 320, 180, a, sizeof a), "0x0"),
          "a window that has not said how high it is: %s", size_text(800, 0, 320, 180, a, sizeof a));
    CHECK(!strcmp(size_text(800, 600, 0, 180, a, sizeof a), "0x0"),
          "no room at all: %s", size_text(800, 600, 0, 180, a, sizeof a));

    /* it never comes out with a side of nothing when there is something to draw */
    CHECK(!strcmp(size_text(10000, 1, 320, 180, a, sizeof a), "320x1"),
          "a window one pixel high still shows: %s", size_text(10000, 1, 320, 180, a, sizeof a));
    CHECK(!strcmp(size_text(1, 10000, 320, 180, a, sizeof a), "1x180"),
          "a window one pixel wide still shows: %s", size_text(1, 10000, 320, 180, a, sizeof a));

    /* the box itself, whatever it is called with */
    int w = -1, h = -1;
    hde_peek_thumb_size(1920, 1080, HDE_PEEK_THUMB_MAX_W, HDE_PEEK_THUMB_MAX_H, &w, &h);
    CHECK(w <= HDE_PEEK_THUMB_MAX_W && h <= HDE_PEEK_THUMB_MAX_H && w > 0 && h > 0,
          "in the box the preview really uses: %dx%d", w, h);
}

/* ---------------------------------------------------------------- the grid */

static void test_grid(void)
{
    char a[64];

    CHECK(!strcmp(grid_text(1, 3, a, sizeof a), "1x1"), "one window: %s", grid_text(1, 3, a, sizeof a));
    CHECK(!strcmp(grid_text(2, 3, a, sizeof a), "2x1"), "two windows side by side: %s", grid_text(2, 3, a, sizeof a));
    CHECK(!strcmp(grid_text(3, 3, a, sizeof a), "3x1"), "three windows in one row: %s", grid_text(3, 3, a, sizeof a));
    /* 4 in rows of 3 is 2x2: a row of one under a row of three looks like a mistake */
    CHECK(!strcmp(grid_text(4, 3, a, sizeof a), "2x2"), "four windows: %s, not 3+1", grid_text(4, 3, a, sizeof a));
    CHECK(!strcmp(grid_text(5, 3, a, sizeof a), "3x2"), "five windows: %s", grid_text(5, 3, a, sizeof a));
    CHECK(!strcmp(grid_text(6, 3, a, sizeof a), "3x2"), "six windows: %s", grid_text(6, 3, a, sizeof a));
    CHECK(!strcmp(grid_text(9, 3, a, sizeof a), "3x3"), "nine windows: %s", grid_text(9, 3, a, sizeof a));

    /* one column when that is all that is allowed */
    CHECK(!strcmp(grid_text(3, 1, a, sizeof a), "1x3"), "three windows, one per row: %s", grid_text(3, 1, a, sizeof a));

    /* nothing, and nonsense */
    CHECK(!strcmp(grid_text(0, 3, a, sizeof a), "0x0"), "no windows: %s", grid_text(0, 3, a, sizeof a));
    CHECK(!strcmp(grid_text(2, 0, a, sizeof a), "1x2"), "no rows allowed is one per row: %s", grid_text(2, 0, a, sizeof a));

    /* the grid always holds every window: cols * rows >= n */
    int ok = 1;
    for (int n = 1; n <= 40; n++) {
        int c = 0, r = 0;
        hde_peek_grid(n, HDE_PEEK_PER_ROW, &c, &r);
        if (c * r < n || c > HDE_PEEK_PER_ROW || c < 1 || r < 1) ok = 0;
    }
    CHECK(ok, "every number of windows from 1 to 40 fits in the grid, none wider than %d", HDE_PEEK_PER_ROW);
}

/* ---------------------------------------------------------------- where the popup goes */

/* a panel at the bottom, 40 high, on a 1920x1080 screen: the work area is the screen above the panel */
static HdeRect work_area_bottom(void)  { HdeRect r = { 0, 0, 1920, 1040 }; return r; }
static HdeRect work_area_top(void)     { HdeRect r = { 0, 40, 1920, 1040 }; return r; }

static void test_popup_position(void)
{
    HdeRect work = work_area_bottom();
    HdeRect btn = { 500, 1040, 100, 40 };            /* a button in the panel at the bottom */
    int x = 0, y = 0;

    /* above the button, centred on it */
    hde_peek_popup_position(&btn, &work, 320, 200, 0, &x, &y);
    CHECK(x == 390 && y == 1040 - 200 - HDE_PEEK_GAP,
          "panel at the bottom: the popup sits above the button, centred on it (%d,%d)", x, y);

    /* panel at the top: below the button */
    HdeRect wtop = work_area_top();
    HdeRect btnt = { 500, 0, 100, 40 };
    hde_peek_popup_position(&btnt, &wtop, 320, 200, 1, &x, &y);
    CHECK(x == 390 && y == 40 + HDE_PEEK_GAP,
          "panel at the top: the popup sits below the button (%d,%d)", x, y);

    /* a button at the left edge: the popup is pushed right, not cut off */
    HdeRect left = { 0, 1040, 100, 40 };
    hde_peek_popup_position(&left, &work, 320, 200, 0, &x, &y);
    CHECK(x == 0 && y == 1040 - 200 - HDE_PEEK_GAP,
          "the button at the left edge: the popup is kept on the screen (x=%d)", x);

    /* a button at the right edge */
    HdeRect right = { 1850, 1040, 70, 40 };
    hde_peek_popup_position(&right, &work, 320, 200, 0, &x, &y);
    CHECK(x + 320 <= work.x + work.w && x >= work.x,
          "the button at the right edge: the popup is kept on the screen (x=%d, right edge %d)", x, x + 320);

    /* no room above (a screen lower than the popup): it goes below instead of off the screen */
    HdeRect lowbtn = { 500, 300, 100, 40 };
    HdeRect lowwork = { 0, 0, 1920, 340 };
    hde_peek_popup_position(&lowbtn, &lowwork, 320, 300, 0, &x, &y);
    CHECK(y >= 0 && y + 300 <= lowwork.h, "no room above: it goes below, still on the screen (y=%d)", y);

    /* no work area known: it simply goes beside the button and is not moved */
    hde_peek_popup_position(&btn, NULL, 320, 200, 0, &x, &y);
    CHECK(x == 390 && y == 1040 - 200 - HDE_PEEK_GAP, "no work area: centred on the button and left there (%d,%d)", x, y);

    /* no button at all (the bar was destroyed between the hover and the answer) */
    hde_peek_popup_position(NULL, &work, 320, 200, 0, &x, &y);
    CHECK(x == 0 && y == 0, "no button: nothing to point at (%d,%d)", x, y);

    /* a popup wider than the screen: left edge, never negative */
    HdeRect tiny = { 0, 0, 200, 400 };
    HdeRect tinybtn = { 50, 360, 100, 40 };
    hde_peek_popup_position(&tinybtn, &tiny, 900, 200, 0, &x, &y);
    CHECK(x >= tiny.x, "a popup wider than the screen: the left edge, not off it (x=%d)", x);
}

/* ---------------------------------------------------------------- grouping */

static void test_grouping(void)
{
    /* never: one button per window, however many there are */
    CHECK(hde_peek_should_group(0, 1, 10) == 0, "never: one window is not a group");
    CHECK(hde_peek_should_group(0, 5, 10) == 0, "never: five windows are five buttons");
    CHECK(hde_peek_should_group(0, 5, 2) == 0, "never: even with no room at all");

    /* always: one button per application, whatever the room */
    CHECK(hde_peek_should_group(2, 2, 10) == 1, "always: two windows of one application are one button");
    CHECK(hde_peek_should_group(2, 9, 1) == 1, "always: nine windows of one application are one button");
    CHECK(hde_peek_should_group(2, 1, 10) == 0, "always: a single window is still its own button");

    /* when the bar is out of room */
    CHECK(hde_peek_should_group(1, 3, 10) == 0, "when out of room: three windows with room for ten stay apart");
    CHECK(hde_peek_should_group(1, 3, 3) == 0, "when out of room: three windows with room for exactly three stay apart");
    CHECK(hde_peek_should_group(1, 3, 2) == 1, "when out of room: three windows with room for two are put together");
    CHECK(hde_peek_should_group(1, 8, 1) == 1, "when out of room: eight windows with room for one are put together");
    CHECK(hde_peek_should_group(1, 1, 0) == 0, "when out of room: a lone window is never a group");

    /* nonsense in, something sensible out */
    CHECK(hde_peek_should_group(1, 0, 5) == 0, "no windows: nothing to group");
    CHECK(hde_peek_should_group(7, 4, 1) == 1, "an unknown mode is read as 'always' rather than left to chance");
}

int main(void)
{
    test_thumb_size();
    test_grid();
    test_popup_position();
    test_grouping();
    printf("\npeek: %d passed, %d failed\n", passes, fails);
    return fails;
}
