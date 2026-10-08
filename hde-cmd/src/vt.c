/* vt.c — HDE Cmd's compact built-in VT100/xterm screen engine.
 *
 * It intentionally implements the useful interactive subset rather than claiming complete VT compatibility: text,
 * cursor movement, erase/insert, scrolling and scrollback, ANSI colours/attributes, alternate screen, cursor/key modes,
 * bracketed paste, basic mouse modes, OSC titles, and the common terminal replies.
 */
#define _XOPEN_SOURCE 700
#include "vt.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define VT_MAX_ROWS 1000
#define VT_MAX_COLUMNS 1000
#define VT_MAX_PARAMS 16
#define VT_OSC_SIZE 4096
#define VT_ATTRS (HDE_CMD_VT_BOLD | HDE_CMD_VT_ITALIC | HDE_CMD_VT_UNDERLINE | HDE_CMD_VT_BLINK | \
                 HDE_CMD_VT_INVERSE | HDE_CMD_VT_STRIKE)

typedef struct {
    HdeCmdVtCell *cells;
    int rows, columns;
    int capacity, first, history, view_offset;
    int x, y, saved_x, saved_y;
    int scroll_top, scroll_bottom;
    int wrap_pending;
    int history_enabled;
} VtScreen;

enum { PARSER_GROUND, PARSER_ESCAPE, PARSER_CSI, PARSER_OSC, PARSER_OSC_ESCAPE, PARSER_STRING,
       PARSER_STRING_ESCAPE, PARSER_CHARSET };

struct HdeCmdVt {
    VtScreen main_screen;
    VtScreen alternate_screen;
    VtScreen *screen;
    int alternate_active;

    uint32_t foreground, background;
    uint8_t attributes;
    uint32_t last_character;

    int cursor_visible;
    int application_cursor;
    int origin_mode;
    int autowrap;
    int insert_mode;
    int newline_mode;
    int bracketed_paste;
    int mouse_mode;
    int sgr_mouse;

    int parser;
    int csi_params[VT_MAX_PARAMS];
    int csi_count;
    int csi_private;
    int csi_intermediate;
    uint32_t utf8_value;
    uint32_t utf8_minimum;
    int utf8_remaining;
    char osc[VT_OSC_SIZE];
    size_t osc_length;
};

static int clamp_int(int value, int low, int high)
{
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

static size_t line_bytes(const VtScreen *s)
{
    return (size_t)s->columns * sizeof(HdeCmdVtCell);
}

static HdeCmdVtCell *physical_line(VtScreen *s, int index)
{
    return s->cells + (size_t)index * (size_t)s->columns;
}

static const HdeCmdVtCell *physical_line_const(const VtScreen *s, int index)
{
    return s->cells + (size_t)index * (size_t)s->columns;
}

static void clear_cell(HdeCmdVtCell *cell)
{
    memset(cell, 0, sizeof *cell);
    cell->codepoint = ' ';
    cell->foreground = HDE_CMD_VT_DEFAULT_COLOR;
    cell->background = HDE_CMD_VT_DEFAULT_COLOR;
}

static void clear_line(VtScreen *s, int index)
{
    HdeCmdVtCell *line = physical_line(s, index);
    for (int x = 0; x < s->columns; x++) clear_cell(&line[x]);
}

static int screen_index(const VtScreen *s, int logical_line)
{
    int index = (s->first + logical_line) % s->capacity;
    return index < 0 ? index + s->capacity : index;
}

static HdeCmdVtCell *live_line(VtScreen *s, int row)
{
    return physical_line(s, screen_index(s, s->history + row));
}

static const HdeCmdVtCell *display_line(const VtScreen *s, int row)
{
    int logical = s->history - s->view_offset + row;
    return physical_line_const(s, screen_index(s, logical));
}

static void screen_init(VtScreen *s, int rows, int columns, int history_enabled)
{
    memset(s, 0, sizeof *s);
    s->rows = clamp_int(rows, 1, VT_MAX_ROWS);
    s->columns = clamp_int(columns, 1, VT_MAX_COLUMNS);
    s->history_enabled = history_enabled;
    s->capacity = s->rows + (history_enabled ? HDE_CMD_VT_SCROLLBACK : 0);
    s->cells = calloc((size_t)s->capacity * (size_t)s->columns, sizeof *s->cells);
    if (!s->cells) {
        s->capacity = s->rows;
        s->cells = calloc((size_t)s->capacity * (size_t)s->columns, sizeof *s->cells);
        s->history_enabled = 0;
    }
    if (!s->cells) return;
    for (int i = 0; i < s->capacity; i++) clear_line(s, i);
    s->scroll_bottom = s->rows - 1;
}

static void screen_reset(VtScreen *s)
{
    if (!s->cells) return;
    s->first = 0;
    s->history = 0;
    s->view_offset = 0;
    s->x = s->y = s->saved_x = s->saved_y = 0;
    s->scroll_top = 0;
    s->scroll_bottom = s->rows - 1;
    s->wrap_pending = 0;
    for (int i = 0; i < s->capacity; i++) clear_line(s, i);
}

static void screen_resize(VtScreen *s, int rows, int columns)
{
    rows = clamp_int(rows, 1, VT_MAX_ROWS);
    columns = clamp_int(columns, 1, VT_MAX_COLUMNS);
    if (rows == s->rows && columns == s->columns) return;

    int old_rows = s->rows;
    int old_columns = s->columns;
    int old_history = s->history;
    int old_total = old_history + old_rows;
    int old_x = s->x, old_y = s->y;
    int old_saved_x = s->saved_x, old_saved_y = s->saved_y;
    int old_view = s->view_offset;
    int old_first = s->first;
    int old_capacity = s->capacity;
    HdeCmdVtCell *old_cells = s->cells;
    int new_capacity = rows + (s->history_enabled ? HDE_CMD_VT_SCROLLBACK : 0);
    HdeCmdVtCell *new_cells = calloc((size_t)new_capacity * (size_t)columns, sizeof *new_cells);
    if (!new_cells) return;

    s->rows = rows;
    s->columns = columns;
    s->capacity = new_capacity;
    s->first = 0;
    s->history = 0;
    s->view_offset = 0;
    s->cells = new_cells;
    for (int i = 0; i < new_capacity; i++) clear_line(s, i);

    int copy_count;
    int source_start;
    int destination_start;
    if (old_total > rows) {
        copy_count = old_total;
        int max_keep = new_capacity;
        if (copy_count > max_keep) copy_count = max_keep;
        source_start = old_total - copy_count;
        destination_start = 0;
        s->history = copy_count - rows;
    } else {
        copy_count = old_total;
        source_start = 0;
        destination_start = rows - old_total;
        s->history = 0;
    }

    int copy_columns = old_columns < columns ? old_columns : columns;
    for (int i = 0; i < copy_count; i++) {
        int source_logical = source_start + i;
        int source_index = (old_first + source_logical) % old_capacity;
        if (source_index < 0) source_index += old_capacity;
        const HdeCmdVtCell *src = old_cells + (size_t)source_index * (size_t)old_columns;
        HdeCmdVtCell *dst = physical_line(s, destination_start + i);
        memcpy(dst, src, (size_t)copy_columns * sizeof *dst);
    }

    int old_cursor_logical = old_history + old_y;
    int old_saved_logical = old_history + old_saved_y;
    int leading_blank = old_total < rows ? rows - old_total : 0;
    int trimmed = old_total > rows ? old_total - (s->history + rows) : 0;
    s->y = clamp_int(old_cursor_logical - trimmed - s->history + leading_blank, 0, rows - 1);
    s->saved_y = clamp_int(old_saved_logical - trimmed - s->history + leading_blank, 0, rows - 1);
    s->x = clamp_int(old_x, 0, columns - 1);
    s->saved_x = clamp_int(old_saved_x, 0, columns - 1);
    s->scroll_top = 0;
    s->scroll_bottom = rows - 1;
    s->wrap_pending = 0;
    s->view_offset = clamp_int(old_view, 0, s->history);
    free(old_cells);
}

static void clear_wide_cluster(VtScreen *s, int y, int x)
{
    if (x < 0 || x >= s->columns) return;
    HdeCmdVtCell *line = live_line(s, y);
    if (line[x].attributes & HDE_CMD_VT_WIDE_CONT) {
        if (x > 0) clear_cell(&line[x - 1]);
        clear_cell(&line[x]);
    } else if (x + 1 < s->columns && (line[x + 1].attributes & HDE_CMD_VT_WIDE_CONT)) {
        clear_cell(&line[x]);
        clear_cell(&line[x + 1]);
    } else {
        clear_cell(&line[x]);
    }
}

static void scroll_up(VtScreen *s, int top, int bottom, int count, int allow_history)
{
    top = clamp_int(top, 0, s->rows - 1);
    bottom = clamp_int(bottom, top, s->rows - 1);
    count = clamp_int(count, 1, s->rows);
    while (count-- > 0) {
        if (top == 0 && bottom == s->rows - 1 && allow_history && s->history_enabled) {
            if (s->history < s->capacity - s->rows) s->history++;
            else s->first = (s->first + 1) % s->capacity;
            s->view_offset = 0;
            clear_line(s, screen_index(s, s->history + s->rows - 1));
        } else {
            for (int y = top; y < bottom; y++)
                memcpy(live_line(s, y), live_line(s, y + 1), line_bytes(s));
            clear_line(s, screen_index(s, s->history + bottom));
        }
    }
}

static void scroll_down(VtScreen *s, int top, int bottom, int count)
{
    top = clamp_int(top, 0, s->rows - 1);
    bottom = clamp_int(bottom, top, s->rows - 1);
    count = clamp_int(count, 1, s->rows);
    while (count-- > 0) {
        for (int y = bottom; y > top; y--)
            memcpy(live_line(s, y), live_line(s, y - 1), line_bytes(s));
        clear_line(s, screen_index(s, s->history + top));
    }
}

static void line_feed(HdeCmdVt *vt, int carriage_return)
{
    VtScreen *s = vt->screen;
    if (carriage_return) s->x = 0;
    s->wrap_pending = 0;
    if (s->y == s->scroll_bottom) {
        scroll_up(s, s->scroll_top, s->scroll_bottom, 1, !vt->alternate_active);
    } else if (s->y < s->rows - 1) {
        s->y++;
    }
}

static void wrap_line(HdeCmdVt *vt)
{
    vt->screen->x = 0;
    line_feed(vt, 0);
}

static int unicode_width(uint32_t codepoint)
{
    int width = wcwidth((wchar_t)codepoint);
    return width < 0 ? 1 : width;
}

static void put_codepoint(HdeCmdVt *vt, uint32_t codepoint)
{
    VtScreen *s = vt->screen;
    if (codepoint == 0) return;
    int width = unicode_width(codepoint);
    if (width <= 0) {
        int x = s->wrap_pending ? s->x : s->x - 1;
        int y = s->y;
        if (x < 0 && y > 0) { y--; x = s->columns - 1; }
        if (x >= 0) {
            HdeCmdVtCell *line = live_line(s, y);
            if (line[x].attributes & HDE_CMD_VT_WIDE_CONT && x > 0) x--;
            if (line[x].combining_count < HDE_CMD_VT_COMBINING)
                line[x].combining[line[x].combining_count++] = codepoint;
        }
        return;
    }
    width = width > 1 ? 2 : 1;
    if (width > s->columns) width = 1;
    if (s->wrap_pending && vt->autowrap) wrap_line(vt);
    if (s->x + width > s->columns) {
        if (vt->autowrap) wrap_line(vt);
        else s->x = s->columns - width;
    }

    HdeCmdVtCell *line = live_line(s, s->y);
    if (vt->insert_mode) {
        for (int x = s->columns - 1; x >= s->x + width; x--)
            line[x] = line[x - width];
        for (int x = s->x; x < s->x + width; x++) clear_cell(&line[x]);
    }
    clear_wide_cluster(s, s->y, s->x);
    if (width == 2) clear_wide_cluster(s, s->y, s->x + 1);
    line = live_line(s, s->y);
    clear_cell(&line[s->x]);
    line[s->x].codepoint = codepoint;
    line[s->x].foreground = vt->foreground;
    line[s->x].background = vt->background;
    line[s->x].attributes = vt->attributes;
    if (width == 2 && s->x + 1 < s->columns) {
        clear_cell(&line[s->x + 1]);
        line[s->x + 1].attributes = HDE_CMD_VT_WIDE_CONT;
        line[s->x + 1].foreground = vt->foreground;
        line[s->x + 1].background = vt->background;
    }
    vt->last_character = codepoint;
    if (s->x + width >= s->columns) {
        s->x = s->columns - 1;
        s->wrap_pending = vt->autowrap;
    } else {
        s->x += width;
        s->wrap_pending = 0;
    }
}

static void erase_line_range(HdeCmdVt *vt, int y, int first, int last)
{
    VtScreen *s = vt->screen;
    if (y < 0 || y >= s->rows) return;
    first = clamp_int(first, 0, s->columns - 1);
    last = clamp_int(last, first, s->columns - 1);
    HdeCmdVtCell *line = live_line(s, y);
    for (int x = first; x <= last; x++) clear_wide_cluster(s, y, x);
    line = live_line(s, y);
    for (int x = first; x <= last; x++) clear_cell(&line[x]);
}

static void erase_display(HdeCmdVt *vt, int mode)
{
    VtScreen *s = vt->screen;
    if (mode == 2 || mode == 3) {
        for (int y = 0; y < s->rows; y++) erase_line_range(vt, y, 0, s->columns - 1);
        if (mode == 3 && !vt->alternate_active) {
            s->first = 0;
            s->history = 0;
            s->view_offset = 0;
        }
        return;
    }
    if (mode == 0) {
        erase_line_range(vt, s->y, s->x, s->columns - 1);
        for (int y = s->y + 1; y < s->rows; y++) erase_line_range(vt, y, 0, s->columns - 1);
    } else if (mode == 1) {
        for (int y = 0; y < s->y; y++) erase_line_range(vt, y, 0, s->columns - 1);
        erase_line_range(vt, s->y, 0, s->x);
    }
}

static uint32_t indexed_color(int index)
{
    static const uint32_t basic[16] = {
        0x000000, 0xcd0000, 0x00cd00, 0xcdcd00, 0x0000ee, 0xcd00cd, 0x00cdcd, 0xe5e5e5,
        0x7f7f7f, 0xff0000, 0x00ff00, 0xffff00, 0x5c5cff, 0xff00ff, 0x00ffff, 0xffffff
    };
    if (index < 0) index = 0;
    if (index < 16) return basic[index];
    if (index < 232) {
        static const int levels[] = { 0, 95, 135, 175, 215, 255 };
        int n = index - 16;
        int red = levels[n / 36];
        int green = levels[(n / 6) % 6];
        int blue = levels[n % 6];
        return (uint32_t)((red << 16) | (green << 8) | blue);
    }
    if (index < 256) {
        int grey = 8 + (index - 232) * 10;
        return (uint32_t)((grey << 16) | (grey << 8) | grey);
    }
    return 0xffffff;
}

static int parameter(const HdeCmdVt *vt, int index, int fallback)
{
    if (index < 0 || index >= vt->csi_count || vt->csi_params[index] < 0) return fallback;
    return vt->csi_params[index];
}

static void set_mode(HdeCmdVt *vt, int mode, int enabled)
{
    switch (mode) {
    case 1:  vt->application_cursor = enabled; break;
    case 6:  vt->origin_mode = enabled; vt->screen->x = 0; vt->screen->y = enabled ? vt->screen->scroll_top : 0; break;
    case 7:  vt->autowrap = enabled; break;
    case 25: vt->cursor_visible = enabled; break;
    case 47:
    case 1047:
    case 1049:
        if (enabled && !vt->alternate_active) {
            if (mode != 47) screen_reset(&vt->alternate_screen);
            vt->alternate_active = 1;
            vt->screen = &vt->alternate_screen;
        } else if (!enabled && vt->alternate_active) {
            vt->alternate_active = 0;
            vt->screen = &vt->main_screen;
            vt->screen->view_offset = 0;
        }
        break;
    case 1000: case 1002: case 1003: vt->mouse_mode = enabled ? mode : 0; break;
    case 1006: vt->sgr_mouse = enabled; break;
    case 2004: vt->bracketed_paste = enabled; break;
    default: break;
    }
}

static void set_sgr(HdeCmdVt *vt)
{
    if (vt->csi_count == 0) {
        vt->foreground = HDE_CMD_VT_DEFAULT_COLOR;
        vt->background = HDE_CMD_VT_DEFAULT_COLOR;
        vt->attributes = 0;
        return;
    }
    for (int i = 0; i < vt->csi_count; i++) {
        int p = parameter(vt, i, 0);
        if (p == 0) {
            vt->foreground = HDE_CMD_VT_DEFAULT_COLOR;
            vt->background = HDE_CMD_VT_DEFAULT_COLOR;
            vt->attributes = 0;
        } else if (p == 1) vt->attributes |= HDE_CMD_VT_BOLD;
        else if (p == 3) vt->attributes |= HDE_CMD_VT_ITALIC;
        else if (p == 4) vt->attributes |= HDE_CMD_VT_UNDERLINE;
        else if (p == 5 || p == 6) vt->attributes |= HDE_CMD_VT_BLINK;
        else if (p == 7) vt->attributes |= HDE_CMD_VT_INVERSE;
        else if (p == 9) vt->attributes |= HDE_CMD_VT_STRIKE;
        else if (p == 22) vt->attributes &= (uint8_t)~HDE_CMD_VT_BOLD;
        else if (p == 23) vt->attributes &= (uint8_t)~HDE_CMD_VT_ITALIC;
        else if (p == 24) vt->attributes &= (uint8_t)~HDE_CMD_VT_UNDERLINE;
        else if (p == 25) vt->attributes &= (uint8_t)~HDE_CMD_VT_BLINK;
        else if (p == 27) vt->attributes &= (uint8_t)~HDE_CMD_VT_INVERSE;
        else if (p == 29) vt->attributes &= (uint8_t)~HDE_CMD_VT_STRIKE;
        else if (p == 39) vt->foreground = HDE_CMD_VT_DEFAULT_COLOR;
        else if (p == 49) vt->background = HDE_CMD_VT_DEFAULT_COLOR;
        else if (p >= 30 && p <= 37) vt->foreground = indexed_color(p - 30);
        else if (p >= 40 && p <= 47) vt->background = indexed_color(p - 40);
        else if (p >= 90 && p <= 97) vt->foreground = indexed_color(p - 90 + 8);
        else if (p >= 100 && p <= 107) vt->background = indexed_color(p - 100 + 8);
        else if ((p == 38 || p == 48) && i + 1 < vt->csi_count) {
            uint32_t color = HDE_CMD_VT_DEFAULT_COLOR;
            int mode = parameter(vt, i + 1, -1);
            if (mode == 5 && i + 2 < vt->csi_count) {
                color = indexed_color(parameter(vt, i + 2, 0));
                i += 2;
            } else if (mode == 2 && i + 4 < vt->csi_count) {
                int r = clamp_int(parameter(vt, i + 2, 0), 0, 255);
                int g = clamp_int(parameter(vt, i + 3, 0), 0, 255);
                int b = clamp_int(parameter(vt, i + 4, 0), 0, 255);
                color = (uint32_t)((r << 16) | (g << 8) | b);
                i += 4;
            }
            if (p == 38) vt->foreground = color;
            else vt->background = color;
        }
    }
}

static void save_cursor(VtScreen *s)
{
    s->saved_x = s->x;
    s->saved_y = s->y;
}

static void restore_cursor(VtScreen *s)
{
    s->x = clamp_int(s->saved_x, 0, s->columns - 1);
    s->y = clamp_int(s->saved_y, 0, s->rows - 1);
    s->wrap_pending = 0;
}

static void insert_chars(HdeCmdVt *vt, int count)
{
    VtScreen *s = vt->screen;
    count = clamp_int(count, 1, s->columns - s->x);
    HdeCmdVtCell *line = live_line(s, s->y);
    for (int x = s->columns - 1; x >= s->x + count; x--) line[x] = line[x - count];
    for (int x = s->x; x < s->x + count; x++) clear_cell(&line[x]);
}

static void delete_chars(HdeCmdVt *vt, int count)
{
    VtScreen *s = vt->screen;
    count = clamp_int(count, 1, s->columns - s->x);
    HdeCmdVtCell *line = live_line(s, s->y);
    for (int x = s->x; x < s->columns - count; x++) line[x] = line[x + count];
    for (int x = s->columns - count; x < s->columns; x++) clear_cell(&line[x]);
}

static void erase_chars(HdeCmdVt *vt, int count)
{
    VtScreen *s = vt->screen;
    count = clamp_int(count, 1, s->columns - s->x);
    erase_line_range(vt, s->y, s->x, s->x + count - 1);
}

static void emit_reply(HdeCmdVtReply reply, void *userdata, const char *text)
{
    if (reply) reply(userdata, text, strlen(text));
}

static void dispatch_csi(HdeCmdVt *vt, unsigned char final, HdeCmdVtReply reply, void *userdata)
{
    VtScreen *s = vt->screen;
    int n = parameter(vt, 0, 1);
    if (n == 0) n = 1;
    switch (final) {
    case 'A': s->y = clamp_int(s->y - n, vt->origin_mode ? s->scroll_top : 0,
                               vt->origin_mode ? s->scroll_bottom : s->rows - 1); s->wrap_pending = 0; break;
    case 'B': case 'e': s->y = clamp_int(s->y + n, vt->origin_mode ? s->scroll_top : 0,
                                         vt->origin_mode ? s->scroll_bottom : s->rows - 1); s->wrap_pending = 0; break;
    case 'C': case 'a': s->x = clamp_int(s->x + n, 0, s->columns - 1); s->wrap_pending = 0; break;
    case 'D': s->x = clamp_int(s->x - n, 0, s->columns - 1); s->wrap_pending = 0; break;
    case 'E': s->y = clamp_int(s->y + n, 0, s->rows - 1); s->x = 0; s->wrap_pending = 0; break;
    case 'F': s->y = clamp_int(s->y - n, 0, s->rows - 1); s->x = 0; s->wrap_pending = 0; break;
    case 'G': case '`': s->x = clamp_int(parameter(vt, 0, 1) - 1, 0, s->columns - 1); s->wrap_pending = 0; break;
    case 'd':
        s->y = clamp_int(parameter(vt, 0, 1) - 1 + (vt->origin_mode ? s->scroll_top : 0),
                         vt->origin_mode ? s->scroll_top : 0, vt->origin_mode ? s->scroll_bottom : s->rows - 1);
        s->wrap_pending = 0;
        break;
    case 'H': case 'f': {
        int base = vt->origin_mode ? s->scroll_top : 0;
        s->y = clamp_int(parameter(vt, 0, 1) - 1 + base, base, vt->origin_mode ? s->scroll_bottom : s->rows - 1);
        s->x = clamp_int(parameter(vt, 1, 1) - 1, 0, s->columns - 1);
        s->wrap_pending = 0;
        break;
    }
    case 'J': erase_display(vt, parameter(vt, 0, 0)); break;
    case 'K': {
        int mode = parameter(vt, 0, 0);
        if (mode == 0) erase_line_range(vt, s->y, s->x, s->columns - 1);
        else if (mode == 1) erase_line_range(vt, s->y, 0, s->x);
        else if (mode == 2) erase_line_range(vt, s->y, 0, s->columns - 1);
        break;
    }
    case 'L': scroll_down(s, s->y, s->scroll_bottom, n); break;
    case 'M': scroll_up(s, s->y, s->scroll_bottom, n, 0); break;
    case 'S': scroll_up(s, s->scroll_top, s->scroll_bottom, n, !vt->alternate_active); break;
    case 'T': scroll_down(s, s->scroll_top, s->scroll_bottom, n); break;
    case '@': insert_chars(vt, n); break;
    case 'P': delete_chars(vt, n); break;
    case 'X': erase_chars(vt, n); break;
    case 'b': for (int i = 0; i < n; i++) put_codepoint(vt, vt->last_character); break;
    case 'm': set_sgr(vt); break;
    case 'r': {
        int top = parameter(vt, 0, 1) - 1;
        int bottom = parameter(vt, 1, s->rows) - 1;
        if (top >= 0 && bottom < s->rows && top < bottom) {
            s->scroll_top = top;
            s->scroll_bottom = bottom;
            s->x = 0;
            s->y = vt->origin_mode ? top : 0;
            s->wrap_pending = 0;
        }
        break;
    }
    case 's': save_cursor(s); break;
    case 'u': restore_cursor(s); break;
    case 'h': case 'l': {
        int enabled = final == 'h';
        if (vt->csi_private) {
            for (int i = 0; i < vt->csi_count; i++) set_mode(vt, parameter(vt, i, 0), enabled);
        } else {
            for (int i = 0; i < vt->csi_count; i++) {
                int mode = parameter(vt, i, 0);
                if (mode == 4) vt->insert_mode = enabled;
                else if (mode == 20) vt->newline_mode = enabled;
            }
        }
        break;
    }
    case 'n':
        if (parameter(vt, 0, 0) == 5) emit_reply(reply, userdata, "\033[0n");
        else if (parameter(vt, 0, 0) == 6) {
            char response[64];
            int row = s->y - (vt->origin_mode ? s->scroll_top : 0) + 1;
            snprintf(response, sizeof response, "\033[%d;%dR", row, s->x + 1);
            emit_reply(reply, userdata, response);
        }
        break;
    case 'c': emit_reply(reply, userdata, "\033[?1;2c"); break;
    case 'g': /* tab-stop clearing; the built-in engine uses fixed eight-column stops. */ break;
    default: break;
    }
}

static void csi_begin(HdeCmdVt *vt)
{
    vt->parser = PARSER_CSI;
    vt->csi_count = 1;
    vt->csi_params[0] = -1;
    vt->csi_private = 0;
    vt->csi_intermediate = 0;
}

static void csi_add_parameter(HdeCmdVt *vt)
{
    if (vt->csi_count < VT_MAX_PARAMS) vt->csi_params[vt->csi_count++] = -1;
}

static void csi_byte(HdeCmdVt *vt, unsigned char byte, HdeCmdVtReply reply, void *userdata)
{
    if (byte >= '0' && byte <= '9') {
        int *value = &vt->csi_params[vt->csi_count - 1];
        if (*value < 0) *value = 0;
        if (*value < 100000) *value = *value * 10 + (byte - '0');
    } else if (byte == ';' || byte == ':') {
        csi_add_parameter(vt);
    } else if ((byte == '?' || byte == '>' || byte == '!') && vt->csi_count == 1 && vt->csi_params[0] < 0 &&
               !vt->csi_intermediate) {
        vt->csi_private = byte == '?';
        vt->csi_intermediate = byte;
    } else if (byte >= 0x20 && byte <= 0x2f) {
        vt->csi_intermediate = byte;
    } else if (byte >= 0x40 && byte <= 0x7e) {
        dispatch_csi(vt, byte, reply, userdata);
        vt->parser = PARSER_GROUND;
    } else if (byte == 0x1b) {
        vt->parser = PARSER_ESCAPE;
    } else if (byte >= 0x30 && byte <= 0x3f) {
        /* Unsupported private/subparameter byte; ignore until the final byte. */
    } else {
        vt->parser = PARSER_GROUND;
    }
}

static void send_osc_title(HdeCmdVt *vt, HdeCmdVtTitle title, void *userdata)
{
    if (!title || vt->osc_length == 0) return;
    vt->osc[vt->osc_length] = '\0';
    char *separator = strchr(vt->osc, ';');
    if (!separator) return;
    int command = atoi(vt->osc);
    if (command != 0 && command != 1 && command != 2) return;
    const char *value = separator + 1;
    while (*value && (unsigned char)*value < 0x20) value++;
    title(userdata, value);
}

static void ground_control(HdeCmdVt *vt, unsigned char byte)
{
    VtScreen *s = vt->screen;
    switch (byte) {
    case 0x07: /* BEL */ break;
    case 0x08:
        s->wrap_pending = 0;
        if (s->x > 0) s->x--;
        if (live_line(s, s->y)[s->x].attributes & HDE_CMD_VT_WIDE_CONT && s->x > 0) s->x--;
        break;
    case 0x09: {
        int next = ((s->x / 8) + 1) * 8;
        s->x = next < s->columns ? next : s->columns - 1;
        s->wrap_pending = 0;
        break;
    }
    case 0x0a: case 0x0b: case 0x0c: line_feed(vt, vt->newline_mode); break;
    case 0x0d: s->x = 0; s->wrap_pending = 0; break;
    case 0x0e: case 0x0f: break;
    case 0x1b: vt->parser = PARSER_ESCAPE; break;
    default: break;
    }
}

static void ground_byte(HdeCmdVt *vt, unsigned char byte)
{
    if (byte < 0x20 || byte == 0x7f) {
        ground_control(vt, byte);
        return;
    }
    if (byte < 0x80) {
        put_codepoint(vt, byte);
        return;
    }
    if (byte >= 0xc2 && byte <= 0xdf) {
        vt->utf8_value = byte & 0x1f;
        vt->utf8_minimum = 0x80;
        vt->utf8_remaining = 1;
    } else if (byte >= 0xe0 && byte <= 0xef) {
        vt->utf8_value = byte & 0x0f;
        vt->utf8_minimum = 0x800;
        vt->utf8_remaining = 2;
    } else if (byte >= 0xf0 && byte <= 0xf4) {
        vt->utf8_value = byte & 0x07;
        vt->utf8_minimum = 0x10000;
        vt->utf8_remaining = 3;
    } else {
        put_codepoint(vt, 0xfffd);
    }
}

static void feed_ground_utf8(HdeCmdVt *vt, unsigned char byte)
{
    if (vt->utf8_remaining) {
        if ((byte & 0xc0) == 0x80) {
            vt->utf8_value = (vt->utf8_value << 6) | (byte & 0x3f);
            vt->utf8_remaining--;
            if (!vt->utf8_remaining) {
                uint32_t cp = vt->utf8_value;
                if (cp < vt->utf8_minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) cp = 0xfffd;
                put_codepoint(vt, cp);
            }
            return;
        }
        vt->utf8_remaining = 0;
        put_codepoint(vt, 0xfffd);
        /* The current non-continuation byte is a new character/control. */
    }
    ground_byte(vt, byte);
}

static void escape_byte(HdeCmdVt *vt, unsigned char byte)
{
    VtScreen *s = vt->screen;
    vt->parser = PARSER_GROUND;
    switch (byte) {
    case '[': csi_begin(vt); break;
    case ']': vt->parser = PARSER_OSC; vt->osc_length = 0; break;
    case 'P': case '^': case '_': vt->parser = PARSER_STRING; break;
    case '(': case ')': case '*': case '+': vt->parser = PARSER_CHARSET; break;
    case '7': save_cursor(s); break;
    case '8': restore_cursor(s); break;
    case 'D': line_feed(vt, 0); break;              /* IND */
    case 'E': line_feed(vt, 1); break;              /* NEL */
    case 'M':                                       /* RI */
        if (s->y == s->scroll_top) scroll_down(s, s->scroll_top, s->scroll_bottom, 1);
        else if (s->y > 0) s->y--;
        break;
    case 'c': hde_cmd_vt_reset(vt); break;
    case '\\': break;                              /* ST */
    case '=': case '>': break;                     /* keypad mode */
    default: break;
    }
}

static void parse_byte(HdeCmdVt *vt, unsigned char byte, HdeCmdVtReply reply,
                       HdeCmdVtTitle title, void *userdata)
{
    if (vt->parser == PARSER_GROUND) {
        feed_ground_utf8(vt, byte);
        return;
    }
    switch (vt->parser) {
    case PARSER_ESCAPE:
        escape_byte(vt, byte);
        break;
    case PARSER_CSI:
        csi_byte(vt, byte, reply, userdata);
        break;
    case PARSER_OSC:
        if (byte == 0x07) {
            send_osc_title(vt, title, userdata);
            vt->parser = PARSER_GROUND;
        } else if (byte == 0x1b) {
            vt->parser = PARSER_OSC_ESCAPE;
        } else if (vt->osc_length + 1 < sizeof vt->osc && byte >= 0x20) {
            vt->osc[vt->osc_length++] = (char)byte;
        }
        break;
    case PARSER_OSC_ESCAPE:
        if (byte == '\\') {
            send_osc_title(vt, title, userdata);
            vt->parser = PARSER_GROUND;
        } else {
            if (vt->osc_length + 2 < sizeof vt->osc) {
                vt->osc[vt->osc_length++] = '\033';
                vt->osc[vt->osc_length++] = (char)byte;
            }
            vt->parser = PARSER_OSC;
        }
        break;
    case PARSER_STRING:
        if (byte == 0x1b) vt->parser = PARSER_STRING_ESCAPE;
        else if (byte == 0x07) vt->parser = PARSER_GROUND;
        break;
    case PARSER_STRING_ESCAPE:
        vt->parser = byte == '\\' ? PARSER_GROUND : PARSER_STRING;
        break;
    case PARSER_CHARSET:
        vt->parser = PARSER_GROUND;
        break;
    default:
        vt->parser = PARSER_GROUND;
        break;
    }
}

HdeCmdVt *hde_cmd_vt_new(int rows, int columns)
{
    HdeCmdVt *vt = calloc(1, sizeof *vt);
    if (!vt) return NULL;
    screen_init(&vt->main_screen, rows, columns, 1);
    screen_init(&vt->alternate_screen, rows, columns, 0);
    if (!vt->main_screen.cells || !vt->alternate_screen.cells) {
        hde_cmd_vt_free(vt);
        return NULL;
    }
    vt->screen = &vt->main_screen;
    hde_cmd_vt_reset(vt);
    return vt;
}

void hde_cmd_vt_free(HdeCmdVt *vt)
{
    if (!vt) return;
    free(vt->main_screen.cells);
    free(vt->alternate_screen.cells);
    free(vt);
}

void hde_cmd_vt_reset(HdeCmdVt *vt)
{
    if (!vt) return;
    screen_reset(&vt->main_screen);
    screen_reset(&vt->alternate_screen);
    vt->screen = &vt->main_screen;
    vt->alternate_active = 0;
    vt->foreground = HDE_CMD_VT_DEFAULT_COLOR;
    vt->background = HDE_CMD_VT_DEFAULT_COLOR;
    vt->attributes = 0;
    vt->last_character = ' ';
    vt->cursor_visible = 1;
    vt->application_cursor = 0;
    vt->origin_mode = 0;
    vt->autowrap = 1;
    vt->insert_mode = 0;
    vt->newline_mode = 0;
    vt->bracketed_paste = 0;
    vt->mouse_mode = 0;
    vt->sgr_mouse = 0;
    vt->parser = PARSER_GROUND;
    vt->csi_count = 0;
    vt->csi_private = 0;
    vt->csi_intermediate = 0;
    vt->utf8_value = 0;
    vt->utf8_minimum = 0;
    vt->utf8_remaining = 0;
    vt->osc_length = 0;
}

void hde_cmd_vt_resize(HdeCmdVt *vt, int rows, int columns)
{
    if (!vt) return;
    rows = clamp_int(rows, 1, VT_MAX_ROWS);
    columns = clamp_int(columns, 1, VT_MAX_COLUMNS);
    screen_resize(&vt->main_screen, rows, columns);
    screen_resize(&vt->alternate_screen, rows, columns);
    vt->screen = vt->alternate_active ? &vt->alternate_screen : &vt->main_screen;
}

void hde_cmd_vt_feed(HdeCmdVt *vt, const char *bytes, size_t length,
                     HdeCmdVtReply reply, HdeCmdVtTitle title, void *userdata)
{
    if (!vt || !bytes) return;
    int was_following = vt->screen->view_offset == 0;
    for (size_t i = 0; i < length; i++)
        parse_byte(vt, (unsigned char)bytes[i], reply, title, userdata);
    if (was_following) vt->screen->view_offset = 0;
}

int hde_cmd_vt_rows(const HdeCmdVt *vt) { return vt ? vt->screen->rows : 0; }
int hde_cmd_vt_columns(const HdeCmdVt *vt) { return vt ? vt->screen->columns : 0; }

const HdeCmdVtCell *hde_cmd_vt_line(const HdeCmdVt *vt, int display_row)
{
    if (!vt || display_row < 0 || display_row >= vt->screen->rows) return NULL;
    return display_line(vt->screen, display_row);
}

int hde_cmd_vt_cursor_x(const HdeCmdVt *vt) { return vt ? vt->screen->x : 0; }
int hde_cmd_vt_cursor_y(const HdeCmdVt *vt) { return vt ? vt->screen->y : 0; }
int hde_cmd_vt_cursor_visible(const HdeCmdVt *vt) { return vt ? vt->cursor_visible : 0; }
int hde_cmd_vt_scrollback(const HdeCmdVt *vt) { return vt ? vt->screen->history : 0; }
int hde_cmd_vt_view_offset(const HdeCmdVt *vt) { return vt ? vt->screen->view_offset : 0; }
int hde_cmd_vt_application_cursor(const HdeCmdVt *vt) { return vt ? vt->application_cursor : 0; }
int hde_cmd_vt_bracketed_paste(const HdeCmdVt *vt) { return vt ? vt->bracketed_paste : 0; }
int hde_cmd_vt_mouse_mode(const HdeCmdVt *vt) { return vt ? vt->mouse_mode : 0; }
int hde_cmd_vt_sgr_mouse(const HdeCmdVt *vt) { return vt ? vt->sgr_mouse : 0; }

void hde_cmd_vt_scroll_view(HdeCmdVt *vt, int lines)
{
    if (!vt) return;
    VtScreen *s = vt->screen;
    s->view_offset = clamp_int(s->view_offset + lines, 0, s->history);
}
