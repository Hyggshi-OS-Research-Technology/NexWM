/* tests/wallpaper-test.c — where the wallpaper comes from (src/hde-wallpaper.c), without GTK or a screen:
 *   - GNOME's gnome-background.xml: the <size> variant closest to the screen
 *   - a shell-style pattern such as *.png in /usr/share/backgrounds
 *   - ~/.config/hde/desktop.ini: [background-desktop] wallpaper=…
 *   - the source HDE picks when nothing was chosen (HDE_WALLPAPER_XML / HDE_BACKGROUNDS_DIR make it testable)
 */
#include "hde-wallpaper.h"
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>

static int fails;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) fails++;
}

static void write_file(const char *path, const char *contents)
{
    GError *error = NULL;
    if (!g_file_set_contents(path, contents, -1, &error)) {
        fprintf(stderr, "cannot write %s: %s\n", path, error->message);
        g_clear_error(&error);
        fails++;
    }
}

static void remove_tree(const char *path)
{
    if (g_file_test(path, G_FILE_TEST_IS_DIR) && !g_file_test(path, G_FILE_TEST_IS_SYMLINK)) {
        GDir *dir = g_dir_open(path, 0, NULL);
        if (dir) {
            const char *name;
            while ((name = g_dir_read_name(dir))) {
                char *child = g_build_filename(path, name, NULL);
                remove_tree(child);
                g_free(child);
            }
            g_dir_close(dir);
        }
        g_rmdir(path);
    } else {
        g_remove(path);
    }
}

static int same_path(const char *a, const char *b)
{
    return a && b && !strcmp(a, b);
}

int main(void)
{
    GError *error = NULL;
    char *tmp = g_dir_make_tmp("hde-wallpaper-test-XXXXXX", &error);
    if (!tmp) {
        fprintf(stderr, "cannot make a temporary directory: %s\n", error ? error->message : "?");
        g_clear_error(&error);
        return 2;
    }

    char *small = g_build_filename(tmp, "small.png", NULL);
    char *right = g_build_filename(tmp, "right.png", NULL);
    char *wide = g_build_filename(tmp, "wide.png", NULL);
    char *a_png = g_build_filename(tmp, "a.png", NULL);
    char *b_png = g_build_filename(tmp, "b.png", NULL);
    char *xml = g_build_filename(tmp, "gnome-background.xml", NULL);
    char *ini = g_build_filename(tmp, "desktop.ini", NULL);
    write_file(small, "small");
    write_file(right, "right");
    write_file(wide, "wide");
    write_file(a_png, "a");
    write_file(b_png, "b");
    write_file(xml,
        "<?xml version=\"1.0\"?>\n"
        "<background>\n"
        "  <static>\n"
        "    <duration>86400.0</duration>\n"
        "    <file>\n"
        "      <size width=\"1280\" height=\"800\">small.png</size>\n"
        "      <size width=\"1920\" height=\"1080\">right.png</size>\n"
        "      <size width=\"2520\" height=\"1080\">wide.png</size>\n"
        "    </file>\n"
        "  </static>\n"
        "</background>\n");

    char *best = hde_wallpaper_from_gnome_xml(xml, 1920, 1080);
    check(same_path(best, right), "gnome-background.xml: the 1920x1080 size is picked for a 1920x1080 screen");
    g_free(best);
    best = hde_wallpaper_from_gnome_xml(xml, 1366, 768);
    check(same_path(best, right), "gnome-background.xml: the smallest picture that covers a 1366x768 screen is picked");
    g_free(best);
    best = hde_wallpaper_from_gnome_xml(xml, 3840, 2160);
    check(same_path(best, right), "gnome-background.xml: without a size that covers the screen, the closest shape is picked");
    g_free(best);
    best = hde_wallpaper_resolve(xml, 1920, 1080);
    check(same_path(best, right), "a .xml wallpaper value is resolved to the picture inside it");
    g_free(best);

    char *pattern = g_build_filename(tmp, "*.png", NULL);
    char *first = hde_wallpaper_expand_glob(pattern);
    check(same_path(first, a_png), "a *.png pattern expands to the first match, sorted");
    g_free(first);
    best = hde_wallpaper_resolve(pattern, 1920, 1080);
    check(same_path(best, a_png), "a *.png wallpaper value is resolved to a picture");
    g_free(best);

    check(hde_wallpaper_desktop_ini_write(ini, xml), "desktop.ini is written");
    char *value = hde_wallpaper_desktop_ini_get(ini);
    check(same_path(value, xml), "desktop.ini keeps the source (the XML, not the picture)");
    g_free(value);
    char *contents = NULL;
    check(g_file_get_contents(ini, &contents, NULL, NULL) &&
          strstr(contents, "[background-desktop]") && strstr(contents, "wallpaper="),
          "desktop.ini has the [background-desktop] wallpaper= shape asked for");
    g_free(contents);
    value = hde_wallpaper_desktop_ini_get(ini);
    best = hde_wallpaper_resolve(value, 1920, 1080);
    check(same_path(best, right), "the wallpaper named by desktop.ini can be applied");
    g_free(value);
    g_free(best);

    check(hde_wallpaper_desktop_ini_write(ini, pattern), "desktop.ini can name a *.png pattern");
    value = hde_wallpaper_desktop_ini_get(ini);
    check(same_path(value, pattern), "the pattern is kept as written");
    g_free(value);
    value = hde_wallpaper_desktop_ini_get(ini);
    best = hde_wallpaper_resolve(value, 1920, 1080);
    check(same_path(best, a_png), "the pattern in desktop.ini is resolved to a picture");
    g_free(value);
    g_free(best);

    g_setenv("HDE_WALLPAPER_XML", xml, TRUE);
    char *source = hde_wallpaper_system_source();
    check(same_path(source, xml), "the active theme's gnome-background.xml is the first system choice");
    g_free(source);

    char *broken_xml = g_build_filename(tmp, "broken-theme.xml", NULL);
    write_file(broken_xml,
        "<background><static><file>"
        "<size width=\"1920\" height=\"1080\">missing.png</size>"
        "</file></static></background>\n");
    g_setenv("HDE_WALLPAPER_XML", broken_xml, TRUE);
    g_setenv("HDE_BACKGROUNDS_DIR", tmp, TRUE);
    source = hde_wallpaper_system_source();
    char *expected_pattern = g_build_filename(tmp, "*.png", NULL);
    if (!same_path(source, expected_pattern))
        fprintf(stderr, "INFO: system source \"%s\", expected \"%s\"\n", source ? source : "(null)", expected_pattern);
    check(same_path(source, expected_pattern),
          "a theme XML with no picture left falls back to the first pictures of the background folder");
    g_free(expected_pattern);
    g_free(source);
    g_unsetenv("HDE_BACKGROUNDS_DIR");
    g_unsetenv("HDE_WALLPAPER_XML");
    g_free(broken_xml);

    g_free(pattern);
    g_free(xml);
    g_free(ini);
    g_free(small);
    g_free(right);
    g_free(wide);
    g_free(a_png);
    g_free(b_png);
    remove_tree(tmp);
    g_free(tmp);

    if (fails) printf("== wallpaper: %d FAILED\n", fails);
    else puts("== wallpaper: all passed");
    return fails ? 1 : 0;
}
