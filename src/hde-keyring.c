/* hde-keyring — the keyring (Secret Service) of an HDE session.
 *
 * Without a keyring, every program keeps its own secrets the way it likes: the browser asks for the Wi-Fi
 * password again after every restart, Git asks for the passphrase of the SSH key on every push, and nothing is
 * unlocked when the user logs in. HDE uses gnome-keyring, which speaks the standard
 * org.freedesktop.secrets interface, so every program that knows that interface — browsers, Git, NetworkManager,
 * VS Code, the programs of GNOME and of KDE — uses it without being told.
 *
 *   hde-keyring                 start the keyring and print what the session has to export, one NAME=VALUE a line.
 *                               This is what hde-session does at login, before anything else starts.
 *   hde-keyring --export        the same lines with "export " in front, for a shell: eval $(hde-keyring --export)
 *   hde-keyring --unlock        start it and unlock the login keyring with the password read from standard input
 *                               (what /etc/pam.d does through pam_gnome_keyring.so)
 *   hde-keyring --check         0 when this session has a Secret Service, and says which one
 *   hde-keyring --wait SECONDS  wait until the Secret Service answers (or give up)
 *   hde-keyring --status        one line about the keyring of this session
 *   hde-keyring --help | --version
 *
 * Unlocking at login is the one half that cannot be done from here: the password is known to PAM, not to the
 * session. packaging/pam/hde-keyring is the two lines that go into the display manager's PAM file so that the
 * login password opens the login keyring as well.
 */
#include "hde-build.h"
#include "hde-keyring-core.h"

#include <gio/gio.h>
#include <glib.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECRETS_NAME "org.freedesktop.secrets"

static GMainLoop   *loop;
static GCancellable *cancel;
static char        *out_text;
static char        *err_text;
static gboolean     communicate_ok;
static gboolean     verbose;

static void log_line(const char *fmt, ...) G_GNUC_PRINTF(1, 2);

static void log_line(const char *fmt, ...)
{
    if (!verbose) return;
    va_list ap;
    va_start(ap, fmt);
    char *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    g_printerr("hde-keyring: %s\n", msg);
    g_free(msg);
}

/* ---------------------------------------------------------------- the session bus */

static GDBusConnection *session_bus(void)
{
    static GDBusConnection *bus = NULL;
    static gboolean tried = FALSE;
    if (!tried) {
        tried = TRUE;
        bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    }
    return bus;
}

/* Is there a Secret Service on the session bus, and who is it? owner may be NULL. */
static gboolean secrets_running(char **owner)
{
    if (owner) *owner = NULL;
    GDBusConnection *bus = session_bus();
    if (!bus) return FALSE;
    GError *err = NULL;
    GVariant *r = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                             "org.freedesktop.DBus", "GetNameOwner",
                                             g_variant_new("(s)", SECRETS_NAME), G_VARIANT_TYPE("(s)"),
                                             G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &err);
    if (!r) {
        g_clear_error(&err);
        return FALSE;
    }
    if (owner) {
        const char *who = NULL;
        g_variant_get(r, "(&s)", &who);
        *owner = g_strdup(who ? who : "");
    }
    g_variant_unref(r);
    return TRUE;
}

/* ---------------------------------------------------------------- running gnome-keyring-daemon */

static void on_communicated(GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void)user_data;
    GError *err = NULL;
    communicate_ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), res, &out_text, &err_text, &err);
    if (!communicate_ok && err && !g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        fprintf(stderr, "hde-keyring: %s\n", err->message);
    g_clear_error(&err);
    if (loop && g_main_loop_is_running(loop)) g_main_loop_quit(loop);
}

static gboolean give_up(gpointer data)
{
    (void)data;
    g_cancellable_cancel(cancel);
    return G_SOURCE_REMOVE;
}

/* Run the daemon and collect what it prints. stdin_text != NULL: written to it (the password, with --unlock).
 * A leash is put on it, because a program that sits there holding the pipe would hold up the whole login. */
static gboolean run_daemon(const char *const *argv, const char *stdin_text, int timeout_sec)
{
    GError *err = NULL;
    GSubprocessFlags flags = G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE;
    if (stdin_text) flags |= G_SUBPROCESS_FLAGS_STDIN_PIPE;

    GSubprocess *proc = g_subprocess_new(flags, &err, argv[0], argv[1], argv[2], NULL);
    if (!proc) {
        fprintf(stderr, "hde-keyring: %s\n", err ? err->message : "the keyring daemon could not be started");
        g_clear_error(&err);
        return FALSE;
    }
    loop = g_main_loop_new(NULL, FALSE);
    cancel = g_cancellable_new();
    g_timeout_add_seconds(timeout_sec > 0 ? timeout_sec : 10, give_up, NULL);
    g_subprocess_communicate_utf8_async(proc, stdin_text, cancel, on_communicated, NULL);
    g_main_loop_run(loop);

    g_object_unref(cancel);
    cancel = NULL;
    g_main_loop_unref(loop);
    loop = NULL;
    if (err_text && *err_text) log_line("the daemon says: %s", g_strstrip(err_text));
    gboolean ok = communicate_ok;
    g_object_unref(proc);
    return ok;
}

/* ---------------------------------------------------------------- commands */

static void print_env(const char *text, gboolean with_export)
{
    char names[HDE_KEYRING_MAX_VARS][HDE_KEYRING_NAME_MAX];
    char values[HDE_KEYRING_MAX_VARS][HDE_KEYRING_VALUE_MAX];
    int n = hde_keyring_parse_env(text, names, values);
    for (int i = 0; i < n; ++i) {
        if (with_export) printf("export %s=\"%s\"\n", names[i], values[i]);
        else printf("%s=%s\n", names[i], values[i]);
    }
}

static int cmd_start(gboolean with_export, gboolean unlock, int wait_sec)
{
    char *daemon = g_find_program_in_path("gnome-keyring-daemon");
    if (!daemon) {
        fprintf(stderr, "hde-keyring: gnome-keyring-daemon was not found: install gnome-keyring "
                        "(sudo apt install gnome-keyring / sudo dnf install gnome-keyring)\n");
        return 1;
    }

    char *stdin_text = NULL;
    if (unlock) {
        /* The login password, on standard input: this is what pam_gnome_keyring.so arranges at login. */
        char buf[1024];
        if (fgets(buf, sizeof buf, stdin)) {
            buf[strcspn(buf, "\r\n")] = '\0';
            stdin_text = g_strdup_printf("%s\n", buf);
        }
    }
    const char *unlock_argv[] = { daemon, "--unlock", "--components=pkcs11,secrets,ssh", NULL };
    const char *start_argv[] = { daemon, "--start", "--components=pkcs11,secrets,ssh", NULL };
    gboolean ok = run_daemon(stdin_text ? unlock_argv : start_argv, stdin_text, 10);
    if (stdin_text) {
        memset(stdin_text, 0, strlen(stdin_text));
        g_free(stdin_text);
    }
    g_free(daemon);
    if (!ok) return 1;

    print_env(out_text ? out_text : "", with_export);

    /* The daemon forks away and the service takes a moment to appear on the bus; programs started right after
     * this one (and the session itself, with --wait) want to know that it is there before they ask it anything. */
    for (int i = 0; i < wait_sec * 10; ++i) {
        if (secrets_running(NULL)) break;
        g_usleep(100000);
    }
    char *owner = NULL;
    if (secrets_running(&owner)) log_line("the Secret Service is on the session bus (%s)", owner);
    else log_line("the daemon was started but no Secret Service answered yet");
    g_free(owner);
    return 0;
}

static int cmd_check(void)
{
    char *owner = NULL;
    if (secrets_running(&owner)) {
        printf("ok: a Secret Service is running on the session bus (%s)\n", SECRETS_NAME);
        if (owner && *owner) printf("    owned by %s\n", owner);
        g_free(owner);
        return 0;
    }
    printf("no: there is no Secret Service on the session bus: install gnome-keyring "
           "(sudo apt install gnome-keyring / sudo dnf install gnome-keyring)\n");
    g_free(owner);
    return 1;
}

static int cmd_status(void)
{
    char *owner = NULL;
    if (!secrets_running(&owner)) {
        printf("Keyring: none - passwords are not kept for this session\n");
        g_free(owner);
        return 1;
    }
    const char *ctrl = g_getenv("GNOME_KEYRING_CONTROL");
    const char *ssh = g_getenv("SSH_AUTH_SOCK");
    printf("Keyring: %s on the session bus\n", SECRETS_NAME);
    if (owner && *owner) printf("  owned by %s\n", owner);
    printf("  GNOME_KEYRING_CONTROL: %s\n", ctrl && *ctrl ? ctrl : "(not set)");
    printf("  SSH_AUTH_SOCK: %s\n", ssh && *ssh ? ssh : "(not set: ssh will ask for the key's passphrase)");
    g_free(owner);
    return 0;
}

static int cmd_wait(int seconds)
{
    if (seconds <= 0) seconds = 10;
    for (int i = 0; i < seconds * 10; ++i) {
        if (secrets_running(NULL)) { log_line("the Secret Service answered after %d ms", i * 100); return 0; }
        g_usleep(100000);
    }
    log_line("no Secret Service after %d seconds", seconds);
    return 1;
}

static void usage(void)
{
    printf("Usage: hde-keyring [OPTION]\n"
           "Start the keyring (Secret Service) of this session and print what it has to export.\n\n"
           "  (no option)        start the keyring, print one NAME=VALUE a line (what hde-session uses)\n"
           "  --export           the same lines with \"export \" in front, for a shell\n"
           "  --unlock           start it and unlock the login keyring with the password on standard input\n"
           "  --check            0 when this session has a Secret Service, and says which\n"
           "  --wait SECONDS     wait until the Secret Service answers (or give up)\n"
           "  --status           one line about the keyring of this session\n"
           "  --help             this text\n"
           "  --version          the version of HDE\n\n"
           "To have the login password open the keyring too, add the two lines of\n"
           "packaging/pam/hde-keyring to the PAM file of the display manager.\n");
}

int main(int argc, char **argv)
{
    gboolean with_export = FALSE, unlock = FALSE, check = FALSE, status = FALSE;
    gboolean help = FALSE, version = FALSE, do_wait = FALSE;
    int wait_sec = 2;       /* how long to give the Secret Service to appear before going on without it */

    verbose = g_getenv("HDE_DEBUG") != NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) help = TRUE;
        else if (!strcmp(argv[i], "--version")) version = TRUE;
        else if (!strcmp(argv[i], "--export")) with_export = TRUE;
        else if (!strcmp(argv[i], "--unlock")) unlock = TRUE;
        else if (!strcmp(argv[i], "--check")) check = TRUE;
        else if (!strcmp(argv[i], "--status")) status = TRUE;
        else if (!strcmp(argv[i], "--wait") && i + 1 < argc) { do_wait = TRUE; wait_sec = atoi(argv[++i]); }
        else { fprintf(stderr, "hde-keyring: unknown option '%s' (try --help)\n", argv[i]); return 2; }
    }
    if (help) { usage(); return 0; }
    if (version) { printf("hde-keyring (HDE) %s\n", HDE_VERSION); return 0; }

    if (check) return cmd_check();
    if (status) return cmd_status();
    if (do_wait) return cmd_wait(wait_sec);
    return cmd_start(with_export, unlock, wait_sec);
}
