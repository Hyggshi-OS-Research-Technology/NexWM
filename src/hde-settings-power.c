/* hde-settings-power.c — Settings > Power: the battery (charge, time left, health), the power mode
 * (power-profiles-daemon), the battery saver and the low-battery warnings (both run by the panel, src/hde-powersave.c),
 * the screen timeout, and automatic suspend (left to a power manager).
 */
#include "hde-settings.h"
#include "hde-power.h"
#include "hde-profiles.h"
#include "hde-run.h"
#include <stdio.h>
#include <string.h>

static const int saver_levels[] = { 10, 15, 20, 30, 50, 100 };
#define N_LEVELS ((int)G_N_ELEMENTS(saver_levels))

static struct {
    GtkWidget *bat_section, *bat_card, *bat_title, *bat_desc, *health_desc, *model_desc, *health_row, *model_row;
    GtkWidget *mode_combo, *mode_desc;
    GtkWidget *saver_section, *saver_box, *saver_switch, *level_combo, *dim_switch, *warn_switch, *saver_state;
    guint timer, watch;
    gboolean mode_guard;
    char **profiles;
} pw;

/* ---------------------------------------------------------------- the battery */
/* a row of a card: GtkListBox wraps it in a GtkListBoxRow, which must be hidden too (else an empty row stays) */
static void row_visible(GtkWidget *row, gboolean on)
{
    GtkWidget *p = gtk_widget_get_parent(row);
    gtk_widget_set_visible(p && GTK_IS_LIST_BOX_ROW(p) ? p : row, on);
}

static void power_refresh(void)
{
    if (!pw.bat_card) return;
    HdePower p;
    hde_power_read(&p);
    gboolean have = p.percent >= 0 && p.n > 0;
    gtk_widget_set_visible(pw.bat_section, have);
    gtk_widget_set_visible(pw.bat_card, have);
    gtk_widget_set_visible(pw.saver_section, have);
    gtk_widget_set_visible(pw.saver_box, have);
    if (!have) return;
    char *title = g_strdup_printf("%d%% · %s", p.percent, hde_bat_state_text(p.state, p.ac));
    gtk_label_set_text(GTK_LABEL(pw.bat_title), title);
    g_free(title);
    GString *d = g_string_new(NULL);
    if (p.minutes >= 0) {
        char *t = hde_power_time_text(p.minutes);
        g_string_append_printf(d, p.state == HDE_BAT_CHARGING ? "%s until full" : "%s left", t);
        g_free(t);
    }
    if (p.power > 0.05 && (p.state == HDE_BAT_CHARGING || p.state == HDE_BAT_DISCHARGING))
        g_string_append_printf(d, "%s%.1f W %s", d->len ? " · " : "", p.power,
                               p.state == HDE_BAT_CHARGING ? "charging" : "used now");
    if (!d->len) g_string_append(d, p.ac == 1 ? "Plugged in" : "On battery");
    gtk_label_set_text(GTK_LABEL(pw.bat_desc), d->str);
    g_string_truncate(d, 0);
    const HdeBattery *b = &p.bat[0];
    if (p.health >= 0 && p.energy_full > 0 && p.energy_full_design > 0)
        g_string_append_printf(d, "%d%% of its capacity when new (%.1f of %.1f Wh)", p.health, p.energy_full,
                               p.energy_full_design);
    else if (p.health >= 0) g_string_append_printf(d, "%d%% of its capacity when new", p.health);
    if (b->cycles >= 0) g_string_append_printf(d, "%s%d charge cycles", d->len ? " · " : "", b->cycles);
    row_visible(pw.health_row, d->len > 0);
    gtk_label_set_text(GTK_LABEL(pw.health_desc), d->str);
    g_string_truncate(d, 0);
    for (int i = 0; i < p.n; i++) {
        const HdeBattery *x = &p.bat[i];
        if (i) g_string_append(d, "\n");
        g_string_append(d, x->name);
        if (x->vendor[0] || x->model[0]) g_string_append_printf(d, " · %s%s%s", x->vendor, x->vendor[0] && x->model[0] ? " " : "", x->model);
        if (x->technology[0]) g_string_append_printf(d, " · %s", x->technology);
        if (x->charge_limit > 0 && x->charge_limit < 100)
            g_string_append_printf(d, " · charges up to %d%% (charge limit of the firmware)", x->charge_limit);
    }
    gtk_label_set_text(GTK_LABEL(pw.model_desc), d->str);
    g_string_free(d, TRUE);

    /* what the panel's battery saver does now (the same decision as src/hde-powersave.c) */
    gboolean on = cfg_get_bool("battery_saver", FALSE);
    int level = CLAMP(cfg_get_int("battery_saver_level", 20), 5, 100);
    char *st;
    if (!on) st = g_strdup("The battery saver is off.");
    else if (hde_power_saver_wanted(&p, TRUE, level, FALSE))
        st = g_strdup_printf("The battery saver is on now (%d%% left).", p.percent);
    else if (!hde_power_on_battery(&p)) st = g_strdup("Plugged in: the battery saver waits until the computer runs on its battery.");
    else st = g_strdup_printf("It turns on at %d%% (%d%% left now).", level, p.percent);
    gtk_label_set_text(GTK_LABEL(pw.saver_state), st);
    g_free(st);
}

static gboolean power_tick(gpointer d) { (void)d; power_refresh(); return G_SOURCE_CONTINUE; }

static void on_battery_details(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    char *panel = hde_program_path("hde-panel");
    const char *argv[] = { panel ? panel : "hde-panel", "--battery", NULL };
    hde_run(argv, NULL, 5, NULL, NULL);
    g_free(panel);
}

/* ---------------------------------------------------------------- the power mode */
static void mode_show(const char *active)
{
    if (!pw.mode_combo || !active) return;
    pw.mode_guard = TRUE;
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(pw.mode_combo), active);
    pw.mode_guard = FALSE;
}

static void on_profiles(const char *active, const char *const *list, gpointer d)
{
    (void)d;
    if (!pw.mode_combo) return;
    if (!active) {
        gtk_widget_set_sensitive(pw.mode_combo, FALSE);
        gtk_label_set_text(GTK_LABEL(pw.mode_desc), "Needs power-profiles-daemon, which this system does not run: "
                           "sudo apt install power-profiles-daemon");
        return;
    }
    pw.mode_guard = TRUE;
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(pw.mode_combo));
    static const char *const order[] = { "power-saver", "balanced", "performance" };
    for (int i = 0; i < 3; i++)
        if (list && g_strv_contains(list, order[i]))
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(pw.mode_combo), order[i], hde_power_profile_label(order[i]));
    pw.mode_guard = FALSE;
    mode_show(active);
    gtk_widget_set_sensitive(pw.mode_combo, TRUE);
    gboolean perf = list && g_strv_contains(list, "performance");
    gtk_label_set_text(GTK_LABEL(pw.mode_desc), perf ? "Power Saver: the battery lasts longer, a bit slower. Performance: "
                       "faster, more power and heat. Also in the Control Center and the battery panel."
                       : "Power Saver: the battery lasts longer, a bit slower (this computer has no Performance mode). "
                         "Also in the Control Center and the battery panel.");
    char *l = list ? g_strjoinv(" ", (char **)list) : g_strdup("");
    fprintf(stderr, "hde-settings: power mode %s (%s)\n", active, l);
    g_free(l);
}

static void on_profile_changed(const char *active, const char *const *list, gpointer d)
{
    (void)list; (void)d;
    mode_show(active);
}

static void on_mode(GtkComboBox *c, gpointer d)
{
    (void)d;
    if (pw.mode_guard) return;
    const char *id = gtk_combo_box_get_active_id(c);
    if (!id) return;
    hde_power_profiles_set(id, NULL, NULL);
    settings_status("Power mode: %s", hde_power_profile_label(id));
    fprintf(stderr, "hde-settings: power mode -> %s\n", id);
}

/* ---------------------------------------------------------------- battery saver */
static void saver_sensitivity(void)
{
    gboolean on = gtk_switch_get_active(GTK_SWITCH(pw.saver_switch));
    gtk_widget_set_sensitive(pw.level_combo, on);
    gtk_widget_set_sensitive(pw.dim_switch, on);
}

static gboolean on_saver(GtkSwitch *s, gboolean on, gpointer d)
{
    (void)s; (void)d;
    cfg_set_bool("battery_saver", on);
    gtk_switch_set_active(s, on);           /* (state-set comes before :active changes) */
    saver_sensitivity();
    power_refresh();
    settings_status(on ? "Battery saver on: it turns on by itself when the battery runs low" : "Battery saver off");
    return FALSE;
}

static void on_level(GtkComboBox *c, gpointer d)
{
    (void)d;
    int i = gtk_combo_box_get_active(c);
    if (i < 0 || i >= N_LEVELS) return;
    cfg_set_int("battery_saver_level", saver_levels[i]);
    power_refresh();
}

static gboolean on_bool(GtkSwitch *s, gboolean on, gpointer key)
{
    (void)s;
    cfg_set_bool(key, on);
    return FALSE;
}

/* ---------------------------------------------------------------- screen timeout, suspend */
static void on_screen_timeout(GtkComboBox *c, gpointer d)
{
    (void)d;
    cfg_set_int("screen_timeout", gtk_combo_box_get_active(c));
    apply_power_settings();
}

static const char *const power_managers[][2] = {
    { "xfce4-power-manager-settings", "xfce4-power-manager" },
    { "mate-power-preferences", "mate-power-manager" },
    { "lxqt-config-powermanagement", "lxqt-powermanagement" },
};

static void on_power_manager(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    for (guint i = 0; i < G_N_ELEMENTS(power_managers); i++)
        if (have_program(power_managers[i][0])) {
            const char *cmd[] = { power_managers[i][0], NULL };
            launch_candidates(cmd);
            return;
        }
}

/* ---------------------------------------------------------------- the page */
static void on_map(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    power_refresh();
    hde_power_profiles_get(on_profiles, NULL);
    if (!pw.timer) pw.timer = g_timeout_add_seconds(5, power_tick, NULL);
}

static void on_unmap(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    if (pw.timer) g_source_remove(pw.timer);
    pw.timer = 0;
}

static void on_destroy(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    on_unmap(NULL, NULL);
    hde_power_profiles_unwatch(pw.watch);
    memset(&pw, 0, sizeof pw);
}

static GtkWidget *card_row(const char *title, GtkWidget **title_l, GtkWidget **desc_l, GtkWidget *control)
{
    GtkWidget *row = row_box(title, " ", control);
    if (desc_l) *desc_l = g_object_get_data(G_OBJECT(row), "hde-description");
    if (title_l) {
        GList *ch = gtk_container_get_children(GTK_CONTAINER(row));
        GList *labels = ch ? gtk_container_get_children(GTK_CONTAINER(ch->data)) : NULL;
        *title_l = labels ? labels->data : NULL;
        g_list_free(labels);
        g_list_free(ch);
    }
    return row;
}

GtkWidget *page_power_new(void)
{
    GtkWidget *box = page_base();

    pw.bat_section = section("Battery");
    gtk_box_pack_start(GTK_BOX(box), pw.bat_section, FALSE, FALSE, 0);
    pw.bat_card = card_new();
    GtkWidget *details = gtk_button_new_with_label("Battery details…");
    gtk_widget_set_valign(details, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(details, "The battery panel of the taskbar: a chart of the charge, the draw, the voltage, "
                                         "the batteries of wireless devices");
    g_signal_connect(details, "clicked", G_CALLBACK(on_battery_details), NULL);
    debug_geometry_watch(details, "power-battery-details");
    gtk_container_add(GTK_CONTAINER(pw.bat_card), card_row("Battery", &pw.bat_title, &pw.bat_desc, details));
    pw.health_row = card_row("Health", NULL, &pw.health_desc, NULL);
    gtk_container_add(GTK_CONTAINER(pw.bat_card), pw.health_row);
    pw.model_row = card_row("Battery model", NULL, &pw.model_desc, NULL);
    gtk_container_add(GTK_CONTAINER(pw.bat_card), pw.model_row);
    gtk_style_context_add_class(gtk_widget_get_style_context(pw.bat_title), "tp-heading");
    gtk_box_pack_start(GTK_BOX(box), pw.bat_card, FALSE, FALSE, 0);
    gtk_widget_show_all(pw.bat_card);           /* (show_all does nothing to a no-show-all widget: first) */
    gtk_widget_set_no_show_all(pw.bat_section, TRUE);
    gtk_widget_set_no_show_all(pw.bat_card, TRUE);

    gtk_box_pack_start(GTK_BOX(box), section("Power mode"), FALSE, FALSE, 0);
    pw.mode_combo = gtk_combo_box_text_new();
    gtk_widget_set_sensitive(pw.mode_combo, FALSE);
    g_signal_connect(pw.mode_combo, "changed", G_CALLBACK(on_mode), NULL);
    debug_geometry_watch(pw.mode_combo, "power-mode");
    GtkWidget *mrow = row_box("Power mode", "…", pw.mode_combo);
    pw.mode_desc = g_object_get_data(G_OBJECT(mrow), "hde-description");
    gtk_box_pack_start(GTK_BOX(box), mrow, FALSE, FALSE, 0);
    pw.watch = hde_power_profiles_watch(on_profile_changed, NULL);

    pw.saver_section = section("Battery saver");
    gtk_box_pack_start(GTK_BOX(box), pw.saver_section, FALSE, FALSE, 0);
    pw.saver_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    pw.saver_switch = gtk_switch_new();
    gtk_widget_set_valign(pw.saver_switch, GTK_ALIGN_CENTER);
    gtk_switch_set_active(GTK_SWITCH(pw.saver_switch), cfg_get_bool("battery_saver", FALSE));
    g_signal_connect(pw.saver_switch, "state-set", G_CALLBACK(on_saver), NULL);
    debug_geometry_watch(pw.saver_switch, "power-saver-switch");
    gtk_box_pack_start(GTK_BOX(pw.saver_box), row_box("Battery saver", "When the battery runs low: the Power Saver mode, a "
                       "dimmer screen and fewer updates in the background. It turns off by itself when you plug in the "
                       "computer.", pw.saver_switch), FALSE, FALSE, 0);
    pw.level_combo = gtk_combo_box_text_new();
    int level = cfg_get_int("battery_saver_level", 20), active = 2;
    for (int i = 0; i < N_LEVELS; i++) {
        char *t = saver_levels[i] >= 100 ? g_strdup("Always on battery") : g_strdup_printf("%d%%", saver_levels[i]);
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(pw.level_combo), t);
        g_free(t);
        if (saver_levels[i] == level) active = i;
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(pw.level_combo), active);
    g_signal_connect(pw.level_combo, "changed", G_CALLBACK(on_level), NULL);
    gtk_box_pack_start(GTK_BOX(pw.saver_box), row_box("Turn on at", "The charge left when the battery saver turns on.",
                                                      pw.level_combo), FALSE, FALSE, 0);
    pw.dim_switch = gtk_switch_new();
    gtk_widget_set_valign(pw.dim_switch, GTK_ALIGN_CENTER);
    gtk_switch_set_active(GTK_SWITCH(pw.dim_switch), cfg_get_bool("battery_saver_dim", TRUE));
    g_signal_connect(pw.dim_switch, "state-set", G_CALLBACK(on_bool), (gpointer)"battery_saver_dim");
    gtk_box_pack_start(GTK_BOX(pw.saver_box), row_box("Dim the screen", "To 70% of its brightness while the battery saver "
                                                      "is on; back when it turns off.", pw.dim_switch), FALSE, FALSE, 0);
    pw.saver_state = info_label("");
    gtk_box_pack_start(GTK_BOX(pw.saver_box), pw.saver_state, FALSE, FALSE, 0);
    pw.warn_switch = gtk_switch_new();
    gtk_widget_set_valign(pw.warn_switch, GTK_ALIGN_CENTER);
    gtk_switch_set_active(GTK_SWITCH(pw.warn_switch), cfg_get_bool("battery_warnings", TRUE));
    g_signal_connect(pw.warn_switch, "state-set", G_CALLBACK(on_bool), (gpointer)"battery_warnings");
    gtk_box_pack_start(GTK_BOX(pw.saver_box), row_box("Low battery warnings", "A notification at 10% left, and one that "
                                                      "stays at 5%.", pw.warn_switch), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), pw.saver_box, FALSE, FALSE, 0);
    gtk_widget_show_all(pw.saver_box);
    gtk_widget_set_no_show_all(pw.saver_section, TRUE);
    gtk_widget_set_no_show_all(pw.saver_box, TRUE);
    saver_sensitivity();

    gtk_box_pack_start(GTK_BOX(box), section("Screen and sleep"), FALSE, FALSE, 0);
    GtkWidget *screen = gtk_combo_box_text_new();
    static const char *const times[] = { "Never", "5 minutes", "10 minutes", "15 minutes", "30 minutes", "1 hour" };
    for (int i = 0; i < 6; i++) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(screen), times[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(screen), CLAMP(cfg_get_int("screen_timeout", 2), 0, 5));
    g_signal_connect(screen, "changed", G_CALLBACK(on_screen_timeout), NULL);
    gtk_box_pack_start(GTK_BOX(box), row_box("Screen timeout", "Turn off the display after this long without using the "
                                             "mouse or the keyboard.", screen), FALSE, FALSE, 0);
    const char *pm = NULL;
    for (guint i = 0; i < G_N_ELEMENTS(power_managers) && !pm; i++)
        if (have_program(power_managers[i][0])) pm = power_managers[i][1];
    GtkWidget *pm_btn = NULL;
    char *sdesc;
    if (pm) {
        pm_btn = gtk_button_new_with_label("Open…");
        gtk_widget_set_valign(pm_btn, GTK_ALIGN_CENTER);
        g_signal_connect(pm_btn, "clicked", G_CALLBACK(on_power_manager), NULL);
        sdesc = g_strdup_printf("Set in %s, which suspends the computer after a while without use (and handles the lid "
                                "and the power button).", pm);
    } else {
        sdesc = g_strdup("HDE does not suspend the computer by itself after a while without use: install a power manager "
                         "for that, e.g. sudo apt install xfce4-power-manager. Closing the lid and the power button work "
                         "without one (systemd-logind), and the Power menu suspends at any time.");
    }
    gtk_box_pack_start(GTK_BOX(box), row_box("Automatic suspend", sdesc, pm_btn), FALSE, FALSE, 0);
    g_free(sdesc);

    g_signal_connect(box, "map", G_CALLBACK(on_map), NULL);
    g_signal_connect(box, "unmap", G_CALLBACK(on_unmap), NULL);
    g_signal_connect(box, "destroy", G_CALLBACK(on_destroy), NULL);
    power_refresh();
    return box;
}

/* hde-settings --power: the same in text */
int power_cli(void)
{
    HdePower p;
    hde_power_read(&p);
    if (p.percent < 0) printf("Battery: none (%s)\n", p.ac == 1 ? "plugged in" : "a computer without a battery");
    else {
        char *d = hde_power_describe(&p);
        printf("Battery: %s\n", d);
        g_free(d);
    }
    gboolean on = cfg_get_bool("battery_saver", FALSE);
    int level = CLAMP(cfg_get_int("battery_saver_level", 20), 5, 100);
    if (!on) printf("Battery saver: off\n");
    else if (level >= 100) printf("Battery saver: on battery%s\n", hde_power_saver_wanted(&p, TRUE, level, FALSE) ? " (on now)" : "");
    else printf("Battery saver: at %d%%%s\n", level, hde_power_saver_wanted(&p, TRUE, level, FALSE) ? " (on now)" : "");
    printf("Low battery warnings: %s\n", cfg_get_bool("battery_warnings", TRUE) ? "at 10% and 5%" : "off");
    return 0;
}
