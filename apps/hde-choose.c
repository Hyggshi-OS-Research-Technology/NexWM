/* hde-choose — "which program for this?" HDE asks, once, and remembers the answer.
 *
 * HDE has a program of its own for some of the things a session does, and the machine usually has another one
 * installed: a terminal (HDE's own hde-cmd one day, gnome-terminal, xterm …), folders (Hyggshi Files, Thunar,
 * Nautilus …), pictures and music and video (Hyggshi Media, eog, mpv …), the screen (HDE's own hde-screenshot, scrot
 * …), what the system is doing (gnome-system-monitor, …). Where HDE used to run the first one on a list, it now asks
 * which one to use — and when there is only one, it does not ask at all.
 *
 *   hde-choose terminal [ARGS…]    start the program for that feature, with ARGS when there are any (the arguments the
 *                                  program needs itself are used when the caller passed none, so `hde-choose files`
 *                                  opens the home folder while `hde-choose files /tmp` opens /tmp)
 *   hde-choose --ask F [ARGS…]     ask again even when a choice was remembered
 *   hde-choose --no-ask F [ARGS…]  never ask: run the remembered one, or the first installed one
 *   hde-choose --list              the features, what is installed for each, and what is remembered
 *   hde-choose --set F PROGRAM     use PROGRAM for F from now on ("ask" = ask me every time)
 *   hde-choose --reset F           ask again the next time
 *   hde-choose --version, --help
 *
 * The answer is written to ~/.config/hde/settings.ini as choice_<feature> (the same file Settings and the rest of HDE
 * read; the dialog's "Remember my choice" writes it, and so does --set). HDE_CHOOSE=PROGRAM overrides it all for one
 * run — that is what the tests and scripts use. With no display there is nothing to ask on: the first installed
 * program is used and the line says so, so a key binding never turns into a silent no-op.
 *
 * Exit status: 0 a program was started (or the command did what was asked), 1 the question was cancelled or the
 * program could not be started, 2 a mistake on the command line (or a feature nobody knows), 127 nothing installed can
 * do this.
 */
#include "hde-choose.h"
#include "hde-build.h"
#include "hde-distro.h"

#include <gtk/gtk.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PROGRAM "hde-choose"
#define MAX_CANDS 32

/* ------------------------------------------------------------------ saying what happens */

static int quiet;      /* --quiet: only what is asked for on stdout (the log lines stay) */

static void log_line(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
static void log_line(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs(PROGRAM ": ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static void usage(FILE *out)
{
    fprintf(out,
            "Usage: %s FEATURE [ARGS...]        the program for that feature, asking when there is more than one\n"
            "       %s --ask FEATURE [ARGS...]  ask, even when a choice was remembered\n"
            "       %s --no-ask FEATURE [ARGS...]  do not ask: the remembered one, or the first installed one\n"
            "       %s --list                   the features, what is installed and what is remembered\n"
            "       %s --set FEATURE PROGRAM    use PROGRAM for FEATURE from now on (\"ask\" to ask every time)\n"
            "       %s --reset FEATURE          ask again the next time\n"
            "       %s --version, --help\n"
            "\n"
            "Features: terminal, files, monitor, pictures, player, screenshot.\n"
            "The answer is kept in ~/.config/hde/settings.ini (choice_<feature>) and can be changed at any time; "
            "HDE_CHOOSE=PROGRAM overrides it for one run.\n",
            PROGRAM, PROGRAM, PROGRAM, PROGRAM, PROGRAM, PROGRAM, PROGRAM);
}

/* ------------------------------------------------------------------ running the program */

/* One word for /bin/sh: single quotes, with an escaped quote inside ("it's" -> 'it'\''s'). Caller frees. */
static char *quote(const char *s)
{
    GString *out = g_string_new("'");
    for (const char *p = s; *p; p++) {
        if (*p == '\'') g_string_append(out, "'\\''");
        else g_string_append_c(out, *p);
    }
    g_string_append_c(out, '\'');
    return g_string_free(out, FALSE);
}

/* Start it: this process *becomes* the program (the dialog is gone by then, and no hde-choose is left behind).
 * It goes through /bin/sh -c like the rest of HDE, so that an argument of ours such as "$HOME" means what it says. */
static int run(const char *feature, const HdeChooseCandidate *c, char **extra, int n_extra)
{
    GString *cmd = g_string_new(NULL);
    char *q = quote(c->program);
    g_string_append(cmd, q);
    g_free(q);
    if (c->args && *c->args && n_extra == 0) g_string_append_printf(cmd, " %s", c->args);
    for (int i = 0; i < n_extra; i++) {
        q = quote(extra[i]);
        g_string_append_printf(cmd, " %s", q);
        g_free(q);
    }
    log_line("%s: running %s", feature, cmd->str);
    char *argv[] = { (char *)"/bin/sh", (char *)"-c", cmd->str, NULL };
    execv("/bin/sh", argv);
    log_line("%s: cannot run '%s': %s", feature, cmd->str, g_strerror(errno));
    g_string_free(cmd, TRUE);
    return 1;
}

/* ------------------------------------------------------------------ the question */

static const char *EXPLAIN =
    "More than one program on this machine can do this. Pick the one HDE should use — "
    "with \"Remember my choice\" it will not ask again (Settings, or hde-choose --reset, can change the answer).";

/* The question. Returns the chosen candidate, or -1 when the user cancelled and -2 when there is no display to ask on
 * (the caller then uses the first installed program and says so, so a key binding is never a silent no-op). */
static int ask(const HdeChooseFeature *f, const HdeChooseCandidate **inst, int n, int *remember)
{
    if (!gtk_init_check(NULL, NULL)) return -2;

    char *esc = g_markup_escape_text(f->title, -1);
    char *markup = g_strdup_printf("<b>%s</b>", esc);
    g_free(esc);

    GtkWidget *d = gtk_dialog_new_with_buttons(f->title, NULL, GTK_DIALOG_MODAL,
                                               "_Cancel", GTK_RESPONSE_CANCEL,
                                               "_Use it", GTK_RESPONSE_ACCEPT, NULL);
    gtk_window_set_icon_name(GTK_WINDOW(d), "system-run");
    gtk_window_set_position(GTK_WINDOW(d), GTK_WIN_POS_CENTER);
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(d));
    gtk_container_set_border_width(GTK_CONTAINER(area), 12);
    gtk_box_set_spacing(GTK_BOX(area), 8);

    GtkWidget *lead = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lead), markup);
    gtk_label_set_xalign(GTK_LABEL(lead), 0.0);
    gtk_box_pack_start(GTK_BOX(area), lead, FALSE, FALSE, 0);

    GtkWidget *explain = gtk_label_new(EXPLAIN);
    gtk_label_set_xalign(GTK_LABEL(explain), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(explain), TRUE);
    gtk_widget_set_size_request(explain, 380, -1);
    gtk_style_context_add_class(gtk_widget_get_style_context(explain), "dim-label");
    gtk_box_pack_start(GTK_BOX(area), explain, FALSE, FALSE, 0);

    GSList *group = NULL;
    GtkWidget *buttons[MAX_CANDS];
    for (int i = 0; i < n; i++) {
        char *label = inst[i]->note ? g_strdup_printf("%s (%s)", inst[i]->program, inst[i]->note)
                                    : g_strdup(inst[i]->program);
        GtkWidget *radio = gtk_radio_button_new_with_label(group, label);
        group = gtk_radio_button_get_group(GTK_RADIO_BUTTON(radio));
        gtk_box_pack_start(GTK_BOX(area), radio, FALSE, FALSE, 0);
        buttons[i] = radio;
        g_free(label);
    }

    GtkWidget *remember_box = gtk_check_button_new_with_label("Remember my choice");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(remember_box), TRUE);
    gtk_box_pack_start(GTK_BOX(area), remember_box, FALSE, FALSE, 6);

    gtk_widget_show_all(d);
    int chosen = -1;
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT) {
        for (int i = 0; i < n; i++)
            if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(buttons[i]))) { chosen = i; break; }
        if (chosen < 0) chosen = 0;
    }
    *remember = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(remember_box));
    gtk_widget_destroy(d);
    while (gtk_events_pending()) gtk_main_iteration();      /* the window is gone before the program starts */
    g_free(markup);
    return chosen;
}

/* Nothing installed: say it, and what to install (the message of the system the user is on, like everywhere in HDE) */
static void nothing_can(const HdeChooseFeature *f)
{
    char *hint = hde_install_hint(f->install);
    log_line("%s: nothing on this machine can do this (%s)", f->feature, f->title);
    fprintf(stderr, "       Install one, e.g.: %s\n", hint);
    if (!quiet && gtk_init_check(NULL, NULL)) {
        GtkWidget *d = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
                                              "%s", "Nothing on this machine can do this");
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d),
                                                 "%s\n\nInstall one and HDE will use it, e.g.:\n%s", f->title, hint);
        gtk_dialog_run(GTK_DIALOG(d));
        gtk_widget_destroy(d);
    }
    free(hint);
}

/* ------------------------------------------------------------------ --list */

static const char *state_of(const char *choice)
{
    if (!choice || !*choice) return "ask (nothing remembered yet)";
    if (!strcmp(choice, "ask")) return "ask (every time)";
    return choice;
}

static int list_features(void)
{
    char *path = hde_choose_settings_file();
    printf("%s (HDE) %s %s\n", PROGRAM, HDE_RELEASE, HDE_VERSION);
    printf("settings: %s\n\n", path);
    free(path);

    int n_features = 0;
    const HdeChooseFeature *all = hde_choose_table(&n_features);
    for (int i = 0; i < n_features; i++) {
        const HdeChooseFeature *f = &all[i];
        const HdeChooseCandidate *inst[MAX_CANDS];
        int n = hde_choose_installed(f, inst, MAX_CANDS);
        char *choice = hde_choose_choice(f->feature);
        printf("%s — %s\n", f->feature, f->title);
        printf("  remembered: %s\n", state_of(choice));
        printf("  installed: ");
        if (!n) printf("nothing");
        for (int c = 0; c < n; c++) printf("%s%s", c ? ", " : "", inst[c]->program);
        printf("\n");
        if (n) {
            const char *pick = NULL;
            if (choice && *choice && strcmp(choice, "ask")) {
                for (int c = 0; c < n; c++) if (!strcmp(inst[c]->program, choice)) pick = inst[c]->program;
            }
            if (!pick && (!choice || !strcmp(choice, "ask"))) pick = inst[0]->program;
            if (pick) printf("  will run:   %s\n", pick);
            else printf("  will run:   ask (the remembered program is not installed any more)\n");
        } else {
            char *hint = hde_install_hint(f->install);
            printf("  install:    %s\n", hint);
            free(hint);
        }
        printf("\n");
        free(choice);
    }
    return 0;
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv)
{
    const char *feature = NULL, *set_value = NULL;
    int force_ask = 0, no_ask = 0, do_list = 0, do_reset = 0;
    char *extra[MAX_CANDS];
    int n_extra = 0;

    /* The options come first; the first word that is not one is the feature, and everything after the feature belongs
     * to the program that will be started (hde-choose terminal -e top: "-e" is the terminal's, not ours). */
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (feature) {
            if (n_extra < MAX_CANDS) extra[n_extra++] = argv[i];
            continue;
        }
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(stdout); return 0; }
        if (!strcmp(a, "-v") || !strcmp(a, "--version")) {
            printf("%s (HDE) %s %s\n", PROGRAM, HDE_RELEASE, HDE_VERSION);
            return 0;
        }
        if (!strcmp(a, "--list")) { do_list = 1; continue; }
        if (!strcmp(a, "--ask")) { force_ask = 1; continue; }
        if (!strcmp(a, "--no-ask")) { no_ask = 1; continue; }
        if (!strcmp(a, "--quiet")) { quiet = 1; continue; }
        if (!strcmp(a, "--set") || !strcmp(a, "--reset")) {
            int is_set = !strcmp(a, "--set");
            if (is_set && i + 2 >= argc) {
                fprintf(stderr, "%s: --set needs a feature and a program (see %s --help)\n", PROGRAM, PROGRAM);
                return 2;
            }
            if (!is_set && i + 1 >= argc) {
                fprintf(stderr, "%s: --reset needs a feature (see %s --help)\n", PROGRAM, PROGRAM);
                return 2;
            }
            feature = argv[++i];
            if (is_set) set_value = argv[++i];
            do_reset = !is_set;
            break;
        }
        if (a[0] == '-' && a[1]) {
            fprintf(stderr, "%s: unknown option '%s' (see %s --help)\n", PROGRAM, a, PROGRAM);
            return 2;
        }
        feature = a;
    }

    if (feature) {
        const HdeChooseFeature *f = hde_choose_feature(feature);
        if (!f) {
            fprintf(stderr, "%s: '%s' is not a feature (see %s --list)\n", PROGRAM, feature, PROGRAM);
            return 2;
        }
        if (set_value) {
            const char *value = !strcmp(set_value, "ask") || !strcmp(set_value, "none") ? "ask" : set_value;
            if (hde_choose_set_choice(f->feature, value) != 0) {
                log_line("%s: cannot write the choice down", f->feature);
                return 1;
            }
            if (strcmp(value, "ask") && !hde_choose_have(value))
                log_line("%s: '%s' is not installed (that is written down, but nothing will run until it is)",
                         f->feature, value);
            printf("%s -> %s\n", f->feature, value);
            return 0;
        }
        if (do_reset) {
            hde_choose_set_choice(f->feature, NULL);
            printf("%s: will ask again the next time\n", f->feature);
            return 0;
        }
    }
    if (do_list) return list_features();
    if (!feature) { usage(stderr); return 2; }

    const HdeChooseFeature *f = hde_choose_feature(feature);
    if (!f) {
        fprintf(stderr, "%s: '%s' is not a feature (see %s --list)\n", PROGRAM, feature, PROGRAM);
        return 2;
    }

    /* HDE_CHOOSE=PROGRAM: one run with this program, whatever is remembered (what the tests and scripts use) */
    const char *env = getenv("HDE_CHOOSE");
    if (env && *env) {
        if (!hde_choose_have(env)) {
            log_line("%s: HDE_CHOOSE says '%s', which is not installed", f->feature, env);
            return 127;
        }
        HdeChooseCandidate override = { env, NULL, NULL };
        log_line("%s: HDE_CHOOSE says %s", f->feature, env);
        return run(f->feature, &override, extra, n_extra);
    }

    const HdeChooseCandidate *inst[MAX_CANDS];
    int n = hde_choose_installed(f, inst, MAX_CANDS);
    if (n == 0) {
        nothing_can(f);
        return 127;
    }

    char *choice = hde_choose_choice(f->feature);
    const HdeChooseCandidate *pick = NULL;
    int remembered = 0;

    if (!force_ask && choice && *choice && strcmp(choice, "ask")) {
        for (int i = 0; i < n; i++)
            if (!strcmp(inst[i]->program, choice)) { pick = inst[i]; remembered = 1; }
        if (!pick)
            log_line("%s: '%s' was remembered but is not installed any more", f->feature, choice);
    }
    if (!pick && n == 1) {
        pick = inst[0];
        log_line("%s: only one program on this machine can do this (%s)", f->feature, pick->program);
    }
    if (!pick) {
        if (no_ask) {
            pick = inst[0];
            log_line("%s: %d programs can do this; --no-ask, so %s", f->feature, n, pick->program);
        } else {
            int remember = 0;
            int chosen = ask(f, inst, n, &remember);
            if (chosen == -2) {
                pick = inst[0];
                log_line("%s: %d programs can do this and there is no display to ask on; using %s",
                         f->feature, n, pick->program);
            } else if (chosen < 0) {
                log_line("%s: nothing started (the question was cancelled)", f->feature);
                free(choice);
                return 1;
            } else {
                pick = inst[chosen];
                if (remember) {
                    if (hde_choose_set_choice(f->feature, pick->program) == 0)
                        log_line("%s: you chose %s, remembered", f->feature, pick->program);
                    else
                        log_line("%s: you chose %s (could not write it down)", f->feature, pick->program);
                } else {
                    log_line("%s: you chose %s (not remembered)", f->feature, pick->program);
                }
            }
        }
    }
    if (remembered) log_line("%s: remembered -> %s", f->feature, pick->program);
    free(choice);
    return run(f->feature, pick, extra, n_extra);
}
