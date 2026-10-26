/* nexsdk/settings.h — the settings of the desktop, as a program should read and write them.
 *
 * HDE keeps one file: ${XDG_CONFIG_HOME:-~/.config}/hde/settings.ini, with groups such as [settings],
 * [panel], [desktop], [hotkeys].  A program that adds a setting follows the same rules as HDE itself:
 *
 *   - read the file again before writing (another program may have written it meanwhile), so that nobody's
 *     change is lost;
 *   - keep the comments and the order of everything that was not changed — people look at this file;
 *   - write a temporary file and rename it over the old one, so that a crash never leaves half a file.
 *
 *   NexSdkSettings *cfg = nexsdk_settings_open();
 *   const char *theme = nexsdk_settings_get(cfg, "settings", "theme_index", "0");
 *   nexsdk_settings_set(cfg, "myapp", "last_folder", "/home/me/Pictures");
 *   nexsdk_settings_free(cfg);       // writes when something changed
 */
#ifndef NEXSDK_SETTINGS_H
#define NEXSDK_SETTINGS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NexSdkSettings NexSdkSettings;

/* ${XDG_CONFIG_HOME:-$HOME/.config}/hde/settings.ini, creating the directory when needed.
 * NULL only when there is no home directory at all. */
char *nexsdk_settings_path(void);

/* Open the desktop's settings file, or a specific one. */
NexSdkSettings *nexsdk_settings_open(void);
NexSdkSettings *nexsdk_settings_open_path(const char *path);

/* The file was changed by another program: read it again (the changes not written yet are kept). */
int nexsdk_settings_reload(NexSdkSettings *s);

/* Write the file if something was changed since it was read (`s->dirty`).  Returns 0 on success. */
int nexsdk_settings_save(NexSdkSettings *s);

/* Free it; unsaved changes are written first (so forgetting save() is not a bug in the program). */
void nexsdk_settings_free(NexSdkSettings *s);

/* group == NULL or "" reads the file without a group ([settings] is the usual one).  Values are trimmed of
 * the whitespace around them and of one pair of quotes.  Returns fallback when the key is not there. */
const char *nexsdk_settings_get(NexSdkSettings *s, const char *group, const char *key, const char *fallback);
int nexsdk_settings_get_int(NexSdkSettings *s, const char *group, const char *key, int fallback);
int nexsdk_settings_get_bool(NexSdkSettings *s, const char *group, const char *key, int fallback);

/* Set a value ("true"/"false" for booleans, as GTK's GKeyFile and Settings do).  An existing key is changed
 * in place; a new key goes to the end of its group, and a new group to the end of the file. */
int nexsdk_settings_set(NexSdkSettings *s, const char *group, const char *key, const char *value);
int nexsdk_settings_set_int(NexSdkSettings *s, const char *group, const char *key, int value);
int nexsdk_settings_set_bool(NexSdkSettings *s, const char *group, const char *key, int value);

/* Remove a key (1 when it was there). */
int nexsdk_settings_remove(NexSdkSettings *s, const char *group, const char *key);

/* 1 when there are changes that would be written by save(). */
int nexsdk_settings_dirty(const NexSdkSettings *s);

/* The path this object reads and writes; NULL for a NULL object. */
const char *nexsdk_settings_file(NexSdkSettings *s);

/* The whole file as text (malloc'd, free with free()): the same text save() would write.  For a preview,
 * for a log, or for a unit test. */
char *nexsdk_settings_to_text(NexSdkSettings *s);

/* Replace the content of the object with this INI text (does not touch the disk until save()). */
int nexsdk_settings_parse(NexSdkSettings *s, const char *text);

#ifdef __cplusplus
}
#endif
#endif
