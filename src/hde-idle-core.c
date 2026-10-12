/* hde-idle-core.c — the rules of hde-idle (see src/hde-idle-core.h): which deadline has passed, what to do about it,
 * and who is holding the computer back. Plain C, no dependencies beyond the C library: tests/idle-test.c runs these
 * rules on a machine without a screen, and the daemon carries out what they answer.
 *
 * One stretch of idleness is: the user stops touching the computer, the deadlines pass one after the other
 * (screen off, then locked, then asleep), and then the user comes back and everything starts over. The daemon
 * remembers where in that stretch it is with the three flags in HdeIdleState, so an action is taken once and not
 * every time it asks.
 */
#include "hde-idle-core.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* Longest a value in settings.ini may be, in minutes (0 means "never"). A day: beyond that nobody is "away", the
 * computer is simply left on. */
#define HDE_IDLE_MINUTES_MAX 1440

/* The defaults HDE ships with. */
#define HDE_IDLE_BLANK_DEFAULT    10
#define HDE_IDLE_LOCK_DEFAULT     15
#define HDE_IDLE_SUSPEND_BAT      20
#define HDE_IDLE_SUSPEND_AC        0       /* a plugged-in computer does not go away by itself */

void hde_idle_config_defaults(HdeIdleConfig *c)
{
    if (!c) return;
    c->blank_minutes = HDE_IDLE_BLANK_DEFAULT;
    c->lock_minutes = HDE_IDLE_LOCK_DEFAULT;
    c->suspend_ac_minutes = HDE_IDLE_SUSPEND_AC;
    c->suspend_bat_minutes = HDE_IDLE_SUSPEND_BAT;
    c->lock_before_suspend = 1;
}

static int clamp_minutes(int v)
{
    if (v < 0) return 0;
    if (v > HDE_IDLE_MINUTES_MAX) return HDE_IDLE_MINUTES_MAX;
    return v;
}

void hde_idle_config_clamp(HdeIdleConfig *c)
{
    if (!c) return;
    c->blank_minutes = clamp_minutes(c->blank_minutes);
    c->lock_minutes = clamp_minutes(c->lock_minutes);
    c->suspend_ac_minutes = clamp_minutes(c->suspend_ac_minutes);
    c->suspend_bat_minutes = clamp_minutes(c->suspend_bat_minutes);
    c->lock_before_suspend = c->lock_before_suspend ? 1 : 0;
}

int hde_idle_config_equal(const HdeIdleConfig *a, const HdeIdleConfig *b)
{
    if (!a || !b) return 0;
    return a->blank_minutes == b->blank_minutes && a->lock_minutes == b->lock_minutes &&
           a->suspend_ac_minutes == b->suspend_ac_minutes && a->suspend_bat_minutes == b->suspend_bat_minutes &&
           a->lock_before_suspend == b->lock_before_suspend;
}

long hde_idle_suspend_ms(const HdeIdleConfig *c, int on_battery)
{
    if (!c) return 0;
    int minutes = on_battery ? c->suspend_bat_minutes : c->suspend_ac_minutes;
    return minutes > 0 ? (long)minutes * 60000L : 0;
}

void hde_idle_minutes_text(int minutes, char *out, size_t len)
{
    if (!out || !len) return;
    if (minutes <= 0) snprintf(out, len, "Never");
    else if (minutes == 1) snprintf(out, len, "1 minute");
    else snprintf(out, len, "%d minutes", minutes);
}

/* "Never" -> "never", for a value that comes after "after" in a sentence. */
static void lower_first(char *s)
{
    if (s && *s) s[0] = (char)tolower((unsigned char)s[0]);
}

void hde_idle_config_text(const HdeIdleConfig *c, char *out, size_t len)
{
    if (!out || !len) return;
    if (!c) { snprintf(out, len, "no idle settings"); return; }
    char blank[32], lock[32], ac[32], bat[32];
    hde_idle_minutes_text(c->blank_minutes, blank, sizeof blank);
    hde_idle_minutes_text(c->lock_minutes, lock, sizeof lock);
    hde_idle_minutes_text(c->suspend_ac_minutes, ac, sizeof ac);
    hde_idle_minutes_text(c->suspend_bat_minutes, bat, sizeof bat);
    /* "asleep after Never while plugged in" reads badly: the values that follow "after" lose their capital
     * here (the drop-downs in Settings keep it). */
    lower_first(blank);
    lower_first(ac);
    snprintf(out, len,
             "screen off after %s, locked after %s, asleep after %s on battery and %s while plugged in",
             blank, lock, bat, ac);
}

void hde_idle_state_init(HdeIdleState *s)
{
    if (!s) return;
    memset(s, 0, sizeof *s);
}

void hde_idle_reset(HdeIdleState *s)
{
    if (!s) return;
    s->blanked = 0;
    s->locked = 0;
    s->suspended = 0;
}

const char *hde_idle_action_name(HdeIdleAction a)
{
    switch (a) {
    case HDE_IDLE_BLANK:    return "blank";
    case HDE_IDLE_UNBLANK:  return "unblank";
    case HDE_IDLE_LOCK:     return "lock";
    case HDE_IDLE_SUSPEND:  return "suspend";
    case HDE_IDLE_NONE:
    default:                return "nothing";
    }
}

void hde_idle_mark(HdeIdleState *s, HdeIdleAction a)
{
    if (!s) return;
    switch (a) {
    case HDE_IDLE_BLANK:    s->blanked = 1; break;
    case HDE_IDLE_UNBLANK:  s->blanked = 0; break;
    case HDE_IDLE_LOCK:     s->locked = 1; break;
    case HDE_IDLE_SUSPEND:  s->suspended = 1; break;
    case HDE_IDLE_NONE:     break;
    }
}

HdeIdleAction hde_idle_decide(const HdeIdleConfig *c, const HdeIdleState *s)
{
    if (!c || !s) return HDE_IDLE_NONE;

    /* The user is back: the screen has to come on again at once, whatever the clock says. */
    if (s->idle_ms <= 0) return s->blanked ? HDE_IDLE_UNBLANK : HDE_IDLE_NONE;

    long sleep_ms = hde_idle_suspend_ms(c, s->on_battery);
    long lock_ms = c->lock_minutes > 0 ? (long)c->lock_minutes * 60000L : 0;
    long blank_ms = c->blank_minutes > 0 ? (long)c->blank_minutes * 60000L : 0;

    int may_sleep = sleep_ms > 0 && s->inhibit_power == 0 && !s->suspended && s->idle_ms >= sleep_ms;
    /* Sleeping is the one thing that cannot wait for the screen: the computer is about to lose its contents, so
     * the session is locked on the way there (when that is what the settings ask for) even if the lock deadline
     * is later — and even if a program is holding the screen on, because the other side of a sleep is a session
     * nobody is watching, and that session is locked. */
    if (may_sleep && c->lock_before_suspend && lock_ms > 0 && !s->locked) return HDE_IDLE_LOCK;
    if (may_sleep) return HDE_IDLE_SUSPEND;

    /* Somebody is watching something: the screen stays on and the session is not locked. The computer may still
     * go to sleep — that is what the other inhibitor is for, and a program that wants neither asks for both. */
    if (s->inhibit_screen > 0) return s->blanked ? HDE_IDLE_UNBLANK : HDE_IDLE_NONE;

    if (lock_ms > 0 && !s->locked && s->idle_ms >= lock_ms) return HDE_IDLE_LOCK;
    if (blank_ms > 0 && !s->blanked && s->idle_ms >= blank_ms) return HDE_IDLE_BLANK;
    return HDE_IDLE_NONE;
}

/* ---------------------------------------------------------------- who is holding the computer back */

void hde_idle_inhibitors_init(HdeIdleInhibitors *s)
{
    if (!s) return;
    memset(s, 0, sizeof *s);
    s->next_cookie = 1;
}

/* "VLC media player" or, when the program said nothing, the D-Bus name it came from. */
static void copy_field(char *dst, size_t len, const char *src, const char *fallback)
{
    const char *text = (src && *src) ? src : (fallback ? fallback : "");
    if (!len) return;
    snprintf(dst, len, "%s", text);
    /* One line: an inhibitor's reason is shown in the log and in Settings, never over several lines. */
    for (char *p = dst; *p; ++p)
        if (*p == '\n' || *p == '\r') *p = ' ';
}

uint32_t hde_idle_inhibit_add(HdeIdleInhibitors *s, const char *who, const char *why, int screen, int power)
{
    if (!s || (!screen && !power)) return 0;
    if (s->n >= HDE_IDLE_INHIBIT_MAX) return 0;
    if (s->next_cookie == 0) s->next_cookie = 1;
    HdeIdleInhibitor *it = &s->items[s->n];
    memset(it, 0, sizeof *it);
    it->cookie = s->next_cookie++;
    copy_field(it->who, sizeof it->who, who, "a program");
    copy_field(it->why, sizeof it->why, why, "");
    it->screen = screen ? 1 : 0;
    it->power = power ? 1 : 0;
    s->n++;
    return it->cookie;
}

int hde_idle_inhibit_remove(HdeIdleInhibitors *s, uint32_t cookie)
{
    if (!s || cookie == 0) return 0;
    for (int i = 0; i < s->n; ++i) {
        if (s->items[i].cookie != cookie) continue;
        for (int j = i; j + 1 < s->n; ++j) s->items[j] = s->items[j + 1];
        s->n--;
        memset(&s->items[s->n], 0, sizeof s->items[s->n]);
        return 1;
    }
    return 0;
}

int hde_idle_inhibit_drop_owner(HdeIdleInhibitors *s, const char *who)
{
    if (!s || !who || !*who) return 0;
    int dropped = 0;
    for (int i = 0; i < s->n; ) {
        if (strcmp(s->items[i].who, who) == 0) {
            if (hde_idle_inhibit_remove(s, s->items[i].cookie)) { dropped++; continue; }
        }
        ++i;
    }
    return dropped;
}

int hde_idle_inhibit_screen(const HdeIdleInhibitors *s)
{
    if (!s) return 0;
    int n = 0;
    for (int i = 0; i < s->n; ++i) if (s->items[i].screen) n++;
    return n;
}

int hde_idle_inhibit_power(const HdeIdleInhibitors *s)
{
    if (!s) return 0;
    int n = 0;
    for (int i = 0; i < s->n; ++i) if (s->items[i].power) n++;
    return n;
}

int hde_idle_inhibit_count(const HdeIdleInhibitors *s)
{
    return s ? s->n : 0;
}

uint32_t hde_idle_inhibit_cookie_at(const HdeIdleInhibitors *s, int index)
{
    if (!s || index < 0 || index >= s->n) return 0;
    return s->items[index].cookie;
}

void hde_idle_inhibit_why(const HdeIdleInhibitors *s, int power, char *out, size_t len)
{
    if (!out || !len) return;
    out[0] = '\0';
    if (!s) return;
    /* The newest inhibitor is the one the user is looking at now. */
    for (int i = s->n - 1; i >= 0; --i) {
        const HdeIdleInhibitor *it = &s->items[i];
        if (power ? !it->power : !it->screen) continue;
        if (it->why[0]) snprintf(out, len, "%s (%s)", it->who, it->why);
        else snprintf(out, len, "%s", it->who);
        return;
    }
}
