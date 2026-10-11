/* hde-keyring-core.h — the one thing about a keyring that can be got wrong and tested: reading what
 * `gnome-keyring-daemon --start` prints. Plain C (src/hde-keyring-core.c), no D-Bus and no display, so
 * tests/keyring-test.c can check it everywhere.
 *
 * The daemon does not speak D-Bus configuration, it prints shell assignments on stdout:
 *
 *   GNOME_KEYRING_CONTROL=/run/user/1000/keyring
 *   SSH_AUTH_SOCK=/run/user/1000/keyring/ssh
 *
 * and the session has to turn those into environment variables before anything else starts, or the browser, Git
 * and the Wi-Fi of this session each end up with a keyring of their own and ask the user for a password again
 * and again. Reading them is easy; reading them *safely* is the point of this file, because whatever is read here
 * is handed to every program of the session: only the variables on the list below are ever passed on, so a daemon
 * that prints `PATH=/wherever` (or a version of the program that changes its output) cannot reach the session.
 */
#ifndef HDE_KEYRING_CORE_H
#define HDE_KEYRING_CORE_H

#include <stddef.h>

/* The most variables the daemon's output is read into. */
#define HDE_KEYRING_MAX_VARS   8
#define HDE_KEYRING_NAME_MAX  48
#define HDE_KEYRING_VALUE_MAX 512

/* The variables HDE passes on to the session, in the order they are looked for. */
typedef enum {
    HDE_KEYRING_VAR_CONTROL = 0,    /* GNOME_KEYRING_CONTROL: where the socket of the secrets service is */
    HDE_KEYRING_VAR_SSH,            /* SSH_AUTH_SOCK: so that ssh-add talks to the keyring */
    HDE_KEYRING_VAR_GPG,            /* GPG_AGENT_INFO: the older name, still printed by older daemons */
    HDE_KEYRING_N_VARS
} HdeKeyringVar;

/* The name of each one ("GNOME_KEYRING_CONTROL", "SSH_AUTH_SOCK", "GPG_AGENT_INFO"). */
const char *hde_keyring_var_name(HdeKeyringVar v);
/* Is this a variable the session may be given? Everything else in the daemon's output is ignored. */
int         hde_keyring_wants_export(const char *name);

/* Read the daemon's output. Fills names/values with what it found (only the variables above, in the order they
 * were printed, each one once) and returns how many that was. Lines that are not `NAME=value` are skipped: a
 * comment, an empty line, `export NAME=value`, quotes round the value and CRLF line ends are all understood. */
int         hde_keyring_parse_env(const char *text,
                                  char names[HDE_KEYRING_MAX_VARS][HDE_KEYRING_NAME_MAX],
                                  char values[HDE_KEYRING_MAX_VARS][HDE_KEYRING_VALUE_MAX]);
/* One variable out of the daemon's output: "" when it is not there. */
const char *hde_keyring_lookup(const char *text, const char *name, char *out, size_t len);

#endif /* HDE_KEYRING_CORE_H */
