/* tests/idle-test.c — the rules of hde-idle (src/hde-idle-core.c) without a screen, a session or a clock: which
 * deadline has passed and what is done about it, and who is holding the computer back. Plain C, so `make check-unit`
 * runs it everywhere (and on a machine that has neither X11 nor Wayland).
 *   cc -O2 -Wall -Wextra -Wpedantic -std=c11 -Isrc -o build/idle-test tests/idle-test.c src/hde-idle-core.c
 * Prints PASS/FAIL lines; exit status = number of failures.
 */
#include "hde-idle-core.h"

#include <stdio.h>
#include <string.h>

static int fails, passes;

#define CHECK(cond, ...) do { \
        if (cond) { passes++; printf("PASS: idle: "); } else { fails++; printf("FAIL: idle: "); } \
        printf(__VA_ARGS__); printf("\n"); \
    } while (0)

#define MIN(ms) ((int64_t)(ms) * 60000)

/* A state that is idle for `minutes`, on the battery unless told otherwise. */
static HdeIdleState idle_for(int minutes, int on_battery)
{
    HdeIdleState s;
    hde_idle_state_init(&s);
    s.idle_ms = MIN(minutes);
    s.on_battery = on_battery;
    return s;
}

static void test_config(void)
{
    HdeIdleConfig c;
    hde_idle_config_defaults(&c);
    CHECK(c.blank_minutes == 10 && c.lock_minutes == 15 && c.suspend_bat_minutes == 20 &&
          c.suspend_ac_minutes == 0 && c.lock_before_suspend == 1,
          "the defaults are: screen off after 10 minutes, locked after 15, asleep after 20 on battery, "
          "never while plugged in");

    CHECK(hde_idle_suspend_ms(&c, 1) == 20 * 60000L, "on the battery the computer sleeps after 20 minutes");
    CHECK(hde_idle_suspend_ms(&c, 0) == 0, "plugged in it never sleeps by itself");

    HdeIdleConfig broke = c;
    broke.blank_minutes = -5;
    broke.lock_minutes = 60000;
    broke.suspend_ac_minutes = 30;
    broke.lock_before_suspend = 42;
    hde_idle_config_clamp(&broke);
    CHECK(broke.blank_minutes == 0, "a negative timeout from a broken settings.ini means \"never\"");
    CHECK(broke.lock_minutes == 1440, "a nonsense timeout is clamped to a day");
    CHECK(broke.lock_before_suspend == 1, "a switch in settings.ini is 1 or 0, never anything else");
    CHECK(broke.suspend_ac_minutes == 30, "a sensible value is left alone");

    HdeIdleConfig same = c;
    CHECK(hde_idle_config_equal(&c, &same), "two configs with the same values are equal");
    same.suspend_bat_minutes = 25;
    CHECK(!hde_idle_config_equal(&c, &same), "changing one timeout makes them different");

    char text[256];
    hde_idle_config_text(&c, text, sizeof text);
    CHECK(strstr(text, "screen off after 10 minutes") && strstr(text, "asleep after 20 minutes on battery") &&
          strstr(text, "never while plugged in"),
          "one line for the log: \"%s\"", text);

    char t[32];
    hde_idle_minutes_text(0, t, sizeof t);   CHECK(!strcmp(t, "Never"), "0 minutes reads \"Never\"");
    hde_idle_minutes_text(1, t, sizeof t);   CHECK(!strcmp(t, "1 minute"), "1 minute is singular");
    hde_idle_minutes_text(15, t, sizeof t);  CHECK(!strcmp(t, "15 minutes"), "15 minutes is plural");
}

static void test_ladder(void)
{
    HdeIdleConfig c;
    hde_idle_config_defaults(&c);

    CHECK(hde_idle_decide(&c, & (HdeIdleState) { 0 }) == HDE_IDLE_NONE, "nothing to do while the user is working");

    HdeIdleState s = idle_for(9, 1);
    CHECK(hde_idle_decide(&c, &s) == HDE_IDLE_NONE, "9 minutes: too early for anything");

    s = idle_for(10, 1);
    CHECK(hde_idle_decide(&c, &s) == HDE_IDLE_BLANK, "10 minutes: the screen goes off");

    s = idle_for(14, 1);
    hde_idle_mark(&s, HDE_IDLE_BLANK);
    CHECK(s.blanked == 1 && hde_idle_decide(&c, &s) == HDE_IDLE_NONE,
          "the screen is off already: it is not turned off again every second");

    s = idle_for(15, 1);
    hde_idle_mark(&s, HDE_IDLE_BLANK);
    CHECK(hde_idle_decide(&c, &s) == HDE_IDLE_LOCK, "15 minutes: the screen is locked");

    s = idle_for(19, 1);
    hde_idle_mark(&s, HDE_IDLE_BLANK);
    hde_idle_mark(&s, HDE_IDLE_LOCK);
    CHECK(s.locked == 1 && hde_idle_decide(&c, &s) == HDE_IDLE_NONE, "locked once, not again");

    s = idle_for(20, 1);
    hde_idle_mark(&s, HDE_IDLE_BLANK);
    hde_idle_mark(&s, HDE_IDLE_LOCK);
    CHECK(hde_idle_decide(&c, &s) == HDE_IDLE_SUSPEND, "20 minutes on battery: the computer sleeps (locked already)");

    /* Plugged in: the ladder stops at the lock, there is nowhere further to go. */
    s = idle_for(120, 0);
    hde_idle_mark(&s, HDE_IDLE_BLANK);
    hde_idle_mark(&s, HDE_IDLE_LOCK);
    CHECK(hde_idle_decide(&c, &s) == HDE_IDLE_NONE, "two hours plugged in: nothing more happens");

    /* Coming back. */
    s = idle_for(30, 1);
    hde_idle_mark(&s, HDE_IDLE_BLANK);
    hde_idle_mark(&s, HDE_IDLE_LOCK);
    s.idle_ms = 0;
    CHECK(hde_idle_decide(&c, &s) == HDE_IDLE_UNBLANK, "the user is back: the screen comes on");
    hde_idle_mark(&s, HDE_IDLE_UNBLANK);
    CHECK(s.blanked == 0, "and it knows the screen is on again");
    CHECK(hde_idle_decide(&c, &s) == HDE_IDLE_NONE, "and nothing else happens while the user works");
}

static void test_suspend_comes_first(void)
{
    /* The settings say: sleep after 5 minutes but lock only after 30. Going to sleep on an unlocked screen would
     * leave the session wide open on waking up, so it is locked on the way. */
    HdeIdleConfig c;
    hde_idle_config_defaults(&c);
    c.blank_minutes = 0;
    c.lock_minutes = 30;
    c.suspend_bat_minutes = 5;

    HdeIdleState s = idle_for(5, 1);
    CHECK(hde_idle_decide(&c, &s) == HDE_IDLE_LOCK, "sleeping in 5 minutes but the screen locks at 30: lock first");
    hde_idle_mark(&s, HDE_IDLE_LOCK);
    CHECK(hde_idle_decide(&c, &s) == HDE_IDLE_SUSPEND, "and then, without waiting, sleep");

    /* Told not to bother: straight to sleep. */
    c.lock_before_suspend = 0;
    s = idle_for(5, 1);
    CHECK(hde_idle_decide(&c, &s) == HDE_IDLE_SUSPEND,
          "\"lock before sleeping\" off: the computer sleeps without locking");

    /* A screen inhibitor cannot stop the session being locked on the way to sleep: the alternative is an unlocked
     * session waiting on the other side of a sleep. */
    c.lock_before_suspend = 1;
    s = idle_for(5, 1);
    s.inhibit_screen = 1;
    CHECK(hde_idle_decide(&c, &s) == HDE_IDLE_LOCK,
          "a film does not stop the computer sleeping, and it sleeps locked");
    hde_idle_mark(&s, HDE_IDLE_LOCK);
    CHECK(hde_idle_decide(&c, &s) == HDE_IDLE_SUSPEND, "the lock comes first, the sleep right after it");
}

static void test_inhibitors(void)
{
    /* A config that never sleeps, so that what a screen inhibitor does can be seen on its own. */
    HdeIdleConfig c;
    hde_idle_config_defaults(&c);
    c.suspend_bat_minutes = 0;
    c.suspend_ac_minutes = 0;

    /* A film: the screen stays on and the session is not locked. */
    HdeIdleState s = idle_for(120, 1);
    s.inhibit_screen = 1;
    CHECK(hde_idle_decide(&c, &s) == HDE_IDLE_NONE, "while a film is playing the screen stays on");
    CHECK(hde_idle_decide(&c, &s) != HDE_IDLE_LOCK, "and the session is not locked under the film");

    /* The screen is already off when the film starts: it comes back on. */
    s = idle_for(120, 1);
    hde_idle_mark(&s, HDE_IDLE_BLANK);
    s.inhibit_screen = 1;
    CHECK(hde_idle_decide(&c, &s) == HDE_IDLE_UNBLANK, "a film starting behind a black screen turns it on again");

    /* A film on a computer that *is* about to sleep (the defaults: asleep after 20 minutes on the battery).
     * Watching something does not keep a laptop awake all night - but it does not get it an unlocked session on
     * the other side of the sleep either, so the screen is locked on the way there. */
    HdeIdleConfig sleeping;
    hde_idle_config_defaults(&sleeping);
    s = idle_for(20, 1);
    s.inhibit_screen = 1;
    CHECK(hde_idle_decide(&sleeping, &s) == HDE_IDLE_LOCK, "a film does not stop the computer sleeping: it locks first");
    hde_idle_mark(&s, HDE_IDLE_LOCK);
    CHECK(hde_idle_decide(&sleeping, &s) == HDE_IDLE_SUSPEND, "and then it sleeps");

    /* A download: the computer stays awake, the screen may still go off. */
    s = idle_for(11, 1);
    s.inhibit_power = 1;
    CHECK(hde_idle_decide(&c, &s) == HDE_IDLE_BLANK, "a download does not stop the screen going off");
    hde_idle_mark(&s, HDE_IDLE_BLANK);
    s.idle_ms = MIN(25);
    CHECK(hde_idle_decide(&c, &s) == HDE_IDLE_LOCK, "nor the screen locking");
    hde_idle_mark(&s, HDE_IDLE_LOCK);
    CHECK(hde_idle_decide(&c, &s) == HDE_IDLE_NONE, "but it does stop the computer sleeping");

    /* Both: nothing happens at all. */
    s = idle_for(120, 1);
    s.inhibit_screen = 1;
    s.inhibit_power = 1;
    CHECK(hde_idle_decide(&c, &s) == HDE_IDLE_NONE, "a program asking for both stops everything");
}

static void test_inhibitor_table(void)
{
    HdeIdleInhibitors s;
    hde_idle_inhibitors_init(&s);
    CHECK(hde_idle_inhibit_screen(&s) == 0 && hde_idle_inhibit_power(&s) == 0, "to begin with nothing is held back");

    CHECK(hde_idle_inhibit_add(&s, "VLC media player", "Playing a film", 1, 1) == 1, "the first inhibitor gets 1");
    CHECK(hde_idle_inhibit_add(&s, "Transmission", "Downloading", 0, 1) == 2, "the second gets 2");
    CHECK(hde_idle_inhibit_screen(&s) == 1, "one program keeps the screen on");
    CHECK(hde_idle_inhibit_power(&s) == 2, "two keep the computer awake");

    char why[256];
    hde_idle_inhibit_why(&s, 0, why, sizeof why);
    CHECK(!strcmp(why, "VLC media player (Playing a film)"), "the screen is held by \"%s\"", why);
    hde_idle_inhibit_why(&s, 1, why, sizeof why);
    CHECK(!strcmp(why, "Transmission (Downloading)"), "the newest one is named for the computer staying awake");

    CHECK(hde_idle_inhibit_remove(&s, 1) == 1, "taking back cookie 1 works");
    CHECK(hde_idle_inhibit_screen(&s) == 0, "the screen is free again");
    CHECK(hde_idle_inhibit_power(&s) == 1, "the download still keeps the computer awake");
    CHECK(hde_idle_inhibit_remove(&s, 1) == 0, "taking back the same cookie twice does nothing");

    /* A program that crashed never calls back: everything it held goes when it disappears from the bus. */
    hde_idle_inhibit_add(&s, "mpv", "", 1, 1);
    hde_idle_inhibit_add(&s, "mpv", "Full screen", 1, 0);
    CHECK(hde_idle_inhibit_drop_owner(&s, "mpv") == 2, "a program that goes away takes both of its inhibitors");
    CHECK(s.n == 1, "the download of the other program is untouched");
    hde_idle_inhibit_why(&s, 1, why, sizeof why);
    CHECK(!strcmp(why, "Transmission (Downloading)"), "and it is the one named now");

    /* A reason that says nothing, and a program that says nothing at all. */
    hde_idle_inhibit_why(&s, 0, why, sizeof why);
    CHECK(why[0] == '\0', "no screen inhibitor: nothing to name");
    uint32_t cookie = hde_idle_inhibit_add(&s, NULL, NULL, 1, 0);
    CHECK(cookie != 0, "a program that did not say who it is still gets a cookie");
    hde_idle_inhibit_why(&s, 0, why, sizeof why);
    CHECK(!strcmp(why, "a program"), "and is called \"%s\"", why);

    /* A reason with a line break in it would wreck the log. */
    hde_idle_inhibit_add(&s, "Bad", "one\ntwo", 1, 0);
    hde_idle_inhibit_why(&s, 0, why, sizeof why);
    CHECK(!strchr(why, '\n'), "a reason with a line break in it is put on one line: \"%s\"", why);

    /* Looking an inhibitor up by its cookie, the way hde-idle does when a program leaves the bus. */
    HdeIdleInhibitors lookup;
    hde_idle_inhibitors_init(&lookup);
    CHECK(hde_idle_inhibit_count(&lookup) == 0, "an empty table counts 0");
    CHECK(hde_idle_inhibit_cookie_at(&lookup, 0) == 0, "and has no cookie anywhere");
    uint32_t a = hde_idle_inhibit_add(&lookup, "mpv", "", 1, 0);
    uint32_t b = hde_idle_inhibit_add(&lookup, "Transmission", "Downloading", 0, 1);
    CHECK(hde_idle_inhibit_count(&lookup) == 2, "two inhibitors are counted");
    CHECK(hde_idle_inhibit_cookie_at(&lookup, 0) == a && hde_idle_inhibit_cookie_at(&lookup, 1) == b,
          "each one can be found at its place");
    CHECK(hde_idle_inhibit_cookie_at(&lookup, 2) == 0 && hde_idle_inhibit_cookie_at(&lookup, -1) == 0,
          "and nowhere else");
    hde_idle_inhibit_remove(&lookup, a);
    CHECK(hde_idle_inhibit_count(&lookup) == 1, "taking one back leaves one");
    CHECK(hde_idle_inhibit_cookie_at(&lookup, 0) == b, "and the one that is left is at the front");

    /* Full table: refuse, do not grow. */
    HdeIdleInhibitors full;
    hde_idle_inhibitors_init(&full);
    uint32_t last = 0;
    int i;
    for (i = 0; i < HDE_IDLE_INHIBIT_MAX + 8; ++i) {
        uint32_t got = hde_idle_inhibit_add(&full, "program", "", 1, 0);
        if (!got) break;
        last = got;
    }
    CHECK(i == HDE_IDLE_INHIBIT_MAX, "the table takes %d inhibitors and no more", HDE_IDLE_INHIBIT_MAX);
    CHECK(last == (uint32_t)HDE_IDLE_INHIBIT_MAX, "cookies are handed out one after the other, never twice");
    CHECK(hde_idle_inhibit_add(&full, "one more", "", 1, 0) == 0, "a full table refuses instead of growing");
    CHECK(hde_idle_inhibit_add(&full, "nothing", "", 0, 0) == 0, "asking for neither is not an inhibitor at all");
}

int main(void)
{
    test_config();
    test_ladder();
    test_suspend_comes_first();
    test_inhibitors();
    test_inhibitor_table();
    printf("\nidle: %d passed, %d failed\n", passes, fails);
    return fails;
}
