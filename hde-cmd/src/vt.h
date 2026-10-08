/* vt.h — the small, self-contained VT screen model used by hde-cmd's default backend.
 *
 * This interface deliberately has no GTK/GLib/VTE dependency so the parser and screen model can be tested on their own.
 */
#ifndef HDE_CMD_VT_H
#define HDE_CMD_VT_H

#include <stddef.h>
#include <stdint.h>

typedef struct HdeCmdVt HdeCmdVt;

enum {
    HDE_CMD_VT_BOLD       = 1u << 0,
    HDE_CMD_VT_ITALIC     = 1u << 1,
    HDE_CMD_VT_UNDERLINE  = 1u << 2,
    HDE_CMD_VT_BLINK      = 1u << 3,
    HDE_CMD_VT_INVERSE    = 1u << 4,
    HDE_CMD_VT_STRIKE     = 1u << 5,
    HDE_CMD_VT_WIDE_CONT  = 1u << 7
};

#define HDE_CMD_VT_DEFAULT_COLOR UINT32_C(0xffffffff)
#define HDE_CMD_VT_SCROLLBACK 2000
#define HDE_CMD_VT_COMBINING 3

typedef struct {
    uint32_t codepoint;                       /* 0 means a blank cell */
    uint32_t combining[HDE_CMD_VT_COMBINING];  /* marks attached to the preceding base character */
    uint32_t foreground;                      /* 0xRRGGBB or HDE_CMD_VT_DEFAULT_COLOR */
    uint32_t background;                      /* 0xRRGGBB or HDE_CMD_VT_DEFAULT_COLOR */
    uint8_t  combining_count;
    uint8_t  attributes;
} HdeCmdVtCell;

typedef void (*HdeCmdVtReply)(void *userdata, const char *bytes, size_t length);
typedef void (*HdeCmdVtTitle)(void *userdata, const char *title);

HdeCmdVt *hde_cmd_vt_new(int rows, int columns);
void      hde_cmd_vt_free(HdeCmdVt *vt);
void      hde_cmd_vt_reset(HdeCmdVt *vt);
void      hde_cmd_vt_resize(HdeCmdVt *vt, int rows, int columns);
void      hde_cmd_vt_feed(HdeCmdVt *vt, const char *bytes, size_t length,
                          HdeCmdVtReply reply, HdeCmdVtTitle title, void *userdata);

int                   hde_cmd_vt_rows(const HdeCmdVt *vt);
int                   hde_cmd_vt_columns(const HdeCmdVt *vt);
const HdeCmdVtCell   *hde_cmd_vt_line(const HdeCmdVt *vt, int display_row);
int                   hde_cmd_vt_cursor_x(const HdeCmdVt *vt);
int                   hde_cmd_vt_cursor_y(const HdeCmdVt *vt);
int                   hde_cmd_vt_cursor_visible(const HdeCmdVt *vt);
int                   hde_cmd_vt_scrollback(const HdeCmdVt *vt);
int                   hde_cmd_vt_view_offset(const HdeCmdVt *vt);
void                  hde_cmd_vt_scroll_view(HdeCmdVt *vt, int lines);
int                   hde_cmd_vt_application_cursor(const HdeCmdVt *vt);
int                   hde_cmd_vt_bracketed_paste(const HdeCmdVt *vt);
int                   hde_cmd_vt_mouse_mode(const HdeCmdVt *vt);
int                   hde_cmd_vt_sgr_mouse(const HdeCmdVt *vt);

#endif /* HDE_CMD_VT_H */
