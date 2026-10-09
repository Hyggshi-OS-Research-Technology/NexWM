/* tests/lock-core-test.c — hde-lock's arithmetic and English without a display: the command line, the password field
 * (characters, not bytes), the clock and the date of the lock screen, the name and its initial, and the lines it
 * shows. Built and run by `make check-unit` (build/lock-core-test): prints PASS/FAIL lines, exit status = failures.
 *   cc -O2 -Wall -Wextra -Wpedantic -std=c11 -Isrc -o build/lock-core-test tests/lock-core-test.c src/hde-lock-core.c
 */
#define _POSIX_C_SOURCE 200809L
#include "hde-lock-core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fails, passes;

#define CHECK(cond, ...) do { \
        if (cond) { passes++; printf("PASS: lock: "); } else { fails++; printf("FAIL: lock: "); } \
        printf(__VA_ARGS__); printf("\n"); \
    } while (0)

static bool parse(const char *line, HdeLockOptions *o, char *err, size_t len)
{
    char buf[256];
    snprintf(buf, sizeof buf, "%s", line);
    char *argv[16];
    int argc = 0;
    for (char *p = strtok(buf, " "); p && argc < 16; p = strtok(NULL, " "))
        argv[argc++] = p;
    return hde_lock_options(argc, argv, o, err, len);
}

int main(void)
{
    /* The clock and the date are English whatever the locale says — the test asks for UTC so the times are fixed */
    setenv("TZ", "UTC0", 1);
    tzset();

    /* ---------------- the command line ---------------- */
    HdeLockOptions o;
    char err[256];
    CHECK(parse("hde-lock", &o, err, sizeof err) && o.backend == HDE_LOCK_SESSION && !o.check && !o.help && !o.version,
          "no options: lock the session that is running, on its own terms");
    CHECK(parse("hde-lock --x11", &o, err, sizeof err) && o.backend == HDE_LOCK_X11, "--x11 asks for the X11 session");
    CHECK(parse("hde-lock --wayland", &o, err, sizeof err) && o.backend == HDE_LOCK_WAYLAND,
          "--wayland asks for the Wayland session");
    CHECK(parse("hde-lock --check", &o, err, sizeof err) && o.check, "--check is remembered");
    CHECK(parse("hde-lock --check --wayland --version --help", &o, err, sizeof err) &&
          o.check && o.backend == HDE_LOCK_WAYLAND && o.version && o.help, "several options at once");
    CHECK(!parse("hde-lock --lock", &o, err, sizeof err) && strstr(err, "--lock") && strstr(err, "--help"),
          "an unknown option says so and points at --help (%s)", err);
    CHECK(parse("hde-lock -h", &o, err, sizeof err) && o.help, "-h is --help");
    CHECK(strncmp(hde_lock_usage(), "Usage:", 6) == 0, "the usage text starts with Usage: (a help screen, not a wall)");
    CHECK(strstr(hde_lock_usage(), "--check") && strstr(hde_lock_usage(), "ext-session-lock-v1"),
          "the usage text tells what the options and both sessions do");

    /* ---------------- the password field ---------------- */
    HdeLockField f;
    hde_lock_field_clear(&f);
    CHECK(f.len == 0 && hde_lock_field_chars(&f) == 0, "an empty field: no dots");
    hde_lock_field_insert(&f, "h");
    hde_lock_field_insert(&f, "u");
    hde_lock_field_insert(&f, "n");
    CHECK(!strcmp(f.text, "hun") && f.len == 3 && hde_lock_field_chars(&f) == 3, "three letters typed: hun, 3 dots");
    hde_lock_field_backspace(&f);
    CHECK(!strcmp(f.text, "hu") && f.len == 2 && hde_lock_field_chars(&f) == 2, "backspace takes back one character");
    hde_lock_field_insert(&f, "\xc3\xa9");         /* é: one character, two bytes */
    CHECK(f.len == 4 && hde_lock_field_chars(&f) == 3, "a two-byte character counts as one (the dots match the keys)");
    hde_lock_field_backspace(&f);
    CHECK(!strcmp(f.text, "hu") && hde_lock_field_chars(&f) == 2, "backspace takes back the whole character");
    hde_lock_field_backspace(&f);
    hde_lock_field_backspace(&f);
    hde_lock_field_backspace(&f);
    CHECK(f.len == 0 && f.text[0] == '\0' && hde_lock_field_chars(&f) == 0, "backspace on an empty field does nothing");
    hde_lock_field_insert(&f, "a");
    hde_lock_field_insert(&f, "");
    hde_lock_field_insert(&f, NULL);
    CHECK(!strcmp(f.text, "a"), "inserting nothing changes nothing");
    hde_lock_field_clear(&f);
    CHECK(f.len == 0 && hde_lock_field_chars(&f) == 0, "clearing the field (Esc) leaves no dots");
    for (int i = 0; i < HDE_LOCK_PASSWORD_MAX + 40; i++) hde_lock_field_insert(&f, "x");
    CHECK(f.len == HDE_LOCK_PASSWORD_MAX && f.len < sizeof f.text,
          "a very long password stops at %d characters instead of running over the buffer", HDE_LOCK_PASSWORD_MAX);

    /* ---------------- the clock, the date, the name ---------------- */
    char buf[128];
    time_t t = 1759906800;                          /* 2025-10-08 07:00:00 UTC, a Wednesday */
    hde_lock_clock(t, buf, sizeof buf);
    CHECK(!strcmp(buf, "07:00"), "the clock is HH:MM in 24 hours (%s)", buf);
    hde_lock_clock(t + 41 * 60, buf, sizeof buf);
    CHECK(!strcmp(buf, "07:41"), "and it follows the minutes (%s)", buf);
    hde_lock_date(t, buf, sizeof buf);
    CHECK(!strcmp(buf, "Wednesday 8 October"), "the date is English, whatever the locale says (%s)", buf);
    hde_lock_date(t + 4 * 24 * 3600, buf, sizeof buf);
    CHECK(!strcmp(buf, "Sunday 12 October"), "and it follows the day (%s)", buf);

    hde_lock_display_name("bao", "Bao Nguyen,0708,,", buf, sizeof buf);
    CHECK(!strcmp(buf, "Bao Nguyen"), "the full name comes from the account, up to the first comma (%s)", buf);
    hde_lock_display_name("bao", ",,,,,", buf, sizeof buf);
    CHECK(!strcmp(buf, "bao"), "an account without a full name shows the login name (%s)", buf);
    hde_lock_display_name("bao", NULL, buf, sizeof buf);
    CHECK(!strcmp(buf, "bao"), "and no GECOS at all is the same (%s)", buf);
    hde_lock_display_name("bao", "  ,,", buf, sizeof buf);
    CHECK(!strcmp(buf, "bao"), "a full name of nothing but spaces is no name (%s)", buf);
    hde_lock_display_name("bao", "  Bao  ,,", buf, sizeof buf);
    CHECK(!strcmp(buf, "Bao"), "and the name is not padded (%s)", buf);
    hde_lock_display_name("bao", "Bao Nguyen", buf, 6);
    CHECK(strlen(buf) == 5 && !strcmp(buf, "Bao N"), "a name longer than the room it has is cut, not overflowing (%s)",
          buf);

    hde_lock_initial("bao", buf, sizeof buf);
    CHECK(!strcmp(buf, "B"), "the initial is the first letter, upper-cased (%s)", buf);
    hde_lock_initial("Ada", buf, sizeof buf);
    CHECK(!strcmp(buf, "A"), "an upper-case name keeps its letter (%s)", buf);
    hde_lock_initial("\xc3\xa9ric", buf, sizeof buf);
    CHECK(!strcmp(buf, "\xc3\xa9"), "a non-ASCII name keeps its whole first character (%s)", buf);
    hde_lock_initial("", buf, sizeof buf);
    CHECK(buf[0] == '\0', "no name, no letter");
    hde_lock_initial(NULL, buf, sizeof buf);
    CHECK(buf[0] == '\0', "no name at all, no letter");

    /* ---------------- the lines ---------------- */
    CHECK(strlen(hde_lock_prompt()) > 0 && strstr(hde_lock_prompt(), "password"),
          "the line under the field says what to do (%s)", hde_lock_prompt());
    CHECK(strstr(hde_lock_wrong(), "Wrong password"), "and says when the password was wrong (%s)", hde_lock_wrong());
    CHECK(strstr(hde_lock_keys(), "Enter") && strstr(hde_lock_keys(), "Esc"),
          "the bottom line lists the keys that work (%s)", hde_lock_keys());
    CHECK(strstr(hde_lock_no_pam(), "PAM") && strstr(hde_lock_no_pam(), "libpam0g-dev"),
          "and the build without PAM explains exactly what to install (English, no translation)");

    printf("== lock-core-test: %d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
