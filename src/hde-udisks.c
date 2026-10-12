/* hde-udisks.c — the removable disks of the machine, read from udisks2 over the system message bus
 * (see src/hde-udisks.h). Used by hde-automount and by the sidebar of Hyggshi Files.
 *
 * How it works: once, and again whenever udisks2 says something changed, `GetManagedObjects` is called on
 * /org/freedesktop/UDisks2 and the answer — every object it manages, with every D-Bus interface and every
 * property of it — is read into HdeDisk structs. Two passes are needed for that, because what makes a USB stick
 * a USB stick (Removable, Ejectable, the drive's model) lives on the *drive*, while the filesystem and the mount
 * points live on the *block device* inside it; the block device only knows which drive it sits in.
 *
 * Nothing here decides anything about a disk: whether it is shown, mounted or called something is src/hde-disks.c,
 * which is plain C and tested without a message bus.
 */
#include "hde-udisks.h"

#include <string.h>

#define UDISKS_BUS   G_BUS_TYPE_SYSTEM
#define UDISKS_NAME  "org.freedesktop.UDisks2"
#define UDISKS_PATH  "/org/freedesktop/UDisks2"
#define UDISKS_BLOCK      "org.freedesktop.UDisks2.Block"
#define UDISKS_FILESYSTEM "org.freedesktop.UDisks2.Filesystem"
#define UDISKS_DRIVE      "org.freedesktop.UDisks2.Drive"

/* What a drive knows that the block devices inside it do not. */
typedef struct {
    char     model[HDE_DISK_NAME_MAX];
    gboolean removable, ejectable, optical, media_available;
} DriveInfo;

struct HdeDisks {
    GDBusConnection *conn;
    GPtrArray       *disks;         /* HdeDisk*, owned, sorted by object path */
    GHashTable      *drives;        /* object path (owned) -> DriveInfo* (owned) */
    HdeDisksChanged  changed;
    gpointer         data;
    guint            watch_id;
    guint            sig_added, sig_removed, sig_props;
    guint            debounce_id;
    gboolean         refreshing;    /* a GetManagedObjects call is on its way */
    gboolean         again;         /* something changed while it was: go round once more */
    gboolean         online;        /* udisks2 has answered at least once */
    gboolean         said_once;     /* the "udisks2 is not running" line has been written to the log */
    gboolean         verbose;       /* HDE_DEBUG=1 */
};

/* One job waiting for udisks2: a mount, an unmount, or the unmount that comes before an eject. */
typedef struct {
    HdeDisks     *disks;
    HdeDisksDone  cb;
    gpointer      data;
    char         *object_path;
    char         *drive_path;       /* where an Eject call goes */
    gboolean      then_eject;       /* this is the unmount half of an eject */
} DiskOp;

static void rescan(HdeDisks *d);

/* ---------------------------------------------------------------- reading D-Bus properties */

static void set_str(char *dst, size_t len, const char *src)
{
    if (!dst || !len) return;
    g_snprintf(dst, len, "%s", src ? src : "");
}

static void prop_string(GVariant *props, const char *key, char *out, size_t len)
{
    GVariant *v = g_variant_lookup_value(props, key, G_VARIANT_TYPE_STRING);
    if (!v) return;
    set_str(out, len, g_variant_get_string(v, NULL));
    g_variant_unref(v);
}

/* udisks2 gives device paths and mount points as byte arrays with a NUL on the end (ay). */
static void prop_bytestring(GVariant *props, const char *key, char *out, size_t len)
{
    GVariant *v = g_variant_lookup_value(props, key, G_VARIANT_TYPE_BYTESTRING);
    if (!v) return;
    set_str(out, len, g_variant_get_bytestring(v));
    g_variant_unref(v);
}

static gboolean prop_bool(GVariant *props, const char *key, gboolean fallback)
{
    gboolean b = fallback;
    g_variant_lookup(props, key, "b", &b);
    return b;
}

/* The properties of one interface of an object, or NULL when the object does not have it. */
static GVariant *iface_props(GVariant *ifaces, const char *name)
{
    GVariantIter it;
    const char *iface = NULL;
    GVariant *props = NULL;
    /* Not g_variant_iter_loop: the loop below stops as soon as the interface is found, and iter_loop only
     * frees the value it handed out on the call after it — returning in between would leak it. */
    g_variant_iter_init(&it, ifaces);
    while (g_variant_iter_next(&it, "{&s@a{sv}}", &iface, &props)) {
        if (g_strcmp0(iface, name) == 0) return props;      /* the caller owns this reference */
        g_variant_unref(props);
    }
    return NULL;
}

/* A drive is optical when it takes discs: udisks2 says so with Optical, or, on older versions, only through what
 * the media in it is called ("optical_dvd", "optical_cd_r", ...). */
static gboolean drive_is_optical(GVariant *props)
{
    if (prop_bool(props, "Optical", FALSE)) return TRUE;
    GVariant *v = g_variant_lookup_value(props, "MediaCompatibility", G_VARIANT_TYPE_STRING_ARRAY);
    if (!v) return FALSE;
    GVariantIter it;
    const char *media = NULL;
    gboolean found = FALSE;
    g_variant_iter_init(&it, v);
    while (g_variant_iter_loop(&it, "&s", &media))
        if (g_str_has_prefix(media, "optical")) found = TRUE;
    g_variant_unref(v);
    return found;
}

static void read_mount_point(GVariant *props, HdeDisk *d)
{
    GVariant *v = g_variant_lookup_value(props, "MountPoints", G_VARIANT_TYPE("aay"));
    if (!v) return;
    GVariantIter it;
    g_variant_iter_init(&it, v);
    GVariant *first = g_variant_iter_next_value(&it);
    if (first) {
        set_str(d->mount_point, sizeof d->mount_point, g_variant_get_bytestring(first));
        d->mounted = d->mount_point[0] ? 1 : 0;
        g_variant_unref(first);
    }
    g_variant_unref(v);
}

static DriveInfo *drive_info_new(GVariant *props)
{
    DriveInfo *di = g_new0(DriveInfo, 1);
    /* "Vendor Model", the way the drive announces itself over USB. */
    char vendor[HDE_DISK_NAME_MAX] = "", model[HDE_DISK_NAME_MAX] = "";
    prop_string(props, "Vendor", vendor, sizeof vendor);
    prop_string(props, "Model", model, sizeof model);
    if (vendor[0] && model[0]) g_snprintf(di->model, sizeof di->model, "%s %s", vendor, model);
    else set_str(di->model, sizeof di->model, vendor[0] ? vendor : model);
    di->removable = prop_bool(props, "Removable", FALSE) || prop_bool(props, "MediaRemovable", FALSE);
    di->ejectable = prop_bool(props, "Ejectable", FALSE);
    di->optical = drive_is_optical(props);
    di->media_available = prop_bool(props, "MediaAvailable", TRUE);
    return di;
}

/* ---------------------------------------------------------------- the scan */

static gint cmp_disk(gconstpointer a, gconstpointer b)
{
    const HdeDisk *x = *(const HdeDisk *const *)a;
    const HdeDisk *y = *(const HdeDisk *const *)b;
    return g_strcmp0(x->object_path, y->object_path);
}

static void collect(HdeDisks *d, GVariant *objects)
{
    GVariantIter it;
    const char *path = NULL;
    GVariant *ifaces = NULL;

    /* The drives first: a block device only knows which drive it sits in. */
    g_hash_table_remove_all(d->drives);
    g_variant_iter_init(&it, objects);
    while (g_variant_iter_loop(&it, "{&o@a{sa{sv}}}", &path, &ifaces)) {
        GVariant *props = iface_props(ifaces, UDISKS_DRIVE);
        if (!props) continue;
        g_hash_table_replace(d->drives, g_strdup(path), drive_info_new(props));
        g_variant_unref(props);
    }

    /* Then every block device that has a filesystem on it. */
    g_ptr_array_set_size(d->disks, 0);
    g_variant_iter_init(&it, objects);
    while (g_variant_iter_loop(&it, "{&o@a{sa{sv}}}", &path, &ifaces)) {
        GVariant *block = iface_props(ifaces, UDISKS_BLOCK);
        if (!block) continue;

        HdeDisk *dsk = g_new0(HdeDisk, 1);
        hde_disk_init(dsk);
        set_str(dsk->object_path, sizeof dsk->object_path, path);
        prop_bytestring(block, "Device", dsk->device, sizeof dsk->device);
        prop_bytestring(block, "IdLabel", dsk->label, sizeof dsk->label);
        prop_string(block, "IdUUID", dsk->uuid, sizeof dsk->uuid);
        prop_string(block, "IdType", dsk->fstype, sizeof dsk->fstype);
        prop_string(block, "Drive", dsk->drive_path, sizeof dsk->drive_path);
        guint64 size = 0;
        g_variant_lookup(block, "Size", "t", &size);
        dsk->size = size;
        dsk->hint_auto = prop_bool(block, "HintAuto", FALSE);
        dsk->hint_ignore = prop_bool(block, "HintIgnore", FALSE);
        dsk->hint_system = prop_bool(block, "HintSystem", FALSE);
        dsk->loop = g_str_has_prefix(dsk->device, "/dev/loop");
        /* A locked container (crypto_LUKS): nothing to mount until it is opened, and opening it asks for a
         * passphrase, so it is shown and never mounted on its own. A device *inside* a container that has been
         * opened (its CryptoBackingDevice) is an ordinary filesystem again and is not marked. */
        dsk->encrypted = g_str_has_prefix(dsk->fstype, "crypto_");

        GVariant *fs = iface_props(ifaces, UDISKS_FILESYSTEM);
        if (fs) {
            dsk->has_filesystem = 1;
            read_mount_point(fs, dsk);
            g_variant_unref(fs);
        }

        DriveInfo *di = dsk->drive_path[0] ? g_hash_table_lookup(d->drives, dsk->drive_path) : NULL;
        if (di) {
            set_str(dsk->drive_model, sizeof dsk->drive_model, di->model);
            dsk->removable = di->removable;
            dsk->ejectable = di->ejectable;
            dsk->optical = di->optical;
            dsk->has_media = di->media_available;
        } else {
            dsk->has_media = 1;         /* no drive to ask: there is a device, so there is something there */
        }

        g_ptr_array_add(d->disks, dsk);
        g_variant_unref(block);
    }
    g_ptr_array_sort(d->disks, cmp_disk);
}

static void rescan_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
    HdeDisks *d = user_data;
    GError *err = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &err);
    d->refreshing = FALSE;

    if (reply) {
        d->online = TRUE;
        GVariant *objects = g_variant_get_child_value(reply, 0);
        collect(d, objects);
        g_variant_unref(objects);
        g_variant_unref(reply);
        if (d->changed) d->changed(d, d->data);
    } else {
        d->online = FALSE;
        /* Said once, not on every signal: a machine without udisks2 would otherwise fill the session log. */
        if (!d->said_once) {
            d->said_once = TRUE;
            g_printerr("hde-disks: udisks2 did not answer (%s): no drive is mounted automatically\n",
                       err ? err->message : "no reason given");
        }
        g_clear_error(&err);
    }
    if (d->again) { d->again = FALSE; rescan(d); }
}

static void rescan(HdeDisks *d)
{
    if (!d->conn) return;
    if (d->refreshing) { d->again = TRUE; return; }
    d->refreshing = TRUE;
    g_dbus_connection_call(d->conn, UDISKS_NAME, UDISKS_PATH, "org.freedesktop.DBus.ObjectManager",
                           "GetManagedObjects", NULL, G_VARIANT_TYPE("(a{oa{sa{sv}}})"), G_DBUS_CALL_FLAGS_NONE,
                           5000, NULL, rescan_done, d);
}

/* udisks2 sends a burst of signals when a stick is plugged in (one per interface, then the properties); one scan
 * a moment later covers the whole burst. */
static gboolean rescan_soon(gpointer data)
{
    HdeDisks *d = data;
    d->debounce_id = 0;
    rescan(d);
    return G_SOURCE_REMOVE;
}

static void queue_rescan(HdeDisks *d)
{
    if (d->debounce_id) return;
    d->debounce_id = g_timeout_add(150, rescan_soon, d);
}

/* ---------------------------------------------------------------- mount, unmount, eject */

static void op_free(DiskOp *op)
{
    if (!op) return;
    g_free(op->object_path);
    g_free(op->drive_path);
    g_free(op);
}

static void op_finish(DiskOp *op, gboolean ok, const char *message, const char *mount_point)
{
    if (op->cb) op->cb(op->disks, op->object_path, ok, message, mount_point, op->data);
    op_free(op);
}

/* udisks2 wants an a{sv} of options; HDE takes the defaults (mount it the ordinary way, as this user). */
static GVariant *empty_options(void)
{
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("(a{sv})"));
    return g_variant_builder_end(&b);
}

static void call_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
    DiskOp *op = user_data;
    GError *err = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &err);
    char mount_point[HDE_DISK_MOUNT_MAX] = "";

    if (!reply) {
        op_finish(op, FALSE, err ? err->message : "udisks2 did not answer", NULL);
        g_clear_error(&err);
        return;
    }
    /* Only Mount answers with something: where it was mounted. */
    if (g_variant_is_of_type(reply, G_VARIANT_TYPE("(s)"))) {
        const char *mp = NULL;
        g_variant_get(reply, "(&s)", &mp);
        set_str(mount_point, sizeof mount_point, mp);
    }
    g_variant_unref(reply);
    if (op->then_eject) {
        /* The first half of an eject: the filesystem is off, now the drive itself. */
        op->then_eject = FALSE;
        const char *target = op->drive_path[0] ? op->drive_path : op->object_path;
        g_dbus_connection_call(op->disks->conn, UDISKS_NAME, target, UDISKS_DRIVE, "Eject", empty_options(),
                               NULL, G_DBUS_CALL_FLAGS_NONE, 20000, NULL, call_done, op);
        return;
    }
    queue_rescan(op->disks);
    op_finish(op, TRUE, NULL, mount_point[0] ? mount_point : NULL);
}

static DiskOp *op_new(HdeDisks *disks, const char *object_path, HdeDisksDone cb, gpointer data)
{
    DiskOp *op = g_new0(DiskOp, 1);
    op->disks = disks;
    op->cb = cb;
    op->data = data;
    op->object_path = g_strdup(object_path ? object_path : "");
    op->drive_path = g_strdup("");
    return op;
}

void hde_disks_mount_async(HdeDisks *disks, const char *object_path, HdeDisksDone cb, gpointer data)
{
    if (!disks || !disks->conn) { if (cb) cb(disks, object_path, FALSE, "udisks2 is not running", NULL, data); return; }
    DiskOp *op = op_new(disks, object_path, cb, data);
    g_dbus_connection_call(disks->conn, UDISKS_NAME, object_path, UDISKS_FILESYSTEM, "Mount", empty_options(),
                           G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 20000, NULL, call_done, op);
}

void hde_disks_unmount_async(HdeDisks *disks, const char *object_path, HdeDisksDone cb, gpointer data)
{
    if (!disks || !disks->conn) { if (cb) cb(disks, object_path, FALSE, "udisks2 is not running", NULL, data); return; }
    DiskOp *op = op_new(disks, object_path, cb, data);
    g_dbus_connection_call(disks->conn, UDISKS_NAME, object_path, UDISKS_FILESYSTEM, "Unmount", empty_options(),
                           NULL, G_DBUS_CALL_FLAGS_NONE, 20000, NULL, call_done, op);
}

void hde_disks_eject_async(HdeDisks *disks, const char *object_path, HdeDisksDone cb, gpointer data)
{
    if (!disks || !disks->conn) { if (cb) cb(disks, object_path, FALSE, "udisks2 is not running", NULL, data); return; }
    HdeDisk *dsk = hde_disks_find(disks, object_path);
    if (!dsk) { if (cb) cb(disks, object_path, FALSE, "that drive is gone", NULL, data); return; }

    DiskOp *op = op_new(disks, object_path, cb, data);
    g_free(op->drive_path);
    op->drive_path = g_strdup(dsk->drive_path[0] ? dsk->drive_path : object_path);
    /* An eject with the filesystem still on it would be refused: unmount first, then spin the drive down. */
    if (dsk->mounted) {
        op->then_eject = TRUE;
        g_dbus_connection_call(disks->conn, UDISKS_NAME, object_path, UDISKS_FILESYSTEM, "Unmount",
                               empty_options(), NULL, G_DBUS_CALL_FLAGS_NONE, 20000, NULL, call_done, op);
    } else {
        g_dbus_connection_call(disks->conn, UDISKS_NAME, op->drive_path, UDISKS_DRIVE, "Eject", empty_options(),
                               NULL, G_DBUS_CALL_FLAGS_NONE, 20000, NULL, call_done, op);
    }
}

/* ---------------------------------------------------------------- the monitor */

static void on_signal(GDBusConnection *conn, const gchar *sender, const gchar *path, const gchar *iface,
                      const gchar *signal, GVariant *params, gpointer user_data)
{
    (void)conn; (void)sender; (void)path; (void)iface; (void)signal; (void)params;
    queue_rescan(user_data);
}

static void name_appeared(GDBusConnection *conn, const gchar *name, const gchar *owner, gpointer user_data)
{
    (void)conn; (void)name; (void)owner;
    HdeDisks *d = user_data;
    d->said_once = FALSE;           /* it is back: say so again if it goes away another time */
    queue_rescan(d);
}

static void name_vanished(GDBusConnection *conn, const gchar *name, gpointer user_data)
{
    (void)conn; (void)name;
    HdeDisks *d = user_data;
    d->online = FALSE;
    if (d->disks->len == 0) return;
    g_ptr_array_set_size(d->disks, 0);
    if (d->changed) d->changed(d, d->data);
}

static void on_bus(GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void)source;
    HdeDisks *d = user_data;
    GError *err = NULL;
    GDBusConnection *conn = g_bus_get_finish(res, &err);
    if (!conn) {
        if (!d->said_once) {
            d->said_once = TRUE;
            g_printerr("hde-disks: no system message bus (%s): no drive is mounted automatically\n",
                       err ? err->message : "no reason given");
        }
        g_clear_error(&err);
        return;
    }
    d->conn = conn;
    d->sig_added = g_dbus_connection_signal_subscribe(conn, UDISKS_NAME, "org.freedesktop.DBus.ObjectManager",
                                                      "InterfacesAdded", UDISKS_PATH, NULL,
                                                      G_DBUS_SIGNAL_FLAGS_NONE, on_signal, d, NULL);
    d->sig_removed = g_dbus_connection_signal_subscribe(conn, UDISKS_NAME, "org.freedesktop.DBus.ObjectManager",
                                                        "InterfacesRemoved", UDISKS_PATH, NULL,
                                                        G_DBUS_SIGNAL_FLAGS_NONE, on_signal, d, NULL);
    /* A filesystem being mounted or a disc being taken out changes properties, not interfaces. */
    d->sig_props = g_dbus_connection_signal_subscribe(conn, UDISKS_NAME, "org.freedesktop.DBus.Properties",
                                                      "PropertiesChanged", NULL, NULL,
                                                      G_DBUS_SIGNAL_FLAGS_NONE, on_signal, d, NULL);
    d->watch_id = g_bus_watch_name(UDISKS_BUS, UDISKS_NAME, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                   name_appeared, name_vanished, d, NULL);
    rescan(d);
}

HdeDisks *hde_disks_new(HdeDisksChanged changed, gpointer data)
{
    HdeDisks *d = g_new0(HdeDisks, 1);
    d->changed = changed;
    d->data = data;
    d->verbose = g_getenv("HDE_DEBUG") != NULL;
    d->disks = g_ptr_array_new_with_free_func(g_free);
    d->drives = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_bus_get(UDISKS_BUS, NULL, on_bus, d);
    return d;
}

void hde_disks_free(HdeDisks *disks)
{
    if (!disks) return;
    if (disks->debounce_id) g_source_remove(disks->debounce_id);
    if (disks->conn) {
        if (disks->sig_added) g_dbus_connection_signal_unsubscribe(disks->conn, disks->sig_added);
        if (disks->sig_removed) g_dbus_connection_signal_unsubscribe(disks->conn, disks->sig_removed);
        if (disks->sig_props) g_dbus_connection_signal_unsubscribe(disks->conn, disks->sig_props);
        g_object_unref(disks->conn);
    }
    if (disks->watch_id) g_bus_unwatch_name(disks->watch_id);
    g_ptr_array_free(disks->disks, TRUE);
    g_hash_table_destroy(disks->drives);
    g_free(disks);
}

GPtrArray *hde_disks_list(HdeDisks *disks)
{
    return disks ? disks->disks : NULL;
}

gboolean hde_disks_online(HdeDisks *disks)
{
    return disks ? disks->online : FALSE;
}

HdeDisk *hde_disks_find(HdeDisks *disks, const char *object_path)
{
    if (!disks || !object_path || !*object_path) return NULL;
    for (guint i = 0; i < disks->disks->len; ++i) {
        HdeDisk *d = g_ptr_array_index(disks->disks, i);
        if (g_strcmp0(d->object_path, object_path) == 0) return d;
    }
    return NULL;
}
