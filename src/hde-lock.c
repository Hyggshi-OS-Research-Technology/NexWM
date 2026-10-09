/* hde-lock.c — HDE's lock screen, for both of HDE's sessions. On the "HDE" session it is an X11 window of its own
 * (XCB, override-redirect: no window manager can move it, draw over it or take the keyboard back) covering the whole
 * screen; on the "HDE (Wayland)" session it is a session-lock client (ext-session-lock-v1, which labwc — the
 * compositor HDE's Wayland session runs on — and HDE's own NexWM compositor offer), where the compositor itself stops
 * showing and feeding input to every other client. Both tell the same story: the clock, the date, the user's name and
 * avatar letter, a field that shows one dot per character typed, and a password checked through PAM — the same
 * accounts the login screen and sudo use. The picture is drawn with cairo into an image that is handed to the X
 * server (XCB) or to a Wayland shared-memory buffer, so both sessions look exactly alike.
 *
 * It is HDE's own lock screen, not NexWM's and not a wrapper around somebody else's: `hde-lock --check` says whether
 * this session can be locked, and src/hde-commands.h (HDE_SH_LOCK) runs hde-lock first and only falls back to
 * swaylock, gtklock, i3lock, slock, ... on a machine where HDE was built without what hde-lock needs.
 *
 * While the screen is locked hde-lock ignores TERM, INT, HUP, QUIT and USR1 — only the password unlocks it (or, on
 * X11, killing the process from outside the session, which is exactly what an X11 lock screen cannot prevent).
 *
 *   hde-lock --x11        one window over the whole X screen, the keyboard and the mouse grabbed
 *   hde-lock --wayland    the compositor's session lock (ext-session-lock-v1), one surface per output
 *   hde-lock --check      take no lock: exit 0 when this session can be locked, 3 when it cannot
 */
#define _GNU_SOURCE
#include "hde-lock-core.h"
#include "hde-build.h"

#include <errno.h>
#include <math.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <cairo/cairo.h>

#if defined(HDE_LOCK_HAVE_XCB)
#include <xcb/xcb.h>
#endif

#if defined(HDE_LOCK_HAVE_WAYLAND)
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "ext-session-lock-v1-client-protocol.h"
#endif

#if defined(HDE_LOCK_HAVE_PAM)
#include <security/pam_appl.h>
#endif

#if !defined(HDE_LOCK_HAVE_XCB) && !defined(HDE_LOCK_HAVE_WAYLAND)
#error "hde-lock needs a session to lock: build with libxcb (the X11 session) or wayland-client + xkbcommon (the Wayland session)"
#endif

/* The colours of the lock screen: HDE's dark greys with the blue accent, a soft red for a failed try. */
#define LOCK_BG        0x161a21
#define LOCK_CARD      0x222831
#define LOCK_CARD_EDGE 0x2f3742
#define LOCK_FIELD     0x171b22
#define LOCK_TEXT      0xe8eaed
#define LOCK_DIM       0x9aa3ad
#define LOCK_FAINT     0x6d7683
#define LOCK_ACCENT    0x3a86ff
#define LOCK_WRONG     0xff6b6b

/* The X11 keysyms this program has to name (see X11/keysymdef.h); spelling them out here keeps hde-lock free of the
 * libX11 headers, so a machine with only libxcb can still lock its X session. */
#define KS_Return    0xff0du
#define KS_KP_Enter  0xff8du
#define KS_BackSpace 0xff08u
#define KS_Escape    0xff1bu
#define KS_Delete    0xffffu
#define KS_Shift_L   0xffe1u
#define KS_Shift_R   0xffe2u
#define KS_Caps_Lock 0xffe5u

static void logline(const char *kind, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* INFO/WARN/FAIL lines go to stderr, which is where the HDE session log picks them up (hde-session, and the tests). */
static void logline(const char *kind, const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "hde-lock: %s: ", kind);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

/* ============================================================================================ what is on the screen */

struct lockui {
    HdeLockField field;
    bool failed;                /* the last password was not accepted */
    char user[64];              /* the login name, for PAM */
    char name[128];             /* the full name (or the login name) and its letter, for the card */
    char initial[8];
    char clock[8];              /* "09:41" */
    char date[64];              /* "Thursday 8 October" */
};

static void ui_names(struct lockui *ui)
{
    struct passwd *pw = getpwuid(getuid());
    const char *user = pw && pw->pw_name && pw->pw_name[0] ? pw->pw_name : "";
    const char *gecos = pw ? pw->pw_gecos : "";
    snprintf(ui->user, sizeof ui->user, "%s", user);
    hde_lock_display_name(user, gecos, ui->name, sizeof ui->name);
    hde_lock_initial(ui->name, ui->initial, sizeof ui->initial);
}

/* Returns true when the clock's minute changed (so the lock screen is drawn again at most once a minute). */
static bool ui_time(struct lockui *ui)
{
    time_t now = time(NULL);
    char clock[8];
    hde_lock_clock(now, clock, sizeof clock);
    if (!strcmp(clock, ui->clock)) return false;
    snprintf(ui->clock, sizeof ui->clock, "%s", clock);
    hde_lock_date(now, ui->date, sizeof ui->date);
    return true;
}

static void set_rgb(cairo_t *cr, uint32_t rgb)
{
    cairo_set_source_rgb(cr, ((rgb >> 16) & 0xff) / 255.0, ((rgb >> 8) & 0xff) / 255.0, (rgb & 0xff) / 255.0);
}

static void rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -0.5 * M_PI, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, 0.5 * M_PI);
    cairo_arc(cr, x + r, y + h - r, r, 0.5 * M_PI, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 1.5 * M_PI);
    cairo_close_path(cr);
}

static void text_centered(cairo_t *cr, double cx, double y, double size, uint32_t rgb, const char *s)
{
    cairo_text_extents_t ext;
    cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, size);
    cairo_text_extents(cr, s, &ext);
    set_rgb(cr, rgb);
    cairo_move_to(cr, cx - ext.width / 2 - ext.x_bearing, y);
    cairo_show_text(cr, s);
}

/* The whole lock screen: the clock and the date at the top, then the card with the name, its letter, the password
 * field (one dot per character) and the line that says what to do. "cx" is the middle of the screen it is drawn on. */
static void ui_draw(cairo_t *cr, const struct lockui *ui, int w, int h, double cx)
{
    set_rgb(cr, LOCK_BG);
    cairo_paint(cr);

    double clock_size = h >= 600 ? 54 : 36;
    text_centered(cr, cx, h * 0.22, clock_size, LOCK_TEXT, ui->clock);
    text_centered(cr, cx, h * 0.22 + clock_size * 0.95, 16, LOCK_DIM, ui->date);

    double card_w = w < 520 ? w - 40 : 420;
    if (card_w < 220) card_w = 220;
    double card_h = 300;
    double card_x = cx - card_w / 2, card_y = h * 0.62 - card_h / 2;
    if (card_y + card_h > h - 20) card_y = h - card_h - 20;
    if (card_y < 20) card_y = 20;

    rounded_rect(cr, card_x, card_y, card_w, card_h, 14);
    set_rgb(cr, LOCK_CARD);
    cairo_fill_preserve(cr);
    set_rgb(cr, LOCK_CARD_EDGE);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);

    double ccx = card_x + card_w / 2;
    cairo_arc(cr, ccx, card_y + 54, 28, 0, 2 * M_PI);
    set_rgb(cr, LOCK_ACCENT);
    cairo_fill(cr);
    text_centered(cr, ccx, card_y + 66, 28, 0xffffff, ui->initial);
    text_centered(cr, ccx, card_y + 118, 17, LOCK_TEXT, ui->name);

    double fw = card_w - 60, fh = 38;
    double fx = ccx - fw / 2, fy = card_y + 148;
    rounded_rect(cr, fx, fy, fw, fh, 9);
    set_rgb(cr, LOCK_FIELD);
    cairo_fill_preserve(cr);
    set_rgb(cr, ui->failed ? LOCK_WRONG : LOCK_ACCENT);
    cairo_set_line_width(cr, ui->failed ? 2 : 1.5);
    cairo_stroke(cr);

    size_t dots = hde_lock_field_chars(&ui->field);
    if (dots > 20) dots = 20;                     /* the field shows what fits; the password may be longer */
    double spacing = 13, r = 4.5;
    double dots_w = dots ? (double)(dots - 1) * spacing : 0;
    for (size_t i = 0; i < dots; i++)
        cairo_arc(cr, ccx - dots_w / 2 + (double)i * spacing, fy + fh / 2, r, 0, 2 * M_PI);
    if (dots) {
        set_rgb(cr, ui->failed ? LOCK_WRONG : LOCK_TEXT);
        cairo_fill(cr);
    }

    text_centered(cr, ccx, fy + fh + 28, 14, ui->failed ? LOCK_WRONG : LOCK_DIM,
                  ui->failed ? hde_lock_wrong() : hde_lock_prompt());
    text_centered(cr, ccx, card_y + card_h - 22, 12, LOCK_FAINT, hde_lock_keys());
}

/* ====================================================================================================== the password */

#if defined(HDE_LOCK_HAVE_PAM)
struct pam_credentials {
    const char *username;
    const char *password;
};

static void pam_responses_free(struct pam_response *responses, int n)
{
    if (!responses) return;
    for (int i = 0; i < n; i++) free(responses[i].resp);
    free(responses);
}

static int pam_conv_(int n, const struct pam_message **msg, struct pam_response **resp, void *data)
{
    if (n <= 0 || !msg || !resp || !data) return PAM_CONV_ERR;
    const struct pam_credentials *credentials = data;
    struct pam_response *r = calloc((size_t)n, sizeof *r);
    if (!r) return PAM_BUF_ERR;
    for (int i = 0; i < n; i++) {
        if (!msg[i]) {
            pam_responses_free(r, n);
            return PAM_CONV_ERR;
        }
        const char *answer = NULL;
        switch (msg[i]->msg_style) {
        case PAM_PROMPT_ECHO_OFF: answer = credentials->password; break;
        case PAM_PROMPT_ECHO_ON:  answer = credentials->username; break;
        case PAM_ERROR_MSG:
        case PAM_TEXT_INFO:       break;
        default:
            pam_responses_free(r, n);
            return PAM_CONV_ERR;
        }
        if (answer) {
            r[i].resp = strdup(answer);
            if (!r[i].resp) {
                pam_responses_free(r, n);
                return PAM_BUF_ERR;
            }
        }
    }
    *resp = r;
    return PAM_SUCCESS;
}
#endif

/* Checks the password against the account PAM knows, for the user who is logged in (getuid()): a lock screen is not a
 * login screen and has no user name to ask for. Returns true when the password was accepted. */
static bool password_ok(const struct lockui *ui, char *why, size_t why_len)
{
    why[0] = '\0';
#if defined(HDE_LOCK_HAVE_PAM)
    /* A system with its own /etc/pam.d/hde-lock says what locking means there; otherwise the service the login
     * screen uses ("login") does exactly what is wanted here: check that account's password. */
    const char *service = access("/etc/pam.d/hde-lock", R_OK) == 0 ? "hde-lock" : "login";
    pam_handle_t *ph = NULL;
    struct pam_credentials credentials = { ui->user, ui->field.text };
    struct pam_conv conv = { pam_conv_, &credentials };
    int rc = pam_start(service, ui->user, &conv, &ph);
    if (rc == PAM_SUCCESS) rc = pam_authenticate(ph, 0);
    if (rc != PAM_SUCCESS) {
        const char *msg = ph ? pam_strerror(ph, rc) : "PAM could not be started";
        snprintf(why, why_len, "%s (PAM service '%s', status %d)",
                 msg ? msg : "the password was not accepted", service, rc);
    }
    if (ph) pam_end(ph, rc);
    return rc == PAM_SUCCESS;
#else
    (void)ui;
    snprintf(why, why_len, "%s", hde_lock_no_pam());
    return false;
#endif
}

/* True when the caller should unlock and exit 0. */
static bool try_unlock(struct lockui *ui)
{
    char why[256];
    if (password_ok(ui, why, sizeof why)) {
        logline("INFO", "the password of '%s' was accepted: unlocking", ui->user);
        return true;
    }
    logline("WARN", "the password was not accepted (%s)", why[0] ? why : "wrong password");
    ui->failed = true;
    hde_lock_field_clear(&ui->field);
    return false;
}

/* ============================================================================================================= X11 */

#if defined(HDE_LOCK_HAVE_XCB)
struct x11_lock {
    xcb_connection_t *conn;
    xcb_screen_t *screen;
    xcb_window_t win;
    xcb_gcontext_t gc;
    uint32_t *pixels;
    cairo_surface_t *surf;
    int w, h;
    uint32_t *keysyms;                  /* the keyboard map, as the X server has it */
    int min_keycode, max_keycode, per_keycode;
    bool shift, caps;
    bool click_seen;                    /* the first click that landed on the lock screen instead of the session */
    struct lockui ui;
};

static uint32_t intern(xcb_connection_t *c, const char *name, bool only_if_exists)
{
    xcb_intern_atom_cookie_t ck = xcb_intern_atom(c, only_if_exists, (uint16_t)strlen(name), name);
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(c, ck, NULL);
    uint32_t atom = r ? r->atom : XCB_ATOM_NONE;
    free(r);
    return atom;
}

static void x11_property(xcb_connection_t *c, xcb_window_t win, const char *name, const char *type, uint32_t value)
{
    uint32_t a = intern(c, name, false);
    if (a == XCB_ATOM_NONE) return;
    uint32_t t = intern(c, type, false);
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, win, a, t, 32, 1, &value);
}

static void x11_paint(struct x11_lock *x)
{
    cairo_t *cr = cairo_create(x->surf);
    /* The card follows the mouse, clamped so that it is always fully on screen: with two screens the lock screen
     * appears on the one the user is looking at, and a mouse in the corner does not put the card half off it. */
    double cx = x->w / 2.0;
    xcb_query_pointer_reply_t *qr =
        xcb_query_pointer_reply(x->conn, xcb_query_pointer(x->conn, x->screen->root), NULL);
    if (qr) {
        cx = qr->root_x;
        double half = (x->w < 520 ? x->w - 40 : 420) / 2.0 + 12;
        if (cx < half) cx = half;
        if (cx > x->w - half) cx = x->w - half;
        free(qr);
    }
    ui_draw(cr, &x->ui, x->w, x->h, cx);
    cairo_destroy(cr);
    cairo_surface_flush(x->surf);

    xcb_put_image(x->conn, XCB_IMAGE_FORMAT_Z_PIXMAP, x->win, x->gc, (uint16_t)x->w, (uint16_t)x->h, 0, 0, 0,
                  x->screen->root_depth, (uint32_t)(x->w * x->h * 4), (const uint8_t *)x->pixels);
    xcb_flush(x->conn);
}

static void x11_keymap(struct x11_lock *x)
{
    free(x->keysyms);
    x->keysyms = NULL;
    xcb_get_keyboard_mapping_reply_t *r =
        xcb_get_keyboard_mapping_reply(x->conn,
                                       xcb_get_keyboard_mapping(x->conn, x->min_keycode,
                                                                (uint8_t)(x->max_keycode - x->min_keycode + 1)),
                                       NULL);
    if (!r) return;
    x->per_keycode = r->keysyms_per_keycode;
    size_t n = (size_t)(x->max_keycode - x->min_keycode + 1) * (size_t)x->per_keycode;
    x->keysyms = malloc(n * sizeof *x->keysyms);
    if (x->keysyms) memcpy(x->keysyms, xcb_get_keyboard_mapping_keysyms(r), n * sizeof *x->keysyms);
    free(r);
}

/* The keysym for a keycode, taking Shift (and Caps Lock for letters) into account, the way XLookupString would. */
static uint32_t x11_keysym(struct x11_lock *x, uint8_t keycode)
{
    if (!x->keysyms || keycode < x->min_keycode || keycode > x->max_keycode || x->per_keycode < 1) return 0;
    size_t base = (size_t)(keycode - x->min_keycode) * (size_t)x->per_keycode;
    uint32_t ks = x->keysyms[base];
    if (x->shift && x->per_keycode > 1) ks = x->keysyms[base + 1];
    if (x->caps && !x->shift && ks >= 'a' && ks <= 'z') ks = ks - 'a' + 'A';
    if (x->caps && x->shift && ks >= 'A' && ks <= 'Z') ks = ks - 'A' + 'a';
    return ks;
}

/* A keysym as the UTF-8 character it stands for (Latin-1 directly, Unicode keysyms by their number). */
static void keysym_utf8(uint32_t ks, char *out, size_t len)
{
    uint32_t c = 0;
    if (ks >= 0x20 && ks <= 0x7e) c = ks;
    else if (ks >= 0xa0 && ks <= 0xff) c = ks;
    else if (ks >= 0x1000000 && ks <= 0x10ffff) c = ks - 0x1000000;       /* Unicode keysyms */
    else if (ks >= 0x01000000 && ks <= 0x0100ffff) c = ks - 0x01000000;   /* the X11 quirk for Latin-1 */
    if (!c || len < 5) { if (len) out[0] = '\0'; return; }
    if (c < 0x80) { out[0] = (char)c; out[1] = '\0'; }
    else if (c < 0x800) { out[0] = (char)(0xc0 | (c >> 6)); out[1] = (char)(0x80 | (c & 0x3f)); out[2] = '\0'; }
    else {
        out[0] = (char)(0xe0 | (c >> 12));
        out[1] = (char)(0x80 | ((c >> 6) & 0x3f));
        out[2] = (char)(0x80 | (c & 0x3f));
        out[3] = '\0';
    }
}

static void x11_key(struct x11_lock *x, uint32_t ks, bool *unlock)
{
    if (ks == KS_Shift_L || ks == KS_Shift_R) { x->shift = true; return; }
    if (ks == KS_Caps_Lock) { x->caps = !x->caps; return; }
    if (ks == KS_Return || ks == KS_KP_Enter) {
        if (try_unlock(&x->ui)) { *unlock = true; return; }
        x11_paint(x);
    } else if (ks == KS_BackSpace) {
        hde_lock_field_backspace(&x->ui.field);
        x11_paint(x);
    } else if (ks == KS_Escape || ks == KS_Delete) {
        x->ui.failed = false;
        hde_lock_field_clear(&x->ui.field);
        x11_paint(x);
    } else {
        char utf8[8];
        keysym_utf8(ks, utf8, sizeof utf8);
        if (utf8[0]) {
            x->ui.failed = false;                 /* typing again is the answer to "wrong password" */
            hde_lock_field_insert(&x->ui.field, utf8);
            x11_paint(x);
        }
    }
}

/* Why the X server said no to a grab: the words the log needs to be read without the header of xproto.h. */
static const char *grab_status_why(uint8_t status)
{
    switch (status) {
    case XCB_GRAB_STATUS_SUCCESS: return "it was taken";
    case XCB_GRAB_STATUS_ALREADY_GRABBED: return "another client is holding it";
    case XCB_GRAB_STATUS_FROZEN: return "another client is holding it, frozen";
    case XCB_GRAB_STATUS_NOT_VIEWABLE: return "the lock window is not on screen";
    case XCB_GRAB_STATUS_INVALID_TIME: return "the time it was asked with was not valid";
    default: return "the X server refused it";
    }
}

/* The X errors a lock screen runs into, by name (the numbers alone say little in a log). */
static const char *x11_error_name(uint8_t code)
{
    switch (code) {
    case XCB_REQUEST: return "BadRequest";
    case XCB_VALUE: return "BadValue";
    case XCB_WINDOW: return "BadWindow";
    case XCB_PIXMAP: return "BadPixmap";
    case XCB_ATOM: return "BadAtom";
    case XCB_CURSOR: return "BadCursor";
    case XCB_FONT: return "BadFont";
    case XCB_MATCH: return "BadMatch";
    case XCB_DRAWABLE: return "BadDrawable";
    case XCB_ACCESS: return "BadAccess";
    case XCB_ALLOC: return "BadAlloc";
    case XCB_COLORMAP: return "BadColor";
    case XCB_G_CONTEXT: return "BadGC";
    case XCB_ID_CHOICE: return "BadIDChoice";
    case XCB_NAME: return "BadName";
    case XCB_LENGTH: return "BadLength";
    case XCB_IMPLEMENTATION: return "BadImplementation";
    default: return "an X error";
    }
}

/* One line on a grab that did not work: the status the X server answered with, or the error it raised instead. */
static void why_of_grab(char *out, size_t n, bool answered, uint8_t status, xcb_generic_error_t *err)
{
    if (err)
        snprintf(out, n, "%s (X error %d)", x11_error_name(err->error_code), err->error_code);
    else if (answered)
        snprintf(out, n, "%s", grab_status_why(status));
    else
        snprintf(out, n, "no answer from the X server");
}

static int x11_lock_run(struct lockui *ui)
{
    struct x11_lock x;
    memset(&x, 0, sizeof x);
    x.ui = *ui;
    int screen_num = 0;
    x.conn = xcb_connect(NULL, &screen_num);
    if (xcb_connection_has_error(x.conn)) {
        logline("FAIL", "no X display to lock (DISPLAY=%s)", getenv("DISPLAY") ? getenv("DISPLAY") : "unset");
        xcb_disconnect(x.conn);
        return 3;
    }
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(x.conn));
    for (int i = 0; i < screen_num && it.rem; i++) xcb_screen_next(&it);
    x.screen = it.data;
    if (!x.screen || (x.screen->root_depth != 24 && x.screen->root_depth != 32)) {
        logline("FAIL", "this X screen has no 24-bit picture to draw the lock screen into");
        xcb_disconnect(x.conn);
        return 3;
    }
    x.w = x.screen->width_in_pixels;
    x.h = x.screen->height_in_pixels;
    x.min_keycode = xcb_get_setup(x.conn)->min_keycode;
    x.max_keycode = xcb_get_setup(x.conn)->max_keycode;

    /* An invisible pointer: while the screen is locked nothing on it should follow the mouse. Both the source
     * and the mask of a cursor are pixmaps (only the mask may be None), so a 1x1 pixmap is drawn for it with
     * every bit clear: a cursor the screen shines through. */
    xcb_pixmap_t empty = xcb_generate_id(x.conn);
    xcb_gcontext_t fill = xcb_generate_id(x.conn);
    xcb_cursor_t blank = xcb_generate_id(x.conn);
    uint32_t zero = 0;
    xcb_create_pixmap(x.conn, 1, empty, x.screen->root, 1, 1);
    xcb_create_gc(x.conn, fill, empty, XCB_GC_FOREGROUND, &zero);
    xcb_rectangle_t dot = { 0, 0, 1, 1 };
    xcb_poly_fill_rectangle(x.conn, empty, fill, 1, &dot);
    xcb_free_gc(x.conn, fill);
    xcb_generic_error_t *cur_err =
        xcb_request_check(x.conn, xcb_create_cursor_checked(x.conn, blank, empty, empty, 0, 0, 0, 0, 0, 0, 0, 0));
    xcb_free_pixmap(x.conn, empty);
    if (cur_err) {
        /* Without the cursor the lock window cannot be created either (the X server refuses that too): say so
         * here instead of looking, later on, like another program holding the keyboard. */
        logline("FAIL", "the X server refused the invisible cursor (%s, X error %d): not locking the screen",
                x11_error_name(cur_err->error_code), cur_err->error_code);
        free(cur_err);
        xcb_disconnect(x.conn);
        return 3;
    }

    uint32_t values[4];
    values[0] = x.screen->black_pixel;
    values[1] = 1;                                     /* override-redirect: no window manager touches this window */
    values[2] = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE |
                XCB_EVENT_MASK_BUTTON_PRESS;   /* a click is received here, so that it says so in the log */
                /* MappingNotify needs no mask: the X server always sends it to every client */
    values[3] = blank;
    x.win = xcb_generate_id(x.conn);
    xcb_create_window(x.conn, XCB_COPY_FROM_PARENT, x.win, x.screen->root, 0, 0, (uint16_t)x.w, (uint16_t)x.h, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT,
                      XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK | XCB_CW_CURSOR, values);
    xcb_change_property(x.conn, XCB_PROP_MODE_REPLACE, x.win, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, 15,
                        "HDE lock screen");
    xcb_change_property(x.conn, XCB_PROP_MODE_REPLACE, x.win, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, 19,
                        "hde-lock\0HDE-Lock\0\0");
    xcb_map_window(x.conn, x.win);
    uint32_t stack = XCB_STACK_MODE_ABOVE;
    xcb_configure_window(x.conn, x.win, XCB_CONFIG_WINDOW_STACK_MODE, &stack);
    x.gc = xcb_generate_id(x.conn);
    xcb_create_gc(x.conn, x.gc, x.win, 0, NULL);
    xcb_flush(x.conn);

    /* The window has to be there and on the screen: if the X server refused it, the screen would stay unlocked while
     * hde-lock looked as if it were locking it (there is no window manager to notice, this window is nobody's). */
    xcb_generic_error_t *win_err = NULL;
    xcb_get_window_attributes_reply_t *attr =
        xcb_get_window_attributes_reply(x.conn, xcb_get_window_attributes(x.conn, x.win), &win_err);
    bool on_screen = attr && attr->map_state == XCB_MAP_STATE_VIEWABLE;
    free(attr);
    if (!on_screen) {
        logline("FAIL", "the lock window is not on the screen (%s): not locking the screen",
                win_err ? x11_error_name(win_err->error_code) : "the X server kept it off the screen");
        free(win_err);
        xcb_destroy_window(x.conn, x.win);
        xcb_flush(x.conn);
        xcb_disconnect(x.conn);
        return 3;
    }
    free(win_err);

    /* The keyboard and the mouse, kept by this window until the password is right. The keyboard is the one that
     * matters: without it the keys would go to whatever window has the focus. As long as this lock window is up
     * nothing on the screen can be clicked, so a program that holds the pointer for a moment (a menu, a flyout,
     * the window manager in the middle of a drag) must not stop the lock: the mouse is tried again below while
     * the screen is locked. Either grab may be refused for a moment while the lock is going up: try for a few
     * seconds, and say what said no if it stays refused. */
    bool key_held = false, ptr_held = false;
    char key_why[64] = "no answer from the X server", ptr_why[64] = "no answer from the X server";
    for (int i = 0; i < 80 && !key_held; i++) {
        if (!key_held) {
            xcb_generic_error_t *err = NULL;
            xcb_grab_keyboard_reply_t *k =
                xcb_grab_keyboard_reply(x.conn, xcb_grab_keyboard(x.conn, 0, x.win, XCB_CURRENT_TIME,
                                                                  XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC), &err);
            key_held = k && k->status == XCB_GRAB_STATUS_SUCCESS;
            why_of_grab(key_why, sizeof key_why, k != NULL, k ? k->status : 0, err);
            free(err);
            free(k);
        }
        if (!ptr_held) {
            xcb_generic_error_t *err = NULL;
            xcb_grab_pointer_reply_t *p =
                xcb_grab_pointer_reply(x.conn,
                                       xcb_grab_pointer(x.conn, 0, x.win, XCB_EVENT_MASK_BUTTON_PRESS,
                                                        XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE, blank,
                                                        XCB_CURRENT_TIME),
                                       &err);
            ptr_held = p && p->status == XCB_GRAB_STATUS_SUCCESS;
            why_of_grab(ptr_why, sizeof ptr_why, p != NULL, p ? p->status : 0, err);
            free(err);
            free(p);
        }
        if (key_held) break;
        usleep(100 * 1000);
    }
    if (!key_held) {
        logline("FAIL", "the keyboard could not be held (%s): not locking the screen", key_why);
        xcb_destroy_window(x.conn, x.win);
        xcb_flush(x.conn);
        xcb_disconnect(x.conn);
        return 4;
    }
    if (!ptr_held)
        logline("WARN", "the mouse could not be held (%s): the lock screen keeps trying for it", ptr_why);

    x.pixels = calloc((size_t)x.w * (size_t)x.h, 4);
    if (!x.pixels) {
        logline("FAIL", "no memory for a %dx%d lock screen", x.w, x.h);
        xcb_disconnect(x.conn);
        return 3;
    }
    x.surf = cairo_image_surface_create_for_data((unsigned char *)x.pixels, CAIRO_FORMAT_ARGB32, x.w, x.h,
                                                 cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, x.w));
    x11_keymap(&x);
    x11_paint(&x);

    /* Other HDE programs (and the tests) can see that the screen is locked. */
    x11_property(x.conn, x.screen->root, "_HDE_LOCKED", "CARDINAL", 1);
    xcb_flush(x.conn);
    logline("INFO", "the X11 session is locked: %dx%d, %s ('%s')", x.w, x.h,
            ptr_held ? "the keyboard and the mouse are held" : "the keyboard is held", x.ui.user);

    int fd = xcb_get_file_descriptor(x.conn);
    bool unlock = false;
    while (!unlock) {
        struct pollfd pfd = { fd, POLLIN, 0 };
        if (poll(&pfd, 1, 500) < 0 && errno != EINTR) break;
        if (!ptr_held) {   /* the program that had the mouse may have let go of it by now */
            xcb_grab_pointer_reply_t *p =
                xcb_grab_pointer_reply(x.conn,
                                       xcb_grab_pointer(x.conn, 0, x.win, XCB_EVENT_MASK_BUTTON_PRESS,
                                                        XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE, blank,
                                                        XCB_CURRENT_TIME),
                                       NULL);
            ptr_held = p && p->status == XCB_GRAB_STATUS_SUCCESS;
            free(p);
            if (ptr_held)
                logline("INFO", "the mouse is held now: the pointer is hidden and cannot click anything");
        }
        xcb_generic_event_t *ev;
        while ((ev = xcb_poll_for_event(x.conn))) {
            uint8_t type = ev->response_type & 0x7f;
            if (type == XCB_EXPOSE) {
                x11_paint(&x);
            } else if (type == XCB_MAPPING_NOTIFY) {
                x11_keymap(&x);
            } else if (type == XCB_KEY_PRESS) {
                x11_key(&x, x11_keysym(&x, ((xcb_key_press_event_t *)ev)->detail), &unlock);
            } else if (type == XCB_KEY_RELEASE) {
                uint32_t ks = x11_keysym(&x, ((xcb_key_release_event_t *)ev)->detail);
                if (ks == KS_Shift_L || ks == KS_Shift_R) x.shift = false;
            } else if (type == XCB_BUTTON_PRESS && !x.click_seen) {
                x.click_seen = true;
                logline("INFO", "a click on the lock screen: it went to the lock screen, not to the session behind it");
            }
            free(ev);
        }
        xcb_flush(x.conn);
        if (ui_time(&x.ui) && !unlock) x11_paint(&x);
        if (xcb_connection_has_error(x.conn)) break;   /* the X server went away: nothing left to keep locked */
    }
    if (unlock) {
        xcb_ungrab_keyboard(x.conn, XCB_CURRENT_TIME);
        xcb_ungrab_pointer(x.conn, XCB_CURRENT_TIME);
        x11_property(x.conn, x.screen->root, "_HDE_LOCKED", "CARDINAL", 0);
        xcb_flush(x.conn);
        logline("INFO", "the X11 session is unlocked");
    }
    cairo_surface_destroy(x.surf);
    free(x.pixels);
    free(x.keysyms);
    xcb_disconnect(x.conn);
    return 0;
}
#endif /* HDE_LOCK_HAVE_XCB */

/* ========================================================================================================= Wayland */

#if defined(HDE_LOCK_HAVE_WAYLAND)
#define LOCK_OUTPUTS_MAX 8

struct wl_lock;

struct output_lock {
    struct wl_output *output;
    struct wl_surface *surface;
    struct ext_session_lock_surface_v1 *lock_surface;
    struct wl_buffer *buffer;
    void *data;
    size_t size;
    int w, h;
    struct wl_lock *lock;
};

struct wl_lock {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct wl_keyboard *keyboard;
    struct ext_session_lock_manager_v1 *manager;
    struct ext_session_lock_v1 *lock;
    struct wl_output *shown[LOCK_OUTPUTS_MAX];   /* the outputs the compositor has, as they come and go */
    int n_shown;
    struct output_lock outputs[LOCK_OUTPUTS_MAX]; /* one lock surface per output, once the lock object exists */
    int n_outputs;
    bool locked, finished, unlock;
    struct xkb_context *xkb;
    struct xkb_keymap *keymap;
    struct xkb_state *state;
    struct lockui *ui;
};

static void wl_paint_output(struct output_lock *o)
{
    if (!o->data || o->w <= 0 || o->h <= 0) return;
    int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, o->w);
    cairo_surface_t *surf = cairo_image_surface_create_for_data(o->data, CAIRO_FORMAT_ARGB32, o->w, o->h, stride);
    cairo_t *cr = cairo_create(surf);
    ui_draw(cr, o->lock->ui, o->w, o->h, o->w / 2.0);
    cairo_destroy(cr);
    cairo_surface_flush(surf);
    cairo_surface_destroy(surf);
    wl_surface_attach(o->surface, o->buffer, 0, 0);
    wl_surface_damage(o->surface, 0, 0, o->w, o->h);
    wl_surface_commit(o->surface);
}

static void wl_paint_all(struct wl_lock *l)
{
    for (int i = 0; i < l->n_outputs; i++)
        if (l->outputs[i].buffer) wl_paint_output(&l->outputs[i]);
}

static int wl_shm_fd(const char *name, size_t size)
{
    int fd = memfd_create(name, MFD_CLOEXEC);
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)size) < 0) { close(fd); return -1; }
    return fd;
}

/* A buffer of exactly the size the compositor asked for: committing anything else on a lock surface is a protocol
 * error, so the surface is (re)sized before it is painted. */
static bool wl_output_resize(struct output_lock *o, int w, int h)
{
    if (o->data && o->w == w && o->h == h) return true;
    if (o->buffer) { wl_buffer_destroy(o->buffer); o->buffer = NULL; }
    if (o->data) { munmap(o->data, o->size); o->data = NULL; o->size = 0; }
    int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, w);
    size_t size = (size_t)stride * (size_t)h;
    int fd = wl_shm_fd("hde-lock", size);
    if (fd < 0) { logline("FAIL", "no shared memory for a %dx%d lock screen", w, h); return false; }
    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        logline("FAIL", "the lock screen picture could not be mapped");
        return false;
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(o->lock->shm, fd, (int)size);
    o->buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    o->data = data;
    o->size = size;
    o->w = w;
    o->h = h;
    return o->buffer != NULL;
}

static void ls_configure(void *data, struct ext_session_lock_surface_v1 *ls, uint32_t serial, uint32_t w, uint32_t h)
{
    struct output_lock *o = data;
    ext_session_lock_surface_v1_ack_configure(ls, serial);
    if (!wl_output_resize(o, (int)w, (int)h)) return;
    wl_paint_output(o);
}

static const struct ext_session_lock_surface_v1_listener ls_listener = { .configure = ls_configure };

static void lock_locked(void *data, struct ext_session_lock_v1 *lock)
{
    (void)lock;
    struct wl_lock *l = data;
    l->locked = true;
    logline("INFO", "the Wayland session is locked: ext-session-lock-v1, %d output%s", l->n_outputs,
            l->n_outputs == 1 ? "" : "s");
}

static void lock_finished(void *data, struct ext_session_lock_v1 *lock)
{
    (void)lock;
    struct wl_lock *l = data;
    l->finished = true;
}

static const struct ext_session_lock_v1_listener lock_listener = { .locked = lock_locked, .finished = lock_finished };

/* A lock surface for one output. The compositor sends the size it wants on that output's lock surface. */
static void lock_add_output(struct wl_lock *l, struct wl_output *output)
{
    if (!l->lock || l->n_outputs >= LOCK_OUTPUTS_MAX) return;
    struct output_lock *o = &l->outputs[l->n_outputs++];
    memset(o, 0, sizeof *o);
    o->output = output;
    o->lock = l;
    o->surface = wl_compositor_create_surface(l->compositor);
    if (!o->surface) return;
    o->lock_surface = ext_session_lock_v1_get_lock_surface(l->lock, o->surface, output);
    ext_session_lock_surface_v1_add_listener(o->lock_surface, &ls_listener, o);
}

static void kbd_keymap(void *data, struct wl_keyboard *kbd, uint32_t format, int32_t fd, uint32_t size)
{
    (void)kbd;
    struct wl_lock *l = data;
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || size == 0) { close(fd); return; }
    char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return;
    struct xkb_keymap *km = xkb_keymap_new_from_string(l->xkb, map, XKB_KEYMAP_FORMAT_TEXT_V1,
                                                      XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    if (!km) return;
    struct xkb_state *st = xkb_state_new(km);
    if (!st) { xkb_keymap_unref(km); return; }
    if (l->keymap) xkb_keymap_unref(l->keymap);
    if (l->state) xkb_state_unref(l->state);
    l->keymap = km;
    l->state = st;
}

static void kbd_enter(void *data, struct wl_keyboard *kbd, uint32_t serial, struct wl_surface *s, struct wl_array *keys)
{
    (void)data; (void)kbd; (void)serial; (void)s; (void)keys;
}

static void kbd_leave(void *data, struct wl_keyboard *kbd, uint32_t serial, struct wl_surface *s)
{
    (void)data; (void)kbd; (void)serial; (void)s;
}

static void kbd_key(void *data, struct wl_keyboard *kbd, uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
    (void)kbd; (void)serial; (void)time;
    struct wl_lock *l = data;
    if (!l->state) return;
    xkb_keycode_t code = key + 8;                  /* the protocol sends evdev codes; XKB wants the +8 keycode */
    xkb_state_update_key(l->state, code, state == WL_KEYBOARD_KEY_STATE_PRESSED ? XKB_KEY_DOWN : XKB_KEY_UP);
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED) return;
    xkb_keysym_t ks = xkb_state_key_get_one_sym(l->state, code);
    if (ks == XKB_KEY_Return || ks == XKB_KEY_KP_Enter) {
        if (try_unlock(l->ui)) l->unlock = true;
        else wl_paint_all(l);
    } else if (ks == XKB_KEY_BackSpace) {
        hde_lock_field_backspace(&l->ui->field);
        wl_paint_all(l);
    } else if (ks == XKB_KEY_Escape || ks == XKB_KEY_Delete) {
        l->ui->failed = false;
        hde_lock_field_clear(&l->ui->field);
        wl_paint_all(l);
    } else {
        char utf8[16];
        int n = xkb_state_key_get_utf8(l->state, code, utf8, sizeof utf8);
        if (n > 0 && (unsigned char)utf8[0] >= 0x20) {
            l->ui->failed = false;
            hde_lock_field_insert(&l->ui->field, utf8);
            wl_paint_all(l);
        }
    }
}

static void kbd_modifiers(void *data, struct wl_keyboard *kbd, uint32_t serial, uint32_t dep, uint32_t lat,
                          uint32_t lock, uint32_t group)
{
    (void)kbd; (void)serial;
    struct wl_lock *l = data;
    if (l->state) xkb_state_update_mask(l->state, dep, lat, lock, 0, 0, group);
}

static void kbd_repeat(void *data, struct wl_keyboard *kbd, int32_t rate, int32_t delay)
{
    (void)data; (void)kbd; (void)rate; (void)delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = kbd_keymap, .enter = kbd_enter, .leave = kbd_leave, .key = kbd_key,
    .modifiers = kbd_modifiers, .repeat_info = kbd_repeat,
};

static void seat_caps(void *data, struct wl_seat *seat, uint32_t caps)
{
    struct wl_lock *l = data;
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !l->keyboard) {
        l->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(l->keyboard, &keyboard_listener, l);
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
    (void)data; (void)seat; (void)name;
}

static const struct wl_seat_listener seat_listener = { .capabilities = seat_caps, .name = seat_name };

static void out_geometry(void *d, struct wl_output *o, int32_t x, int32_t y, int32_t pw, int32_t ph, int32_t sub,
                         const char *make, const char *model, int32_t transform)
{
    (void)d; (void)o; (void)x; (void)y; (void)pw; (void)ph; (void)sub; (void)make; (void)model; (void)transform;
}
static void out_mode(void *d, struct wl_output *o, uint32_t flags, int32_t w, int32_t h, int32_t refresh)
{
    (void)d; (void)o; (void)flags; (void)w; (void)h; (void)refresh;
}
static void out_done(void *d, struct wl_output *o) { (void)d; (void)o; }
static void out_scale(void *d, struct wl_output *o, int32_t factor) { (void)d; (void)o; (void)factor; }
static void out_name(void *d, struct wl_output *o, const char *name) { (void)d; (void)o; (void)name; }
static void out_description(void *d, struct wl_output *o, const char *desc) { (void)d; (void)o; (void)desc; }

static const struct wl_output_listener output_listener = {
    .geometry = out_geometry, .mode = out_mode, .done = out_done, .scale = out_scale,
    .name = out_name, .description = out_description,
};

static void reg_global(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t version)
{
    struct wl_lock *l = data;
    if (!strcmp(iface, wl_compositor_interface.name))
        l->compositor = wl_registry_bind(reg, name, &wl_compositor_interface, version < 4 ? version : 4);
    else if (!strcmp(iface, wl_shm_interface.name))
        l->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, wl_seat_interface.name)) {
        l->seat = wl_registry_bind(reg, name, &wl_seat_interface, version < 5 ? version : 5);
        wl_seat_add_listener(l->seat, &seat_listener, l);
    } else if (!strcmp(iface, wl_output_interface.name)) {
        struct wl_output *o = wl_registry_bind(reg, name, &wl_output_interface, version < 4 ? version : 4);
        wl_output_add_listener(o, &output_listener, l);
        if (l->n_shown < LOCK_OUTPUTS_MAX) l->shown[l->n_shown++] = o;
        lock_add_output(l, o);                     /* nothing happens before the lock object exists */
    } else if (!strcmp(iface, ext_session_lock_manager_v1_interface.name))
        l->manager = wl_registry_bind(reg, name, &ext_session_lock_manager_v1_interface, 1);
}

static void reg_remove(void *data, struct wl_registry *reg, uint32_t name)
{
    (void)data; (void)reg; (void)name;
}

static const struct wl_registry_listener registry_listener = { .global = reg_global, .global_remove = reg_remove };

static void wl_lock_cleanup(struct wl_lock *l)
{
    for (int i = 0; i < l->n_outputs; i++) {
        struct output_lock *o = &l->outputs[i];
        if (o->buffer) wl_buffer_destroy(o->buffer);
        if (o->data) munmap(o->data, o->size);
        if (o->lock_surface) ext_session_lock_surface_v1_destroy(o->lock_surface);
        if (o->surface) wl_surface_destroy(o->surface);
    }
    if (l->keyboard) wl_keyboard_destroy(l->keyboard);
    if (l->state) xkb_state_unref(l->state);
    if (l->keymap) xkb_keymap_unref(l->keymap);
    if (l->xkb) xkb_context_unref(l->xkb);
    if (l->lock) ext_session_lock_v1_destroy(l->lock);
}

static int wl_lock_run(struct lockui *ui)
{
    struct wl_lock l;
    memset(&l, 0, sizeof l);
    l.ui = ui;
    l.display = wl_display_connect(NULL);
    if (!l.display) {
        logline("FAIL", "no Wayland display to lock (WAYLAND_DISPLAY=%s)",
                getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "unset");
        return 3;
    }
    l.registry = wl_display_get_registry(l.display);
    wl_registry_add_listener(l.registry, &registry_listener, &l);
    if (wl_display_roundtrip(l.display) < 0) {
        logline("FAIL", "the Wayland compositor did not answer");
        wl_display_disconnect(l.display);
        return 3;
    }
    if (!l.manager) {
        logline("FAIL", "this compositor does not offer ext-session-lock-v1, so no client can lock the session on it");
        wl_display_disconnect(l.display);
        return 3;
    }
    if (!l.compositor || !l.shm || !l.seat) {
        logline("FAIL", "the compositor has no wl_compositor, wl_shm or wl_seat for a lock screen");
        wl_display_disconnect(l.display);
        return 3;
    }
    l.xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

    /* The lock object, then a lock surface per output right away: the protocol asks clients to do that, because the
     * compositor may wait for those surfaces before it says the session is locked (and it must not show anything of
     * the old session in between). */
    l.lock = ext_session_lock_manager_v1_lock(l.manager);
    ext_session_lock_v1_add_listener(l.lock, &lock_listener, &l);
    for (int i = 0; i < l.n_shown; i++) lock_add_output(&l, l.shown[i]);
    if (wl_display_roundtrip(l.display) < 0) {
        logline("FAIL", "the compositor did not answer while locking");
        wl_lock_cleanup(&l);
        wl_display_disconnect(l.display);
        return 3;
    }
    if (l.finished) {
        logline("FAIL", "another program is already locking this session");
        wl_lock_cleanup(&l);
        wl_display_disconnect(l.display);
        return 4;
    }

    int fd = wl_display_get_fd(l.display);
    while (!l.unlock && !l.finished) {
        if (wl_display_prepare_read(l.display) != 0) {
            wl_display_dispatch_pending(l.display);
        } else {
            wl_display_flush(l.display);
            struct pollfd pfd = { fd, POLLIN, 0 };
            if (poll(&pfd, 1, 500) > 0 && (pfd.revents & POLLIN)) wl_display_read_events(l.display);
            else {
                wl_display_cancel_read(l.display);
                /* The compositor is gone: a lock screen over a dead session is of no use to anybody, and the shell of
                 * HDE_SH_LOCK is waiting for this program. (The compositor holding the lock is a different thing: that
                 * is the `finished` event below, and the session stays locked either way.) */
                if (pfd.revents & (POLLERR | POLLHUP)) break;
            }
            wl_display_dispatch_pending(l.display);
        }
        if (l.locked && ui_time(ui)) wl_paint_all(&l);
    }
    int rc = 0;
    if (l.unlock) {
        ext_session_lock_v1_unlock_and_destroy(l.lock);
        l.lock = NULL;
        for (int i = 0; i < l.n_outputs; i++)
            if (l.outputs[i].lock_surface) {
                ext_session_lock_surface_v1_destroy(l.outputs[i].lock_surface);
                l.outputs[i].lock_surface = NULL;
            }
        wl_display_roundtrip(l.display);   /* the protocol asks for this: be sure the unlock reached the compositor */
        logline("INFO", "the Wayland session is unlocked");
    } else if (l.finished) {
        logline("WARN", "the compositor took the lock back (another lock screen asked first, or it stopped locking)");
        rc = 4;
    }
    wl_lock_cleanup(&l);
    wl_display_disconnect(l.display);
    return rc;
}
#endif /* HDE_LOCK_HAVE_WAYLAND */

/* ========================================================================================================= --check */

static HdeLockBackend pick_backend(HdeLockBackend want)
{
    if (want != HDE_LOCK_SESSION) return want;
    if (getenv("WAYLAND_DISPLAY") && *getenv("WAYLAND_DISPLAY")) return HDE_LOCK_WAYLAND;
    return HDE_LOCK_X11;
}

#if defined(HDE_LOCK_HAVE_WAYLAND) && defined(HDE_LOCK_HAVE_PAM)
struct wl_probe { bool found; };

static void probe_global(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t version)
{
    (void)reg; (void)name; (void)version;
    struct wl_probe *p = data;
    if (!strcmp(iface, ext_session_lock_manager_v1_interface.name)) p->found = true;
}

static void probe_remove(void *data, struct wl_registry *reg, uint32_t name)
{
    (void)data; (void)reg; (void)name;
}

static const struct wl_registry_listener probe_listener = { .global = probe_global, .global_remove = probe_remove };

/* Does this compositor offer the session lock? Nothing is bound and nothing is locked: the interface names are
 * enough, so this cannot disturb a session that is already locked by somebody else. */
static bool wayland_can_lock(void)
{
    struct wl_display *d = wl_display_connect(NULL);
    if (!d) return false;
    struct wl_probe p = { false };
    struct wl_registry *reg = wl_display_get_registry(d);
    wl_registry_add_listener(reg, &probe_listener, &p);
    if (wl_display_roundtrip(d) < 0) p.found = false;
    wl_display_disconnect(d);
    return p.found;
}
#endif /* HDE_LOCK_HAVE_WAYLAND && HDE_LOCK_HAVE_PAM */

static int check_session(HdeLockBackend b)
{
#if !defined(HDE_LOCK_HAVE_PAM)
    (void)b;
    printf("hde-lock: cannot lock: %s\n", hde_lock_no_pam());
    return 3;
#else
    if (b == HDE_LOCK_X11) {
#if defined(HDE_LOCK_HAVE_XCB)
        int screen_num = 0;
        xcb_connection_t *c = xcb_connect(NULL, &screen_num);
        if (xcb_connection_has_error(c)) {
            printf("hde-lock: cannot lock: no X display (DISPLAY is %s)\n",
                   getenv("DISPLAY") ? getenv("DISPLAY") : "unset");
            xcb_disconnect(c);
            return 3;
        }
        xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(c));
        for (int i = 0; i < screen_num && it.rem; i++) xcb_screen_next(&it);
        int w = it.data ? it.data->width_in_pixels : 0, h = it.data ? it.data->height_in_pixels : 0;
        xcb_disconnect(c);
        printf("hde-lock: the X11 session can be locked (%dx%d, the password through PAM)\n", w, h);
        return 0;
#else
        printf("hde-lock: cannot lock: this build has no X11 session lock (build HDE with libxcb)\n");
        return 3;
#endif
    }
#if defined(HDE_LOCK_HAVE_WAYLAND)
    if (!getenv("WAYLAND_DISPLAY") || !*getenv("WAYLAND_DISPLAY")) {
        printf("hde-lock: cannot lock: no Wayland display (WAYLAND_DISPLAY is unset)\n");
        return 3;
    }
    if (!wayland_can_lock()) {
        printf("hde-lock: cannot lock: this compositor does not offer ext-session-lock-v1\n");
        return 3;
    }
    printf("hde-lock: the Wayland session can be locked (ext-session-lock-v1)\n");
    return 0;
#else
    printf("hde-lock: cannot lock: this build has no Wayland session lock (build HDE with wayland-client and xkbcommon)\n");
    return 3;
#endif
#endif /* HDE_LOCK_HAVE_PAM */
}

/* ============================================================================================================= main */

/* A lock screen only ever goes away when the password is right: not on Ctrl+C, not on a TERM from the session. */
static void ignore_signals(void)
{
    static const int signals[] = { SIGTERM, SIGINT, SIGHUP, SIGQUIT, SIGUSR1, 0 };
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_IGN;
    for (int i = 0; signals[i]; i++) sigaction(signals[i], &sa, NULL);
}

int main(int argc, char **argv)
{
    HdeLockOptions o;
    char err[256];
    if (!hde_lock_options(argc, argv, &o, err, sizeof err)) {
        fprintf(stderr, "%s\n", err);
        return 2;
    }
    if (o.help) {
        fputs(hde_lock_usage(), stdout);
        return 0;
    }
    if (o.version) {
        printf("hde-lock (HDE) %s\n", HDE_VERSION);
#if defined(HDE_LOCK_HAVE_XCB)
        printf("  X11 session (XCB): yes\n");
#else
        printf("  X11 session (XCB): no\n");
#endif
#if defined(HDE_LOCK_HAVE_WAYLAND)
        printf("  Wayland session (ext-session-lock-v1): yes\n");
#else
        printf("  Wayland session (ext-session-lock-v1): no\n");
#endif
#if defined(HDE_LOCK_HAVE_PAM)
        printf("  Password check (PAM): yes\n");
#else
        printf("  Password check (PAM): no\n");
#endif
        return 0;
    }

    HdeLockBackend b = pick_backend(o.backend);
    if (o.check) return check_session(b);

    struct lockui ui;
    memset(&ui, 0, sizeof ui);
    ui_names(&ui);
    ui_time(&ui);

    if (b == HDE_LOCK_X11) {
#if defined(HDE_LOCK_HAVE_XCB)
        ignore_signals();
        return x11_lock_run(&ui);
#else
        logline("FAIL", "this build of hde-lock has no X11 session lock (build HDE with libxcb)");
        return 3;
#endif
    }
#if defined(HDE_LOCK_HAVE_WAYLAND)
    ignore_signals();
    return wl_lock_run(&ui);
#else
    logline("FAIL", "this build of hde-lock has no Wayland session lock (build HDE with wayland-client and xkbcommon)");
    return 3;
#endif
}
