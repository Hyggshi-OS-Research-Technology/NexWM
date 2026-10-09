/* mpris.c — export the player window on the standard MPRIS 2 session-bus interface. */
#include "mpris.h"

#include <string.h>

#define MPRIS_BUS_NAME "org.mpris.MediaPlayer2.hde-media"
#define MPRIS_OBJECT_PATH "/org/mpris/MediaPlayer2"
#define MPRIS_ROOT_IFACE "org.mpris.MediaPlayer2"
#define MPRIS_PLAYER_IFACE "org.mpris.MediaPlayer2.Player"
#define DBUS_PROPERTIES_IFACE "org.freedesktop.DBus.Properties"

static const gchar mpris_xml[] =
    "<node>"
    "  <interface name='org.mpris.MediaPlayer2'>"
    "    <method name='Raise'/>"
    "    <method name='Quit'/>"
    "    <property name='CanQuit' type='b' access='read'/>"
    "    <property name='CanRaise' type='b' access='read'/>"
    "    <property name='HasTrackList' type='b' access='read'/>"
    "    <property name='Identity' type='s' access='read'/>"
    "    <property name='DesktopEntry' type='s' access='read'/>"
    "    <property name='SupportedUriSchemes' type='as' access='read'/>"
    "    <property name='SupportedMimeTypes' type='as' access='read'/>"
    "  </interface>"
    "  <interface name='org.mpris.MediaPlayer2.Player'>"
    "    <method name='Next'/>"
    "    <method name='Previous'/>"
    "    <method name='Pause'/>"
    "    <method name='PlayPause'/>"
    "    <method name='Stop'/>"
    "    <method name='Play'/>"
    "    <method name='Seek'><arg name='Offset' type='x' direction='in'/></method>"
    "    <method name='SetPosition'><arg name='TrackId' type='o' direction='in'/>"
    "      <arg name='Position' type='x' direction='in'/></method>"
    "    <method name='OpenUri'><arg name='Uri' type='s' direction='in'/></method>"
    "    <property name='PlaybackStatus' type='s' access='read'/>"
    "    <property name='LoopStatus' type='s' access='readwrite'/>"
    "    <property name='Rate' type='d' access='readwrite'/>"
    "    <property name='Shuffle' type='b' access='readwrite'/>"
    "    <property name='Metadata' type='a{sv}' access='read'/>"
    "    <property name='Volume' type='d' access='readwrite'/>"
    "    <property name='Position' type='x' access='read'>"
    "      <annotation name='org.freedesktop.DBus.Property.EmitsChangedSignal' value='false'/>"
    "    </property>"
    "    <property name='MinimumRate' type='d' access='read'/>"
    "    <property name='MaximumRate' type='d' access='read'/>"
    "    <property name='CanGoNext' type='b' access='read'/>"
    "    <property name='CanGoPrevious' type='b' access='read'/>"
    "    <property name='CanPlay' type='b' access='read'/>"
    "    <property name='CanPause' type='b' access='read'/>"
    "    <property name='CanSeek' type='b' access='read'/>"
    "    <property name='CanControl' type='b' access='read'/>"
    "    <signal name='Seeked'><arg name='Position' type='x'/></signal>"
    "  </interface>"
    "</node>";

struct HdeMpris {
    guint refs;
    const HdeMprisHandlers *handlers;
    gpointer user_data;
    GDBusNodeInfo *node_info;
    GDBusConnection *connection;
    guint owner_id;
    guint root_registration;
    guint player_registration;
    gboolean closing;
};

static void mpris_unref(HdeMpris *mpris)
{
    if (mpris && --mpris->refs == 0) g_free(mpris);
}

static void mpris_bus_data_free(gpointer user_data)
{
    mpris_unref(user_data);
}

static void mpris_method_call(GDBusConnection *connection, const gchar *sender, const gchar *object_path,
                              const gchar *interface_name, const gchar *method_name, GVariant *parameters,
                              GDBusMethodInvocation *invocation, gpointer user_data)
{
    HdeMpris *mpris = user_data;
    (void)connection;
    (void)sender;
    (void)object_path;
    if (mpris->closing || !mpris->handlers || !mpris->handlers->method_call) {
        g_dbus_method_invocation_return_error_literal(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
                                                       "The media player is no longer available");
        return;
    }
    mpris->handlers->method_call(mpris->user_data, interface_name, method_name, parameters, invocation);
}

static GVariant *mpris_get_property(GDBusConnection *connection, const gchar *sender, const gchar *object_path,
                                    const gchar *interface_name, const gchar *property_name, GError **error,
                                    gpointer user_data)
{
    HdeMpris *mpris = user_data;
    (void)connection;
    (void)sender;
    (void)object_path;
    if (mpris->closing || !mpris->handlers || !mpris->handlers->get_property) {
        g_set_error_literal(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY, "The media player is no longer available");
        return NULL;
    }
    return mpris->handlers->get_property(mpris->user_data, interface_name, property_name, error);
}

static gboolean mpris_set_property(GDBusConnection *connection, const gchar *sender, const gchar *object_path,
                                   const gchar *interface_name, const gchar *property_name, GVariant *value,
                                   GError **error, gpointer user_data)
{
    HdeMpris *mpris = user_data;
    (void)connection;
    (void)sender;
    (void)object_path;
    if (mpris->closing || !mpris->handlers || !mpris->handlers->set_property) {
        g_set_error_literal(error, G_DBUS_ERROR, G_DBUS_ERROR_PROPERTY_READ_ONLY, "The media player is no longer available");
        return FALSE;
    }
    return mpris->handlers->set_property(mpris->user_data, interface_name, property_name, value, error);
}

static const GDBusInterfaceVTable mpris_vtable = {
    .method_call = mpris_method_call,
    .get_property = mpris_get_property,
    .set_property = mpris_set_property
};

static void mpris_bus_acquired(GDBusConnection *connection, const gchar *name, gpointer user_data)
{
    HdeMpris *mpris = user_data;
    (void)name;
    if (mpris->closing) return;

    g_set_object(&mpris->connection, connection);
    GError *error = NULL;
    GDBusInterfaceInfo *root = g_dbus_node_info_lookup_interface(mpris->node_info, MPRIS_ROOT_IFACE);
    GDBusInterfaceInfo *player = g_dbus_node_info_lookup_interface(mpris->node_info, MPRIS_PLAYER_IFACE);
    mpris->root_registration = g_dbus_connection_register_object(connection, MPRIS_OBJECT_PATH, root,
                                                                   &mpris_vtable, mpris, NULL, &error);
    if (!mpris->root_registration) {
        g_warning("hde-media: cannot export the MPRIS interface: %s", error ? error->message : "unknown error");
        g_clear_error(&error);
        return;
    }
    mpris->player_registration = g_dbus_connection_register_object(connection, MPRIS_OBJECT_PATH, player,
                                                                     &mpris_vtable, mpris, NULL, &error);
    if (!mpris->player_registration) {
        g_warning("hde-media: cannot export the MPRIS player interface: %s", error ? error->message : "unknown error");
        g_clear_error(&error);
        g_dbus_connection_unregister_object(connection, mpris->root_registration);
        mpris->root_registration = 0;
    }
}

static void mpris_name_acquired(GDBusConnection *connection, const gchar *name, gpointer user_data)
{
    HdeMpris *mpris = user_data;
    (void)connection;
    (void)name;
    if (!mpris->closing) g_message("hde-media: MPRIS controls are available on the session bus");
}

static void mpris_name_lost(GDBusConnection *connection, const gchar *name, gpointer user_data)
{
    HdeMpris *mpris = user_data;
    (void)connection;
    (void)name;
    if (!mpris->closing) g_debug("hde-media: the MPRIS session bus is unavailable");
}

HdeMpris *hde_mpris_new(const HdeMprisHandlers *handlers, gpointer user_data)
{
    if (!handlers) return NULL;

    HdeMpris *mpris = g_new0(HdeMpris, 1);
    mpris->refs = 1;                         /* the player object owns one reference */
    mpris->handlers = handlers;
    mpris->user_data = user_data;
    GError *error = NULL;
    mpris->node_info = g_dbus_node_info_new_for_xml(mpris_xml, &error);
    if (!mpris->node_info) {
        g_warning("hde-media: cannot prepare MPRIS: %s", error ? error->message : "unknown error");
        g_clear_error(&error);
        g_free(mpris);
        return NULL;
    }
    mpris->refs++;                              /* callbacks may outlive the player object's reference */
    mpris->owner_id = g_bus_own_name(G_BUS_TYPE_SESSION, MPRIS_BUS_NAME, G_BUS_NAME_OWNER_FLAGS_NONE,
                                     mpris_bus_acquired, mpris_name_acquired, mpris_name_lost, mpris,
                                     mpris_bus_data_free);
    if (!mpris->owner_id) {
        mpris_unref(mpris);                      /* no callback owns the extra reference */
        g_dbus_node_info_unref(mpris->node_info);
        g_free(mpris);
        return NULL;
    }
    return mpris;
}

void hde_mpris_free(HdeMpris *mpris)
{
    if (!mpris) return;
    mpris->closing = TRUE;
    if (mpris->owner_id) {
        g_bus_unown_name(mpris->owner_id);
        mpris->owner_id = 0;
    }
    if (mpris->connection) {
        if (mpris->root_registration) g_dbus_connection_unregister_object(mpris->connection, mpris->root_registration);
        if (mpris->player_registration) g_dbus_connection_unregister_object(mpris->connection, mpris->player_registration);
        g_clear_object(&mpris->connection);
    }
    g_clear_pointer(&mpris->node_info, g_dbus_node_info_unref);
    mpris_unref(mpris);                     /* the bus callback reference is released after unowning completes */
}

void hde_mpris_properties_changed(HdeMpris *mpris, const gchar *interface_name, GVariant *changed)
{
    if (!changed) return;
    GVariant *properties = g_variant_take_ref(changed); /* this API consumes the caller's reference */
    if (!mpris || mpris->closing || !mpris->connection) {
        g_variant_unref(properties);
        return;
    }
    const gchar *nothing[] = { NULL };
    GVariant *parameters = g_variant_new("(s@a{sv}@as)", interface_name, properties,
                                         g_variant_new_strv(nothing, 0));
    g_dbus_connection_emit_signal(mpris->connection, NULL, MPRIS_OBJECT_PATH, DBUS_PROPERTIES_IFACE,
                                  "PropertiesChanged", parameters, NULL);
}

void hde_mpris_seeked(HdeMpris *mpris, gint64 position_us)
{
    if (!mpris || mpris->closing || !mpris->connection) return;
    g_dbus_connection_emit_signal(mpris->connection, NULL, MPRIS_OBJECT_PATH, MPRIS_PLAYER_IFACE, "Seeked",
                                  g_variant_new("(x)", position_us), NULL);
}
