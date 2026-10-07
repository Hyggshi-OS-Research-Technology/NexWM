/* tests/choose-stub/gtk/gtk.h — the GTK3 and GLib functions apps/hde-choose.c uses, and nothing else.
 *
 * A stand-in for the toolkit, in the spirit of the stand-in for mpv of tests/player-window-test.sh: the question of
 * hde-choose ("which program for this?") cannot be answered in CI (there is no display there) and cannot be answered
 * the same way twice (it is a user clicking a radio button). tests/choose-stub/gtk.c implements these declarations and
 * says what the user "clicked" through the HDE_CHOOSE_STUB environment variable, so tests/choose-run-test.sh can
 * check the dialog itself: the chosen line is the one that starts, and "Remember my choice" decides whether it is
 * written down. Only tests/choose-stub is ever built with -Itests/choose-stub, so the real programs are compiled
 * against the real GTK. Never install anything from here. */
#ifndef STUB_GTK_H
#define STUB_GTK_H
#include <stddef.h>
#include <stdarg.h>
typedef int gboolean; typedef char gchar; typedef int gint; typedef unsigned int guint; typedef void *gpointer;
typedef long gssize; typedef float gfloat;
#define FALSE 0
#define TRUE 1
#define G_GNUC_PRINTF(a, b) __attribute__((format(printf, a, b)))
typedef struct _GSList GSList; struct _GSList { gpointer data; GSList *next; };
typedef struct _GString GString; struct _GString { gchar *str; size_t len; size_t allocated_len; };
GString *g_string_new(const gchar *init);
GString *g_string_append(GString *s, const gchar *val);
GString *g_string_append_c(GString *s, gchar c);
GString *g_string_append_printf(GString *s, const gchar *fmt, ...);
gchar *g_string_free(GString *s, gboolean free_segment);
gchar *g_markup_escape_text(const gchar *text, gssize length);
gchar *g_strdup(const gchar *s);
gchar *g_strdup_printf(const gchar *fmt, ...);
void g_free(gpointer p);
const gchar *g_strerror(gint errnum);
const gchar *g_getenv(const gchar *name);
typedef struct _GtkWidget GtkWidget; typedef struct _GtkWindow GtkWindow; typedef struct _GtkDialog GtkDialog;
typedef struct _GtkBox GtkBox; typedef struct _GtkLabel GtkLabel; typedef struct _GtkContainer GtkContainer;
typedef struct _GtkToggleButton GtkToggleButton; typedef struct _GtkRadioButton GtkRadioButton;
typedef struct _GtkCheckButton GtkCheckButton; typedef struct _GtkMessageDialog GtkMessageDialog;
typedef struct _GtkStyleContext GtkStyleContext;
typedef enum { GTK_RESPONSE_NONE = -1, GTK_RESPONSE_CANCEL = -6, GTK_RESPONSE_ACCEPT = -3 } GtkResponseType;
typedef enum { GTK_DIALOG_MODAL = 1 << 0 } GtkDialogFlags;
typedef enum { GTK_MESSAGE_ERROR } GtkMessageType;
typedef enum { GTK_BUTTONS_CLOSE } GtkButtonsType;
typedef enum { GTK_WIN_POS_NONE, GTK_WIN_POS_CENTER } GtkWindowPosition;
#define GTK_WINDOW(w) ((GtkWindow *)(w))
#define GTK_DIALOG(w) ((GtkDialog *)(w))
#define GTK_BOX(w) ((GtkBox *)(w))
#define GTK_LABEL(w) ((GtkLabel *)(w))
#define GTK_CONTAINER(w) ((GtkContainer *)(w))
#define GTK_TOGGLE_BUTTON(w) ((GtkToggleButton *)(w))
#define GTK_RADIO_BUTTON(w) ((GtkRadioButton *)(w))
#define GTK_MESSAGE_DIALOG(w) ((GtkMessageDialog *)(w))
gboolean gtk_init_check(int *argc, char ***argv);
GtkWidget *gtk_dialog_new_with_buttons(const gchar *title, GtkWindow *parent, GtkDialogFlags flags,
                                       const gchar *first_button_text, ...);
void gtk_window_set_icon_name(GtkWindow *window, const gchar *name);
void gtk_window_set_position(GtkWindow *window, GtkWindowPosition position);
GtkWidget *gtk_dialog_get_content_area(GtkDialog *dialog);
void gtk_container_set_border_width(GtkContainer *container, guint border_width);
void gtk_box_set_spacing(GtkBox *box, gint spacing);
void gtk_box_pack_start(GtkBox *box, GtkWidget *child, gboolean expand, gboolean fill, guint padding);
GtkWidget *gtk_label_new(const gchar *str);
void gtk_label_set_markup(GtkLabel *label, const gchar *str);
void gtk_label_set_xalign(GtkLabel *label, gfloat xalign);
void gtk_label_set_line_wrap(GtkLabel *label, gboolean wrap);
void gtk_widget_set_size_request(GtkWidget *widget, gint width, gint height);
GtkStyleContext *gtk_widget_get_style_context(GtkWidget *widget);
void gtk_style_context_add_class(GtkStyleContext *context, const gchar *class_name);
GtkWidget *gtk_radio_button_new_with_label(GSList *group, const gchar *label);
GSList *gtk_radio_button_get_group(GtkRadioButton *radio_button);
GtkWidget *gtk_check_button_new_with_label(const gchar *label);
void gtk_toggle_button_set_active(GtkToggleButton *toggle_button, gboolean is_active);
gboolean gtk_toggle_button_get_active(GtkToggleButton *toggle_button);
gint gtk_dialog_run(GtkDialog *dialog);
void gtk_widget_show_all(GtkWidget *widget);
void gtk_widget_destroy(GtkWidget *widget);
gboolean gtk_events_pending(void);
gboolean gtk_main_iteration(void);
GtkWidget *gtk_message_dialog_new(GtkWindow *parent, GtkDialogFlags flags, GtkMessageType type,
                                  GtkButtonsType buttons, const gchar *message_format, ...);
void gtk_message_dialog_format_secondary_text(GtkMessageDialog *message_dialog, const gchar *message_format, ...);
#endif
