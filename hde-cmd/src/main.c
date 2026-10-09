/* main.c — HDE Cmd command line. The terminal itself and its VT engine live in this folder. */
#include "app.h"

#include <stdio.h>
#include <string.h>

#define HDE_CMD_VERSION "1.0"

static void usage(FILE *out)
{
    fprintf(out,
            "Usage: hde-cmd [OPTIONS] [--] [COMMAND [ARG...]]\n"
            "       hde-cmd [OPTIONS] -e COMMAND [ARG...]\n"
            "\n"
            "Options:\n"
            "  -d, -C, --directory DIR      start in DIR (also --cwd, --working-directory)\n"
            "  -t, --title TITLE            set the initial window title\n"
            "  -e, --execute COMMAND        run COMMAND instead of the user's shell (also --command)\n"
            "      --vte                    use the optional VTE backend (when built with VTE)\n"
            "  -h, --help                   show this help\n"
            "      --version                show the version and available backends\n"
            "\n"
            "Examples:\n"
            "  hde-cmd\n"
            "  hde-cmd --directory ~/Projects\n"
            "  hde-cmd -C ~/Projects -- make check\n"
            "  hde-cmd -e git status\n"
            "\n"
            "In the window: right-click for Copy/Paste and terminal actions; Ctrl+Shift+T opens a new terminal,\n"
            "Ctrl+Shift+O opens a folder in a new terminal, and Ctrl+Shift+W closes this window. Ctrl+Shift+C/V\n"
            "copy and paste, Ctrl+Plus/Minus/0 changes font size, Shift+PageUp/PageDown scrolls, and F11 toggles\n"
            "full screen. The built-in VT engine is the default; VTE is an optional backend.\n");
}

static int fail(const char *message, const char *value)
{
    if (value) fprintf(stderr, "hde-cmd: %s: %s\n", message, value);
    else fprintf(stderr, "hde-cmd: %s\n", message);
    return 2;
}

int main(int argc, char **argv)
{
    HdeCmdOptions options = { 0 };
    int i = 1;
    for (; i < argc; i++) {
        const char *arg = argv[i];
        if (!strcmp(arg, "--help") || !strcmp(arg, "-h")) {
            usage(stdout);
            return 0;
        }
        if (!strcmp(arg, "--version")) {
#ifdef HDE_CMD_HAVE_VTE
            printf("hde-cmd (HDE) %s; built-in VT engine: yes; optional VTE backend: yes\n", HDE_CMD_VERSION);
#else
            printf("hde-cmd (HDE) %s; built-in VT engine: yes; optional VTE backend: no (install VTE development files and rebuild)\n",
                   HDE_CMD_VERSION);
#endif
            return 0;
        }
        if (!strcmp(arg, "--vte")) {
#ifndef HDE_CMD_HAVE_VTE
            return fail("this build has no VTE backend", "install libvte-2.91-dev (Debian/Ubuntu) or vte291-devel (Fedora) and rebuild");
#else
            options.use_vte = 1;
#endif
            continue;
        }
        if (!strcmp(arg, "-d") || !strcmp(arg, "-C") || !strcmp(arg, "--working-directory") ||
            !strcmp(arg, "--directory") || !strcmp(arg, "--cwd")) {
            if (++i >= argc) return fail("missing directory after", arg);
            options.working_directory = argv[i];
            continue;
        }
        if (!strncmp(arg, "--working-directory=", 20)) {
            options.working_directory = arg + 20;
            continue;
        }
        if (!strncmp(arg, "--directory=", 12)) {
            options.working_directory = arg + 12;
            continue;
        }
        if (!strncmp(arg, "--cwd=", 6)) {
            options.working_directory = arg + 6;
            continue;
        }
        if (!strcmp(arg, "-t") || !strcmp(arg, "--title")) {
            if (++i >= argc) return fail("missing title after", arg);
            options.title = argv[i];
            continue;
        }
        if (!strncmp(arg, "--title=", 8)) {
            options.title = arg + 8;
            continue;
        }
        if (!strcmp(arg, "-e") || !strcmp(arg, "--execute") || !strcmp(arg, "--command")) {
            if (++i >= argc) return fail("missing command after", arg);
            options.command = &argv[i];
            break;
        }
        if (!strcmp(arg, "--")) {
            if (i + 1 < argc) options.command = &argv[i + 1];
            break;
        }
        if (arg[0] == '-') return fail("unknown option", arg);
        /* A command written without -- is accepted as a convenience too. */
        options.command = &argv[i];
        break;
    }
    return hde_cmd_ui_run(&options, argc > 0 ? argv[0] : "hde-cmd");
}
