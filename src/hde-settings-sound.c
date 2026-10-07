/* Hyggshi Settings — Sound page: real volume / mute / microphone / output device
 * (PulseAudio or PipeWire via pactl, plain PipeWire via wpctl, ALSA via amixer). */
#include "hde-settings.h"
#include "hde-commands.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SH_SET_MUTE_FMT \
    "if " HDE_SH_HAVE_PACTL "; then pactl set-sink-mute @DEFAULT_SINK@ %d; " \
    "elif command -v wpctl >/dev/null 2>&1; then wpctl set-mute @DEFAULT_AUDIO_SINK@ %d; " \
    "else amixer -q sset Master %s; fi"
#define SH_SET_MIC_FMT \
    "if " HDE_SH_HAVE_PACTL "; then pactl set-source-mute @DEFAULT_SOURCE@ %d; " \
    "elif command -v wpctl >/dev/null 2>&1; then wpctl set-mute @DEFAULT_AUDIO_SOURCE@ %d; " \
    "else amixer -q sset Capture %s; fi"
#define SH_LIST_SINKS \
    "pactl info 2>/dev/null | sed -n 's/^Default Sink: /DEFAULT\\t/p'; " \
    "pactl list sinks 2>/dev/null | awk -F': ' '/^[ \\t]*Name: /{n=$2} /^[ \\t]*Description: /{print n \"\\t\" $2}'"

static GtkWidget *vol_scale, *vol_label, *mute_sw, *mic_sw, *out_combo, *out_row, *status_row;
static guint vol_timer, poll_timer;
static int pending_vol = -1;
static gboolean loading, user_dragging;

static void set_mute_quiet(GtkWidget *sw, gboolean v);
static gboolean on_mute(GtkSwitch *s, gboolean v, gpointer d);
static gboolean on_mic(GtkSwitch *s, gboolean v, gpointer d);

static void on_vol_get(gboolean ok, int st, const char *out, const char *err, gpointer d)
{
    (void)st; (void)err; (void)d;
    int v = -1, m = 0;
    gboolean have = ok && sscanf(out, "%d %d", &v, &m) >= 1 && v >= 0;
    gtk_widget_set_sensitive(vol_scale, have);
    gtk_widget_set_sensitive(mute_sw, have);
    GtkWidget *dl = g_object_get_data(G_OBJECT(status_row), "hde-description");
    if (dl) gtk_label_set_text(GTK_LABEL(dl), have ? (m ? "Output is muted" : "Output is active")
                                                   : "No sound server or audio device found (install pipewire-pulse or pulseaudio).");
    if (!have || user_dragging || pending_vol >= 0) return;
    loading = TRUE;
    gtk_range_set_value(GTK_RANGE(vol_scale), CLAMP(v, 0, 150));
    set_mute_quiet(mute_sw, m != 0);
    loading = FALSE;
}

static void on_mic_get(gboolean ok, int st, const char *out, const char *err, gpointer d)
{
    (void)st; (void)err; (void)d;
    gtk_widget_set_sensitive(mic_sw, ok);
    if (ok) set_mute_quiet(mic_sw, atoi(out) > 0);
}

static void refresh_volume(void)
{
    run_shell_async(HDE_SH_VOLUME_GET, on_vol_get, NULL);
    run_shell_async(HDE_SH_MIC_GET, on_mic_get, NULL);
}

static gboolean vol_apply(gpointer d)
{
    (void)d;
    vol_timer = 0;
    if (pending_vol < 0) return G_SOURCE_REMOVE;
    int v = pending_vol;
    pending_vol = -1;
    char *cmd = g_strdup_printf(HDE_SH_VOLUME_SET_FMT, v, v, v);
    run_shell_async(cmd, NULL, NULL);
    g_free(cmd);
    return G_SOURCE_REMOVE;
}

static void on_vol_changed(GtkRange *r, gpointer d)
{
    (void)d;
    int v = (int)gtk_range_get_value(r);
    char buf[16];
    g_snprintf(buf, sizeof buf, "%d%%", v);
    gtk_label_set_text(GTK_LABEL(vol_label), buf);
    if (loading) return;
    pending_vol = v;
    if (!vol_timer) vol_timer = g_timeout_add(120, vol_apply, NULL);
}

static gboolean on_scale_press(GtkWidget *w, GdkEvent *e, gpointer d) { (void)w; (void)e; (void)d; user_dragging = TRUE; return FALSE; }
static gboolean on_scale_release(GtkWidget *w, GdkEvent *e, gpointer d) { (void)w; (void)e; (void)d; user_dragging = FALSE; return FALSE; }

static void set_mute_quiet(GtkWidget *sw, gboolean v)
{
    GCallback cb = sw == mic_sw ? G_CALLBACK(on_mic) : G_CALLBACK(on_mute);
    g_signal_handlers_block_by_func(sw, cb, NULL);
    gtk_switch_set_active(GTK_SWITCH(sw), v);
    gtk_switch_set_state(GTK_SWITCH(sw), v);
    g_signal_handlers_unblock_by_func(sw, cb, NULL);
}

static gboolean on_mute(GtkSwitch *s, gboolean v, gpointer d)
{
    (void)s; (void)d;
    char *cmd = g_strdup_printf(SH_SET_MUTE_FMT, v ? 1 : 0, v ? 1 : 0, v ? "mute" : "unmute");
    run_shell_async(cmd, NULL, NULL);
    g_free(cmd);
    settings_status(v ? "Output muted" : "Output unmuted");
    return FALSE;
}

static gboolean on_mic(GtkSwitch *s, gboolean v, gpointer d)
{
    (void)s; (void)d;
    char *cmd = g_strdup_printf(SH_SET_MIC_FMT, v ? 1 : 0, v ? 1 : 0, v ? "nocap" : "cap");
    run_shell_async(cmd, NULL, NULL);
    g_free(cmd);
    settings_status(v ? "Microphone muted" : "Microphone unmuted");
    return FALSE;
}

static void on_sinks(gboolean ok, int st, const char *out, const char *err, gpointer d)
{
    (void)st; (void)err; (void)d;
    loading = TRUE;
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(out_combo));
    char *def = NULL;
    int n = 0;
    gchar **lines = g_strsplit(ok ? out : "", "\n", -1);
    for (int i = 0; lines[i]; i++) {
        char *tab = strchr(lines[i], '\t');
        if (!tab) continue;
        *tab = '\0';
        if (!strcmp(lines[i], "DEFAULT")) { g_free(def); def = g_strdup(tab + 1); continue; }
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(out_combo), lines[i], tab + 1);
        n++;
    }
    g_strfreev(lines);
    if (def) gtk_combo_box_set_active_id(GTK_COMBO_BOX(out_combo), def);
    g_free(def);
    gtk_widget_set_visible(out_row, n > 0);
    loading = FALSE;
}

static void on_sink_set(gboolean ok, int st, const char *out, const char *err, gpointer d)
{
    (void)st; (void)out; (void)d;
    if (!ok) settings_status("Could not change output: %s", err);
    else settings_status("Output device changed");
    refresh_volume();
}

static void on_output(GtkComboBox *c, gpointer d)
{
    (void)d;
    if (loading) return;
    const char *id = gtk_combo_box_get_active_id(c);
    if (!id) return;
    char *q = g_shell_quote(id);
    /* change the default device and also move the streams that are playing to it */
    char *cmd = g_strdup_printf("pactl set-default-sink %s && for i in $(pactl list short sink-inputs 2>/dev/null | cut -f1); "
                                "do pactl move-sink-input \"$i\" %s; done; exit 0", q, q);
    run_shell_async(cmd, on_sink_set, NULL);
    g_free(cmd);
    g_free(q);
}

static void on_mixer(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    const char *cmds[] = { "pavucontrol", "pavucontrol-qt", "helvum", "qpwgraph", "alsamixer-gui", NULL };
    launch_candidates(cmds);
}

static gboolean poll_cb(gpointer d)
{
    (void)d;
    refresh_volume();
    return G_SOURCE_CONTINUE;
}

static void on_map(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    refresh_volume();
    if (have_program("pactl")) run_shell_async(SH_LIST_SINKS, on_sinks, NULL);
    if (!poll_timer) poll_timer = g_timeout_add_seconds(3, poll_cb, NULL);
}

static void on_unmap(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    if (poll_timer) { g_source_remove(poll_timer); poll_timer = 0; }
}

GtkWidget *page_sound_new(void)
{
    GtkWidget *box = page_base();
    gtk_box_pack_start(GTK_BOX(box), section("Output"), FALSE, FALSE, 0);
    status_row = row_box("Sound", "Checking…", NULL);
    gtk_box_pack_start(GTK_BOX(box), status_row, FALSE, FALSE, 0);
    out_combo = gtk_combo_box_text_new();
    gtk_widget_set_size_request(out_combo, 260, -1);
    g_signal_connect(out_combo, "changed", G_CALLBACK(on_output), NULL);
    out_row = row_box("Output device", "Speakers, headphones, HDMI or Bluetooth audio.", out_combo);
    gtk_box_pack_start(GTK_BOX(box), out_row, FALSE, FALSE, 0);
    gtk_widget_set_no_show_all(out_row, TRUE);

    GtkWidget *vb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    vol_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
    gtk_scale_set_draw_value(GTK_SCALE(vol_scale), FALSE);
    gtk_widget_set_size_request(vol_scale, 220, -1);
    vol_label = gtk_label_new("—");
    gtk_label_set_width_chars(GTK_LABEL(vol_label), 5);
    g_signal_connect(vol_scale, "value-changed", G_CALLBACK(on_vol_changed), NULL);
    g_signal_connect(vol_scale, "button-press-event", G_CALLBACK(on_scale_press), NULL);
    g_signal_connect(vol_scale, "button-release-event", G_CALLBACK(on_scale_release), NULL);
    gtk_box_pack_start(GTK_BOX(vb), vol_scale, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vb), vol_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row_box("Volume", "Main speaker or headphone volume.", vb), FALSE, FALSE, 0);
    mute_sw = gtk_switch_new();
    g_signal_connect(mute_sw, "state-set", G_CALLBACK(on_mute), NULL);
    gtk_box_pack_start(GTK_BOX(box), row_box("Mute", "Mute audio output.", mute_sw), FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), section("Input"), FALSE, FALSE, 0);
    mic_sw = gtk_switch_new();
    g_signal_connect(mic_sw, "state-set", G_CALLBACK(on_mic), NULL);
    gtk_box_pack_start(GTK_BOX(box), row_box("Microphone mute", "Mute the default microphone.", mic_sw), FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), section("Sound keys"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), info_label("F1 mutes, F2 lowers and F3 raises the volume; the media keys work too. "
                                                "You can turn the F1–F3 sound keys off in Keyboard & Shortcuts."), FALSE, FALSE, 0);
    GtkWidget *mixer = gtk_button_new_with_label("Open advanced mixer…");
    gtk_widget_set_halign(mixer, GTK_ALIGN_START);
    g_signal_connect(mixer, "clicked", G_CALLBACK(on_mixer), NULL);
    gtk_box_pack_start(GTK_BOX(box), mixer, FALSE, FALSE, 10);

    g_signal_connect(box, "map", G_CALLBACK(on_map), NULL);
    g_signal_connect(box, "unmap", G_CALLBACK(on_unmap), NULL);
    return box;
}
