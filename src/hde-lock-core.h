/* hde-lock-core.h — the parts of hde-lock (src/hde-lock.c) that need neither a display nor a toolkit: the command
 * line, the password field, the clock and the date, and the lines the lock screen shows. Kept apart so that
 * tests/lock-core-test.c can check them without an X server or a Wayland compositor (and on a machine without PAM). */
#ifndef HDE_LOCK_CORE_H
#define HDE_LOCK_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

/* The longest password this lock screen takes (the rest is ignored, like it would be by PAM). */
#define HDE_LOCK_PASSWORD_MAX 255

/* Which session to lock. The lock screen is HDE's own, on both of HDE's sessions: the "HDE" (X11) session and the
 * "HDE (Wayland)" session, so it picks the one that is running unless told otherwise. */
typedef enum {
    HDE_LOCK_SESSION = 0,   /* by the session: WAYLAND_DISPLAY first, then DISPLAY */
    HDE_LOCK_X11,
    HDE_LOCK_WAYLAND,
} HdeLockBackend;

typedef struct {
    HdeLockBackend backend;
    bool check;             /* --check: say whether this session can be locked (takes no lock, needs no password) */
    bool help, version;
} HdeLockOptions;

/* The command line. Returns false and writes an English sentence into err on a bad option. */
bool hde_lock_options(int argc, char **argv, HdeLockOptions *o, char *err, size_t err_len);
const char *hde_lock_usage(void);            /* the --help text (starts with "Usage:") */

/* The password field: the characters typed so far, and one dot per character on the lock screen. */
typedef struct {
    char text[HDE_LOCK_PASSWORD_MAX + 1];   /* the characters, NUL-terminated */
    size_t len;
} HdeLockField;

void hde_lock_field_clear(HdeLockField *f);
void hde_lock_field_insert(HdeLockField *f, const char *utf8);     /* a character (UTF-8), or nothing when full */
void hde_lock_field_backspace(HdeLockField *f);                    /* takes back one character (all its bytes) */
size_t hde_lock_field_chars(const HdeLockField *f);                /* characters, not bytes: the dots drawn */

/* The clock and the date, in English whatever the locale says ("09:41", "Thursday 8 October"). */
void hde_lock_clock(time_t now, char *out, size_t len);
void hde_lock_date(time_t now, char *out, size_t len);

/* The name on the lock screen: the full name from the account (GECOS, up to the first comma), or the login name. */
void hde_lock_display_name(const char *user, const char *gecos, char *out, size_t len);
/* The letter in the circle: the name's first character, A-Z upper-cased. Writes an empty string for an empty name. */
void hde_lock_initial(const char *name, char *out, size_t len);

/* What the lock screen says. All English, no locale, no translations: HDE speaks English. */
const char *hde_lock_prompt(void);          /* under the field: what to do now */
const char *hde_lock_wrong(void);           /* under the field: the password was not accepted */
const char *hde_lock_keys(void);            /* the line at the bottom: the keys that work */
const char *hde_lock_no_pam(void);          /* why this build cannot lock a session (no PAM) */

#endif /* HDE_LOCK_CORE_H */
