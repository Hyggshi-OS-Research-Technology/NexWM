/* hde-idle-core.h — the brain of hde-idle (src/hde-idle.c), the daemon that watches how long nobody has touched the
 * computer and then blanks the screen, locks it and puts the computer to sleep. Plain C: no display, no toolkit, no
 * D-Bus, so tests/idle-test.c can check every rule of it on a machine without a screen.
 *
 * The daemon only supplies the two things this file cannot know: how long the session has been idle (X11: the
 * XScreenSaver extension; Wayland: ext-idle-notify-v1) and whether the computer runs on its battery. Everything that
 * follows from those — which deadline has passed, what to do about it, and who is holding the computer awake — is
 * decided here.
 *
 * Two different things can hold the computer back, and the difference matters:
 *   - a screen inhibitor (org.freedesktop.ScreenSaver.Inhibit) keeps the screen on: no blanking, no locking.
 *     A film being watched, a slideshow, a remote desktop session the user is looking at.
 *   - a power inhibitor (org.freedesktop.PowerManagement.Inhibit) keeps the computer awake: no sleeping.
 *     A download, a backup, a long build.
 * An inhibitor may ask for either or both, so a film that is still downloading keeps both, and a download on a
 * screen that is already off keeps only the second.
 */
#ifndef HDE_IDLE_CORE_H
#define HDE_IDLE_CORE_H

#include <stddef.h>
#include <stdint.h>

/* How many inhibitors can be held at once (one per program that asked). A full table refuses, it never grows. */
#define HDE_IDLE_INHIBIT_MAX 32
/* Longest name and reason kept for an inhibitor ("VLC media player", "Playing a film"). */
#define HDE_IDLE_NAME_MAX 96

/* ---------------------------------------------------------------- settings (settings.ini, group [settings]) */
typedef struct {
    int blank_minutes;          /* screen off after this many minutes of doing nothing; 0 = never */
    int lock_minutes;           /* lock the screen after ...; 0 = never */
    int suspend_ac_minutes;     /* sleep after ... while the adapter is plugged in; 0 = never */
    int suspend_bat_minutes;    /* sleep after ... while running on the battery; 0 = never */
    int lock_before_suspend;    /* lock first when going to sleep, so the session is locked on waking up */
} HdeIdleConfig;

/* The values HDE ships with: the screen goes off after 10 minutes, the screen locks after 15, and a computer on its
 * battery sleeps after 20 minutes. A plugged-in computer never sleeps by itself — nobody wants a desktop to vanish
 * under them while they are reading. */
void hde_idle_config_defaults(HdeIdleConfig *c);
/* Bring settings from a broken settings.ini back into sense: everything clamped to 0..1440 minutes (0 = never). */
void hde_idle_config_clamp(HdeIdleConfig *c);
int  hde_idle_config_equal(const HdeIdleConfig *a, const HdeIdleConfig *b);
/* Minutes until the computer sleeps: the battery's or the adapter's, 0 = never. */
long hde_idle_suspend_ms(const HdeIdleConfig *c, int on_battery);
/* One line for the log: "screen off after 10 minutes, lock after 15 minutes, sleep after 20 minutes on battery,
 * never while plugged in" */
void hde_idle_config_text(const HdeIdleConfig *c, char *out, size_t len);

/* ---------------------------------------------------------------- what is happening now */
typedef struct {
    int64_t idle_ms;            /* how long nobody has touched keyboard or mouse */
    int     on_battery;         /* 1 the adapter is unplugged, 0 plugged in or no battery at all */
    int     inhibit_screen;     /* > 0: a program is holding the screen on */
    int     inhibit_power;      /* > 0: a program is holding the computer awake */
    int     blanked;            /* the screen is off because of us */
    int     locked;             /* the screen has been locked for this stretch of idleness */
    int     suspended;          /* the computer has been put to sleep for this stretch of idleness */
} HdeIdleState;

void hde_idle_state_init(HdeIdleState *s);
/* The user is back (or the computer woke up): everything starts over. */
void hde_idle_reset(HdeIdleState *s);

typedef enum {
    HDE_IDLE_NONE = 0,
    HDE_IDLE_BLANK,             /* turn the screen off */
    HDE_IDLE_UNBLANK,           /* the user is back: turn it on again */
    HDE_IDLE_LOCK,              /* lock the screen (hde-lock, src/hde-lock.c) */
    HDE_IDLE_SUSPEND            /* put the computer to sleep */
} HdeIdleAction;

/* What to do now. Given the same state this always answers the same thing, and it never answers an action that has
 * already been taken for this stretch of idleness, so it is safe to ask as often as the daemon likes. */
HdeIdleAction hde_idle_decide(const HdeIdleConfig *c, const HdeIdleState *s);
/* Remember that an action was taken, so it is not taken again. */
void hde_idle_mark(HdeIdleState *s, HdeIdleAction a);
const char *hde_idle_action_name(HdeIdleAction a);
/* "Never", "1 minute", "10 minutes" — for the drop-downs in Settings > Power. */
void hde_idle_minutes_text(int minutes, char *out, size_t len);

/* ---------------------------------------------------------------- who is holding the computer back */
typedef struct {
    uint32_t cookie;            /* the number handed back to the program that asked (never 0) */
    char     who[HDE_IDLE_NAME_MAX];    /* the program: "VLC media player" */
    char     why[HDE_IDLE_NAME_MAX];    /* the reason: "Playing a film" */
    int      screen;            /* keeps the screen on */
    int      power;             /* keeps the computer awake */
} HdeIdleInhibitor;

typedef struct {
    HdeIdleInhibitor items[HDE_IDLE_INHIBIT_MAX];
    int      n;
    uint32_t next_cookie;
} HdeIdleInhibitors;

void     hde_idle_inhibitors_init(HdeIdleInhibitors *s);
/* Returns the cookie (> 0), or 0 when the table is full or the request asks for nothing. */
uint32_t hde_idle_inhibit_add(HdeIdleInhibitors *s, const char *who, const char *why, int screen, int power);
/* Take back an inhibitor by its cookie. Returns 1 when one was there. */
int      hde_idle_inhibit_remove(HdeIdleInhibitors *s, uint32_t cookie);
/* A program that asked and then crashed never calls back: drop everything it held. Returns how many that was. */
int      hde_idle_inhibit_drop_owner(HdeIdleInhibitors *s, const char *who);
int      hde_idle_inhibit_screen(const HdeIdleInhibitors *s);   /* how many keep the screen on */
int      hde_idle_inhibit_power(const HdeIdleInhibitors *s);    /* how many keep the computer awake */
int      hde_idle_inhibit_count(const HdeIdleInhibitors *s);    /* how many there are in all */
/* The cookie of the inhibitor at `index` (0 .. count-1), for a caller that has to look each one up — hde-idle
 * does, to drop the inhibitors of a program that has left the bus. 0 when there is nothing at that index. */
uint32_t hde_idle_inhibit_cookie_at(const HdeIdleInhibitors *s, int index);
/* Who, for the log: "VLC media player (Playing a film)". Empty when nobody is holding anything. */
void     hde_idle_inhibit_why(const HdeIdleInhibitors *s, int power, char *out, size_t len);

#endif /* HDE_IDLE_CORE_H */
