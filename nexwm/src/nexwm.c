/* nexwm.c — NexWM, the window manager of HDE: what the program is, which backend it runs, and with which
 * configuration.
 *
 *   nexwm                 the X11 window manager (when $DISPLAY is set), the Wayland compositor when it is not
 *   nexwm --x11           the X11 window manager, whatever $WAYLAND_DISPLAY says
 *   nexwm --wayland       the Wayland compositor (needs a build made with wlroots: see wayland.c)
 *   nexwm --replace       take the role over from a window manager that is already running (X11)
 *   nexwm --config FILE   read the key bindings and the settings from FILE instead of ~/.config/hde/nexwm.conf
 *   nexwm --version, --help
 *
 * The session starts it the way it starts Metacity or labwc (hde-session wm: see src/hde-wm.h), and Settings offers it
 * next to them. What it does once it runs is in x11.c (the window manager) and wayland.c (the compositor).
 *
 * Exit status: 0 the window manager ran and left cleanly; 2 the configuration file has a mistake; 3 the display (or
 * the role of window manager) is not there; 4 this build has no Wayland compositor.
 */
#include "nexwm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hde-build.h"      /* HDE_VERSION: the commit this was built from */

int nexwm_x11_run(const char *config_path, int replace);
int nexwm_wayland_run(const char *config_path, int replace, const char *session);

/* What this build has. NexWM is one program with two sides and each one is compiled in only when its library was
 * there (the Makefile looks for libxcb and wlroots with pkg-config), so the program has to say which ones it is —
 * otherwise a session that asked for a side this build does not have would look as if NexWM itself were broken.
 * tests/nexwm-test.sh and tests/fedora-test.sh read these lines to know what they can ask of it. */
static void build_report(FILE *out)
{
#ifdef NEXWM_HAVE_XCB
    fprintf(out, "X11 window manager (XCB): yes\n");
#else
    fprintf(out, "X11 window manager (XCB): no — build with libxcb: libxcb1-dev on Debian/Ubuntu, libxcb-devel on Fedora\n");
#endif
#ifdef NEXWM_HAVE_WLROOTS
    fprintf(out, "Wayland compositor (wlroots): yes (the compositor itself is still being written: nexwm/README.md)\n");
#else
    fprintf(out, "Wayland compositor (wlroots): no — build with libwlroots-dev (Debian/Ubuntu) or wlroots-devel (Fedora)\n");
#endif
}

static void usage(FILE *out)
{
    fprintf(out,
            "Usage: nexwm [--x11|--wayland] [--replace] [--config FILE]\n"
            "%s %s — HDE's own window manager: X11 (XCB) and Wayland (wlroots).\n"
            "\n"
            "  --x11              run as the X11 window manager (the default when $DISPLAY is set)\n"
            "  --wayland          run as the Wayland compositor (a build made with wlroots)\n"
            "  --session CMD      with --wayland: the session command to run inside the compositor (the one\n"
            "                     hde-session --wayland --wm nexwm starts it with)\n"
            "  --replace          take over from the window manager that is running (X11)\n"
            "  --config FILE      the configuration file (default %s)\n"
            "  -v, --version      print the version and the build\n"
            "  -h, --help         this help\n"
            "\n"
            "The configuration file (%s):\n"
            "  border 2                       the frame around a window, in pixels\n"
            "  focus click | mouse            click to focus (the default), or follow the pointer\n"
            "  desktops 4                     how many workspaces\n"
            "  colors 0x2a2a2a 0x3a86ff       the frame of an unfocused window, and of the focused one\n"
            "  key Super+Return spawn xterm   a key binding (Super, Ctrl, Alt, Shift + a key)\n"
            "\n"
            "Actions: spawn CMD, close, kill, next, prev, workspace N, move-to N, maximize, unmaximize, fullscreen,\n"
            "         snap left|right|up|down, quit. The defaults are in the README (nexwm/README.md), and what the\n"
            "         running window manager listens to is the _NEXWM_KEYS property of the root window (xprop).\n",
            NEXWM_NAME, NEXWM_RELEASE, "~/.config/hde/nexwm.conf", "~/.config/hde/nexwm.conf");
    fprintf(out, "\nWhat this build has:\n");
    build_report(out);
}

int main(int argc, char **argv)
{
    const char *config = NULL;
    const char *session = NULL;                 /* --session: the session command a compositor runs inside itself */
    int want_wayland = 0, want_x11 = 0, replace = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--x11")) { want_x11 = 1; continue; }
        if (!strcmp(a, "--wayland") || !strcmp(a, "--wl")) { want_wayland = 1; continue; }
        if (!strcmp(a, "--replace")) { replace = 1; continue; }
        if (!strcmp(a, "--config")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "nexwm: --config needs a file\n");
                return 1;
            }
            config = argv[++i];
            continue;
        }
        if (!strncmp(a, "--config=", 9)) { config = a + 9; continue; }
        if (!strcmp(a, "--session")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "nexwm: --session needs a command (the session to run in the compositor)\n");
                return 1;
            }
            session = argv[++i];
            continue;
        }
        if (!strncmp(a, "--session=", 10)) { session = a + 10; continue; }
        if (!strcmp(a, "-v") || !strcmp(a, "--version")) {
            printf("nexwm (%s) %s %s\n", NEXWM_NAME, NEXWM_RELEASE, HDE_VERSION);
            build_report(stdout);
            return 0;
        }
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage(stdout);
            return 0;
        }
        fprintf(stderr, "nexwm: unknown option '%s' (see nexwm --help)\n", a);
        return 1;
    }

    if (config) {
        /* say it now rather than after taking the screen over */
        FILE *f = fopen(config, "r");
        if (!f) {
            fprintf(stderr, "nexwm: cannot read the configuration file %s\n", config);
            return 2;
        }
        fclose(f);
    }

    char *default_config = NULL;
    if (!config) {
        default_config = nexwm_config_path();
        config = default_config;
    }

    int rc;
    if (want_wayland) {
        rc = nexwm_wayland_run(config, replace, session);
    } else if (want_x11 || getenv("DISPLAY")) {
        rc = nexwm_x11_run(config, replace);
    } else {
        fprintf(stderr, "nexwm: no display: set $DISPLAY for the X11 window manager, or start it with --wayland\n");
        rc = 3;
    }
    free(default_config);
    return rc;
}
