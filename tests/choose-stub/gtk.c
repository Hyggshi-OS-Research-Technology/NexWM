/* tests/choose-stub/gtk.c — the stand-in for GTK3 of tests/choose-run-test.sh: enough of the toolkit to run
 * apps/hde-choose.c and to answer its question without a display.
 * HDE_CHOOSE_STUB=none      gtk_init_check() fails (no display): the caller must fall back and say so
 * HDE_CHOOSE_STUB=choice N  the dialog returns the Nth radio button as the chosen one
 * HDE_CHOOSE_STUB=cancel    the user closed the dialog
 * HDE_CHOOSE_STUB=remember0 the "Remember my choice" box is left unticked                                      */
#include <gtk/gtk.h>
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

static int mode_none, mode_cancel, choice = 0, remember = 1;
static int n_radios_seen;
static int radio_reads;
void hde_choose_stub_init(void)
{
    const char *m = getenv("HDE_CHOOSE_STUB");
    if (!m || !*m) m = "none";
    if (!strcmp(m, "none")) mode_none = 1;
    else if (!strcmp(m, "cancel")) mode_cancel = 1;
    else if (!strncmp(m, "choice", 6)) choice = atoi(m + 6);
    else if (!strncmp(m, "remember0", 9)) remember = 0;
}

gboolean gtk_init_check(int *argc, char ***argv) { (void)argc; (void)argv; hde_choose_stub_init();
    if (mode_none) { fprintf(stderr, "stub-gtk: no display\n"); return FALSE; } return TRUE; }
GtkWidget *gtk_dialog_new_with_buttons(const gchar *t, GtkWindow *p, GtkDialogFlags f, const gchar *b, ...)
{ (void)p; (void)f; (void)b; fprintf(stderr, "stub-gtk: dialog '%s'\n", t); return (GtkWidget *)calloc(1, 8); }
void gtk_window_set_icon_name(GtkWindow *w, const gchar *n) { (void)w; (void)n; }
void gtk_window_set_position(GtkWindow *w, GtkWindowPosition p) { (void)w; (void)p; }
GtkWidget *gtk_dialog_get_content_area(GtkDialog *d) { return (GtkWidget *)d; }
void gtk_container_set_border_width(GtkContainer *c, guint b) { (void)c; (void)b; }
void gtk_box_set_spacing(GtkBox *b, gint s) { (void)b; (void)s; }
void gtk_box_pack_start(GtkBox *b, GtkWidget *c, gboolean e, gboolean f, guint p) { (void)b; (void)c; (void)e; (void)f; (void)p; }
GtkWidget *gtk_label_new(const gchar *s) { (void)s; return (GtkWidget *)calloc(1, 8); }
void gtk_label_set_markup(GtkLabel *l, const gchar *s) { (void)l; fprintf(stderr, "stub-gtk: title '%s'\n", s); }
void gtk_label_set_xalign(GtkLabel *l, gfloat x) { (void)l; (void)x; }
void gtk_label_set_line_wrap(GtkLabel *l, gboolean w) { (void)l; (void)w; }
void gtk_widget_set_size_request(GtkWidget *w, gint a, gint b) { (void)w; (void)a; (void)b; }
GtkStyleContext *gtk_widget_get_style_context(GtkWidget *w) { return (GtkStyleContext *)w; }
void gtk_style_context_add_class(GtkStyleContext *c, const gchar *n) { (void)c; (void)n; }
GtkWidget *gtk_radio_button_new_with_label(GSList *g, const gchar *l)
{ (void)g; n_radios_seen++; fprintf(stderr, "stub-gtk: radio %d: %s\n", n_radios_seen, l); return (GtkWidget *)l; }
GSList *gtk_radio_button_get_group(GtkRadioButton *r) { (void)r; return NULL; }
GtkWidget *gtk_check_button_new_with_label(const gchar *l) { (void)l; return (GtkWidget *)calloc(1, 8); }
void gtk_toggle_button_set_active(GtkToggleButton *t, gboolean a) { (void)t; (void)a; }
gboolean gtk_toggle_button_get_active(GtkToggleButton *t) { (void)t;
    /* the radios in order (the program asks each one, stopping at the first active), then the Remember box */
    if (radio_reads < n_radios_seen) { int i = radio_reads++; return (gboolean)(i == choice - 1); }
    return (gboolean)remember; }
gint gtk_dialog_run(GtkDialog *d) { (void)d; return mode_cancel ? GTK_RESPONSE_CANCEL : GTK_RESPONSE_ACCEPT; }
void gtk_widget_show_all(GtkWidget *w) { (void)w; }
void gtk_widget_destroy(GtkWidget *w) { (void)w; }
gboolean gtk_events_pending(void) { return FALSE; }
gboolean gtk_main_iteration(void) { return FALSE; }
GtkWidget *gtk_message_dialog_new(GtkWindow *p, GtkDialogFlags f, GtkMessageType t, GtkButtonsType b, const gchar *m, ...)
{ (void)p; (void)f; (void)t; (void)b; fprintf(stderr, "stub-gtk: message '%s'\n", m); return (GtkWidget *)calloc(1, 8); }
void gtk_message_dialog_format_secondary_text(GtkMessageDialog *d, const gchar *f, ...) { (void)d; (void)f; }

/* ---- the GLib part ---- */
GString *g_string_new(const gchar *init)
{ GString *s = calloc(1, sizeof *s); s->str = strdup(init ? init : ""); s->len = strlen(s->str); s->allocated_len = s->len + 1; return s; }
GString *g_string_append(GString *s, const gchar *v)
{ size_t n = strlen(s->str) + strlen(v) + 1; s->str = realloc(s->str, n); strcat(s->str, v); s->len = strlen(s->str); return s; }
GString *g_string_append_c(GString *s, gchar c)
{ char b[2] = { c, 0 }; return g_string_append(s, b); }
GString *g_string_append_printf(GString *s, const gchar *fmt, ...)
{ va_list ap; va_start(ap, fmt); char *t = NULL; if (vasprintf(&t, fmt, ap) >= 0) { s = g_string_append(s, t); free(t); } va_end(ap); return s; }
gchar *g_string_free(GString *s, gboolean free_segment)
{ gchar *r = free_segment ? NULL : s->str; if (free_segment) free(s->str); free(s); return r; }
gchar *g_markup_escape_text(const gchar *t, gssize n) { (void)n; return strdup(t); }
gchar *g_strdup(const gchar *s) { return strdup(s); }
gchar *g_strdup_printf(const gchar *fmt, ...)
{ va_list ap; va_start(ap, fmt); char *t = NULL; if (vasprintf(&t, fmt, ap) < 0) t = NULL; va_end(ap); return t; }
void g_free(gpointer p) { free(p); }
const gchar *g_strerror(gint e) { return strerror(e); }
const gchar *g_getenv(const gchar *n) { return getenv(n); }

/* the program of this stand-in: apps/hde-choose.c is compiled with -Dmain=hde_choose_main (see the Makefile), so
 * this is what a shell starts — exactly one main in the binary */
int hde_choose_main(int argc, char **argv);
int main(int argc, char **argv) { return hde_choose_main(argc, argv); }
