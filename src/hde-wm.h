/* hde-wm.h — bảng window manager dùng chung cho hde-session và Hyggshi Settings (header-only, C thuần).
 *
 * Thứ tự cũng là thứ tự chọn của chế độ "auto": các WM dựa trên GTK (Metacity, Marco, Mutter, Muffin)
 * được ưu tiên trước Xfwm4/Openbox vì chúng vẽ thanh tiêu đề bằng chính theme GTK — Dark mode và theme
 * chọn trong Settings áp dụng luôn cho viền cửa sổ, và đổi ngay khi hde-xsettings phát theme mới.
 */
#ifndef HDE_WM_H
#define HDE_WM_H

#include <string.h>

typedef struct {
    const char *id;            /* giá trị wm= trong ~/.config/hde/settings.ini */
    const char *binary;
    const char *name;
    const char *const *args;   /* đối số khi chạy; --replace để thay WM đang chạy mà không cần đăng xuất */
    int gtk;                   /* 1 = WM dựa trên GTK (viền cửa sổ theo theme GTK / Dark mode) */
    int can_replace;           /* 1 = tự thay thế WM khác (giao thức WM_Sn), 0 = phải dừng WM cũ trước */
    const char *description;
} HdeWm;

static const char *const hde_wm_args_replace[] = { "--replace", NULL };
static const char *const hde_wm_args_mutter[] = { "--replace", "--x11", NULL };
static const char *const hde_wm_args_none[] = { NULL };

static const HdeWm hde_wms[] = {
    { "metacity", "metacity", "Metacity", hde_wm_args_replace, 1, 1,
      "GTK window manager from GNOME Flashback. Light and fast; title bars follow the GTK theme and Dark mode." },
    { "marco", "marco", "Marco", hde_wm_args_replace, 1, 1,
      "MATE's GTK window manager (Metacity fork) with an optional compositor." },
    { "mutter", "mutter", "Mutter", hde_wm_args_mutter, 1, 1,
      "GNOME's compositing GTK window manager (X11 mode). Smooth, needs working OpenGL." },
    { "muffin", "muffin", "Muffin", hde_wm_args_replace, 1, 1,
      "Cinnamon's compositing window manager (Mutter fork)." },
    { "xfwm4", "xfwm4", "Xfwm4", hde_wm_args_replace, 0, 1,
      "XFCE's window manager with a built-in compositor." },
    { "openbox", "openbox", "Openbox", hde_wm_args_replace, 0, 1,
      "Minimal and highly configurable (themes in ~/.config/openbox)." },
    { "icewm", "icewm", "IceWM", hde_wm_args_replace, 0, 1,
      "Classic, very lightweight window manager." },
    { "fluxbox", "fluxbox", "Fluxbox", hde_wm_args_none, 0, 0,
      "Lightweight window manager with window tabs." },
    { "nexwm", "nexwm", "NexWM", hde_wm_args_none, 0, 0,
      "Hyggshi's experimental window manager." },
};
#define HDE_N_WMS (sizeof hde_wms / sizeof hde_wms[0])

static inline const HdeWm *hde_wm_find(const char *id)
{
    if (!id) return NULL;
    for (unsigned i = 0; i < HDE_N_WMS; i++)
        if (!strcmp(hde_wms[i].id, id)) return &hde_wms[i];
    return NULL;
}

/* Đoán id từ tên WM đang chạy (_NET_WM_NAME của cửa sổ _NET_SUPPORTING_WM_CHECK). */
static inline const HdeWm *hde_wm_from_running_name(const char *name)
{
    if (!name) return NULL;
    char low[128];
    size_t n = strlen(name);
    if (n >= sizeof low) n = sizeof low - 1;
    for (size_t i = 0; i < n; i++) low[i] = (char)((name[i] >= 'A' && name[i] <= 'Z') ? name[i] + 32 : name[i]);
    low[n] = '\0';
    if (strstr(low, "muffin")) return hde_wm_find("muffin");
    for (unsigned i = 0; i < HDE_N_WMS; i++)
        if (strstr(low, hde_wms[i].id)) return &hde_wms[i];
    return NULL;
}

#endif
