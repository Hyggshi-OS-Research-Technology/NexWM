/* hde-editor.c — Hyggshi Text Editor: A simple, beginner-friendly GTK3 text editor for HDE. */
#define _POSIX_C_SOURCE 200809L

#include "hde-build.h"
#include "hde-theme.h"
#include <ctype.h>
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define APP_TITLE "Hyggshi Text Editor"
#define MAX_UNDO 100

typedef struct {
    GtkWidget *page_box;
    GtkWidget *text_view;
    GtkTextBuffer *buffer;
    GtkWidget *tab_label;
    char *filepath;
    gboolean modified;
    gboolean suppress_undo_record;
    GPtrArray *undo_stack;
    GPtrArray *redo_stack;
} EditorTab;

static GtkCssProvider *app_css;
static GtkWidget *main_win;
static GtkWidget *notebook;
static GtkWidget *find_revealer;
static GtkWidget *find_entry;
static GtkWidget *replace_entry;
static GtkWidget *find_status_lbl;
static GtkWidget *wrap_toggle;
static GtkWidget *status_left_lbl;
static GtkWidget *status_right_lbl;
static int font_size_pt = 13;
static int untitled_counter = 1;

static void load_css(void)
{
    HdeThemeInfo ti;
    hde_theme_info_load(&ti);
    char *accent = hde_theme_accent_css(&ti);
    char *css = g_strdup_printf(
        ".editor-toolbar { background-color: alpha(@theme_fg_color, 0.04);"
        "  border-bottom: 1px solid alpha(@theme_fg_color, 0.12); padding: 6px 8px; }"
        ".editor-findbar { background-color: alpha(@theme_fg_color, 0.06);"
        "  border-bottom: 1px solid alpha(@theme_fg_color, 0.12); padding: 8px 12px; }"
        ".editor-statusbar { background-color: alpha(@theme_fg_color, 0.04);"
        "  border-top: 1px solid alpha(@theme_fg_color, 0.12); padding: 4px 12px; font-size: 12px; opacity: 0.85; }"
        ".editor-view { font-family: monospace; font-size: %dpt; padding: 10px; }"
        "%s",
        font_size_pt, accent);
    if (!app_css) {
        app_css = gtk_css_provider_new();
        GdkScreen *scr = gdk_screen_get_default();
        if (scr)
            gtk_style_context_add_provider_for_screen(scr, GTK_STYLE_PROVIDER(app_css),
                                                      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    gtk_css_provider_load_from_data(app_css, css, -1, NULL);
    g_free(css);
    g_free(accent);
    hde_theme_info_clear(&ti);
}

static void on_theme_changed(gpointer d)
{
    (void)d;
    load_css();
}

static EditorTab *current_tab(void)
{
    if (!notebook) return NULL;
    int idx = gtk_notebook_get_current_page(GTK_NOTEBOOK(notebook));
    if (idx < 0) return NULL;
    GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook), idx);
    return page ? g_object_get_data(G_OBJECT(page), "editor-tab") : NULL;
}

static char *buffer_get_all_text(GtkTextBuffer *buf)
{
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buf, &start, &end);
    return gtk_text_buffer_get_text(buf, &start, &end, FALSE);
}

static void update_tab_title(EditorTab *t)
{
    if (!t || !t->tab_label) return;
    char *base = t->filepath ? g_path_get_basename(t->filepath) : g_strdup("Untitled Document");
    char *title = t->modified ? g_strdup_printf("• %s", base) : g_strdup(base);
    gtk_label_set_text(GTK_LABEL(t->tab_label), title);
    if (t == current_tab() && main_win) {
        char *wtitle = g_strdup_printf("%s — %s", title, APP_TITLE);
        gtk_window_set_title(GTK_WINDOW(main_win), wtitle);
        g_free(wtitle);
    }
    g_free(title);
    g_free(base);
}

static void update_statusbar(void)
{
    EditorTab *t = current_tab();
    if (!t || !status_right_lbl) return;

    GtkTextIter iter;
    GtkTextMark *ins = gtk_text_buffer_get_insert(t->buffer);
    gtk_text_buffer_get_iter_at_mark(t->buffer, &iter, ins);
    int line = gtk_text_iter_get_line(&iter) + 1;
    int col = gtk_text_iter_get_line_offset(&iter) + 1;
    int total_lines = gtk_text_buffer_get_line_count(t->buffer);
    int total_chars = gtk_text_buffer_get_char_count(t->buffer);

    char *text = buffer_get_all_text(t->buffer);
    int words = 0;
    int in_word = 0;
    for (const char *p = text; p && *p; p++) {
        if (isspace((unsigned char)*p)) in_word = 0;
        else if (!in_word) { in_word = 1; words++; }
    }
    g_free(text);

    char rbuf[160];
    g_snprintf(rbuf, sizeof rbuf, "Line %d, Column %d  ·  %d %s, %d %s, %d %s  ·  %d pt  ·  UTF-8",
               line, col,
               total_lines, total_lines == 1 ? "line" : "lines",
               words, words == 1 ? "word" : "words",
               total_chars, total_chars == 1 ? "character" : "characters",
               font_size_pt);
    gtk_label_set_text(GTK_LABEL(status_right_lbl), rbuf);
}

static void set_status_message(const char *msg)
{
    if (status_left_lbl)
        gtk_label_set_text(GTK_LABEL(status_left_lbl), msg ? msg : "Ready");
}

static void on_buffer_changed(GtkTextBuffer *buf, gpointer d)
{
    EditorTab *t = d;
    if (!t) return;
    if (!t->suppress_undo_record) {
        char *snap = buffer_get_all_text(buf);
        if (t->undo_stack->len == 0 ||
            g_strcmp0(snap, g_ptr_array_index(t->undo_stack, t->undo_stack->len - 1)) != 0) {
            if (t->undo_stack->len >= MAX_UNDO)
                g_ptr_array_remove_index(t->undo_stack, 0);
            g_ptr_array_add(t->undo_stack, snap);
            g_ptr_array_set_size(t->redo_stack, 0);
        } else {
            g_free(snap);
        }
        if (!t->modified) {
            t->modified = TRUE;
            update_tab_title(t);
        }
    }
    update_statusbar();
}

static void on_cursor_moved(GtkTextBuffer *buf, GParamSpec *ps, gpointer d)
{
    (void)buf; (void)ps; (void)d;
    update_statusbar();
}

/* ---------------------------------------------------------------- File Operations */
static gboolean save_tab_to_path(EditorTab *t, const char *path)
{
    if (!t || !path) return FALSE;
    char *text = buffer_get_all_text(t->buffer);
    GError *err = NULL;
    gboolean ok = g_file_set_contents(path, text, -1, &err);
    g_free(text);
    if (!ok) {
        GtkWidget *dlg = gtk_message_dialog_new(
            GTK_WINDOW(main_win), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            "Could not save “%s”: %s", path, err ? err->message : "Unknown error");
        gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
        g_clear_error(&err);
        return FALSE;
    }
    if (t->filepath != path) {
        g_free(t->filepath);
        t->filepath = g_strdup(path);
    }
    t->modified = FALSE;
    update_tab_title(t);
    char *msg = g_strdup_printf("Saved %s", path);
    set_status_message(msg);
    g_free(msg);
    return TRUE;
}

static gboolean save_tab_as_dialog(EditorTab *t)
{
    if (!t) return FALSE;
    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        "Save Document As", GTK_WINDOW(main_win), GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
    if (t->filepath) {
        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dlg), t->filepath);
    } else {
        const char *docs = g_get_user_special_dir(G_USER_DIRECTORY_DOCUMENTS);
        if (docs && *docs) gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dlg), docs);
        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), "Document.txt");
    }
    gboolean saved = FALSE;
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        if (fn) {
            saved = save_tab_to_path(t, fn);
            g_free(fn);
        }
    }
    gtk_widget_destroy(dlg);
    return saved;
}

static gboolean save_tab(EditorTab *t)
{
    if (!t) return FALSE;
    if (t->filepath) return save_tab_to_path(t, t->filepath);
    return save_tab_as_dialog(t);
}

/* Returns TRUE if safe to close the tab, FALSE if the user cancelled */
static gboolean confirm_close_tab(EditorTab *t)
{
    if (!t || !t->modified) return TRUE;
    char *name = t->filepath ? g_path_get_basename(t->filepath) : g_strdup("Untitled Document");
    GtkWidget *dlg = gtk_message_dialog_new(
        GTK_WINDOW(main_win), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE,
        "Save changes to “%s” before closing?", name);
    gtk_message_dialog_format_secondary_text(
        GTK_MESSAGE_DIALOG(dlg),
        "Your changes will be lost if you close this document without saving.");
    gtk_dialog_add_buttons(GTK_DIALOG(dlg),
                           "_Don't Save", GTK_RESPONSE_NO,
                           "_Cancel", GTK_RESPONSE_CANCEL,
                           "_Save", GTK_RESPONSE_YES,
                           NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_YES);
    int resp = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    g_free(name);

    if (resp == GTK_RESPONSE_YES) return save_tab(t);
    if (resp == GTK_RESPONSE_NO) return TRUE;
    return FALSE;
}

static void editor_tab_free(gpointer p)
{
    EditorTab *t = p;
    if (!t) return;
    g_free(t->filepath);
    if (t->undo_stack) g_ptr_array_free(t->undo_stack, TRUE);
    if (t->redo_stack) g_ptr_array_free(t->redo_stack, TRUE);
    g_free(t);
}

static EditorTab *create_tab(const char *filepath, const char *initial_text);

static void on_close_tab_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    EditorTab *t = d;
    if (!t || !confirm_close_tab(t)) return;
    int idx = gtk_notebook_page_num(GTK_NOTEBOOK(notebook), t->page_box);
    if (idx >= 0) gtk_notebook_remove_page(GTK_NOTEBOOK(notebook), idx);
    if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook)) == 0)
        create_tab(NULL, "");
}

static EditorTab *create_tab(const char *filepath, const char *initial_text)
{
    EditorTab *t = g_new0(EditorTab, 1);
    t->filepath = filepath ? g_strdup(filepath) : NULL;
    t->undo_stack = g_ptr_array_new_with_free_func(g_free);
    t->redo_stack = g_ptr_array_new_with_free_func(g_free);

    GtkWidget *sc = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    t->page_box = sc;
    g_object_set_data_full(G_OBJECT(sc), "editor-tab", t, editor_tab_free);

    t->text_view = gtk_text_view_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(t->text_view), "editor-view");
    gboolean wrap = (!wrap_toggle || gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(wrap_toggle)));
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(t->text_view), wrap ? GTK_WRAP_WORD_CHAR : GTK_WRAP_NONE);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(t->text_view), 12);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(t->text_view), 12);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(t->text_view), 10);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(t->text_view), 10);

    t->buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(t->text_view));
    t->suppress_undo_record = TRUE;
    gtk_text_buffer_set_text(t->buffer, initial_text ? initial_text : "", -1);
    t->suppress_undo_record = FALSE;
    g_ptr_array_add(t->undo_stack, g_strdup(initial_text ? initial_text : ""));

    g_signal_connect(t->buffer, "changed", G_CALLBACK(on_buffer_changed), t);
    g_signal_connect(t->buffer, "notify::cursor-position", G_CALLBACK(on_cursor_moved), t);
    gtk_container_add(GTK_CONTAINER(sc), t->text_view);

    /* Tab header widget */
    GtkWidget *tab_hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    char *default_name = filepath ? g_path_get_basename(filepath)
                                  : g_strdup_printf("Untitled %d", untitled_counter++);
    t->tab_label = gtk_label_new(default_name);
    g_free(default_name);
    GtkWidget *close_btn = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_MENU);
    gtk_button_set_relief(GTK_BUTTON(close_btn), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(close_btn, "Close document");
    g_signal_connect(close_btn, "clicked", G_CALLBACK(on_close_tab_clicked), t);
    gtk_box_pack_start(GTK_BOX(tab_hb), t->tab_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tab_hb), close_btn, FALSE, FALSE, 0);
    gtk_widget_show_all(tab_hb);
    gtk_widget_show_all(sc);

    int page_idx = gtk_notebook_append_page(GTK_NOTEBOOK(notebook), sc, tab_hb);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), page_idx);
    update_tab_title(t);
    update_statusbar();
    gtk_widget_grab_focus(t->text_view);
    return t;
}

static void open_file_path(const char *path)
{
    if (!path || !*path) return;
    /* Check if already open */
    int n_pages = gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook));
    for (int i = 0; i < n_pages; i++) {
        GtkWidget *pg = gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook), i);
        EditorTab *et = g_object_get_data(G_OBJECT(pg), "editor-tab");
        if (et && et->filepath && !g_strcmp0(et->filepath, path)) {
            gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), i);
            return;
        }
    }

    char *contents = NULL;
    gsize len = 0;
    GError *err = NULL;
    if (!g_file_get_contents(path, &contents, &len, &err)) {
        if (g_error_matches(err, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
            g_clear_error(&err);
            create_tab(path, "");
            return;
        }
        GtkWidget *dlg = gtk_message_dialog_new(
            GTK_WINDOW(main_win), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            "Could not open “%s”: %s", path, err ? err->message : "Unknown error");
        gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
        g_clear_error(&err);
        return;
    }

    /* Validate UTF-8 or convert from locale/ISO-8859-1 */
    char *utf8 = NULL;
    if (!g_utf8_validate(contents, (gssize)len, NULL)) {
        utf8 = g_convert_with_fallback(contents, (gssize)len, "UTF-8", "ISO-8859-1", "?", NULL, NULL, NULL);
    }

    /* Reuse single empty untitled tab if unmodified */
    EditorTab *cur = current_tab();
    if (cur && !cur->filepath && !cur->modified && gtk_text_buffer_get_char_count(cur->buffer) == 0) {
        cur->filepath = g_strdup(path);
        cur->suppress_undo_record = TRUE;
        gtk_text_buffer_set_text(cur->buffer, utf8 ? utf8 : contents, -1);
        cur->suppress_undo_record = FALSE;
        g_ptr_array_set_size(cur->undo_stack, 0);
        g_ptr_array_add(cur->undo_stack, g_strdup(utf8 ? utf8 : contents));
        cur->modified = FALSE;
        update_tab_title(cur);
        update_statusbar();
    } else {
        create_tab(path, utf8 ? utf8 : contents);
    }
    char *msg = g_strdup_printf("Opened %s", path);
    set_status_message(msg);
    g_free(msg);
    g_free(utf8);
    g_free(contents);
}

/* ---------------------------------------------------------------- Toolbar Actions */
static void on_new_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    create_tab(NULL, "");
    set_status_message("Created new blank document");
}

static void on_open_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        "Open Text Document", GTK_WINDOW(main_win), GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(dlg), TRUE);

    GtkFileFilter *ft = gtk_file_filter_new();
    gtk_file_filter_set_name(ft, "Text & Code Files");
    gtk_file_filter_add_mime_type(ft, "text/*");
    gtk_file_filter_add_pattern(ft, "*.txt");
    gtk_file_filter_add_pattern(ft, "*.md");
    gtk_file_filter_add_pattern(ft, "*.ini");
    gtk_file_filter_add_pattern(ft, "*.conf");
    gtk_file_filter_add_pattern(ft, "*.json");
    gtk_file_filter_add_pattern(ft, "*.sh");
    gtk_file_filter_add_pattern(ft, "*.c");
    gtk_file_filter_add_pattern(ft, "*.h");
    gtk_file_filter_add_pattern(ft, "*.py");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), ft);

    GtkFileFilter *fa = gtk_file_filter_new();
    gtk_file_filter_set_name(fa, "All Files");
    gtk_file_filter_add_pattern(fa, "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), fa);

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        GSList *files = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(dlg));
        for (GSList *l = files; l; l = l->next) {
            open_file_path((const char *)l->data);
            g_free(l->data);
        }
        g_slist_free(files);
    }
    gtk_widget_destroy(dlg);
}

static void on_save_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    save_tab(current_tab());
}

static void on_save_as_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    save_tab_as_dialog(current_tab());
}

static void on_undo_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    EditorTab *t = current_tab();
    if (!t || t->undo_stack->len <= 1) return;
    char *cur = g_ptr_array_steal_index(t->undo_stack, t->undo_stack->len - 1);
    g_ptr_array_add(t->redo_stack, cur);
    const char *prev = g_ptr_array_index(t->undo_stack, t->undo_stack->len - 1);
    t->suppress_undo_record = TRUE;
    gtk_text_buffer_set_text(t->buffer, prev, -1);
    t->suppress_undo_record = FALSE;
    t->modified = TRUE;
    update_tab_title(t);
    update_statusbar();
    set_status_message("Undid last change");
}

static void on_redo_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    EditorTab *t = current_tab();
    if (!t || t->redo_stack->len == 0) return;
    char *next = g_ptr_array_steal_index(t->redo_stack, t->redo_stack->len - 1);
    t->suppress_undo_record = TRUE;
    gtk_text_buffer_set_text(t->buffer, next, -1);
    t->suppress_undo_record = FALSE;
    g_ptr_array_add(t->undo_stack, next);
    t->modified = TRUE;
    update_tab_title(t);
    update_statusbar();
    set_status_message("Redid change");
}

static void on_cut_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    EditorTab *t = current_tab();
    if (!t) return;
    gtk_text_buffer_cut_clipboard(t->buffer, gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), TRUE);
}

static void on_copy_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    EditorTab *t = current_tab();
    if (!t) return;
    gtk_text_buffer_copy_clipboard(t->buffer, gtk_clipboard_get(GDK_SELECTION_CLIPBOARD));
}

static void on_paste_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    EditorTab *t = current_tab();
    if (!t) return;
    gtk_text_buffer_paste_clipboard(t->buffer, gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), NULL, TRUE);
}

static void on_zoom_change(GtkButton *b, gpointer d)
{
    (void)b;
    int delta = GPOINTER_TO_INT(d);
    if (delta == 0) font_size_pt = 13;
    else font_size_pt = CLAMP(font_size_pt + delta, 8, 36);
    load_css();
    update_statusbar();
}

static void on_wrap_toggled(GtkToggleButton *b, gpointer d)
{
    (void)d;
    gboolean wrap = gtk_toggle_button_get_active(b);
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook));
    for (int i = 0; i < n; i++) {
        GtkWidget *pg = gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook), i);
        EditorTab *et = g_object_get_data(G_OBJECT(pg), "editor-tab");
        if (et && et->text_view)
            gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(et->text_view), wrap ? GTK_WRAP_WORD_CHAR : GTK_WRAP_NONE);
    }
}

/* ---------------------------------------------------------------- Find & Replace */
static void toggle_find_bar(gboolean show)
{
    gtk_revealer_set_reveal_child(GTK_REVEALER(find_revealer), show);
    if (show) gtk_widget_grab_focus(find_entry);
    else if (current_tab()) gtk_widget_grab_focus(current_tab()->text_view);
}

static void on_find_toggle_btn(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    gboolean cur = gtk_revealer_get_reveal_child(GTK_REVEALER(find_revealer));
    toggle_find_bar(!cur);
}

static void do_find(gboolean forward)
{
    EditorTab *t = current_tab();
    const char *needle = gtk_entry_get_text(GTK_ENTRY(find_entry));
    if (!t || !needle || !*needle) return;

    GtkTextIter sel_start, sel_end, match_start, match_end;
    gboolean has_sel = gtk_text_buffer_get_selection_bounds(t->buffer, &sel_start, &sel_end);
    GtkTextIter start_at = forward ? (has_sel ? sel_end : sel_start) : sel_start;
    if (!has_sel) {
        GtkTextMark *ins = gtk_text_buffer_get_insert(t->buffer);
        gtk_text_buffer_get_iter_at_mark(t->buffer, &start_at, ins);
    }

    GtkTextSearchFlags flags = GTK_TEXT_SEARCH_CASE_INSENSITIVE;
    gboolean found = forward
        ? gtk_text_iter_forward_search(&start_at, needle, flags, &match_start, &match_end, NULL)
        : gtk_text_iter_backward_search(&start_at, needle, flags, &match_start, &match_end, NULL);

    if (!found) {
        GtkTextIter bound;
        if (forward) gtk_text_buffer_get_start_iter(t->buffer, &bound);
        else gtk_text_buffer_get_end_iter(t->buffer, &bound);
        found = forward
            ? gtk_text_iter_forward_search(&bound, needle, flags, &match_start, &match_end, NULL)
            : gtk_text_iter_backward_search(&bound, needle, flags, &match_start, &match_end, NULL);
    }

    if (found) {
        gtk_text_buffer_select_range(t->buffer, &match_start, &match_end);
        gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(t->text_view), &match_start, 0.15, FALSE, 0, 0);
        gtk_label_set_text(GTK_LABEL(find_status_lbl), "Match found");
    } else {
        gtk_label_set_text(GTK_LABEL(find_status_lbl), "Not found");
    }
}

static void on_find_next(GtkWidget *w, gpointer d) { (void)w; (void)d; do_find(TRUE); }
static void on_find_prev(GtkWidget *w, gpointer d) { (void)w; (void)d; do_find(FALSE); }

static void on_replace_one(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    EditorTab *t = current_tab();
    const char *repl = gtk_entry_get_text(GTK_ENTRY(replace_entry));
    if (!t || !repl) return;
    GtkTextIter s, e;
    if (gtk_text_buffer_get_selection_bounds(t->buffer, &s, &e)) {
        gtk_text_buffer_delete(t->buffer, &s, &e);
        gtk_text_buffer_insert(t->buffer, &s, repl, -1);
    }
    do_find(TRUE);
}

static void on_replace_all(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    EditorTab *t = current_tab();
    const char *needle = gtk_entry_get_text(GTK_ENTRY(find_entry));
    const char *repl = gtk_entry_get_text(GTK_ENTRY(replace_entry));
    if (!t || !needle || !*needle || !repl) return;

    GtkTextIter iter, ms, me;
    gtk_text_buffer_get_start_iter(t->buffer, &iter);
    int count = 0;
    while (gtk_text_iter_forward_search(&iter, needle, GTK_TEXT_SEARCH_CASE_INSENSITIVE, &ms, &me, NULL)) {
        gtk_text_buffer_delete(t->buffer, &ms, &me);
        gtk_text_buffer_insert(t->buffer, &ms, repl, -1);
        iter = ms;
        count++;
    }
    char msg[64];
    g_snprintf(msg, sizeof msg, "Replaced %d %s", count, count == 1 ? "occurrence" : "occurrences");
    gtk_label_set_text(GTK_LABEL(find_status_lbl), msg);
}

static void on_find_close(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    toggle_find_bar(FALSE);
}

static void on_drag_data_received(GtkWidget *w, GdkDragContext *ctx, gint x, gint y,
                                  GtkSelectionData *sel, guint info, guint time, gpointer d)
{
    (void)w; (void)x; (void)y; (void)info; (void)d;
    char **uris = gtk_selection_data_get_uris(sel);
    if (!uris) {
        gtk_drag_finish(ctx, FALSE, FALSE, time);
        return;
    }
    for (int i = 0; uris[i]; i++) {
        char *fn = g_filename_from_uri(uris[i], NULL, NULL);
        if (fn) {
            open_file_path(fn);
            g_free(fn);
        }
    }
    g_strfreev(uris);
    gtk_drag_finish(ctx, TRUE, FALSE, time);
}

static gboolean on_window_delete(GtkWidget *w, GdkEvent *ev, gpointer d)
{
    (void)w; (void)ev; (void)d;
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook));
    for (int i = 0; i < n; i++) {
        GtkWidget *pg = gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook), i);
        EditorTab *et = g_object_get_data(G_OBJECT(pg), "editor-tab");
        if (et && et->modified) {
            gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), i);
            if (!confirm_close_tab(et))
                return TRUE; /* cancel quit */
        }
    }
    return FALSE;
}

static void show_about_dialog(void)
{
    const char *authors[] = { "Hyggshi OS Research Technology", NULL };
    gtk_show_about_dialog(GTK_WINDOW(main_win),
        "program-name", "Hyggshi Text Editor",
        "version", HDE_VERSION,
        "comments", "Beginner-friendly multi-tab text editor with undo/redo, find & replace, and font zoom for Hyggshi Desktop Environment.",
        "website", "https://github.com/Hyggshi-OS-Research-Technology/NexWM",
        "website-label", "NexWM / HDE Repository",
        "copyright", "Copyright (c) 2024-2026 Hyggshi OS Research Technology",
        "license-type", GTK_LICENSE_MIT_X11,
        "authors", authors,
        "logo-icon-name", "accessories-text-editor",
        NULL);
}

static void on_about_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    show_about_dialog();
}

static gboolean on_editor_key(GtkWidget *w, GdkEventKey *ev, gpointer d)
{
    (void)w; (void)d;
    gboolean ctrl = (ev->state & GDK_CONTROL_MASK) != 0;
    gboolean shift = (ev->state & GDK_SHIFT_MASK) != 0;
    if (ctrl) {
        switch (ev->keyval) {
        case GDK_KEY_n:
        case GDK_KEY_N: on_new_clicked(NULL, NULL); return TRUE;
        case GDK_KEY_o:
        case GDK_KEY_O: on_open_clicked(NULL, NULL); return TRUE;
        case GDK_KEY_s:
        case GDK_KEY_S:
            if (shift) on_save_as_clicked(NULL, NULL);
            else on_save_clicked(NULL, NULL);
            return TRUE;
        case GDK_KEY_z:
        case GDK_KEY_Z:
            if (shift) on_redo_clicked(NULL, NULL);
            else on_undo_clicked(NULL, NULL);
            return TRUE;
        case GDK_KEY_y:
        case GDK_KEY_Y: on_redo_clicked(NULL, NULL); return TRUE;
        case GDK_KEY_f:
        case GDK_KEY_F:
        case GDK_KEY_h:
        case GDK_KEY_H: toggle_find_bar(TRUE); return TRUE;
        case GDK_KEY_w:
        case GDK_KEY_W: on_close_tab_clicked(NULL, current_tab()); return TRUE;
        case GDK_KEY_plus:
        case GDK_KEY_equal:
        case GDK_KEY_KP_Add: on_zoom_change(NULL, GINT_TO_POINTER(1)); return TRUE;
        case GDK_KEY_minus:
        case GDK_KEY_KP_Subtract: on_zoom_change(NULL, GINT_TO_POINTER(-1)); return TRUE;
        case GDK_KEY_0:
        case GDK_KEY_KP_0: on_zoom_change(NULL, GINT_TO_POINTER(0)); return TRUE;
        case GDK_KEY_q:
        case GDK_KEY_Q:
            if (!on_window_delete(main_win, NULL, NULL))
                gtk_widget_destroy(main_win);
            return TRUE;
        default: break;
        }
    }
    if (ev->keyval == GDK_KEY_F1) {
        on_about_clicked(NULL, NULL);
        return TRUE;
    }
    if (ev->keyval == GDK_KEY_Escape && gtk_revealer_get_reveal_child(GTK_REVEALER(find_revealer))) {
        toggle_find_bar(FALSE);
        return TRUE;
    }
    return FALSE;
}

static void on_switch_page(GtkNotebook *nb, GtkWidget *page, guint page_num, gpointer d)
{
    (void)nb; (void)page_num; (void)d;
    EditorTab *t = g_object_get_data(G_OBJECT(page), "editor-tab");
    if (t) {
        update_tab_title(t);
        update_statusbar();
    }
}

static GtkWidget *toolbar_btn(const char *icon_name, const char *label, const char *tip, GCallback cb, gpointer d)
{
    GtkWidget *b = gtk_button_new();
    GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    if (icon_name) {
        GtkWidget *img = gtk_image_new_from_icon_name(icon_name, GTK_ICON_SIZE_MENU);
        gtk_box_pack_start(GTK_BOX(hb), img, FALSE, FALSE, 0);
    }
    if (label) {
        GtkWidget *lbl = gtk_label_new(label);
        gtk_box_pack_start(GTK_BOX(hb), lbl, FALSE, FALSE, 0);
    }
    gtk_container_add(GTK_CONTAINER(b), hb);
    if (tip) gtk_widget_set_tooltip_text(b, tip);
    if (cb) g_signal_connect(b, "clicked", cb, d);
    return b;
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--version") || !strcmp(argv[i], "-v")) {
            printf("hde-editor (%s) %s %s\n", APP_TITLE, HDE_RELEASE, HDE_VERSION);
            return 0;
        }
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("Usage: hde-editor [FILE...]\n"
                   "Hyggshi Text Editor — Simple, beginner-friendly text editor for HDE.\n");
            return 0;
        }
    }

    gtk_init(&argc, &argv);
    hde_theme_apply_process();
    load_css();
    hde_theme_watch(on_theme_changed, NULL);

    main_win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(main_win), APP_TITLE);
    gtk_window_set_icon_name(GTK_WINDOW(main_win), "accessories-text-editor");
    gtk_window_set_default_size(GTK_WINDOW(main_win), 880, 600);
    g_signal_connect(main_win, "delete-event", G_CALLBACK(on_window_delete), NULL);
    g_signal_connect(main_win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(main_win, "key-press-event", G_CALLBACK(on_editor_key), NULL);

    gtk_drag_dest_set(main_win, GTK_DEST_DEFAULT_ALL, NULL, 0, GDK_ACTION_COPY);
    gtk_drag_dest_add_uri_targets(main_win);
    g_signal_connect(main_win, "drag-data-received", G_CALLBACK(on_drag_data_received), NULL);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(main_win), root);

    /* Beginner-friendly labeled toolbar */
    GtkWidget *tbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_style_context_add_class(gtk_widget_get_style_context(tbar), "editor-toolbar");

    gtk_box_pack_start(GTK_BOX(tbar), toolbar_btn("document-new-symbolic", "New", "Create a new document (Ctrl+N)", G_CALLBACK(on_new_clicked), NULL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tbar), toolbar_btn("document-open-symbolic", "Open…", "Open a text file (Ctrl+O)", G_CALLBACK(on_open_clicked), NULL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tbar), toolbar_btn("document-save-symbolic", "Save", "Save the current document (Ctrl+S)", G_CALLBACK(on_save_clicked), NULL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tbar), toolbar_btn("document-save-as-symbolic", "Save As…", "Save with a new file name (Ctrl+Shift+S)", G_CALLBACK(on_save_as_clicked), NULL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tbar), gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 2);

    gtk_box_pack_start(GTK_BOX(tbar), toolbar_btn("edit-undo-symbolic", "Undo", "Undo last change (Ctrl+Z)", G_CALLBACK(on_undo_clicked), NULL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tbar), toolbar_btn("edit-redo-symbolic", "Redo", "Redo change (Ctrl+Y)", G_CALLBACK(on_redo_clicked), NULL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tbar), toolbar_btn("edit-cut-symbolic", NULL, "Cut selection (Ctrl+X)", G_CALLBACK(on_cut_clicked), NULL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tbar), toolbar_btn("edit-copy-symbolic", NULL, "Copy selection (Ctrl+C)", G_CALLBACK(on_copy_clicked), NULL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tbar), toolbar_btn("edit-paste-symbolic", NULL, "Paste from clipboard (Ctrl+V)", G_CALLBACK(on_paste_clicked), NULL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tbar), gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 2);

    gtk_box_pack_start(GTK_BOX(tbar), toolbar_btn("edit-find-symbolic", "Find & Replace", "Find or replace text (Ctrl+F)", G_CALLBACK(on_find_toggle_btn), NULL), FALSE, FALSE, 0);

    wrap_toggle = gtk_toggle_button_new_with_label("Wrap Lines");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(wrap_toggle), TRUE);
    gtk_widget_set_tooltip_text(wrap_toggle, "Wrap long lines automatically so you never have to scroll sideways");
    g_signal_connect(wrap_toggle, "toggled", G_CALLBACK(on_wrap_toggled), NULL);
    gtk_box_pack_end(GTK_BOX(tbar), toolbar_btn("help-about-symbolic", "About", "About Hyggshi Text Editor (F1)", G_CALLBACK(on_about_clicked), NULL), FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(tbar), wrap_toggle, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(tbar), toolbar_btn("zoom-in-symbolic", "A+", "Make text larger (Ctrl++)", G_CALLBACK(on_zoom_change), GINT_TO_POINTER(1)), FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(tbar), toolbar_btn("zoom-out-symbolic", "A-", "Make text smaller (Ctrl+-)", G_CALLBACK(on_zoom_change), GINT_TO_POINTER(-1)), FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(root), tbar, FALSE, FALSE, 0);

    /* Find & Replace Revealer Bar */
    find_revealer = gtk_revealer_new();
    GtkWidget *fbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_style_context_add_class(gtk_widget_get_style_context(fbar), "editor-findbar");

    find_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(find_entry), "Find text…");
    g_signal_connect(find_entry, "activate", G_CALLBACK(on_find_next), NULL);

    replace_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(replace_entry), "Replace with…");

    GtkWidget *fn_btn = gtk_button_new_with_label("Next");
    GtkWidget *fp_btn = gtk_button_new_with_label("Previous");
    GtkWidget *r1_btn = gtk_button_new_with_label("Replace");
    GtkWidget *ra_btn = gtk_button_new_with_label("Replace All");
    GtkWidget *fc_btn = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_MENU);
    find_status_lbl = gtk_label_new("");

    g_signal_connect(fn_btn, "clicked", G_CALLBACK(on_find_next), NULL);
    g_signal_connect(fp_btn, "clicked", G_CALLBACK(on_find_prev), NULL);
    g_signal_connect(r1_btn, "clicked", G_CALLBACK(on_replace_one), NULL);
    g_signal_connect(ra_btn, "clicked", G_CALLBACK(on_replace_all), NULL);
    g_signal_connect(fc_btn, "clicked", G_CALLBACK(on_find_close), NULL);

    gtk_box_pack_start(GTK_BOX(fbar), gtk_label_new("Find:"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(fbar), find_entry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(fbar), fn_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(fbar), fp_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(fbar), gtk_label_new("Replace:"), FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(fbar), replace_entry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(fbar), r1_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(fbar), ra_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(fbar), find_status_lbl, TRUE, TRUE, 4);
    gtk_box_pack_end(GTK_BOX(fbar), fc_btn, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(find_revealer), fbar);
    gtk_box_pack_start(GTK_BOX(root), find_revealer, FALSE, FALSE, 0);

    /* Multi-tab Notebook */
    notebook = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(notebook), TRUE);
    g_signal_connect(notebook, "switch-page", G_CALLBACK(on_switch_page), NULL);
    gtk_box_pack_start(GTK_BOX(root), notebook, TRUE, TRUE, 0);

    /* Status Bar */
    GtkWidget *sbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_style_context_add_class(gtk_widget_get_style_context(sbar), "editor-statusbar");
    status_left_lbl = gtk_label_new("Ready — Start typing or click Open… to edit a file");
    gtk_label_set_xalign(GTK_LABEL(status_left_lbl), 0);
    gtk_label_set_ellipsize(GTK_LABEL(status_left_lbl), PANGO_ELLIPSIZE_END);
    status_right_lbl = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(status_right_lbl), 1.0);
    gtk_box_pack_start(GTK_BOX(sbar), status_left_lbl, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(sbar), status_right_lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), sbar, FALSE, FALSE, 0);

    create_tab(NULL, "");
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-')
            open_file_path(argv[i]);
    }

    gtk_widget_show_all(main_win);
    gtk_main();
    return 0;
}
