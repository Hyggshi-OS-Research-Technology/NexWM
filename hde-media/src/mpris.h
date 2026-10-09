/* mpris.h — the small MPRIS 2 D-Bus export used by Hyggshi Media. */
#ifndef HDE_MEDIA_MPRIS_H
#define HDE_MEDIA_MPRIS_H

#include <gio/gio.h>

typedef struct HdeMpris HdeMpris;

typedef struct {
    void (*method_call)(gpointer user_data, const gchar *interface_name, const gchar *method_name,
                        GVariant *parameters, GDBusMethodInvocation *invocation);
    GVariant *(*get_property)(gpointer user_data, const gchar *interface_name, const gchar *property_name,
                              GError **error);
    gboolean (*set_property)(gpointer user_data, const gchar *interface_name, const gchar *property_name,
                             GVariant *value, GError **error);
} HdeMprisHandlers;

/* Export org.mpris.MediaPlayer2.hde-media on the session bus. */
HdeMpris *hde_mpris_new(const HdeMprisHandlers *handlers, gpointer user_data);
void      hde_mpris_free(HdeMpris *mpris);

/* `changed` is an a{sv} variant; this function takes ownership of it. */
void      hde_mpris_properties_changed(HdeMpris *mpris, const gchar *interface_name, GVariant *changed);
void      hde_mpris_seeked(HdeMpris *mpris, gint64 position_us);

#endif /* HDE_MEDIA_MPRIS_H */
