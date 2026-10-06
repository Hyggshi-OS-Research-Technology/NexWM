/* hde-settings-peripherals.c — GNOME's (and Cinnamon's) touchpad and mouse settings follow HDE's.
 *
 * Mutter and Muffin, two of the window managers HDE runs, apply org.gnome.desktop.peripherals.* (Muffin:
 * org.cinnamon.desktop.peripherals.*) to the devices themselves. When those say something else than Settings > Input,
 * the window manager and hde-xsettings keep changing each other's values; GNOME / Cinnamon programs show the wrong
 * thing too. So every time HDE applies its input settings (a change in Settings, the touchpad window, every login),
 * the keys that differ are written: natural-scroll, tap-to-click, speed (-1..1) and accel-profile. Nothing happens
 * when the schemas are not installed, or when GSettings would only keep the values in memory (no dconf).
 */
#define G_SETTINGS_ENABLE_BACKEND
#include <gio/gio.h>
#include <gio/gsettingsbackend.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "hde-settings.h"
#include "hde-input.h"

static int put_bool(GSettings *gs, GSettingsSchema *sc, const char *id, const char *key, gboolean v)
{
    if (!g_settings_schema_has_key(sc, key)) return 0;
    GSettingsSchemaKey *k = g_settings_schema_get_key(sc, key);
    gboolean ok = g_variant_type_equal(g_settings_schema_key_get_value_type(k), G_VARIANT_TYPE_BOOLEAN);
    g_settings_schema_key_unref(k);
    if (!ok || g_settings_get_boolean(gs, key) == v) return 0;
    g_settings_set_boolean(gs, key, v);
    fprintf(stderr, "hde-settings: gsettings %s %s %s (as in HDE)\n", id, key, v ? "true" : "false");
    return 1;
}

static int put_double(GSettings *gs, GSettingsSchema *sc, const char *id, const char *key, double v)
{
    if (!g_settings_schema_has_key(sc, key)) return 0;
    GSettingsSchemaKey *k = g_settings_schema_get_key(sc, key);
    gboolean ok = g_variant_type_equal(g_settings_schema_key_get_value_type(k), G_VARIANT_TYPE_DOUBLE) &&
                  g_settings_schema_key_range_check(k, g_variant_new_double(v));
    g_settings_schema_key_unref(k);
    if (!ok || fabs(g_settings_get_double(gs, key) - v) < 0.005) return 0;
    g_settings_set_double(gs, key, v);
    fprintf(stderr, "hde-settings: gsettings %s %s %.2f (as in HDE)\n", id, key, v);
    return 1;
}

static int put_choice(GSettings *gs, GSettingsSchema *sc, const char *id, const char *key, const char *v)
{
    if (!g_settings_schema_has_key(sc, key)) return 0;
    GSettingsSchemaKey *k = g_settings_schema_get_key(sc, key);
    GVariant *val = g_variant_ref_sink(g_variant_new_string(v));
    gboolean ok = g_variant_type_equal(g_settings_schema_key_get_value_type(k), G_VARIANT_TYPE_STRING) &&
                  g_settings_schema_key_range_check(k, val);
    g_variant_unref(val);
    g_settings_schema_key_unref(k);
    if (!ok) return 0;
    char *cur = g_settings_get_string(gs, key);
    gboolean same = !g_strcmp0(cur, v);
    g_free(cur);
    if (same) return 0;
    g_settings_set_string(gs, key, v);
    fprintf(stderr, "hde-settings: gsettings %s %s '%s' (as in HDE)\n", id, key, v);
    return 1;
}

static int sync_schema(GSettingsSchemaSource *src, const char *base, const HdeInputPrefs *p)
{
    int n = 0;
    char *id = g_strdup_printf("%s.touchpad", base);
    GSettingsSchema *sc = g_settings_schema_source_lookup(src, id, TRUE);
    if (sc) {
        GSettings *gs = g_settings_new_full(sc, NULL, NULL);
        n += put_bool(gs, sc, id, "natural-scroll", p->touchpad_natural);
        n += put_bool(gs, sc, id, "tap-to-click", p->tap_to_click);
        if (p->has_speed) n += put_double(gs, sc, id, "speed", CLAMP(p->speed * 2.0 - 1.0, -1.0, 1.0));
        g_object_unref(gs);
        g_settings_schema_unref(sc);
    }
    g_free(id);
    id = g_strdup_printf("%s.mouse", base);
    sc = g_settings_schema_source_lookup(src, id, TRUE);
    if (sc) {
        GSettings *gs = g_settings_new_full(sc, NULL, NULL);
        if (p->has_mouse_natural) n += put_bool(gs, sc, id, "natural-scroll", p->mouse_natural);
        if (p->has_speed) n += put_double(gs, sc, id, "speed", CLAMP(p->speed * 2.0 - 1.0, -1.0, 1.0));
        if (p->has_acceleration) n += put_choice(gs, sc, id, "accel-profile", p->acceleration ? "adaptive" : "flat");
        g_object_unref(gs);
        g_settings_schema_unref(sc);
    }
    g_free(id);
    return n;
}

void gsettings_sync_input(void)
{
    GSettingsSchemaSource *src = g_settings_schema_source_get_default();
    if (!src) return;
    GSettingsSchema *probe = g_settings_schema_source_lookup(src, "org.gnome.desktop.peripherals.touchpad", TRUE);
    if (!probe) probe = g_settings_schema_source_lookup(src, "org.cinnamon.desktop.peripherals.touchpad", TRUE);
    if (!probe) return;
    g_settings_schema_unref(probe);
    GSettingsBackend *be = g_settings_backend_get_default();
    gboolean memory = be && !strcmp(G_OBJECT_TYPE_NAME(be), "GMemorySettingsBackend");
    if (be) g_object_unref(be);
    if (memory) return;                 /* no dconf: the values would be gone at once */
    HdeInputPrefs p;
    hde_input_prefs_load(&p);
    int n = sync_schema(src, "org.gnome.desktop.peripherals", &p) + sync_schema(src, "org.cinnamon.desktop.peripherals", &p);
    if (n) g_settings_sync();
}
