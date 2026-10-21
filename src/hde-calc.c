/* hde-calc.c — Hyggshi Calculator: Full-featured Standard, Scientific, Programmer, and Unit Converter for HDE (GTK3). */
#define _POSIX_C_SOURCE 200809L

#include "hde-build.h"
#include "hde-calc-core.h"
#include "hde-theme.h"
#include <gtk/gtk.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define APP_TITLE "Hyggshi Calculator"

static HdeCalcEnv calc_env;
static GtkCssProvider *app_css;
static GtkWidget *main_win;
static GtkWidget *stack;

/* Standard & Scientific widgets */
static GtkWidget *expr_entry;
static GtkWidget *result_lbl;
static GtkWidget *sub_expr_lbl;
static GtkWidget *deg_btn;
static GtkWidget *mem_badge_lbl;
static GtkWidget *sci_revealer;
static GtkWidget *history_list;
static gboolean result_just_evaluated;

/* Programmer mode widgets */
static GtkWidget *prog_expr_entry;
static GtkWidget *prog_hex_lbl;
static GtkWidget *prog_dec_lbl;
static GtkWidget *prog_oct_lbl;
static GtkWidget *prog_bin_lbl;
static GtkWidget *prog_bits_combo;
static uint64_t prog_value;
static int prog_bit_width = 64;

/* Converter mode widgets */
static GtkWidget *conv_cat_combo;
static GtkWidget *conv_from_combo;
static GtkWidget *conv_to_combo;
static GtkWidget *conv_in_entry;
static GtkWidget *conv_out_lbl;
static gboolean conv_updating;

static void load_css(void)
{
    HdeThemeInfo ti;
    hde_theme_info_load(&ti);
    char *accent = hde_theme_accent_css(&ti);
    const char *ac = ti.accent ? ti.accent : "#3584e4";
    gboolean dark = hde_theme_shell_dark();

    const char *card_bg = dark ? "rgba(255, 255, 255, 0.05)" : "rgba(0, 0, 0, 0.04)";
    const char *card_border = dark ? "rgba(255, 255, 255, 0.10)" : "rgba(0, 0, 0, 0.08)";
    const char *num_btn_bg = dark ? "rgba(255, 255, 255, 0.07)" : "rgba(0, 0, 0, 0.05)";
    const char *op_btn_bg = dark ? "rgba(255, 255, 255, 0.04)" : "rgba(0, 0, 0, 0.03)";

    char *css = g_strdup_printf(
        ".calc-display {"
        "  background-color: %s;"
        "  border: 1px solid %s;"
        "  border-radius: 14px;"
        "  padding: 14px 18px;"
        "}"
        ".calc-result {"
        "  font-family: monospace, sans-serif;"
        "  font-size: 36px;"
        "  font-weight: 800;"
        "  letter-spacing: 0.5px;"
        "}"
        ".calc-sub-expr {"
        "  font-family: monospace, sans-serif;"
        "  font-size: 13px;"
        "  opacity: 0.72;"
        "}"
        ".calc-btn {"
        "  min-height: 42px;"
        "  min-width: 52px;"
        "  font-size: 15px;"
        "  font-weight: 600;"
        "  border-radius: 10px;"
        "  background-color: %s;"
        "  border: 1px solid %s;"
        "}"
        ".calc-btn-op {"
        "  background-color: %s;"
        "  color: %s;"
        "  font-weight: 700;"
        "}"
        ".calc-btn-eq {"
        "  background-color: %s;"
        "  color: #ffffff;"
        "  font-weight: 800;"
        "  border-color: %s;"
        "}"
        ".calc-btn-clear {"
        "  background-color: rgba(224, 27, 36, 0.15);"
        "  color: #e01b24;"
        "  font-weight: 700;"
        "}"
        ".calc-prog-val {"
        "  font-family: monospace, sans-serif;"
        "  font-size: 16px;"
        "  font-weight: 700;"
        "  color: %s;"
        "}"
        ".calc-hist-expr {"
        "  font-size: 12px;"
        "  opacity: 0.70;"
        "}"
        ".calc-hist-res {"
        "  font-family: monospace, sans-serif;"
        "  font-size: 16px;"
        "  font-weight: 700;"
        "  color: %s;"
        "}"
        "%s",
        card_bg, card_border,
        num_btn_bg, card_border,
        op_btn_bg, ac,
        ac, ac,
        ac,
        ac,
        accent);
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

/* ---------------------------------------------------------------- History Sidebar */
static void on_history_row_activated(GtkListBox *lb, GtkListBoxRow *row, gpointer d)
{
    (void)lb; (void)d;
    const char *res = g_object_get_data(G_OBJECT(row), "calc-result");
    if (!res) return;
    gtk_entry_set_text(GTK_ENTRY(expr_entry), res);
    gtk_editable_set_position(GTK_EDITABLE(expr_entry), -1);
    result_just_evaluated = FALSE;
}

static void add_history_entry(const char *expr, const char *res_str)
{
    if (!history_list || !expr || !*expr || !res_str) return;
    GtkWidget *row = gtk_list_box_row_new();
    g_object_set_data_full(G_OBJECT(row), "calc-result", g_strdup(res_str), g_free);
    GtkWidget *vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_start(vb, 8);
    gtk_widget_set_margin_end(vb, 8);
    gtk_widget_set_margin_top(vb, 6);
    gtk_widget_set_margin_bottom(vb, 6);

    char *e_txt = g_strdup_printf("%s =", expr);
    GtkWidget *el = gtk_label_new(e_txt);
    g_free(e_txt);
    gtk_label_set_xalign(GTK_LABEL(el), 1.0);
    gtk_label_set_ellipsize(GTK_LABEL(el), PANGO_ELLIPSIZE_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(el), "calc-hist-expr");

    GtkWidget *rl = gtk_label_new(res_str);
    gtk_label_set_xalign(GTK_LABEL(rl), 1.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(rl), "calc-hist-res");

    gtk_box_pack_start(GTK_BOX(vb), el, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vb), rl, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(row), vb);
    gtk_widget_show_all(row);
    gtk_list_box_prepend(GTK_LIST_BOX(history_list), row);
}

static void on_clear_history(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    if (!history_list) return;
    GList *children = gtk_container_get_children(GTK_CONTAINER(history_list));
    for (GList *l = children; l; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(children);
}

/* ---------------------------------------------------------------- Standard & Scientific */
static void update_mem_badge(void)
{
    if (!mem_badge_lbl) return;
    if (fabs(calc_env.memory) < 1e-15) {
        gtk_label_set_text(GTK_LABEL(mem_badge_lbl), "M: 0");
    } else {
        char mbuf[64], fbuf[48];
        hde_calc_format(calc_env.memory, fbuf, sizeof fbuf);
        g_snprintf(mbuf, sizeof mbuf, "M = %s", fbuf);
        gtk_label_set_text(GTK_LABEL(mem_badge_lbl), mbuf);
    }
}

static void evaluate_current_expr(gboolean commit_history)
{
    const char *text = gtk_entry_get_text(GTK_ENTRY(expr_entry));
    if (!text || !*text) {
        gtk_label_set_text(GTK_LABEL(result_lbl), "0");
        gtk_label_set_text(GTK_LABEL(sub_expr_lbl), "Ready");
        return;
    }
    double val = 0.0;
    char err[128] = "";
    HdeCalcEnv preview_env = calc_env;
    if (hde_calc_eval(text, commit_history ? &calc_env : &preview_env, &val, err, sizeof err) == 0) {
        char buf[64];
        hde_calc_format(val, buf, sizeof buf);
        gtk_label_set_text(GTK_LABEL(result_lbl), buf);
        if (commit_history) {
            char *sub = g_strdup_printf("%s =", text);
            gtk_label_set_text(GTK_LABEL(sub_expr_lbl), sub);
            g_free(sub);
            add_history_entry(text, buf);
            gtk_entry_set_text(GTK_ENTRY(expr_entry), buf);
            gtk_editable_set_position(GTK_EDITABLE(expr_entry), -1);
            result_just_evaluated = TRUE;
        } else {
            char *sub = g_strdup_printf("Preview: %s", buf);
            gtk_label_set_text(GTK_LABEL(sub_expr_lbl), sub);
            g_free(sub);
        }
    } else if (commit_history) {
        gtk_label_set_text(GTK_LABEL(result_lbl), "Error");
        gtk_label_set_text(GTK_LABEL(sub_expr_lbl), err[0] ? err : "Syntax error");
    }
}

static void on_expr_changed(GtkEditable *ed, gpointer d)
{
    (void)ed; (void)d;
    evaluate_current_expr(FALSE);
}

static void on_expr_activate(GtkEntry *e, gpointer d)
{
    (void)e; (void)d;
    evaluate_current_expr(TRUE);
}

static void insert_token(const char *tok)
{
    const char *cur = gtk_entry_get_text(GTK_ENTRY(expr_entry));
    gboolean is_binary_op = (!strcmp(tok, "+") || !strcmp(tok, "-") || !strcmp(tok, "*") ||
                             !strcmp(tok, "/") || !strcmp(tok, "^") || !strcmp(tok, " mod ") ||
                             !strcmp(tok, "%") || !strcmp(tok, "!") || !strcmp(tok, "²") || !strcmp(tok, "³"));
    if (result_just_evaluated && !is_binary_op) {
        gtk_entry_set_text(GTK_ENTRY(expr_entry), "");
        cur = "";
    }
    result_just_evaluated = FALSE;
    char *next = g_strconcat(cur ? cur : "", tok, NULL);
    gtk_entry_set_text(GTK_ENTRY(expr_entry), next);
    gtk_editable_set_position(GTK_EDITABLE(expr_entry), -1);
    g_free(next);
}

static void on_btn_token(GtkButton *b, gpointer d)
{
    (void)b;
    const char *tok = d;
    if (!tok) return;
    if (!strcmp(tok, "=")) {
        evaluate_current_expr(TRUE);
    } else if (!strcmp(tok, "C")) {
        result_just_evaluated = FALSE;
        gtk_entry_set_text(GTK_ENTRY(expr_entry), "");
        gtk_label_set_text(GTK_LABEL(result_lbl), "0");
        gtk_label_set_text(GTK_LABEL(sub_expr_lbl), "Cleared");
    } else if (!strcmp(tok, "BS")) {
        result_just_evaluated = FALSE;
        const char *cur = gtk_entry_get_text(GTK_ENTRY(expr_entry));
        if (cur && *cur) {
            long clen = g_utf8_strlen(cur, -1);
            if (clen > 0) {
                char * cut = g_utf8_substring(cur, 0, clen - 1);
                gtk_entry_set_text(GTK_ENTRY(expr_entry), cut);
                gtk_editable_set_position(GTK_EDITABLE(expr_entry), -1);
                g_free(cut);
            }
        }
    } else if (!strcmp(tok, "NEG")) {
        const char *cur = gtk_entry_get_text(GTK_ENTRY(expr_entry));
        if (cur && cur[0] == '-') {
            gtk_entry_set_text(GTK_ENTRY(expr_entry), cur + 1);
        } else {
            char *n = g_strconcat("-", cur ? cur : "", NULL);
            gtk_entry_set_text(GTK_ENTRY(expr_entry), n);
            g_free(n);
        }
        gtk_editable_set_position(GTK_EDITABLE(expr_entry), -1);
    } else if (!strcmp(tok, "INV")) {
        const char *cur = gtk_entry_get_text(GTK_ENTRY(expr_entry));
        char *n = (cur && *cur) ? g_strdup_printf("1/(%s)", cur) : g_strdup("1/(");
        gtk_entry_set_text(GTK_ENTRY(expr_entry), n);
        gtk_editable_set_position(GTK_EDITABLE(expr_entry), -1);
        g_free(n);
        evaluate_current_expr(TRUE);
    } else {
        insert_token(tok);
    }
}

static void on_mem_action(GtkButton *b, gpointer d)
{
    (void)b;
    const char *act = d;
    double cur_val = 0.0;
    const char *txt = gtk_entry_get_text(GTK_ENTRY(expr_entry));
    if (txt && *txt) hde_calc_eval(txt, &calc_env, &cur_val, NULL, 0);
    else cur_val = calc_env.last_ans;

    if (!strcmp(act, "MC")) calc_env.memory = 0.0;
    else if (!strcmp(act, "MS")) calc_env.memory = cur_val;
    else if (!strcmp(act, "M+")) calc_env.memory += cur_val;
    else if (!strcmp(act, "M-")) calc_env.memory -= cur_val;
    else if (!strcmp(act, "MR")) {
        char buf[64];
        hde_calc_format(calc_env.memory, buf, sizeof buf);
        insert_token(buf);
    }
    update_mem_badge();
}

static void on_deg_toggled(GtkButton *b, gpointer d)
{
    (void)d;
    calc_env.deg_mode = !calc_env.deg_mode;
    gtk_button_set_label(b, calc_env.deg_mode ? "DEG" : "RAD");
    evaluate_current_expr(FALSE);
}

static void on_copy_result(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    const char *res = gtk_label_get_text(GTK_LABEL(result_lbl));
    if (cb && res) gtk_clipboard_set_text(cb, res, -1);
}

static GtkWidget *make_calc_btn(const char *label, const char *tok, const char *css_extra)
{
    GtkWidget *b = gtk_button_new_with_label(label);
    gtk_style_context_add_class(gtk_widget_get_style_context(b), "calc-btn");
    if (css_extra)
        gtk_style_context_add_class(gtk_widget_get_style_context(b), css_extra);
    g_signal_connect(b, "clicked", G_CALLBACK(on_btn_token), (gpointer)tok);
    return b;
}

static GtkWidget *build_calc_page(gboolean scientific_by_default)
{
    (void)scientific_by_default;
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_container_set_border_width(GTK_CONTAINER(hbox), 14);

    /* Left: Display + Scientific + Standard Keypad */
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_hexpand(vbox, TRUE);

    /* Display Card */
    GtkWidget *disp = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_style_context_add_class(gtk_widget_get_style_context(disp), "calc-display");

    GtkWidget *top_meta = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    deg_btn = gtk_button_new_with_label("DEG");
    gtk_widget_set_tooltip_text(deg_btn, "Toggle Degrees / Radians for trigonometric functions");
    g_signal_connect(deg_btn, "clicked", G_CALLBACK(on_deg_toggled), NULL);
    mem_badge_lbl = gtk_label_new("M: 0");
    gtk_style_context_add_class(gtk_widget_get_style_context(mem_badge_lbl), "calc-sub-expr");
    sub_expr_lbl = gtk_label_new("Ready");
    gtk_label_set_xalign(GTK_LABEL(sub_expr_lbl), 1.0);
    gtk_label_set_ellipsize(GTK_LABEL(sub_expr_lbl), PANGO_ELLIPSIZE_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(sub_expr_lbl), "calc-sub-expr");
    GtkWidget *copy_btn = gtk_button_new_with_label("Copy");
    gtk_widget_set_tooltip_text(copy_btn, "Copy result to clipboard");
    g_signal_connect(copy_btn, "clicked", G_CALLBACK(on_copy_result), NULL);

    gtk_box_pack_start(GTK_BOX(top_meta), deg_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top_meta), mem_badge_lbl, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(top_meta), sub_expr_lbl, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(top_meta), copy_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(disp), top_meta, FALSE, FALSE, 0);

    result_lbl = gtk_label_new("0");
    gtk_label_set_xalign(GTK_LABEL(result_lbl), 1.0);
    gtk_label_set_selectable(GTK_LABEL(result_lbl), TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(result_lbl), PANGO_ELLIPSIZE_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(result_lbl), "calc-result");
    gtk_box_pack_start(GTK_BOX(disp), result_lbl, FALSE, FALSE, 2);

    expr_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(expr_entry), "Type an expression (e.g. 2*pi + sin(30) or sqrt(144)) and press Enter");
    g_signal_connect(expr_entry, "changed", G_CALLBACK(on_expr_changed), NULL);
    g_signal_connect(expr_entry, "activate", G_CALLBACK(on_expr_activate), NULL);
    gtk_box_pack_start(GTK_BOX(disp), expr_entry, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(vbox), disp, FALSE, FALSE, 0);

    /* Memory Bar */
    GtkWidget *mem_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_set_homogeneous(GTK_BOX(mem_bar), TRUE);
    static const char *const mem_btns[] = { "MC", "MR", "M+", "M-", "MS" };
    for (guint i = 0; i < G_N_ELEMENTS(mem_btns); i++) {
        GtkWidget *mb = gtk_button_new_with_label(mem_btns[i]);
        g_signal_connect(mb, "clicked", G_CALLBACK(on_mem_action), (gpointer)mem_btns[i]);
        gtk_box_pack_start(GTK_BOX(mem_bar), mb, TRUE, TRUE, 0);
    }
    GtkWidget *ans_b = make_calc_btn("Ans", "ans", NULL);
    gtk_box_pack_start(GTK_BOX(mem_bar), ans_b, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), mem_bar, FALSE, FALSE, 0);

    /* Scientific Keypad Grid */
    sci_revealer = gtk_revealer_new();
    gtk_revealer_set_reveal_child(GTK_REVEALER(sci_revealer), TRUE);
    GtkWidget *sci_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(sci_grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(sci_grid), 6);
    gtk_grid_set_column_homogeneous(GTK_GRID(sci_grid), TRUE);

    static const struct { const char *lbl; const char *tok; } sci_keys[3][6] = {
        { { "sin", "sin(" }, { "cos", "cos(" }, { "tan", "tan(" }, { "ln", "ln(" }, { "log₁₀", "log(" }, { "log₂", "log2(" } },
        { { "asin", "asin(" }, { "acos", "acos(" }, { "atan", "atan(" }, { "eˣ", "exp(" }, { "x²", "²" }, { "x³", "³" } },
        { { "√x", "sqrt(" }, { "∛x", "cbrt(" }, { "x^y", "^" }, { "n!", "!" }, { "π", "pi" }, { "e", "e" } }
    };
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 6; c++) {
            gtk_grid_attach(GTK_GRID(sci_grid),
                            make_calc_btn(sci_keys[r][c].lbl, sci_keys[r][c].tok, NULL),
                            c, r, 1, 1);
        }
    }
    gtk_container_add(GTK_CONTAINER(sci_revealer), sci_grid);
    gtk_box_pack_start(GTK_BOX(vbox), sci_revealer, FALSE, FALSE, 0);

    /* Standard Keypad Grid */
    GtkWidget *std_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(std_grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(std_grid), 6);
    gtk_grid_set_column_homogeneous(GTK_GRID(std_grid), TRUE);
    gtk_grid_set_row_homogeneous(GTK_GRID(std_grid), TRUE);
    gtk_widget_set_vexpand(std_grid, TRUE);

    static const struct { const char *lbl; const char *tok; const char *cls; } std_keys[5][5] = {
        { { "C", "C", "calc-btn-clear" }, { "(", "(", NULL }, { ")", ")", NULL }, { "⌫", "BS", NULL }, { "÷", "/", "calc-btn-op" } },
        { { "7", "7", NULL }, { "8", "8", NULL }, { "9", "9", NULL }, { "1/x", "INV", NULL }, { "×", "*", "calc-btn-op" } },
        { { "4", "4", NULL }, { "5", "5", NULL }, { "6", "6", NULL }, { "|x|", "abs(", NULL }, { "−", "-", "calc-btn-op" } },
        { { "1", "1", NULL }, { "2", "2", NULL }, { "3", "3", NULL }, { "mod", " mod ", NULL }, { "+", "+", "calc-btn-op" } },
        { { "±", "NEG", NULL }, { "0", "0", NULL }, { ".", ".", NULL }, { "%", "%", NULL }, { "=", "=", "calc-btn-eq" } }
    };
    for (int r = 0; r < 5; r++) {
        for (int c = 0; c < 5; c++) {
            gtk_grid_attach(GTK_GRID(std_grid),
                            make_calc_btn(std_keys[r][c].lbl, std_keys[r][c].tok, std_keys[r][c].cls),
                            c, r, 1, 1);
        }
    }
    gtk_box_pack_start(GTK_BOX(vbox), std_grid, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), vbox, TRUE, TRUE, 0);

    /* Right: Calculation History Sidebar */
    GtkWidget *hist_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_size_request(hist_box, 220, -1);

    GtkWidget *hh = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *ht = gtk_label_new("History");
    gtk_label_set_xalign(GTK_LABEL(ht), 0);
    GtkWidget *clr_btn = gtk_button_new_with_label("Clear");
    g_signal_connect(clr_btn, "clicked", G_CALLBACK(on_clear_history), NULL);
    gtk_box_pack_start(GTK_BOX(hh), ht, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(hh), clr_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hist_box), hh, FALSE, FALSE, 0);

    GtkWidget *sc = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sc), GTK_SHADOW_IN);
    history_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(history_list), GTK_SELECTION_NONE);
    g_signal_connect(history_list, "row-activated", G_CALLBACK(on_history_row_activated), NULL);
    gtk_container_add(GTK_CONTAINER(sc), history_list);
    gtk_box_pack_start(GTK_BOX(hist_box), sc, TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(hbox), hist_box, FALSE, FALSE, 0);
    return hbox;
}

/* ---------------------------------------------------------------- Programmer Mode */
static void update_prog_labels(void)
{
    prog_value = hde_calc_mask_bits(prog_value, prog_bit_width);
    char hex_b[64], dec_b[64], oct_b[64], bin_b[128];
    hde_calc_format_base(prog_value, 16, prog_bit_width, hex_b, sizeof hex_b);
    hde_calc_format_base(prog_value, 10, prog_bit_width, dec_b, sizeof dec_b);
    hde_calc_format_base(prog_value, 8, prog_bit_width, oct_b, sizeof oct_b);
    hde_calc_format_base(prog_value, 2, prog_bit_width, bin_b, sizeof bin_b);

    if (prog_hex_lbl) gtk_label_set_text(GTK_LABEL(prog_hex_lbl), hex_b);
    if (prog_dec_lbl) gtk_label_set_text(GTK_LABEL(prog_dec_lbl), dec_b);
    if (prog_oct_lbl) gtk_label_set_text(GTK_LABEL(prog_oct_lbl), oct_b);
    if (prog_bin_lbl) gtk_label_set_text(GTK_LABEL(prog_bin_lbl), bin_b);
}

static void on_prog_eval(GtkEntry *e, gpointer d)
{
    (void)d;
    const char *txt = gtk_entry_get_text(e);
    double val = 0.0;
    if (hde_calc_eval(txt, &calc_env, &val, NULL, 0) == 0) {
        prog_value = hde_calc_mask_bits((uint64_t)(int64_t)val, prog_bit_width);
        update_prog_labels();
    }
}

static void on_prog_bits_changed(GtkComboBox *c, gpointer d)
{
    (void)d;
    int idx = gtk_combo_box_get_active(c);
    prog_bit_width = (idx == 1) ? 32 : (idx == 2) ? 16 : (idx == 3) ? 8 : 64;
    update_prog_labels();
}

static void on_prog_btn(GtkButton *b, gpointer d)
{
    (void)b;
    const char *tok = d;
    if (!strcmp(tok, "CLR")) {
        prog_value = 0;
        gtk_entry_set_text(GTK_ENTRY(prog_expr_entry), "");
        update_prog_labels();
        return;
    }
    if (!strcmp(tok, "NOT")) {
        prog_value = hde_calc_mask_bits(~prog_value, prog_bit_width);
        char dec_b[64];
        hde_calc_format_base(prog_value, 10, prog_bit_width, dec_b, sizeof dec_b);
        gtk_entry_set_text(GTK_ENTRY(prog_expr_entry), dec_b);
        update_prog_labels();
        return;
    }
    if (!strcmp(tok, "=")) {
        on_prog_eval(GTK_ENTRY(prog_expr_entry), NULL);
        return;
    }
    const char *cur = gtk_entry_get_text(GTK_ENTRY(prog_expr_entry));
    char *next = g_strconcat(cur ? cur : "", tok, NULL);
    gtk_entry_set_text(GTK_ENTRY(prog_expr_entry), next);
    gtk_editable_set_position(GTK_EDITABLE(prog_expr_entry), -1);
    g_free(next);
    on_prog_eval(GTK_ENTRY(prog_expr_entry), NULL);
}

static GtkWidget *build_programmer_page(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(box), 16);

    GtkWidget *disp = gtk_grid_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(disp), "calc-display");
    gtk_grid_set_row_spacing(GTK_GRID(disp), 6);
    gtk_grid_set_column_spacing(GTK_GRID(disp), 12);

    prog_hex_lbl = gtk_label_new("0");
    prog_dec_lbl = gtk_label_new("0");
    prog_oct_lbl = gtk_label_new("0");
    prog_bin_lbl = gtk_label_new("0000");

    GtkWidget *bases[4] = { prog_hex_lbl, prog_dec_lbl, prog_oct_lbl, prog_bin_lbl };
    const char *bnames[4] = { "HEX", "DEC", "OCT", "BIN" };
    for (int i = 0; i < 4; i++) {
        GtkWidget *bl = gtk_label_new(bnames[i]);
        gtk_label_set_xalign(GTK_LABEL(bl), 0);
        gtk_style_context_add_class(gtk_widget_get_style_context(bl), "calc-sub-expr");
        gtk_label_set_xalign(GTK_LABEL(bases[i]), 0);
        gtk_label_set_selectable(GTK_LABEL(bases[i]), TRUE);
        gtk_style_context_add_class(gtk_widget_get_style_context(bases[i]), "calc-prog-val");
        gtk_grid_attach(GTK_GRID(disp), bl, 0, i, 1, 1);
        gtk_grid_attach(GTK_GRID(disp), bases[i], 1, i, 1, 1);
    }
    gtk_box_pack_start(GTK_BOX(box), disp, FALSE, FALSE, 0);

    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    prog_bits_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(prog_bits_combo), "QWORD (64-bit)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(prog_bits_combo), "DWORD (32-bit)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(prog_bits_combo), "WORD (16-bit)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(prog_bits_combo), "BYTE (8-bit)");
    gtk_combo_box_set_active(GTK_COMBO_BOX(prog_bits_combo), 0);
    g_signal_connect(prog_bits_combo, "changed", G_CALLBACK(on_prog_bits_changed), NULL);

    prog_expr_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(prog_expr_entry), "Enter bitwise expression (e.g. 0xFF & 0x0F, 1 << 8, 0b1010 | 0b0101)");
    g_signal_connect(prog_expr_entry, "changed", G_CALLBACK(on_prog_eval), NULL);
    g_signal_connect(prog_expr_entry, "activate", G_CALLBACK(on_prog_eval), NULL);

    gtk_box_pack_start(GTK_BOX(bar), prog_bits_combo, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), prog_expr_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), bar, FALSE, FALSE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
    gtk_grid_set_row_homogeneous(GTK_GRID(grid), TRUE);
    gtk_widget_set_vexpand(grid, TRUE);

    static const struct { const char *lbl; const char *tok; } pkeys[4][6] = {
        { { "0x", "0x" }, { "0b", "0b" }, { "A", "A" }, { "B", "B" }, { "C", "C" }, { "CLR", "CLR" } },
        { { "AND", " & " }, { "OR", " | " }, { "D", "D" }, { "E", "E" }, { "F", "F" }, { "NOT", "NOT" } },
        { { "XOR", " xor " }, { "LSH <<", " << " }, { "7", "7" }, { "8", "8" }, { "9", "9" }, { "(", "(" } },
        { { "RSH >>", " >> " }, { "MOD", " % " }, { "4", "4" }, { "5", "5" }, { "6", "6" }, { "=", "=" } }
    };
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 6; c++) {
            GtkWidget *b = gtk_button_new_with_label(pkeys[r][c].lbl);
            gtk_style_context_add_class(gtk_widget_get_style_context(b), "calc-btn");
            g_signal_connect(b, "clicked", G_CALLBACK(on_prog_btn), (gpointer)pkeys[r][c].tok);
            gtk_grid_attach(GTK_GRID(grid), b, c, r, 1, 1);
        }
    }
    gtk_box_pack_start(GTK_BOX(box), grid, TRUE, TRUE, 0);
    return box;
}

/* ---------------------------------------------------------------- Unit Converter Mode */
static void update_conversion(void)
{
    if (conv_updating) return;
    HdeUnitCategory cat = (HdeUnitCategory)CLAMP(gtk_combo_box_get_active(GTK_COMBO_BOX(conv_cat_combo)), 0, HDE_UNIT_CAT_COUNT - 1);
    int from_idx = gtk_combo_box_get_active(GTK_COMBO_BOX(conv_from_combo));
    int to_idx = gtk_combo_box_get_active(GTK_COMBO_BOX(conv_to_combo));
    const char *in_txt = gtk_entry_get_text(GTK_ENTRY(conv_in_entry));
    double in_val = 0.0;
    if (in_txt && *in_txt) {
        if (hde_calc_eval(in_txt, &calc_env, &in_val, NULL, 0) != 0)
            in_val = strtod(in_txt, NULL);
    }
    double out_val = hde_unit_convert(cat, from_idx, to_idx, in_val);
    int count = 0;
    const HdeUnitDef *units = hde_unit_list(cat, &count);
    char num_b[64], full_b[128];
    hde_calc_format(out_val, num_b, sizeof num_b);
    if (units && to_idx >= 0 && to_idx < count)
        g_snprintf(full_b, sizeof full_b, "%s %s", num_b, units[to_idx].symbol);
    else
        g_snprintf(full_b, sizeof full_b, "%s", num_b);
    gtk_label_set_text(GTK_LABEL(conv_out_lbl), full_b);
}

static void populate_conv_units(void)
{
    conv_updating = TRUE;
    HdeUnitCategory cat = (HdeUnitCategory)CLAMP(gtk_combo_box_get_active(GTK_COMBO_BOX(conv_cat_combo)), 0, HDE_UNIT_CAT_COUNT - 1);
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(conv_from_combo));
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(conv_to_combo));
    int count = 0;
    const HdeUnitDef *units = hde_unit_list(cat, &count);
    for (int i = 0; i < count; i++) {
        char *lbl = g_strdup_printf("%s (%s)", units[i].name, units[i].symbol);
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(conv_from_combo), lbl);
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(conv_to_combo), lbl);
        g_free(lbl);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(conv_from_combo), count > 2 ? 2 : 0);
    gtk_combo_box_set_active(GTK_COMBO_BOX(conv_to_combo), count > 3 ? 3 : (count > 1 ? 1 : 0));
    conv_updating = FALSE;
    update_conversion();
}

static void on_conv_cat_changed(GtkComboBox *c, gpointer d)
{
    (void)c; (void)d;
    populate_conv_units();
}

static void on_conv_changed(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    update_conversion();
}

static void on_conv_swap(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    int f = gtk_combo_box_get_active(GTK_COMBO_BOX(conv_from_combo));
    int t = gtk_combo_box_get_active(GTK_COMBO_BOX(conv_to_combo));
    conv_updating = TRUE;
    gtk_combo_box_set_active(GTK_COMBO_BOX(conv_from_combo), t);
    gtk_combo_box_set_active(GTK_COMBO_BOX(conv_to_combo), f);
    conv_updating = FALSE;
    update_conversion();
}

static GtkWidget *build_converter_page(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_container_set_border_width(GTK_CONTAINER(box), 24);

    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_style_context_add_class(gtk_widget_get_style_context(card), "calc-display");

    conv_cat_combo = gtk_combo_box_text_new();
    for (int i = 0; i < HDE_UNIT_CAT_COUNT; i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(conv_cat_combo), hde_unit_category_name((HdeUnitCategory)i));
    gtk_combo_box_set_active(GTK_COMBO_BOX(conv_cat_combo), 0);

    conv_from_combo = gtk_combo_box_text_new();
    conv_to_combo = gtk_combo_box_text_new();
    conv_in_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(conv_in_entry), "1");

    GtkWidget *swap_btn = gtk_button_new_with_label("⇅ Swap Units");
    g_signal_connect(swap_btn, "clicked", G_CALLBACK(on_conv_swap), NULL);

    conv_out_lbl = gtk_label_new("0");
    gtk_label_set_selectable(GTK_LABEL(conv_out_lbl), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(conv_out_lbl), "calc-result");

    g_signal_connect(conv_cat_combo, "changed", G_CALLBACK(on_conv_cat_changed), NULL);
    g_signal_connect(conv_from_combo, "changed", G_CALLBACK(on_conv_changed), NULL);
    g_signal_connect(conv_to_combo, "changed", G_CALLBACK(on_conv_changed), NULL);
    g_signal_connect(conv_in_entry, "changed", G_CALLBACK(on_conv_changed), NULL);

    gtk_box_pack_start(GTK_BOX(card), gtk_label_new("Category"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), conv_cat_combo, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), gtk_label_new("From Unit & Value"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), conv_from_combo, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), conv_in_entry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), swap_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), gtk_label_new("To Unit & Converted Value"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), conv_to_combo, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), conv_out_lbl, FALSE, FALSE, 6);

    gtk_box_pack_start(GTK_BOX(box), card, FALSE, FALSE, 0);
    populate_conv_units();
    return box;
}

static void show_about_dialog(void)
{
    const char *authors[] = { "Hyggshi OS Research Technology", NULL };
    gtk_show_about_dialog(GTK_WINDOW(main_win),
        "program-name", "Hyggshi Calculator",
        "version", HDE_VERSION,
        "comments", "Standard, Scientific, 64-bit Programmer, and Unit Converter calculator for Hyggshi Desktop Environment.",
        "website", "https://github.com/Hyggshi-OS-Research-Technology/NexWM",
        "website-label", "NexWM / HDE Repository",
        "copyright", "Copyright (c) 2024-2026 Hyggshi OS Research Technology",
        "license-type", GTK_LICENSE_MIT_X11,
        "authors", authors,
        "logo-icon-name", "accessories-calculator",
        NULL);
}

static void on_about_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    show_about_dialog();
}

static gboolean on_calc_key(GtkWidget *w, GdkEventKey *ev, gpointer d)
{
    (void)w; (void)d;
    if (ev->keyval == GDK_KEY_F1) {
        show_about_dialog();
        return TRUE;
    }
    return FALSE;
}

int main(int argc, char **argv)
{
    hde_calc_env_init(&calc_env);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--version") || !strcmp(argv[i], "-v")) {
            printf("hde-calc (%s) %s %s\n", APP_TITLE, HDE_RELEASE, HDE_VERSION);
            return 0;
        }
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("Usage: hde-calc [-e EXPRESSION]\n"
                   "Hyggshi Calculator — Standard, Scientific, Programmer, and Unit Converter for HDE.\n");
            return 0;
        }
        if ((!strcmp(argv[i], "-e") || !strcmp(argv[i], "--eval")) && i + 1 < argc) {
            double val = 0.0;
            char err[128] = "";
            if (hde_calc_eval(argv[i + 1], &calc_env, &val, err, sizeof err) != 0) {
                fprintf(stderr, "hde-calc: %s\n", err);
                return 1;
            }
            char out[64];
            hde_calc_format(val, out, sizeof out);
            printf("%s\n", out);
            return 0;
        }
    }

    gtk_init(&argc, &argv);
    hde_theme_apply_process();
    load_css();
    hde_theme_watch(on_theme_changed, NULL);

    main_win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(main_win), APP_TITLE);
    gtk_window_set_icon_name(GTK_WINDOW(main_win), "accessories-calculator");
    gtk_window_set_default_size(GTK_WINDOW(main_win), 780, 560);
    g_signal_connect(main_win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(main_win, "key-press-event", G_CALLBACK(on_calc_key), NULL);

    stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);

    GtkWidget *hbar = gtk_header_bar_new();
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(hbar), TRUE);
    gtk_header_bar_set_title(GTK_HEADER_BAR(hbar), APP_TITLE);
    GtkWidget *switcher = gtk_stack_switcher_new();
    gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(switcher), GTK_STACK(stack));
    gtk_header_bar_set_custom_title(GTK_HEADER_BAR(hbar), switcher);

    GtkWidget *about_btn = gtk_button_new_from_icon_name("help-about-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(about_btn, "About Hyggshi Calculator (F1)");
    g_signal_connect(about_btn, "clicked", G_CALLBACK(on_about_clicked), NULL);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(hbar), about_btn);

    gtk_window_set_titlebar(GTK_WINDOW(main_win), hbar);

    gtk_stack_add_titled(GTK_STACK(stack), build_calc_page(TRUE), "scientific", "Scientific & Standard");
    gtk_stack_add_titled(GTK_STACK(stack), build_programmer_page(), "programmer", "Programmer");
    gtk_stack_add_titled(GTK_STACK(stack), build_converter_page(), "converter", "Unit Converter");

    gtk_container_add(GTK_CONTAINER(main_win), stack);
    gtk_widget_show_all(main_win);
    gtk_widget_grab_focus(expr_entry);

    gtk_main();
    return 0;
}
