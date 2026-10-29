/* nexsdk/examples/settings-theme.c — read the desktop's theme, and remember something of your own.
 *
 *   ./settings-theme                 what the theme is
 *   ./settings-theme dark            choose the dark style (1 light, 2 dark)
 *   ./settings-theme last-folder /x  remember a folder of your application
 *
 * The file it edits is ${XDG_CONFIG_HOME:-~/.config}/hde/settings.ini, the same one hde-settings writes;
 * comments and the keys it does not touch stay exactly as they were.
 */
#include <nexsdk/nexsdk.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    NexSdkSettings *cfg = nexsdk_settings_open();
    if (!cfg) {
        fprintf(stderr, "no settings file to open\n");
        return 1;
    }
    printf("Settings : %s\n", nexsdk_settings_file(cfg));

    if (argc >= 3 && !strcmp(argv[1], "dark")) {
        nexsdk_settings_set(cfg, "settings", "theme_index", argv[2]);
        nexsdk_settings_save(cfg);
        printf("Style    : %s (written)\n", argv[2]);
    } else if (argc >= 4 && !strcmp(argv[1], "last-folder")) {
        nexsdk_settings_set(cfg, "example", "last_folder", argv[2]);
        nexsdk_settings_save(cfg);
        printf("Remembered %s\n", argv[2]);
    }

    int theme = nexsdk_settings_get_int(cfg, "settings", "theme_index", 0);
    printf("Style    : %s\n", theme == 1 ? "light" : theme == 2 ? "dark" : "the GTK theme's own");
    printf("Accent   : %s\n", nexsdk_settings_get(cfg, "settings", "accent", "automatic"));
    printf("Font     : %s\n", nexsdk_settings_get(cfg, "settings", "font", "the theme's own"));
    printf("Panel    : %s, %d px high\n", nexsdk_settings_get(cfg, "panel", "position", "bottom"),
           nexsdk_settings_get_int(cfg, "panel", "height", 40));
    printf("Example  : %s\n", nexsdk_settings_get(cfg, "example", "last_folder", "(nothing remembered yet)"));

    nexsdk_settings_free(cfg);
    return 0;
}
