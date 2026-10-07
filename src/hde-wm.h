/* hde-wm.h — window manager table shared by hde-session and Hyggshi Settings (header-only, plain C).
 *
 * The order is also the selection order of "auto" mode: GTK-based WMs (Metacity, Marco, Mutter, Muffin)
 * come before Xfwm4/Openbox because they draw title bars with the GTK theme itself — Dark mode and the theme
 * chosen in Settings apply to window borders too, and switch immediately when hde-xsettings publishes a new theme.
 */
#ifndef HDE_WM_H
#define HDE_WM_H

#include <string.h>

typedef struct {
    const char *id;            /* the wm= value in ~/.config/hde/settings.ini */
    const char *binary;
    const char *name;
    const char *const *args;   /* launch arguments; --replace takes over from the running WM without logging out */
    int gtk;                   /* 1 = GTK-based WM (window borders follow the GTK theme / Dark mode) */
    int can_replace;           /* 1 = replaces another WM by itself (WM_Sn protocol), 0 = the old WM must be stopped first */
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

/* Guess the id from the name of the running WM (_NET_WM_NAME of the _NET_SUPPORTING_WM_CHECK window). */
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
