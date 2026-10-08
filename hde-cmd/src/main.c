/* main.c — HDE Cmd command line. The terminal itself and its VT engine live in this folder. */
#include "app.h"

#include <stdio.h>
#include <string.h>

#define HDE_CMD_VERSION "1.0"

static void usage(FILE *out)
{
    fprintf(out,
            "Usage: hde-cmd [OPTIONS] [-- COMMAND [ARG...]]\n"
            "       hde-cmd -e COMMAND [ARG...]\n"
            "\n"
            "Options:\n"
            "  -d, --working-directory DIR  start in DIR\n"
            "  -t, --title TITLE            set the initial window title\n"
            "  -e, --execute COMMAND        run COMMAND instead of the user's shell\n"
            "      --vte                    use the optional VTE backend (when built with VTE)\n"
            "  -h, --help                   show this help\n"
            "      --version                show the version and available backends\n"
            "\n"
            "The built-in VT engine is the default. VTE is an optional, explicit backend; a build without VTE still has\n"
            "a working terminal. Ctrl+Shift+C/V copy and paste; Shift+PageUp/PageDown scroll the output.\n");
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
        if (!strcmp(arg, "-d") || !strcmp(arg, "--working-directory")) {
            if (++i >= argc) return fail("missing directory after", arg);
            options.working_directory = argv[i];
            continue;
        }
        if (!strcmp(arg, "-t") || !strcmp(arg, "--title")) {
            if (++i >= argc) return fail("missing title after", arg);
            options.title = argv[i];
            continue;
        }
        if (!strcmp(arg, "-e") || !strcmp(arg, "--execute")) {
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
