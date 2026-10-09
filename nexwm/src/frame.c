/* frame.c — NexWM: the arithmetic of the frames (see frame.h). No display, no allocation, no XCB: every function here
 * is a rectangle in and a rectangle out, which is why tests/nexwm-test.c can check all of it with `make check-unit` on
 * a machine that has no X server at all, and why a wrong number shows up in the test rather than on the screen.
 */
#include "frame.h"

#include <string.h>

/* ---------------------------------------------------------------- the frame around a window */

void nexwm_frame_extents(const NexwmFrameStyle *st, int *left, int *right, int *top, int *bottom)
{
    if (left)   *left = st->border;
    if (right)  *right = st->border;
    if (top)    *top = st->border + (st->titlebar > 0 ? st->titlebar : 0);
    if (bottom) *bottom = st->border;
}

NexwmRect nexwm_frame_around(const NexwmFrameStyle *st, NexwmRect client)
{
    NexwmRect f;
    int bar = st->titlebar > 0 ? st->titlebar : 0;
    f.x = client.x - st->border;
    f.y = client.y - st->border - bar;
    f.w = client.w + 2 * st->border;
    f.h = client.h + 2 * st->border + bar;
    return f;
}

NexwmRect nexwm_frame_content(const NexwmFrameStyle *st, NexwmRect frame)
{
    NexwmRect c;
    int bar = st->titlebar > 0 ? st->titlebar : 0;
    c.x = frame.x + st->border;
    c.y = frame.y + st->border + bar;
    c.w = frame.w - 2 * st->border;
    c.h = frame.h - 2 * st->border - bar;
    return c;
}

/* ---------------------------------------------------------------- the title bar */

/* the height of the bar, and how much of it a button takes */
static int bar_size(const NexwmFrameStyle *st) { return st->titlebar > 0 ? st->titlebar : 0; }

static int button_size(const NexwmFrameStyle *st)
{
    int bar = bar_size(st);
    if (st->button_size > 0 && st->button_size <= bar) return st->button_size;
    int size = bar - 2 * NEXWM_FRAME_PAD;
    /* a thin bar has no room for the padding: the buttons are the whole height of it (and of a sane width) */
    if (size < 6) size = bar;
    return size > 0 ? size : 0;
}

static int button_pad(const NexwmFrameStyle *st)
{
    int size = button_size(st);
    int bar = bar_size(st);
    if (size <= 0 || size >= bar) return 0;
    return (bar - size) / 2;
}

int nexwm_frame_buttons(const NexwmFrameStyle *st, int frame_w, NexwmFrameButton *out, int max)
{
    int bar = bar_size(st);
    int size = button_size(st);
    int n = st->n_buttons > 0 ? st->n_buttons : 0;
    if (bar <= 0 || size <= 0 || !out || max <= 0) return 0;
    if (n > max) n = max;
    if (n > 8) n = 8;                                  /* the buttons of a title bar are a handful, not a list */

    /* the buttons stand together at the right end of the bar, right to left, each one a square of the bar's height
     * (Openbox, Metacity and the rest put them there; a title bar's left end belongs to the icon) */
    int pad = button_pad(st);
    int y = st->border + pad;
    int right = frame_w - NEXWM_FRAME_PAD;
    for (int i = n - 1; i >= 0; i--) {
        out[i].kind = st->buttons[i];
        out[i].rect.x = right - size;
        out[i].rect.y = y;
        out[i].rect.w = size;
        out[i].rect.h = size;
        right -= size;
    }
    return n;
}

int nexwm_frame_title_x(const NexwmFrameStyle *st, int frame_w, int text_w, int left_end)
{
    int left = NEXWM_FRAME_PAD;
    if (left_end > 0) left = left_end + NEXWM_FRAME_PAD;

    int right = frame_w - NEXWM_FRAME_PAD;
    NexwmFrameButton b[8];
    int n = nexwm_frame_buttons(st, frame_w, b, 8);
    if (n > 0) right = b[0].rect.x - NEXWM_FRAME_PAD;   /* b[0] is the leftmost of the group */

    if (right - left < text_w) {                        /* no room for it between the icon and the buttons: the whole
                                                         * bar is the space, and the drawing clips it */
        left = NEXWM_FRAME_PAD;
        right = frame_w - NEXWM_FRAME_PAD;
    }
    int x;
    if (st->title_align == 1)      x = left;                          /* left */
    else if (st->title_align == 2) x = right - text_w;                /* right */
    else                           x = left + (right - left - text_w) / 2; /* centre (default) */
    if (x < NEXWM_FRAME_PAD) x = NEXWM_FRAME_PAD;
    return x;
}

int nexwm_frame_hit(const NexwmFrameStyle *st, int frame_w, int frame_h, int px, int py, int *button, unsigned *sides)
{
    if (button) *button = 0;
    if (sides) *sides = 0;
    if (frame_w <= 0 || frame_h <= 0 || px < 0 || py < 0 || px >= frame_w || py >= frame_h) return NEXWM_HIT_NONE;

    int bar = bar_size(st);
    int in_bar = bar > 0 && py >= st->border && py < st->border + bar;

    /* a click on a button is a click on that button: the buttons win over the edges they stand on (the close button
     * of a bar touches the right end of it, which is also a resize zone) */
    if (in_bar) {
        NexwmFrameButton b[8];
        int n = nexwm_frame_buttons(st, frame_w, b, 8);
        for (int i = 0; i < n; i++) {
            if (px >= b[i].rect.x && px < b[i].rect.x + b[i].rect.w &&
                py >= b[i].rect.y && py < b[i].rect.y + b[i].rect.h) {
                if (button) *button = b[i].kind;
                return NEXWM_HIT_BUTTON;
            }
        }
    }

    /* The sides: below the title bar they are exactly the border of the frame; in the bar they are a little wider
     * (NEXWM_FRAME_GRAB or resize_grip), because a two-pixel frame is not something to aim at. */
    unsigned s = 0;
    int grab_size = st->resize_grip > 0 ? st->resize_grip : NEXWM_FRAME_GRAB;
    int side_w = in_bar ? (st->border > grab_size ? st->border : grab_size) : st->border;
    int top_h = st->border;
    if (bar > 0) {
        int grab = bar > grab_size ? grab_size : bar;
        top_h = st->border + grab;
    }
    if (top_h > 0 && py < top_h) s |= NEXWM_SIDE_TOP;
    if (st->border > 0 && py >= frame_h - st->border) s |= NEXWM_SIDE_BOTTOM;
    if (side_w > 0 && px < side_w) s |= NEXWM_SIDE_LEFT;
    if (side_w > 0 && px >= frame_w - side_w) s |= NEXWM_SIDE_RIGHT;
    if (s) {
        if (sides) *sides = s;
        return NEXWM_HIT_EDGE;
    }
    if (in_bar) return NEXWM_HIT_TITLE;                 /* the middle of the bar: drag the window */
    return NEXWM_HIT_CLIENT;
}

/* ---------------------------------------------------------------- the mouse */

NexwmRect nexwm_frame_resize(NexwmRect client, unsigned sides, int dx, int dy, int min_w, int min_h)
{
    if (min_w < 1) min_w = 1;
    if (min_h < 1) min_h = 1;
    NexwmRect r = client;
    if (sides & NEXWM_SIDE_LEFT) {
        if (r.w - dx < min_w) dx = r.w - min_w;         /* the left edge stops at the minimum size: the right one
                                                         * stays where it is */
        r.x += dx;
        r.w -= dx;
    } else if (sides & NEXWM_SIDE_RIGHT) {
        r.w += dx;
        if (r.w < min_w) r.w = min_w;
    }
    if (sides & NEXWM_SIDE_TOP) {
        if (r.h - dy < min_h) dy = r.h - min_h;
        r.y += dy;
        r.h -= dy;
    } else if (sides & NEXWM_SIDE_BOTTOM) {
        r.h += dy;
        if (r.h < min_h) r.h = min_h;
    }
    return r;
}

int nexwm_frame_drop(const NexwmRect *workarea, int px, int py, int margin)
{
    if (!workarea) return NEXWM_DROP_NONE;
    if (py <= workarea->y + margin) return NEXWM_DROP_MAXIMIZE;              /* the top: the whole work area */
    if (px <= workarea->x + margin) return NEXWM_DROP_LEFT;                  /* the left: half of it */
    if (px >= workarea->x + workarea->w - 1 - margin) return NEXWM_DROP_RIGHT;
    return NEXWM_DROP_NONE;
}

void nexwm_frame_place(NexwmRect workarea, int w, int h, int cascade, NexwmRect *out)
{
    if (!out) return;
    if (w > workarea.w) w = workarea.w;
    if (h > workarea.h) h = workarea.h;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    int x = workarea.x + (workarea.w - w) / 2 + cascade;
    int y = workarea.y + (workarea.h - h) / 2 + cascade;
    if (x + w > workarea.x + workarea.w) x = workarea.x + workarea.w - w;
    if (y + h > workarea.y + workarea.h) y = workarea.y + workarea.h - h;
    if (x < workarea.x) x = workarea.x;
    if (y < workarea.y) y = workarea.y;
    out->x = x;
    out->y = y;
    out->w = w;
    out->h = h;
}

/* ---------------------------------------------------------------- the title, the colours */

void nexwm_frame_title(const char *title, char *out, size_t n)
{
    if (!out || n == 0) return;
    out[0] = '\0';
    if (!title) return;

    const unsigned char *p = (const unsigned char *)title;
    size_t o = 0;
    while (*p && o + 1 < n) {
        unsigned cp = '?';
        int len = 1;
        if (p[0] < 0x80) {
            cp = p[0];
        } else if ((p[0] & 0xe0) == 0xc0 && (p[1] & 0xc0) == 0x80) {
            cp = ((unsigned)(p[0] & 0x1f) << 6) | (unsigned)(p[1] & 0x3f);
            len = 2;
            if (cp < 0x80) cp = '?';                    /* an overlong form: not the character it claims to be */
        } else if ((p[0] & 0xf0) == 0xe0 && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80) {
            cp = ((unsigned)(p[0] & 0x0f) << 12) | ((unsigned)(p[1] & 0x3f) << 6) | (unsigned)(p[2] & 0x3f);
            len = 3;
            if (cp < 0x800) cp = '?';
        } else if ((p[0] & 0xf8) == 0xf0 && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80 && (p[3] & 0xc0) == 0x80) {
            cp = ((unsigned)(p[0] & 0x07) << 18) | ((unsigned)(p[1] & 0x3f) << 12) | ((unsigned)(p[2] & 0x3f) << 6) |
                 (unsigned)(p[3] & 0x3f);
            len = 4;
            if (cp < 0x10000 || cp > 0x10ffff) cp = '?';
        }
        /* what a core font cannot draw is drawn as a question mark, and a control character would move the cursor of
         * the text (a title is a line, not a screen): Latin-1 only */
        if (cp < 0x20 || cp == 0x7f) cp = ' ';
        if (cp > 0xff) cp = '?';
        out[o++] = (char)cp;
        p += len;
    }
    out[o] = '\0';
}

unsigned long nexwm_frame_shade(unsigned long rgb, int percent)
{
    int r = (int)((rgb >> 16) & 0xff), g = (int)((rgb >> 8) & 0xff), b = (int)(rgb & 0xff);
    r = r * percent / 100;
    g = g * percent / 100;
    b = b * percent / 100;
    if (r > 255) r = 255; else if (r < 0) r = 0;
    if (g > 255) g = 255; else if (g < 0) g = 0;
    if (b > 255) b = 255; else if (b < 0) b = 0;
    return ((unsigned long)r << 16) | ((unsigned long)g << 8) | (unsigned long)b;
}

int nexwm_frame_icon_pick(const uint32_t *data, uint32_t n, int want, int *w, int *h, const uint32_t **pixels)
{
    if (w) *w = 0;
    if (h) *h = 0;
    if (pixels) *pixels = NULL;
    if (!data || n < 3 || want < 1) return 0;

    uint32_t i = 0;
    int best = -1, best_w = 0, best_h = 0;              /* the biggest one, for when none is big enough */
    int exact = -1, exact_w = 0, exact_h = 0;           /* the smallest one that is at least `want` px */
    while (i + 2 <= n) {
        uint32_t iw = data[i], ih = data[i + 1];
        if (iw == 0 || ih == 0 || iw > 4096 || ih > 4096) break;         /* not a picture: the list is not one */
        uint64_t count = (uint64_t)iw * ih;
        if (i + 2 + count > n) break;                                    /* it claims more pixels than it has */
        int size = (int)(iw < ih ? iw : ih);
        if (size >= want && (exact < 0 || size < exact)) {
            exact = (int)(i + 2);
            exact_w = (int)iw;
            exact_h = (int)ih;
        }
        if (size >= best) {
            best = (int)(i + 2);
            best_w = (int)iw;
            best_h = (int)ih;
        }
        i += (uint32_t)(2 + count);
    }
    if (exact >= 0) {
        if (w) *w = exact_w;
        if (h) *h = exact_h;
        if (pixels) *pixels = data + exact;
        return 1;
    }
    if (best >= 0) {
        if (w) *w = best_w;
        if (h) *h = best_h;
        if (pixels) *pixels = data + best;
        return 1;
    }
    return 0;
}

void nexwm_frame_icon_draw(const uint32_t *icon, int iw, int ih, int want, unsigned long background, uint8_t *out)
{
    if (!icon || !out || iw < 1 || ih < 1 || want < 1) return;
    unsigned br = (unsigned)((background >> 16) & 0xff);
    unsigned bg = (unsigned)((background >> 8) & 0xff);
    unsigned bb = (unsigned)(background & 0xff);
    size_t o = 0;
    for (int y = 0; y < want; y++) {
        int sy = y * ih / want;
        for (int x = 0; x < want; x++) {
            int sx = x * iw / want;
            uint32_t px = icon[(size_t)sy * (size_t)iw + (size_t)sx];
            unsigned a = (px >> 24) & 0xff;
            unsigned r = (px >> 16) & 0xff, g = (px >> 8) & 0xff, b = px & 0xff;
            unsigned m = 255 - a;
            out[o++] = (uint8_t)((b * a + bb * m) / 255);
            out[o++] = (uint8_t)((g * a + bg * m) / 255);
            out[o++] = (uint8_t)((r * a + br * m) / 255);
            out[o++] = 0;                                /* the fourth byte of a 24-bit picture: unused */
        }
    }
}
