/* hde-profiles.c — power-profiles-daemon, see hde-profiles.h */
#include "hde-profiles.h"
#include "hde-run.h"
#include <gio/gio.h>
#include <string.h>

#define PPD_NAME  "net.hadess.PowerProfiles"
#define PPD_PATH  "/net/hadess/PowerProfiles"
#define PPD_IFACE "net.hadess.PowerProfiles"

const char *hde_power_profile_label(const char *id)
{
    if (!g_strcmp0(id, "power-saver")) return "Power Saver";
    if (!g_strcmp0(id, "performance")) return "Performance";
    if (!g_strcmp0(id, "balanced")) return "Balanced";
    return id ? id : "";
}

const char *hde_power_profile_icon(const char *id)
{
    if (!g_strcmp0(id, "power-saver")) return "power-profile-power-saver-symbolic|battery-good-symbolic|battery-symbolic";
    if (!g_strcmp0(id, "performance")) return "power-profile-performance-symbolic|starred-symbolic|emblem-system-symbolic";
    return "power-profile-balanced-symbolic|emblem-system-symbolic|preferences-system-symbolic";
}

typedef struct { HdeProfilesCb cb; gpointer data; } ProfReq;

static void on_ppd_getall(GObject *src, GAsyncResult *res, gpointer d)
{
    ProfReq *q = d;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
    if (!r) { q->cb(NULL, NULL, q->data); g_free(q); return; }
    GVariant *props = g_variant_get_child_value(r, 0);
    const char *active = NULL;
    g_variant_lookup(props, "ActiveProfile", "&s", &active);
    GPtrArray *list = g_ptr_array_new_with_free_func(g_free);
    GVariant *profs = g_variant_lookup_value(props, "Profiles", G_VARIANT_TYPE("aa{sv}"));
    if (profs) {
        GVariantIter it;
        GVariant *p;
        g_variant_iter_init(&it, profs);
        while ((p = g_variant_iter_next_value(&it))) {
            const char *name = NULL;
            if (g_variant_lookup(p, "Profile", "&s", &name)) g_ptr_array_add(list, g_strdup(name));
            g_variant_unref(p);
        }
        g_variant_unref(profs);
    }
    g_ptr_array_add(list, NULL);
    q->cb(active, (const char *const *)list->pdata, q->data);
    g_ptr_array_free(list, TRUE);
    g_variant_unref(props);
    g_variant_unref(r);
    g_free(q);
}

static gboolean profiles_none(gpointer d)
{
    ProfReq *q = d;
    q->cb(NULL, NULL, q->data);
    g_free(q);
    return G_SOURCE_REMOVE;
}

void hde_power_profiles_get(HdeProfilesCb cb, gpointer data)
{
    ProfReq *q = g_new0(ProfReq, 1);
    q->cb = cb;
    q->data = data;
    GDBusConnection *bus = hde_system_bus();
    if (!bus) {
        g_idle_add(profiles_none, q);
        return;
    }
    g_dbus_connection_call(bus, PPD_NAME, PPD_PATH, "org.freedesktop.DBus.Properties", "GetAll",
                           g_variant_new("(s)", PPD_IFACE), G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, 3000, NULL,
                           on_ppd_getall, q);
}

typedef struct { void (*done)(gboolean, gpointer); gpointer data; char *profile; } SetReq;

static void on_ppd_set(GObject *src, GAsyncResult *res, gpointer d)
{
    SetReq *q = d;
    GError *e = NULL;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &e);
    if (r) g_variant_unref(r);
    else if (g_getenv("HDE_DEBUG")) g_printerr("hde: power mode %s: %s\n", q->profile, e ? e->message : "failed");
    g_clear_error(&e);
    if (q->done) q->done(r != NULL, q->data);
    g_free(q->profile);
    g_free(q);
}

void hde_power_profiles_set(const char *profile, void (*done)(gboolean ok, gpointer data), gpointer data)
{
    GDBusConnection *bus = hde_system_bus();
    if (!bus || !profile) { if (done) done(FALSE, data); return; }
    SetReq *q = g_new0(SetReq, 1);
    q->done = done;
    q->data = data;
    q->profile = g_strdup(profile);
    g_dbus_connection_call(bus, PPD_NAME, PPD_PATH, "org.freedesktop.DBus.Properties", "Set",
                           g_variant_new("(ssv)", PPD_IFACE, "ActiveProfile", g_variant_new_string(profile)), NULL,
                           G_DBUS_CALL_FLAGS_NONE, 5000, NULL, on_ppd_set, q);
}

static void on_props_changed(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                             const char *signal, GVariant *params, gpointer d)
{
    (void)c; (void)sender; (void)path; (void)iface; (void)signal;
    ProfReq *q = d;
    const char *name = NULL;
    GVariant *changed = NULL;
    g_variant_get(params, "(&s@a{sv}@as)", &name, &changed, NULL);
    const char *active = NULL;
    if (!g_strcmp0(name, PPD_IFACE) && changed && g_variant_lookup(changed, "ActiveProfile", "&s", &active))
        q->cb(active, NULL, q->data);
    if (changed) g_variant_unref(changed);
}

guint hde_power_profiles_watch(HdeProfilesCb cb, gpointer data)
{
    GDBusConnection *bus = hde_system_bus();
    if (!bus) return 0;
    ProfReq *q = g_new0(ProfReq, 1);
    q->cb = cb;
    q->data = data;
    return g_dbus_connection_signal_subscribe(bus, NULL, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                                              PPD_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_props_changed, q, g_free);
}

void hde_power_profiles_unwatch(guint id)
{
    GDBusConnection *bus = hde_system_bus();
    if (bus && id) g_dbus_connection_signal_unsubscribe(bus, id);
}
