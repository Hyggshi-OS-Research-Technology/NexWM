/* hde-automount — the part of HDE that mounts a USB stick when it is plugged in.
 *
 * Until now HDE left this to the file manager, and before it had one of its own, to nothing at all: a stick was
 * plugged in and nothing happened until the user went looking for it. This daemon watches udisks2
 * (src/hde-udisks.c), mounts what it is given, says so with a notification and offers to open it in Hyggshi Files.
 *
 *   hde-automount              watch and mount (run by hde-session, in both of HDE's sessions)
 *   hde-automount --list       what is plugged in now
 *   hde-automount --mount /dev/sdb1      mount one (an object path works too)
 *   hde-automount --unmount /dev/sdb1    unmount one
 *   hde-automount --eject /dev/sdb1      unmount and spin the drive down, ready to be pulled out
 *   hde-automount --check      0 when this machine can mount drives (udisks2 answers)
 *
 * Settings in ~/.config/hde/settings.ini: automount (on by default), automount_open (open the folder in Files
 * as soon as it is mounted, off by default).
 *
 * One thing it is careful about: a drive the user unmounts stays unmounted. Without that, unmounting a stick in
 * Files would be undone a moment later by the very daemon whose job it is to mount sticks.
 */
#include "hde-build.h"
#include "hde-disks.h"
#include "hde-udisks.h"

#include <gio/gio.h>
#include <glib.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static GMainLoop *loop;
static HdeDisks  *disks;
static gboolean   verbose;
static gboolean   opt_open_after_mount;
static gboolean   automount_enabled = TRUE;

/* Object paths of drives that have been unmounted (by us, or by Files, or by the user in a terminal): left alone
 * until they are plugged in again. */
static GHashTable *left_alone;
/* Object paths with a mount on its way to udisks2: without this, a second scan a moment later would ask for the
 * same drive to be mounted again before the first answer has come back. */
static GHashTable *mounting;
/* The last list we were told about, to notice a drive going from mounted to unmounted. */
static GHashTable *last_seen;
/* Notifications with an "Open in Files" button: notification id -> path to open. */
static GHashTable *actions;
static guint32     notify_id_drive, notify_id_error;

static void log_line(const char *fmt, ...) G_GNUC_PRINTF(1, 2);

static void log_line(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    if (verbose) g_printerr("hde-automount: %s\n", msg);
    g_free(msg);
}

/* ---------------------------------------------------------------- notifications */

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

typedef struct {
    guint32 *id_slot;
    char    *path;          /* the "Open in Files" button, NULL when there is none */
} NotifyData;

static void on_notified(GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void)source;
    NotifyData *nd = user_data;
    GError *err = NULL;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &err);
    if (nd->path) {
        guint id = 0;
        if (r) g_variant_get(r, "(u)", &id);
        if (id) g_hash_table_replace(actions, GUINT_TO_POINTER(id), g_strdup(nd->path));
    }
    if (r) g_variant_unref(r);
    g_clear_error(&err);
    g_free(nd->path);
    g_free(nd);
}

/* urgency 1 normal, 2 critical. action_key/action_label add a button; path is what the button opens. */
static void notify(guint32 *id_slot, const char *icon, const char *summary, const char *body,
                   const char *action_key, const char *action_label, const char *path, guchar urgency)
{
    GDBusConnection *bus = session_bus();
    if (!bus) return;
    GVariantBuilder acts, hints;
    g_variant_builder_init(&acts, G_VARIANT_TYPE("as"));
    if (action_key && action_label) {
        g_variant_builder_add(&acts, "s", action_key);
        g_variant_builder_add(&acts, "s", action_label);
    }
    g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&hints, "{sv}", "urgency", g_variant_new_byte(urgency));
    g_variant_builder_add(&hints, "{sv}", "category", g_variant_new_string("device"));
    g_variant_builder_add(&hints, "{sv}", "desktop-entry", g_variant_new_string("hde-files"));

    NotifyData *nd = g_new0(NotifyData, 1);
    nd->id_slot = id_slot;
    nd->path = g_strdup(path);
    g_dbus_connection_call(bus, "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                           "org.freedesktop.Notifications", "Notify",
                           g_variant_new("(susssasa{sv}i)", "HDE", id_slot ? *id_slot : 0, icon, summary, body,
                                         &acts, &hints, -1),
                           G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, on_notified, nd);
}

/* ---------------------------------------------------------------- opening a folder in Files */

static void open_in_files(const char *path)
{
    if (!path || !*path) return;
    char *argv[3];
    argv[0] = (char *)(g_find_program_in_path("hde-files") ? "hde-files" : "xdg-open");
    argv[1] = (char *)path;
    argv[2] = NULL;
    g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
    log_line("opening %s in %s", path, argv[0]);
}

/* ---------------------------------------------------------------- the mounting */

static void on_mounted(HdeDisks *d, const char *object_path, gboolean ok, const char *message,
                      const char *mount_point, gpointer data)
{
    (void)d; (void)data;
    g_hash_table_remove(mounting, object_path);
    HdeDisk *disk = hde_disks_find(disks, object_path);
    char title[HDE_DISK_NAME_MAX * 2], subtitle[256];

    if (disk) {
        hde_disk_title(disk, title, sizeof title);
        hde_disk_subtitle(disk, subtitle, sizeof subtitle);
    } else {
        g_snprintf(title, sizeof title, "Drive");
        subtitle[0] = '\0';
    }

    if (!ok) {
        log_line("could not mount %s: %s", object_path, message ? message : "no reason given");
        notify(&notify_id_error, "dialog-error", title,
               message ? message : "The drive could not be mounted.", NULL, NULL, NULL, 2);
        return;
    }
    if (!mount_point || !*mount_point) return;          /* an unmount or an eject: nothing to say here */

    log_line("mounted %s on %s", object_path, mount_point);
    char *body = g_strdup_printf("%s is ready to use.\n%s", mount_point, subtitle);
    notify(&notify_id_drive, hde_disk_icon(disk), title, body, "open", "Open in Files", mount_point, 1);
    g_free(body);
    if (opt_open_after_mount) open_in_files(mount_point);
}

static void on_unmounted(HdeDisks *d, const char *object_path, gboolean ok, const char *message,
                         const char *mount_point, gpointer data)
{
    (void)d; (void)mount_point; (void)data;
    if (ok) {
        log_line("unmounted %s", object_path);
        return;
    }
    log_line("could not unmount %s: %s", object_path, message ? message : "no reason given");
}

static void on_ejected(HdeDisks *d, const char *object_path, gboolean ok, const char *message,
                       const char *mount_point, gpointer data)
{
    (void)d; (void)mount_point; (void)data;
    HdeDisk *disk = hde_disks_find(disks, object_path);
    char title[HDE_DISK_NAME_MAX * 2];
    if (disk) hde_disk_title(disk, title, sizeof title);
    else g_snprintf(title, sizeof title, "Drive");

    if (!ok) {
        log_line("could not eject %s: %s", object_path, message ? message : "no reason given");
        notify(&notify_id_error, "dialog-error", title,
               "The drive is still in use: close the files that are open on it and try again.",
               NULL, NULL, NULL, 2);
        return;
    }
    log_line("ejected %s", object_path);
    /* The one sentence that makes pulling a stick out safe to do. */
    notify(&notify_id_drive, hde_disk_icon(disk), title, "It is now safe to remove the drive.",
           NULL, NULL, NULL, 1);
    g_hash_table_remove(left_alone, object_path);      /* it is gone: if it comes back, mount it again */
}

/* Mount everything that has just been plugged in and should be mounted. */
static void mount_what_is_new(void)
{
    GPtrArray *list = hde_disks_list(disks);
    if (!list) return;
    for (guint i = 0; i < list->len; ++i) {
        HdeDisk *d = g_ptr_array_index(list, i);
        if (!hde_disk_should_mount(d)) continue;
        if (g_hash_table_contains(left_alone, d->object_path)) continue;
        if (g_hash_table_contains(mounting, d->object_path)) continue;
        if (!automount_enabled) { log_line("not mounting %s: turned off in Settings", d->device); continue; }
        log_line("mounting %s (%s)", d->device, d->label[0] ? d->label : d->drive_model);
        g_hash_table_replace(mounting, g_strdup(d->object_path), GINT_TO_POINTER(1));
        hde_disks_mount_async(disks, d->object_path, on_mounted, NULL);
    }
}

/* A drive that was mounted and now is not was unmounted by somebody: leave it alone from now on. */
static void notice_unmounts(GPtrArray *list)
{
    GHashTable *now = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    for (guint i = 0; i < list->len; ++i) {
        HdeDisk *d = g_ptr_array_index(list, i);
        HdeDisk *copy = g_new0(HdeDisk, 1);
        *copy = *d;
        g_hash_table_replace(now, g_strdup(d->object_path), copy);
    }
    GHashTableIter it;
    gpointer key, value;
    g_hash_table_iter_init(&it, last_seen);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        const char *path = key;
        HdeDisk *before = value;
        HdeDisk *after = g_hash_table_lookup(now, path);
        if (!after) {                                   /* pulled out: forget everything about it */
            g_hash_table_remove(left_alone, path);
            continue;
        }
        if (before->mounted && !after->mounted && !g_hash_table_contains(left_alone, path)) {
            log_line("%s was unmounted: leaving it alone", before->device);
            g_hash_table_replace(left_alone, g_strdup(path), GINT_TO_POINTER(1));
        }
    }
    g_hash_table_destroy(last_seen);
    last_seen = now;
}

static void on_changed(HdeDisks *d, gpointer data)
{
    (void)d; (void)data;
    GPtrArray *list = hde_disks_list(disks);
    if (!list) return;
    notice_unmounts(list);
    mount_what_is_new();
}

/* ---------------------------------------------------------------- the settings file */

static void read_settings(void)
{
    char *path = g_build_filename(g_get_user_config_dir(), "hde", "settings.ini", NULL);
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        automount_enabled = g_key_file_get_boolean(kf, "settings", "automount", NULL) != FALSE;
        opt_open_after_mount = g_key_file_get_boolean(kf, "settings", "automount_open", NULL) == TRUE;
    }
    g_key_file_free(kf);
    g_free(path);
}

static gboolean settings_changed_timeout(gpointer data);

static void on_settings_changed(GFileMonitor *m, GFile *f, GFile *other, GFileMonitorEvent ev, gpointer data)
{
    (void)m; (void)f; (void)other; (void)data;
    if (ev == G_FILE_MONITOR_EVENT_CHANGED || ev == G_FILE_MONITOR_EVENT_CREATED)
        g_timeout_add(300, settings_changed_timeout, NULL);
}

static gboolean settings_changed_timeout(gpointer data)
{
    (void)data;
    read_settings();
    return G_SOURCE_REMOVE;
}

/* ---------------------------------------------------------------- "Open in Files" was clicked */

static void on_action_invoked(GDBusConnection *conn, const gchar *sender, const gchar *path, const gchar *iface,
                             const gchar *signal, GVariant *params, gpointer user_data)
{
    (void)conn; (void)sender; (void)path; (void)iface; (void)signal; (void)user_data;
    guint32 id = 0;
    gchar *key = NULL;
    g_variant_get(params, "(us)", &id, &key);
    if (!g_strcmp0(key, "open")) {
        const char *where = g_hash_table_lookup(actions, GUINT_TO_POINTER(id));
        if (where) open_in_files(where);
    }
    g_free(key);
}

static void on_notification_closed(GDBusConnection *conn, const gchar *sender, const gchar *path,
                                   const gchar *iface, const gchar *signal, GVariant *params, gpointer user_data)
{
    (void)conn; (void)sender; (void)path; (void)iface; (void)signal; (void)user_data;
    guint32 id = 0, reason = 0;
    g_variant_get(params, "(uu)", &id, &reason);        /* (id, reason) */
    (void)reason;
    g_hash_table_remove(actions, GUINT_TO_POINTER(id));
}

/* ---------------------------------------------------------------- one-shot commands */

static char *resolve_path(const char *what)
{
    if (!what) return NULL;
    if (g_str_has_prefix(what, "/org/freedesktop/UDisks2/")) return g_strdup(what);
    GPtrArray *list = hde_disks_list(disks);
    if (!list) return NULL;
    for (guint i = 0; i < list->len; ++i) {
        HdeDisk *d = g_ptr_array_index(list, i);
        if (g_strcmp0(d->device, what) == 0 || g_strcmp0(d->mount_point, what) == 0)
            return g_strdup(d->object_path);
    }
    return NULL;
}

static void print_list(void)
{
    GPtrArray *list = hde_disks_list(disks);
    if (!list || list->len == 0) { printf("No drive with a filesystem on it was found.\n"); return; }
    for (guint i = 0; i < list->len; ++i) {
        HdeDisk *d = g_ptr_array_index(list, i);
        if (!hde_disk_should_show(d)) continue;
        char title[HDE_DISK_NAME_MAX * 2], subtitle[256];
        hde_disk_title(d, title, sizeof title);
        hde_disk_subtitle(d, subtitle, sizeof subtitle);
        printf("%-12s %-46s %-24s %s%s%s\n", d->device, d->object_path, title, subtitle,
               d->mounted ? " at " : "", d->mounted ? d->mount_point : "");
    }
}

/* ---------------------------------------------------------------- main */

static void usage(void)
{
    printf("Usage: hde-automount [OPTION]\n"
           "Mount a USB stick, an SD card or a disc as soon as it is plugged in.\n\n"
           "  (no option)        watch for drives and mount them (this is what hde-session runs)\n"
           "  --list             what is plugged in now\n"
           "  --mount PATH       mount one drive (a device such as /dev/sdb1, or its mount point)\n"
           "  --unmount PATH     unmount one\n"
           "  --eject PATH       unmount it and spin the drive down, ready to be pulled out\n"
           "  --check            say whether this machine can mount drives at all\n"
           "  --help             this text\n"
           "  --version          the version of HDE\n\n"
           "Settings (~/.config/hde/settings.ini): automount=1, automount_open=0.\n");
}

static gboolean one_shot_ok = TRUE;

/* Wait (a moment) until udisks2 has answered: a one-shot command that ran before the first scan would see
 * nothing at all. */
static gboolean wait_timed_out(gpointer data)
{
    (void)data;
    if (loop) g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

static void on_first_change(HdeDisks *d, gpointer data)
{
    (void)d; (void)data;
    if (loop && hde_disks_online(disks)) g_main_loop_quit(loop);
}

typedef struct {
    gboolean mount, unmount, eject;
    char *what;
} OneShot;

static void one_shot_done(HdeDisks *d, const char *object_path, gboolean ok, const char *message,
                          const char *mount_point, gpointer data)
{
    (void)d; (void)object_path; (void)data;
    one_shot_ok = ok;
    if (ok) {
        if (mount_point) printf("%s\n", mount_point);
    } else {
        fprintf(stderr, "hde-automount: %s\n", message ? message : "that did not work");
    }
    if (loop) g_main_loop_quit(loop);
}

int main(int argc, char **argv)
{
    gboolean do_list = FALSE, do_check = FALSE, help = FALSE, version = FALSE;
    OneShot one = { FALSE, FALSE, FALSE, NULL };

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) help = TRUE;
        else if (!strcmp(argv[i], "--version")) version = TRUE;
        else if (!strcmp(argv[i], "--list")) do_list = TRUE;
        else if (!strcmp(argv[i], "--check")) do_check = TRUE;
        else if (!strcmp(argv[i], "--mount") && i + 1 < argc) { one.mount = TRUE; one.what = argv[++i]; }
        else if (!strcmp(argv[i], "--unmount") && i + 1 < argc) { one.unmount = TRUE; one.what = argv[++i]; }
        else if (!strcmp(argv[i], "--eject") && i + 1 < argc) { one.eject = TRUE; one.what = argv[++i]; }
        else { fprintf(stderr, "hde-automount: unknown option '%s' (try --help)\n", argv[i]); return 2; }
    }
    if (help) { usage(); return 0; }
    if (version) { printf("hde-automount (HDE) %s\n", HDE_VERSION); return 0; }

    verbose = g_getenv("HDE_DEBUG") != NULL;
    left_alone = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    mounting = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    last_seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    actions = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    read_settings();

    gboolean one_shot = do_list || do_check || one.mount || one.unmount || one.eject;
    disks = hde_disks_new(one_shot ? on_first_change : on_changed, NULL);

    if (one_shot) {
        loop = g_main_loop_new(NULL, FALSE);
        g_timeout_add_seconds(5, wait_timed_out, NULL);
        g_main_loop_run(loop);
        g_main_loop_unref(loop);
        loop = NULL;

        if (do_check) {
            if (hde_disks_online(disks)) { printf("ok: udisks2 answers, drives can be mounted\n"); return 0; }
            fprintf(stderr, "hde-automount: udisks2 is not running (install udisks2)\n");
            return 1;
        }
        if (do_list) { print_list(); return hde_disks_online(disks) ? 0 : 1; }

        char *path = resolve_path(one.what);
        if (!path) {
            fprintf(stderr, "hde-automount: no drive '%s' (try --list)\n", one.what ? one.what : "");
            return 1;
        }
        loop = g_main_loop_new(NULL, FALSE);
        g_timeout_add_seconds(30, wait_timed_out, NULL);
        if (one.mount) hde_disks_mount_async(disks, path, one_shot_done, NULL);
        else if (one.unmount) hde_disks_unmount_async(disks, path, one_shot_done, NULL);
        else hde_disks_eject_async(disks, path, one_shot_done, NULL);
        g_main_loop_run(loop);
        g_free(path);
        return one_shot_ok ? 0 : 1;
    }

    /* The daemon: watch, mount, and answer the buttons on its own notifications. */
    GDBusConnection *bus = session_bus();
    if (bus) {
        g_dbus_connection_signal_subscribe(bus, "org.freedesktop.Notifications", "org.freedesktop.Notifications",
                                           "ActionInvoked", "/org/freedesktop/Notifications", NULL,
                                           G_DBUS_SIGNAL_FLAGS_NONE, on_action_invoked, NULL, NULL);
        g_dbus_connection_signal_subscribe(bus, "org.freedesktop.Notifications", "org.freedesktop.Notifications",
                                           "NotificationClosed", "/org/freedesktop/Notifications", NULL,
                                           G_DBUS_SIGNAL_FLAGS_NONE, on_notification_closed, NULL, NULL);
    }
    char *ini = g_build_filename(g_get_user_config_dir(), "hde", "settings.ini", NULL);
    GFile *f = g_file_new_for_path(ini);
    GFileMonitor *mon = g_file_monitor_file(f, G_FILE_MONITOR_NONE, NULL, NULL);
    if (mon) g_signal_connect(mon, "changed", G_CALLBACK(on_settings_changed), NULL);
    g_object_unref(f);
    g_free(ini);

    log_line("watching for drives (udisks2%s)", automount_enabled ? "" : ", mounting turned off in Settings");
    loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);

    if (mon) g_object_unref(mon);
    hde_disks_free(disks);
    g_hash_table_destroy(left_alone);
    g_hash_table_destroy(mounting);
    g_hash_table_destroy(last_seen);
    g_hash_table_destroy(actions);
    g_main_loop_unref(loop);
    return 0;
}
