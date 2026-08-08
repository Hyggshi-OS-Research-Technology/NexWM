#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

/*
 * main.c - Entry point for NexWM
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include "wm.h"
#include "config.h"
#include "log.h"

#define NEXWM_VERSION "0.1.0"

static void print_usage(const char *prog)
{
    printf("Usage: %s [OPTIONS]\n", prog);
    printf("\nNexWM - Nex Window Manager\n");
    printf("A modern, lightweight X11 window manager\n\n");
    printf("Options:\n");
    printf("  -c, --config FILE    Use configuration file FILE\n");
    printf("  -d, --debug          Enable debug logging\n");
    printf("  -v, --version        Show version information\n");
    printf("  -h, --help           Show this help message\n\n");
}

static void print_version(void)
{
    printf("NexWM version %s\n", NEXWM_VERSION);
    printf("Part of the Nex / Hyggshi OS ecosystem\n\n");
}

int main(int argc, char **argv)
{
    const char *config_path = NULL;
    int debug_mode = 0;

    static struct option long_options[] = {
        {"config",  required_argument, 0, 'c'},
        {"debug",   no_argument,       0, 'd'},
        {"version", no_argument,       0, 'v'},
        {"help",    no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "c:dvh", long_options, NULL)) != -1) {
        switch (opt) {
            case 'c': config_path = optarg; break;
            case 'd': debug_mode = 1; break;
            case 'v': print_version(); return EXIT_SUCCESS;
            case 'h':
            default: print_usage(argv[0]); return (opt == 'h') ? EXIT_SUCCESS : EXIT_FAILURE;
        }
    }

    nex_log_init(debug_mode ? NEX_LOG_DEBUG : NEX_LOG_INFO);
    NEX_INFO("========================================");
    NEX_INFO("NexWM starting");
    NEX_INFO("Version: %s", NEXWM_VERSION);
    NEX_INFO("========================================");

    if (config_path) NEX_INFO("Using config: %s", config_path);
    if (nex_config_load(config_path) < 0) {
        NEX_WARN("Failed to load config, using defaults");
        nex_config_default();
    }

    if (nex_wm_init() < 0) {
        NEX_FATAL("Failed to initialize window manager");
        return EXIT_FAILURE;
    }

    nex_wm_run();
    nex_wm_cleanup();
    return EXIT_SUCCESS;
}
