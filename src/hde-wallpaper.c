/* hde-wallpaper.c — see hde-wallpaper.h. GLib only: no GTK and no display, so the rules are testable. */
#include "hde-wallpaper.h"
#include <glib/gstdio.h>
#include <glob.h>
#include <stdlib.h>
#include <string.h>

#define WALLPAPER_GROUP "background-desktop"
#define WALLPAPER_KEY "wallpaper"

typedef struct {
    char *path;
    int width, height;
} WallpaperImage;

static void wallpaper_image_free(gpointer data)
{
    WallpaperImage *image = data;
    if (!image) return;
    g_free(image->path);
    g_free(image);
}

static gboolean has_suffix_ci(const char *text, const char *suffix)
{
    gsize text_len = strlen(text), suffix_len = strlen(suffix);
    return text_len >= suffix_len &&
           !g_ascii_strcasecmp(text + text_len - suffix_len, suffix);
}

static gboolean has_glob(const char *value)
{
    return strpbrk(value, "*?[") != NULL;
}

/* Trim spaces and optional quotes, and expand the "~" of the home directory. */
static char *wallpaper_value_clean(const char *value)
{
    char *clean = g_strdup(value ? value : "");
    g_strstrip(clean);
    gsize len = strlen(clean);
    if (len >= 2 && ((clean[0] == '"' && clean[len - 1] == '"') ||
                     (clean[0] == '\'' && clean[len - 1] == '\''))) {
        char *unquoted = g_strndup(clean + 1, len - 2);
        g_free(clean);
        clean = unquoted;
    }
    if (clean[0] == '~' && (clean[1] == '/' || clean[1] == '\0')) {
        char *expanded = g_strconcat(g_get_home_dir(), clean + 1, NULL);
        g_free(clean);
        clean = expanded;
    }
    return clean;
}

/* ---------- GNOME's gnome-background.xml ---------- */

typedef struct {
    GPtrArray *images;          /* WallpaperImage * */
    GString *size_text;
    GString *file_text;
    GString *wallpaper_text;
    gboolean in_size, in_file, in_wallpaper;
    gboolean file_had_size;
    int size_width, size_height;
    char *base_dir;
} XmlParser;

static void xml_add_image(XmlParser *parser, const char *text, int width, int height)
{
    char *path = g_strdup(text ? text : "");
    g_strstrip(path);
    if (!*path) {
        g_free(path);
        return;
    }
    char *full = g_path_is_absolute(path) ? g_strdup(path) : g_build_filename(parser->base_dir, path, NULL);
    WallpaperImage *image = g_new0(WallpaperImage, 1);
    image->path = full;
    image->width = width;
    image->height = height;
    g_ptr_array_add(parser->images, image);
    g_free(path);
}

static void xml_start_element(GMarkupParseContext *context, const char *name, const char **attribute_names,
                              const char **attribute_values, gpointer data, GError **error)
{
    (void)context;
    (void)error;
    XmlParser *parser = data;
    if (!strcmp(name, "size")) {
        parser->in_size = TRUE;
        parser->size_width = parser->size_height = 0;
        g_string_truncate(parser->size_text, 0);
        for (int i = 0; attribute_names && attribute_names[i]; i++) {
            if (!strcmp(attribute_names[i], "width"))
                parser->size_width = atoi(attribute_values[i]);
            else if (!strcmp(attribute_names[i], "height"))
                parser->size_height = atoi(attribute_values[i]);
        }
    } else if (!strcmp(name, "file")) {
        parser->in_file = TRUE;
        parser->file_had_size = FALSE;
        g_string_truncate(parser->file_text, 0);
    } else if (!strcmp(name, "wallpaper")) {
        /* Some themes write <wallpaper>/path</wallpaper> instead of <file><size>…</size></file>. */
        parser->in_wallpaper = TRUE;
        g_string_truncate(parser->wallpaper_text, 0);
    }
}

static void xml_end_element(GMarkupParseContext *context, const char *name, gpointer data, GError **error)
{
    (void)context;
    (void)error;
    XmlParser *parser = data;
    if (!strcmp(name, "size")) {
        xml_add_image(parser, parser->size_text->str, parser->size_width, parser->size_height);
        parser->file_had_size = TRUE;
        parser->in_size = FALSE;
        g_string_truncate(parser->size_text, 0);
    } else if (!strcmp(name, "file")) {
        if (!parser->file_had_size)
            xml_add_image(parser, parser->file_text->str, 0, 0);
        parser->in_file = FALSE;
        g_string_truncate(parser->file_text, 0);
    } else if (!strcmp(name, "wallpaper")) {
        xml_add_image(parser, parser->wallpaper_text->str, 0, 0);
        parser->in_wallpaper = FALSE;
        g_string_truncate(parser->wallpaper_text, 0);
    }
}

static void xml_text(GMarkupParseContext *context, const char *text, gsize length, gpointer data, GError **error)
{
    (void)context;
    (void)error;
    XmlParser *parser = data;
    if (parser->in_size)
        g_string_append_len(parser->size_text, text, (gssize)length);
    else if (parser->in_wallpaper)
        g_string_append_len(parser->wallpaper_text, text, (gssize)length);
    else if (parser->in_file)
        g_string_append_len(parser->file_text, text, (gssize)length);
}

static char *xml_choose_image(GPtrArray *images, int screen_width, int screen_height)
{
    WallpaperImage *best = NULL;
    gint64 best_score = G_MAXINT64;
    for (guint i = 0; i < images->len; i++) {
        WallpaperImage *image = g_ptr_array_index(images, i);
        if (!g_file_test(image->path, G_FILE_TEST_IS_REGULAR)) continue;
        gint64 score;
        if (image->width > 0 && image->height > 0 && screen_width > 0 && screen_height > 0) {
            gint64 width = image->width, height = image->height;
            gint64 screen_area = (gint64)screen_width * screen_height;
            gint64 image_area = width * height;
            gint64 area_diff = image_area > screen_area ? image_area - screen_area : screen_area - image_area;
            gint64 aspect_a = width * screen_height;
            gint64 aspect_b = height * screen_width;
            gint64 aspect_diff = aspect_a > aspect_b ? aspect_a - aspect_b : aspect_b - aspect_a;
            gboolean covers = width >= screen_width && height >= screen_height;
            /* Prefer a picture that covers the screen, then the closest area and aspect ratio. */
            score = (covers ? 0 : (G_GINT64_CONSTANT(1) << 50)) + area_diff + aspect_diff * 1000;
        } else {
            /* <file> or <wallpaper> without a size: only used when there is no sized entry. */
            score = G_GINT64_CONSTANT(1) << 60;
        }
        if (score < best_score) {
            best_score = score;
            best = image;
        }
    }
    return best ? g_strdup(best->path) : NULL;
}

char *hde_wallpaper_from_gnome_xml(const char *xml_path, int width, int height)
{
    if (!xml_path || !*xml_path) return NULL;
    char *contents = NULL;
    gsize length = 0;
    GError *error = NULL;
    if (!g_file_get_contents(xml_path, &contents, &length, &error)) {
        g_clear_error(&error);
        return NULL;
    }

    XmlParser parser = { 0 };
    parser.images = g_ptr_array_new_with_free_func(wallpaper_image_free);
    parser.size_text = g_string_new(NULL);
    parser.file_text = g_string_new(NULL);
    parser.wallpaper_text = g_string_new(NULL);
    parser.base_dir = g_path_get_dirname(xml_path);
    GMarkupParser markup = {
        .start_element = xml_start_element,
        .end_element = xml_end_element,
        .text = xml_text,
    };
    GMarkupParseContext *context = g_markup_parse_context_new(&markup, 0, &parser, NULL);
    gboolean parsed = g_markup_parse_context_parse(context, contents, (gssize)length, &error);
    if (parsed)
        parsed = g_markup_parse_context_end_parse(context, &error);
    g_markup_parse_context_free(context);
    g_clear_error(&error);
    g_free(contents);

    char *best = NULL;
    if (parsed || parser.images->len)
        best = xml_choose_image(parser.images, width, height);

    g_string_free(parser.size_text, TRUE);
    g_string_free(parser.file_text, TRUE);
    g_string_free(parser.wallpaper_text, TRUE);
    g_ptr_array_free(parser.images, TRUE);
    g_free(parser.base_dir);
    return best;
}

/* ---------- globs and paths ---------- */

static gint compare_strings(gconstpointer a, gconstpointer b)
{
    return g_strcmp0(*(char *const *)a, *(char *const *)b);
}

char *hde_wallpaper_expand_glob(const char *pattern)
{
    if (!pattern || !has_glob(pattern)) return NULL;
    glob_t matches;
    memset(&matches, 0, sizeof matches);
    int result = glob(pattern, GLOB_NOSORT, NULL, &matches);
    char *first = NULL;
    if (result == 0) {
        GPtrArray *files = g_ptr_array_new_with_free_func(g_free);
        for (size_t i = 0; i < matches.gl_pathc; i++) {
            if (g_file_test(matches.gl_pathv[i], G_FILE_TEST_IS_REGULAR))
                g_ptr_array_add(files, g_strdup(matches.gl_pathv[i]));
        }
        g_ptr_array_sort(files, compare_strings);
        if (files->len)
            first = g_strdup(g_ptr_array_index(files, 0));
        g_ptr_array_free(files, TRUE);
    }
    globfree(&matches);
    return first;
}

static char *resolve_file(const char *path, int width, int height)
{
    if (has_suffix_ci(path, ".xml"))
        return hde_wallpaper_from_gnome_xml(path, width, height);
    return g_canonicalize_filename(path, NULL);
}

char *hde_wallpaper_resolve(const char *value, int width, int height)
{
    if (!value) return NULL;
    char *clean = wallpaper_value_clean(value);
    char *resolved = NULL;
    if (!*clean) {
        /* nothing to do */
    } else if (g_file_test(clean, G_FILE_TEST_IS_REGULAR)) {
        resolved = resolve_file(clean, width, height);
    } else if (has_glob(clean)) {
        char *match = hde_wallpaper_expand_glob(clean);
        if (match) {
            resolved = resolve_file(match, width, height);
            g_free(match);
        }
    } else if (g_file_test(clean, G_FILE_TEST_IS_DIR)) {
        /* A folder: the first picture in it (handy for a theme folder). */
        char *pattern = g_build_filename(clean, "*", NULL);
        char *match = hde_wallpaper_expand_glob(pattern);
        if (match) {
            resolved = resolve_file(match, width, height);
            g_free(match);
        }
        g_free(pattern);
    }
    g_free(clean);
    return resolved;
}

/* ---------- desktop.ini ---------- */

char *hde_wallpaper_desktop_ini_path(void)
{
    const char *override = g_getenv("HDE_DESKTOP_INI");
    if (override && *override) return g_strdup(override);
    return g_build_filename(g_get_user_config_dir(), "hde", "desktop.ini", NULL);
}

char *hde_wallpaper_desktop_ini_get(const char *ini_path)
{
    if (!ini_path || !*ini_path) return NULL;
    GKeyFile *key_file = g_key_file_new();
    char *value = NULL;
    if (g_key_file_load_from_file(key_file, ini_path, G_KEY_FILE_NONE, NULL))
        value = g_key_file_get_string(key_file, WALLPAPER_GROUP, WALLPAPER_KEY, NULL);
    g_key_file_free(key_file);
    if (value) {
        g_strstrip(value);
        if (!*value) {
            g_free(value);
            value = NULL;
        }
    }
    return value;
}

gboolean hde_wallpaper_desktop_ini_write(const char *ini_path, const char *value)
{
    if (!ini_path || !*ini_path || !value) return FALSE;

    char *current = hde_wallpaper_desktop_ini_get(ini_path);
    gboolean unchanged = current && !strcmp(current, value);
    g_free(current);
    if (unchanged) return TRUE;

    GKeyFile *key_file = g_key_file_new();
    g_key_file_load_from_file(key_file, ini_path, G_KEY_FILE_KEEP_COMMENTS, NULL);
    g_key_file_set_string(key_file, WALLPAPER_GROUP, WALLPAPER_KEY, value);
    char *directory = g_path_get_dirname(ini_path);
    g_mkdir_with_parents(directory, 0755);
    gboolean written = g_key_file_save_to_file(key_file, ini_path, NULL);
    g_free(directory);
    g_key_file_free(key_file);
    return written;
}

/* ---------- what the system offers ---------- */

char *hde_wallpaper_system_source(void)
{
    const char *xml_override = g_getenv("HDE_WALLPAPER_XML");
    const char *xml = xml_override && *xml_override
        ? xml_override
        : "/usr/share/desktop-base/active-theme/wallpaper/gnome-background.xml";
    if (g_file_test(xml, G_FILE_TEST_IS_REGULAR)) {
        /* Only choose it when it names at least one picture that is really there; an old theme whose images were
         * removed must not leave the desktop black. */
        char *probe = hde_wallpaper_from_gnome_xml(xml, 0, 0);
        if (probe) {
            g_free(probe);
            return g_strdup(xml);
        }
    }

    const char *dir_override = g_getenv("HDE_BACKGROUNDS_DIR");
    char *user_backgrounds = g_build_filename(g_get_user_data_dir(), "backgrounds", NULL);
    const char *dirs[4];
    guint directory_count = 0;
    if (dir_override && *dir_override) {
        dirs[directory_count++] = dir_override;
    } else {
        dirs[directory_count++] = user_backgrounds;
        dirs[directory_count++] = "/usr/share/backgrounds";
        dirs[directory_count++] = "/usr/share/wallpapers";
        dirs[directory_count++] = "/usr/share/xfce4/backdrops";
    }
    static const char *const extensions[] = { "png", "jpg", "jpeg", "webp", "svg", NULL };

    char *source = NULL;
    for (guint i = 0; i < directory_count && !source; i++) {
        for (guint e = 0; extensions[e] && !source; e++) {
            char *pattern = g_strdup_printf("%s/*.%s", dirs[i], extensions[e]);
            char *match = hde_wallpaper_expand_glob(pattern);
            if (match) {
                source = pattern;
                pattern = NULL;
                g_free(match);
            }
            g_free(pattern);
        }
    }
    g_free(user_backgrounds);
    return source;
}
