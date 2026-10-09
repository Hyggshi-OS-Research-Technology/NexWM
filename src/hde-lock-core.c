/* hde-lock-core.c — see hde-lock-core.h. No X11, no Wayland, no PAM, no toolkit: everything here is arithmetic and
 * English, so it can be tested anywhere (tests/lock-core-test.c) and reused by both of hde-lock's sessions. */
#define _POSIX_C_SOURCE 200809L
#include "hde-lock-core.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ the command line */

const char *hde_lock_usage(void)
{
    return "Usage: hde-lock [--x11|--wayland] [--check] [--version] [--help]\n"
           "\n"
           "HDE's lock screen: it covers every screen, keeps the keyboard and the mouse to itself, and asks for the\n"
           "password of the user who is logged in. On the \"HDE\" (X11) session it is a window of its own over the\n"
           "whole screen; on the \"HDE (Wayland)\" session it uses the compositor's session lock\n"
           "(ext-session-lock-v1, which labwc and HDE's own NexWM compositor offer).\n"
           "\n"
           "  --x11       lock the X11 session (the window manager cannot draw over it)\n"
           "  --wayland   lock the Wayland session (the compositor blanks everything else)\n"
           "  --check     take no lock: exit 0 when this session can be locked, 3 when it cannot, and say why\n"
           "  --version   print what this build of hde-lock can do\n"
           "  --help      this text\n"
           "\n"
           "Keys: Enter unlocks, Esc clears the field, Backspace takes back a character. Ctrl+C and a TERM signal\n"
           "do not unlock (nor do they end hde-lock) while the screen is locked: only the password does.\n";
}

bool hde_lock_options(int argc, char **argv, HdeLockOptions *o, char *err, size_t err_len)
{
    o->backend = HDE_LOCK_SESSION;
    o->check = false;
    o->help = false;
    o->version = false;
    err[0] = '\0';
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--x11")) o->backend = HDE_LOCK_X11;
        else if (!strcmp(a, "--wayland")) o->backend = HDE_LOCK_WAYLAND;
        else if (!strcmp(a, "--check")) o->check = true;
        else if (!strcmp(a, "--version")) o->version = true;
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) o->help = true;
        else {
            snprintf(err, err_len, "hde-lock: unknown option '%s' (see hde-lock --help)", a);
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ the password field */

void hde_lock_field_clear(HdeLockField *f)
{
    f->len = 0;
    f->text[0] = '\0';
}

void hde_lock_field_insert(HdeLockField *f, const char *utf8)
{
    if (!utf8 || !*utf8) return;
    size_t n = 1;
    while (utf8[n] && (utf8[n] & 0xc0) == 0x80) n++;      /* the rest of a UTF-8 character */
    if (f->len + n + 1 > sizeof f->text) return;          /* full: the rest of the password would be lost anyway */
    memcpy(f->text + f->len, utf8, n);
    f->len += n;
    f->text[f->len] = '\0';
}

void hde_lock_field_backspace(HdeLockField *f)
{
    if (!f->len) return;
    size_t n = f->len - 1;
    while (n && (f->text[n] & 0xc0) == 0x80) n--;         /* back to the start of the character */
    f->len = n;
    f->text[n] = '\0';
}

size_t hde_lock_field_chars(const HdeLockField *f)
{
    size_t chars = 0;
    for (size_t i = 0; i < f->len; i++)
        if ((f->text[i] & 0xc0) != 0x80) chars++;         /* one per character, not per byte */
    return chars;
}

/* ------------------------------------------------------------------ the clock, the date, the name */

static const char *const weekdays[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *const months[] = { "January", "February", "March", "April", "May", "June", "July", "August",
                                      "September", "October", "November", "December" };

void hde_lock_clock(time_t now, char *out, size_t len)
{
    struct tm tm;
    if (!localtime_r(&now, &tm)) { snprintf(out, len, "--:--"); return; }
    snprintf(out, len, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

void hde_lock_date(time_t now, char *out, size_t len)
{
    struct tm tm;
    if (!localtime_r(&now, &tm)) { out[0] = '\0'; return; }
    unsigned day = tm.tm_wday < 7 ? (unsigned)tm.tm_wday : 0;
    unsigned mon = tm.tm_mon >= 0 && tm.tm_mon < 12 ? (unsigned)tm.tm_mon : 0;
    snprintf(out, len, "%s %d %s", weekdays[day], tm.tm_mday, months[mon]);
}

void hde_lock_display_name(const char *user, const char *gecos, char *out, size_t len)
{
    if (!user) user = "";
    if (!gecos) gecos = "";
    size_t n = 0;
    while (gecos[n] && gecos[n] != ',' && n + 1 < len) n++;            /* the full name is up to the first comma */
    while (n && (gecos[n - 1] == ' ' || gecos[n - 1] == '\t')) n--;    /* and it is not padded */
    size_t start = 0;
    while (start < n && (gecos[start] == ' ' || gecos[start] == '\t')) start++;
    if (n > start) {
        size_t k = n - start;
        if (k > len - 1) k = len - 1;
        memcpy(out, gecos + start, k);
        out[k] = '\0';
        return;
    }
    snprintf(out, len, "%s", user);                                    /* no full name: the login name will do */
}

void hde_lock_initial(const char *name, char *out, size_t len)
{
    if (!name || !*name || len < 2) { if (len) out[0] = '\0'; return; }
    unsigned char c = (unsigned char)name[0];
    if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 'a' + 'A');
    out[0] = (char)c;
    size_t n = 1;
    while (n < 3 && name[n] && ((unsigned char)name[n] & 0xc0) == 0x80 && n + 1 < len) {   /* a whole character */
        out[n] = name[n];
        n++;
    }
    out[n] = '\0';
}

/* ------------------------------------------------------------------ what the lock screen says */

const char *hde_lock_prompt(void) { return "Enter your password to unlock"; }
const char *hde_lock_wrong(void) { return "Wrong password, try again"; }
const char *hde_lock_keys(void) { return "Enter unlocks  |  Esc clears  |  Backspace deletes"; }
const char *hde_lock_no_pam(void)
{
    return "hde-lock was built without PAM, so it cannot check a password: install libpam0g-dev (Debian, Ubuntu) "
           "or pam-devel (Fedora) and build HDE again";
}
