/* tests/keyring-test.c — reading what `gnome-keyring-daemon --start` prints (src/hde-keyring-core.c), with no
 * keyring on the machine and no session to put it in: the variables the session is given, the ones it is not, and
 * the shapes the output comes in (quoted, `export`, CRLF, a comment, a line that is not an assignment at all).
 *   cc -O2 -Wall -Wextra -Wpedantic -std=c11 -Isrc -o build/keyring-test tests/keyring-test.c src/hde-keyring-core.c
 * Prints PASS/FAIL lines; exit status = number of failures.
 */
#include "hde-keyring-core.h"

#include <stdio.h>
#include <string.h>

static int fails, passes;

#define CHECK(cond, ...) do { \
        if (cond) { passes++; printf("PASS: keyring: "); } else { fails++; printf("FAIL: keyring: "); } \
        printf(__VA_ARGS__); printf("\n"); \
    } while (0)

/* What gnome-keyring-daemon --start prints on a modern machine. */
static const char *const DAEMON_OUTPUT =
    "GNOME_KEYRING_CONTROL=/run/user/1000/keyring\n"
    "SSH_AUTH_SOCK=/run/user/1000/keyring/ssh\n";

static void test_names(void)
{
    CHECK(!strcmp(hde_keyring_var_name(HDE_KEYRING_VAR_CONTROL), "GNOME_KEYRING_CONTROL"), "the control variable");
    CHECK(!strcmp(hde_keyring_var_name(HDE_KEYRING_VAR_SSH), "SSH_AUTH_SOCK"), "the ssh agent socket");
    CHECK(!strcmp(hde_keyring_var_name(HDE_KEYRING_VAR_GPG), "GPG_AGENT_INFO"), "the gpg agent (older daemons)");

    CHECK(hde_keyring_wants_export("SSH_AUTH_SOCK"), "the ssh agent socket is passed on");
    CHECK(hde_keyring_wants_export("GNOME_KEYRING_CONTROL"), "so is the control directory");
    CHECK(hde_keyring_wants_export("GPG_AGENT_INFO"), "so is the gpg agent");

    /* Whatever else the program (or something pretending to be it) prints stays out of the session. */
    CHECK(!hde_keyring_wants_export("PATH"), "a PATH in the output is ignored");
    CHECK(!hde_keyring_wants_export("LD_PRELOAD"), "so is an LD_PRELOAD");
    CHECK(!hde_keyring_wants_export("DISPLAY"), "so is a DISPLAY");
    CHECK(!hde_keyring_wants_export(""), "and so is an empty name");
    CHECK(!hde_keyring_wants_export(NULL), "and a missing one");
}

static void test_parse(void)
{
    char names[HDE_KEYRING_MAX_VARS][HDE_KEYRING_NAME_MAX];
    char values[HDE_KEYRING_MAX_VARS][HDE_KEYRING_VALUE_MAX];

    int n = hde_keyring_parse_env(DAEMON_OUTPUT, names, values);
    CHECK(n == 2, "the daemon's two variables are read (%d)", n);
    CHECK(!strcmp(names[0], "GNOME_KEYRING_CONTROL") && !strcmp(values[0], "/run/user/1000/keyring"),
          "the control directory: %s=%s", names[0], values[0]);
    CHECK(!strcmp(names[1], "SSH_AUTH_SOCK") && !strcmp(values[1], "/run/user/1000/keyring/ssh"),
          "the ssh socket: %s=%s", names[1], values[1]);

    char v[HDE_KEYRING_VALUE_MAX];
    CHECK(!strcmp(hde_keyring_lookup(DAEMON_OUTPUT, "SSH_AUTH_SOCK", v, sizeof v), "/run/user/1000/keyring/ssh"),
          "asking for one variable gives its value");
    CHECK(hde_keyring_lookup(DAEMON_OUTPUT, "GPG_AGENT_INFO", v, sizeof v)[0] == '\0',
          "and \"\" for one that is not there");
    CHECK(hde_keyring_lookup(DAEMON_OUTPUT, "PATH", v, sizeof v)[0] == '\0',
          "even when the output really does contain it");

    /* Nothing at all. */
    CHECK(hde_keyring_parse_env("", names, values) == 0, "empty output gives nothing");
    CHECK(hde_keyring_parse_env(NULL, names, values) == 0, "neither does missing output");
    CHECK(hde_keyring_parse_env("# only a comment\n\n", names, values) == 0, "a comment is not a variable");
    CHECK(hde_keyring_parse_env("this is not an assignment\n", names, values) == 0, "nor is a sentence");
    CHECK(hde_keyring_parse_env("=no name\n", names, values) == 0, "a value with no name is skipped");
    CHECK(hde_keyring_parse_env("1BAD=12\n", names, values) == 0, "a name starting with a digit is skipped");
    CHECK(hde_keyring_parse_env("NO PE=12\n", names, values) == 0, "a name with a space in it is skipped");

    /* The shapes the output comes in. */
    n = hde_keyring_parse_env("export SSH_AUTH_SOCK=/run/user/1000/keyring/ssh\n", names, values);
    CHECK(n == 1 && !strcmp(values[0], "/run/user/1000/keyring/ssh"),
          "`export NAME=value` is understood (the output is meant for a shell)");

    n = hde_keyring_parse_env("SSH_AUTH_SOCK=\"/run/user/1000/keyring/ssh\"\n", names, values);
    CHECK(n == 1 && !strcmp(values[0], "/run/user/1000/keyring/ssh"), "quotes round the value are taken off");

    n = hde_keyring_parse_env("SSH_AUTH_SOCK=/run/user/1000/keyring/ssh\r\n", names, values);
    CHECK(n == 1 && !strcmp(values[0], "/run/user/1000/keyring/ssh"), "a stray CR before the line end is taken off");

    n = hde_keyring_parse_env("  SSH_AUTH_SOCK = /run/user/1000/keyring/ssh  \n", names, values);
    CHECK(n == 1 && !strcmp(values[0], "/run/user/1000/keyring/ssh"), "spaces round the = are trimmed");

    /* Only the variables on the list, and each one once. */
    n = hde_keyring_parse_env("PATH=/evil\n"
                              "SSH_AUTH_SOCK=/first\n"
                              "SSH_AUTH_SOCK=/second\n"
                              "GPG_AGENT_INFO=/run/user/1000/keyring/gpg:0:1\n", names, values);
    CHECK(n == 2, "PATH is dropped and a repeated variable is only taken once (%d)", n);
    CHECK(!strcmp(values[0], "/first"), "the first value wins");
    CHECK(!strcmp(values[1], "/run/user/1000/keyring/gpg:0:1"), "a value with colons in it is kept whole");

    /* A line longer than the buffer must not run over it. */
    char long_line[2048];
    memset(long_line, 'x', sizeof long_line - 1);
    long_line[0] = '\0';
    snprintf(long_line, sizeof long_line, "SSH_AUTH_SOCK=");
    memset(long_line + strlen(long_line), 'x', 1500);
    long_line[sizeof long_line - 1] = '\0';
    n = hde_keyring_parse_env(long_line, names, values);
    CHECK(n == 1, "a value far too long is still read");
    CHECK(strlen(values[0]) <= HDE_KEYRING_VALUE_MAX - 1 && strlen(values[0]) > 0,
          "and cut down to what fits, not written past the end (%zu characters)", strlen(values[0]));
}

int main(void)
{
    test_names();
    test_parse();
    printf("\nkeyring: %d passed, %d failed\n", passes, fails);
    return fails;
}
