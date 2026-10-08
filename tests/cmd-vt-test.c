/* cmd-vt-test.c — tests for HDE Cmd's built-in VT engine, with no GTK or display server. */
#include "vt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests, failed;

#define CHECK(condition, ...) do { \
    tests++; \
    if (condition) printf("PASS: cmd-vt: " __VA_ARGS__); \
    else { printf("FAIL: cmd-vt: " __VA_ARGS__); failed++; } \
    putchar('\n'); \
} while (0)

static void feed(HdeCmdVt *vt, const char *text)
{
    hde_cmd_vt_feed(vt, text, strlen(text), NULL, NULL, NULL);
}

static char cell_char(const HdeCmdVt *vt, int row, int col)
{
    const HdeCmdVtCell *line = hde_cmd_vt_line(vt, row);
    if (!line || col < 0 || col >= hde_cmd_vt_columns(vt)) return '?';
    return line[col].codepoint < 128 ? (char)line[col].codepoint : '?';
}

static void line_text(const HdeCmdVt *vt, int row, char *out, size_t size)
{
    size_t at = 0;
    const HdeCmdVtCell *line = hde_cmd_vt_line(vt, row);
    int columns = hde_cmd_vt_columns(vt);
    if (!line || size == 0) return;
    for (int x = 0; x < columns && at + 1 < size; x++) {
        if (line[x].attributes & HDE_CMD_VT_WIDE_CONT) continue;
        uint32_t cp = line[x].codepoint;
        out[at++] = cp > 0 && cp < 128 ? (char)cp : ' ';
    }
    out[at] = '\0';
}

struct Capture { char bytes[256]; char title[256]; size_t length; };

static void capture_reply(void *userdata, const char *bytes, size_t length)
{
    struct Capture *c = userdata;
    if (length > sizeof c->bytes - c->length - 1) length = sizeof c->bytes - c->length - 1;
    memcpy(c->bytes + c->length, bytes, length);
    c->length += length;
    c->bytes[c->length] = '\0';
}

static void capture_title(void *userdata, const char *title)
{
    struct Capture *c = userdata;
    snprintf(c->title, sizeof c->title, "%s", title);
}

int main(void)
{
    HdeCmdVt *vt = hde_cmd_vt_new(3, 10);
    if (!vt) {
        fprintf(stderr, "FAIL: cmd-vt: could not allocate the terminal model\n");
        return 1;
    }
    CHECK(hde_cmd_vt_rows(vt) == 3 && hde_cmd_vt_columns(vt) == 10, "the screen keeps its requested dimensions");
    CHECK(cell_char(vt, 0, 0) == ' ' && hde_cmd_vt_cursor_x(vt) == 0 && hde_cmd_vt_cursor_y(vt) == 0,
          "a new screen is blank and its cursor starts at the origin");

    feed(vt, "hello");
    CHECK(cell_char(vt, 0, 0) == 'h' && cell_char(vt, 0, 4) == 'o' && hde_cmd_vt_cursor_x(vt) == 5,
          "plain text is placed at the cursor");

    feed(vt, "\033[2;4H!");
    CHECK(cell_char(vt, 1, 3) == '!' && hde_cmd_vt_cursor_x(vt) == 4 && hde_cmd_vt_cursor_y(vt) == 1,
          "CSI row and column positioning is one-based");

    feed(vt, "\033[2K");
    CHECK(cell_char(vt, 1, 0) == ' ' && cell_char(vt, 1, 3) == ' ' && cell_char(vt, 1, 9) == ' ',
          "CSI erase-in-line mode 2 clears the entire current line");

    feed(vt, "\033[1;1H\033[1;31;44mR");
    const HdeCmdVtCell *line = hde_cmd_vt_line(vt, 0);
    CHECK(line[0].foreground == 0xcd0000 && line[0].background == 0x0000ee &&
          (line[0].attributes & HDE_CMD_VT_BOLD), "SGR sets ANSI foreground, background and bold");
    feed(vt, "\033[0m\033[1;2H\033[38;5;196;48;2;1;2;3mC");
    line = hde_cmd_vt_line(vt, 0);
    CHECK(line[1].foreground == 0xff0000 && line[1].background == 0x010203,
          "SGR supports the 256-colour and true-colour forms");

    feed(vt, "\033[?25l\033[?1h\033[?2004h\033[?1000h\033[?1006h");
    CHECK(!hde_cmd_vt_cursor_visible(vt) && hde_cmd_vt_application_cursor(vt) &&
          hde_cmd_vt_bracketed_paste(vt), "DEC modes control cursor visibility, cursor keys and bracketed paste");
    CHECK(hde_cmd_vt_mouse_mode(vt) == 1000 && hde_cmd_vt_sgr_mouse(vt), "xterm mouse tracking modes are recorded");

    feed(vt, "\033[?25h\033[?1l\033[?2004l\033[?1000l\033[?1006l\033[0m");
    hde_cmd_vt_reset(vt);
    feed(vt, "one\r\ntwo\r\ntri\r\nfour");
    char text[32];
    line_text(vt, 0, text, sizeof text);
    CHECK(!strncmp(text, "two", 3), "scrolling the full screen moves the preceding row up");
    line_text(vt, 1, text, sizeof text);
    CHECK(!strncmp(text, "tri", 3) && hde_cmd_vt_scrollback(vt) == 1,
          "output at the bottom creates a scrollback line");
    hde_cmd_vt_scroll_view(vt, 1);
    line_text(vt, 0, text, sizeof text);
    CHECK(!strncmp(text, "one", 3) && hde_cmd_vt_view_offset(vt) == 1,
          "scrolling the view up reveals previous output");
    hde_cmd_vt_scroll_view(vt, -1);
    CHECK(hde_cmd_vt_view_offset(vt) == 0, "scrolling the view down returns to live output");

    feed(vt, "\033[2;1H\033[2Kbottom");
    feed(vt, "\033[2;1H\033[1L");
    line_text(vt, 1, text, sizeof text);
    CHECK(text[0] == ' ', "CSI insert-lines creates a blank row at the cursor");
    feed(vt, "\033[2;1H\033[1M");
    CHECK(cell_char(vt, 1, 0) == 'b', "CSI delete-lines scrolls the region upward");

    hde_cmd_vt_reset(vt);
    feed(vt, "main\033[?1049halt\033[?1049l");
    line_text(vt, 0, text, sizeof text);
    CHECK(!strncmp(text, "main", 4), "the alternate screen preserves and restores the main screen");
    feed(vt, "\033[2;3H\033[6n");
    struct Capture capture = { 0 };
    hde_cmd_vt_feed(vt, "\033[6n", 4, capture_reply, capture_title, &capture);
    CHECK(!strcmp(capture.bytes, "\033[2;3R"), "device-status report returns the cursor position");
    hde_cmd_vt_feed(vt, "\033]0;Cmd title\007", 14, capture_reply, capture_title, &capture);
    CHECK(!strcmp(capture.title, "Cmd title"), "OSC 0 sends the window title to the UI");

    hde_cmd_vt_reset(vt);
    feed(vt, "1234567890");
    CHECK(hde_cmd_vt_cursor_x(vt) == 9 && hde_cmd_vt_cursor_y(vt) == 0,
          "writing at the right edge defers automatic wrapping until the next character");
    feed(vt, "X");
    CHECK(cell_char(vt, 1, 0) == 'X' && hde_cmd_vt_cursor_y(vt) == 1,
          "the next character wraps to the following row");

    hde_cmd_vt_reset(vt);
    feed(vt, "keep\033[2;3Hx");
    hde_cmd_vt_resize(vt, 4, 12);
    CHECK(hde_cmd_vt_rows(vt) == 4 && hde_cmd_vt_columns(vt) == 12 && cell_char(vt, 1, 0) == 'k',
          "resizing keeps existing screen content and adopts the new dimensions");

    hde_cmd_vt_free(vt);
    printf("\ncmd-vt-test: %d passed, %d failed\n", tests, failed);
    return failed ? 1 : 0;
}
