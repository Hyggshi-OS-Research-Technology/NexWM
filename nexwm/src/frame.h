/* frame.h — NexWM: the arithmetic of the frames — the title bar, its buttons, and the mouse.
 *
 * Everything in here is a pure function: rectangles and numbers in, rectangles and numbers out. No X server, no XCB,
 * no allocation — which is the point. The part of a window manager that is easy to get wrong is not the calling of
 * XCB (x11.c does that, and the log says what it did); it is the arithmetic: where the frame is around a window, what
 * the pointer is on, what a drag does to a size, where a window without a position of its own goes. Written this way
 * it can be checked on any machine — tests/nexwm-test.c does, with `make check-unit` — and the same numbers are the
 * ones x11.c draws with, so a frame that is drawn wrong is a frame that was computed wrong (and the test would have
 * caught the arithmetic before the drawing ever happened).
 *
 * The frame around a window, as it is drawn:
 *
 *   (0,0) ┌────────────────────────────────────────────────┐  the border of `border` px, in the colour of the frame
 *         │             the title bar, `titlebar` px        │  (the focused one in the other colour)
 *         ├────────────────────────────────────────────────┤
 *         │                                                │
 *         │                    the window                  │
 *         │                                                │
 *         └────────────────────────────────────────────────┘
 *
 * The title bar is inside the top border: the border of the frame goes around the title bar as well, so the frame is
 * `border` px wide everywhere and the title bar sits at y = border. The buttons are at the right end of the bar, laid
 * out from right to left in the order the configuration file names them; the title is centred in what is left, and
 * the icon takes the left end.
 */
#ifndef HDE_NEXWM_FRAME_H
#define HDE_NEXWM_FRAME_H

#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------- the rectangles */

typedef struct {
    int x, y, w, h;
} NexwmRect;

/* The sides of a frame, as the mouse uses them: one bit per side, so a corner is two of them. Dragging the left side
 * of a window by dx means "the left edge follows the pointer": the place of the window and its size both change. */
#define NEXWM_SIDE_LEFT   (1u << 0)
#define NEXWM_SIDE_RIGHT  (1u << 1)
#define NEXWM_SIDE_TOP    (1u << 2)     /* the title bar (or the top border of a window without one) */
#define NEXWM_SIDE_BOTTOM (1u << 3)

/* What the pointer is on, inside a frame (the coordinates are the frame's own: 0,0 is its upper-left corner) */
#define NEXWM_HIT_NONE   0              /* outside the frame (or the frame has nothing to grab) */
#define NEXWM_HIT_CLIENT 1              /* the window itself: its own clicks, no dragging */
#define NEXWM_HIT_TITLE  2              /* the title bar: dragging here moves the window */
#define NEXWM_HIT_BUTTON 3              /* one of the buttons of the title bar (which one is in *button) */
#define NEXWM_HIT_EDGE   4              /* the edge of the frame: dragging here resizes it (which sides are in *sides) */

/* What a window dropped after a move drag does, to judge by where the pointer was let go */
#define NEXWM_DROP_NONE     0           /* it stays where it was dragged to */
#define NEXWM_DROP_MAXIMIZE 1           /* the pointer was at the top of the work area */
#define NEXWM_DROP_LEFT     2           /* ... at its left edge: the left half */
#define NEXWM_DROP_RIGHT    3           /* ... at its right edge: the right half */

/* ---------------------------------------------------------------- the look of the frames */

typedef struct {
    int        border;        /* px of frame on each side (0 = none) */
    int        titlebar;      /* px of title bar (0 = no title bar: the frames are borders only) */
    const int *buttons;       /* NEXWM_BUTTON_* (nexwm.h), in the order they are drawn, left to right */
    int        n_buttons;
} NexwmFrameStyle;

/* the numbers a frame is drawn by: the space around a button and the title, and how wide the grab zones are */
#define NEXWM_FRAME_PAD  5        /* the space between the ends of the bar and what is drawn in it */
#define NEXWM_FRAME_GRAB 4        /* the top edge of the bar, and its two ends, resize with at least this many px */

typedef struct {
    int       kind;               /* NEXWM_BUTTON_* (nexwm.h) */
    NexwmRect rect;               /* where it is, in the coordinates of the frame */
} NexwmFrameButton;

/* ---------------------------------------------------------------- the frame around a window */

/* The four sides of the frame of a window, in the order EWMH asks for them in _NET_FRAME_EXTENTS: left, right, top,
 * bottom. The title bar is part of the top. A window that is not framed gets zeros (the caller says which windows
 * those are: a panel, the desktop, a full screen window that asked for no frame). */
void nexwm_frame_extents(const NexwmFrameStyle *st, int *left, int *right, int *top, int *bottom);

/* The frame around a client: `border` px on every side, plus the title bar above it. */
NexwmRect nexwm_frame_around(const NexwmFrameStyle *st, NexwmRect client);
/* ... and where the client sits inside its frame: the other way round. */
NexwmRect nexwm_frame_content(const NexwmFrameStyle *st, NexwmRect frame);

/* ---------------------------------------------------------------- the title bar */

/* The buttons of a title bar `frame_w` px wide: their places, right to left in the order of the style. Returns how
 * many were put in `out` (0 when the style has no title bar or no buttons). `out` holds at least 8 of them. */
int nexwm_frame_buttons(const NexwmFrameStyle *st, int frame_w, NexwmFrameButton *out, int max);

/* Where the title (text_w px wide) begins: in the middle of what the icon and the buttons leave of the bar. A title
 * too wide for that space is centred in the whole bar and the drawing clips it — it must not disappear, and it must
 * not be drawn over the buttons either. `left_end` is where the icon ends (0 when there is none). */
int nexwm_frame_title_x(const NexwmFrameStyle *st, int frame_w, int text_w, int left_end);

/* What is at (px, py) of a frame `frame_w` x `frame_h`: NEXWM_HIT_*. *button gets the NEXWM_BUTTON_* under the
 * pointer (a click on a button is a click on it, never a drag), *sides the NEXWM_SIDE_* bits of an edge. */
int nexwm_frame_hit(const NexwmFrameStyle *st, int frame_w, int frame_h, int px, int py, int *button, unsigned *sides);

/* ---------------------------------------------------------------- the mouse */

/* The client rectangle after dragging `sides` by (dx, dy): the left side follows the pointer (the window moves and
 * gets smaller), the right one only changes the size, and neither goes under the minimum size of the window (its
 * WM_NORMAL_HINTS, or 1). */
NexwmRect nexwm_frame_resize(NexwmRect client, unsigned sides, int dx, int dy, int min_w, int min_h);

/* What letting a move drag go at (px, py) means: a window dropped at the top of the work area is maximized (that is
 * what every desktop does with it), at its left or right edge it takes that half of the screen. `margin` is how near
 * an edge counts (the pointer does not have to be exactly on the pixel). */
int nexwm_frame_drop(const NexwmRect *workarea, int px, int py, int margin);

/* Where a window without a position of its own goes: the middle of the work area, moved by `cascade` px for every
 * window that is already there (so a second one is not exactly on the first), and never off the work area. */
void nexwm_frame_place(NexwmRect workarea, int w, int h, int cascade, NexwmRect *out);

/* ---------------------------------------------------------------- the title, the icon, the colours */

/* The title as a core font can draw it: _NET_WM_NAME is UTF-8 (the panel shows the whole thing), the X core font of
 * the title bar knows Latin-1 and nothing else, and a control character would move the cursor. So: UTF-8 in, Latin-1
 * out, one character for every one that cannot be drawn. */
void nexwm_frame_title(const char *title, char *out, size_t n);

/* A colour made lighter (percent > 100) or darker (percent < 100): the strip under the pointer, the pressed button.
 * 100 is the colour itself; every channel is clamped to 0 .. 255. */
unsigned long nexwm_frame_shade(unsigned long rgb, int percent);

/* Which of the pictures of an _NET_WM_ICON is the one for a title bar that wants `want` px: the smallest one that is
 * at least that big, or the biggest there is when none is. `data` is the property as it comes (width, height, then
 * width * height ARGB words, one picture after another). Returns 0 when there is no usable picture; *pixels points
 * into `data` (the caller does not free it). */
int nexwm_frame_icon_pick(const uint32_t *data, uint32_t n, int want, int *w, int *h, const uint32_t **pixels);

/* A picture, drawn `want` x `want` and put over `background` (an icon can be transparent), as the bytes XCB wants for
 * a 24-bit image: B, G, R, 0 per pixel, top row first. `out` holds want * want * 4 bytes. The picture is scaled to
 * fit (nearest pixel: a title bar is not the place for a filter). */
void nexwm_frame_icon_draw(const uint32_t *icon, int iw, int ih, int want, unsigned long background, uint8_t *out);

#endif /* HDE_NEXWM_FRAME_H */
