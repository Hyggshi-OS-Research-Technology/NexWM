/* nexsdk/src/hde-settings.c — ~/.config/hde/settings.ini, read and written without losing anything.
 *
 * The file is kept as its lines: a comment, a blank line, a [group] or a key=value.  Changing a value edits
 * that line; adding a key inserts a line at the end of its group; removing a key removes its line.  So the
 * comments that explain the settings survive a program writing one of them, and the file stays diff-able —
 * which matters because this is a file people open in an editor.
 *
 * Writing goes to settings.ini.new and is renamed over the old file: a program that is interrupted while it
 * writes cannot leave a half-written settings file behind.
 */
#define _POSIX_C_SOURCE 200809L

#include "nexsdk/nexsdk.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

typedef enum { LINE_BLANK, LINE_COMMENT, LINE_GROUP, LINE_KEY } LineKind;

typedef struct {
    char *text;      /* the line as it is written, without the newline */
    LineKind kind;
    char *group;     /* for LINE_KEY: the group it belongs to ("" before the first [group]) */
    char *key;       /* for LINE_KEY, and for LINE_GROUP the name of the group */
    char *value;     /* for LINE_KEY */
} Line;

struct NexSdkSettings {
    char *path;
    Line *lines;
    int count;
    int cap;
    int dirty;
    int missing;     /* the file did not exist: a new one is written on save() */
};

/* ------------------------------------------------------------------ small helpers */

static char *dup_range(const char *s, size_t n)
{
    char *out = malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

static char *trim_dup(const char *s, size_t n)
{
    while (n > 0 && isspace((unsigned char)*s)) {
        s++;
        n--;
    }
    while (n > 0 && isspace((unsigned char)s[n - 1])) n--;
    return dup_range(s, n);
}

static int lines_reserve(NexSdkSettings *s, int need)
{
    if (need <= s->cap) return 0;
    int cap = s->cap ? s->cap * 2 : 32;
    while (cap < need) cap *= 2;
    Line *nl = realloc(s->lines, (size_t)cap * sizeof *nl);
    if (!nl) return -1;
    s->lines = nl;
    s->cap = cap;
    return 0;
}

static void line_clear(Line *l)
{
    free(l->text);
    free(l->group);
    free(l->key);
    free(l->value);
    memset(l, 0, sizeof *l);
}

static void settings_clear(NexSdkSettings *s)
{
    for (int i = 0; i < s->count; i++) line_clear(&s->lines[i]);
    s->count = 0;
}

/* A line from text -> kind + parts.  Returns 0 when it was understood. */
static int parse_line(Line *l, const char *text, size_t len, const char *group)
{
    l->text = dup_range(text, len);
    if (!l->text) return -1;

    const char *p = text;
    size_t n = len;
    while (n > 0 && isspace((unsigned char)*p)) {
        p++;
        n--;
    }
    if (n == 0) {
        l->kind = LINE_BLANK;
        return 0;
    }
    if (*p == '#' || *p == ';') {
        l->kind = LINE_COMMENT;
        return 0;
    }
    if (*p == '[') {
        const char *close = memchr(p, ']', n);
        if (!close) {
            l->kind = LINE_COMMENT;   /* a broken group line is left alone */
            return 0;
        }
        l->kind = LINE_GROUP;
        l->key = trim_dup(p + 1, (size_t)(close - p - 1));
        return l->key ? 0 : -1;
    }
    const char *eq = memchr(p, '=', n);
    if (!eq) {
        l->kind = LINE_COMMENT;       /* neither a setting nor a group: keep it as it is */
        return 0;
    }
    l->kind = LINE_KEY;
    l->key = trim_dup(p, (size_t)(eq - p));
    const char *v = eq + 1;
    size_t vlen = n - (size_t)(v - p);

    /* An inline comment: "#" or ";" at the start of the value or after a space, outside quotes.
     * "theme_index=2   # 2 = Dark" keeps 2, and "url=x#y" keeps x#y. */
    char quote = 0;
    for (size_t i = 0; i < vlen; i++) {
        char c = v[i];
        if (quote) {
            if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            continue;
        }
        if ((c == '#' || c == ';') && (i == 0 || isspace((unsigned char)v[i - 1]))) {
            vlen = i;
            break;
        }
    }

    /* one pair of quotes is part of the syntax, not of the value */
    const char *vt = v;
    size_t vn = vlen;
    while (vn > 0 && isspace((unsigned char)*vt)) {
        vt++;
        vn--;
    }
    while (vn > 0 && isspace((unsigned char)vt[vn - 1])) vn--;
    if (vn >= 2 && (vt[0] == '"' || vt[0] == '\'') && vt[vn - 1] == vt[0]) {
        vt++;
        vn -= 2;
    }
    while (vn > 0 && isspace((unsigned char)*vt)) {
        vt++;
        vn--;
    }
    while (vn > 0 && isspace((unsigned char)vt[vn - 1])) vn--;
    l->value = dup_range(vt, vn);
    l->group = strdup(group ? group : "");
    return l->key && l->value && l->group ? 0 : -1;
}

/* ------------------------------------------------------------------ open / free */

char *nexsdk_settings_path(void)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    char dir[1200];
    if (xdg && *xdg)
        snprintf(dir, sizeof dir, "%s/hde", xdg);
    else if (home && *home)
        snprintf(dir, sizeof dir, "%s/.config/hde", home);
    else
        return NULL;
    char *path = malloc(strlen(dir) + sizeof "/settings.ini");
    if (!path) return NULL;
    sprintf(path, "%s/settings.ini", dir);
    return path;
}

static int mkdir_p(const char *path)
{
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

static NexSdkSettings *settings_new(const char *path)
{
    NexSdkSettings *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->path = path ? strdup(path) : nexsdk_settings_path();
    if (!s->path) {
        free(s);
        return NULL;
    }
    char *slash = strrchr(s->path, '/');
    if (slash) {
        *slash = '\0';
        mkdir_p(s->path);
        *slash = '/';
    }
    return s;
}

NexSdkSettings *nexsdk_settings_open_path(const char *path)
{
    NexSdkSettings *s = settings_new(path);
    if (!s) return NULL;
    if (nexsdk_settings_reload(s) != 0 && !s->missing) {
        nexsdk_settings_free(s);
        return NULL;
    }
    s->dirty = 0;
    return s;
}

NexSdkSettings *nexsdk_settings_open(void)
{
    return nexsdk_settings_open_path(NULL);
}

void nexsdk_settings_free(NexSdkSettings *s)
{
    if (!s) return;
    if (s->dirty) nexsdk_settings_save(s);
    settings_clear(s);
    free(s->lines);
    free(s->path);
    free(s);
}

/* ------------------------------------------------------------------ read / write */

int nexsdk_settings_parse(NexSdkSettings *s, const char *text)
{
    if (!s) return -1;
    settings_clear(s);
    if (!text) return 0;
    char group[128] = "";
    const char *p = text;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if (lines_reserve(s, s->count + 1) != 0) return -1;
        Line *l = &s->lines[s->count];
        memset(l, 0, sizeof *l);
        if (parse_line(l, p, len, group) != 0) {
            line_clear(l);
            return -1;
        }
        if (l->kind == LINE_GROUP) snprintf(group, sizeof group, "%s", l->key ? l->key : "");
        s->count++;
        if (!eol) break;
        p = eol + 1;
    }
    return 0;
}

int nexsdk_settings_reload(NexSdkSettings *s)
{
    if (!s) return -1;
    FILE *fp = fopen(s->path, "r");
    if (!fp) {
        s->missing = errno == ENOENT;
        return s->missing ? 1 : -1;
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size < 0 || size > 4 * 1024 * 1024) {
        fclose(fp);
        return -1;
    }
    char *text = malloc((size_t)size + 1);
    if (!text) {
        fclose(fp);
        return -1;
    }
    size_t got = fread(text, 1, (size_t)size, fp);
    fclose(fp);
    text[got] = '\0';
    int rc = nexsdk_settings_parse(s, text);
    free(text);
    if (rc == 0) s->missing = 0;
    return rc;
}

static int line_needs_insert(NexSdkSettings *s, int index, const char *group)
{
    /* the line has to be followed by the end of the group: another group, or the end of the file, not
     * counting blank lines and comments that belong to the next group */
    for (int i = index + 1; i < s->count; i++) {
        const Line *l = &s->lines[i];
        if (l->kind == LINE_GROUP || l->kind == LINE_KEY) {
            return l->kind == LINE_GROUP;
        }
    }
    (void)group;
    return 1;
}

static int insert_line(NexSdkSettings *s, int at, const char *text, LineKind kind, const char *group,
                       const char *key, const char *value)
{
    if (lines_reserve(s, s->count + 1) != 0) return -1;
    memmove(&s->lines[at + 1], &s->lines[at], (size_t)(s->count - at) * sizeof(Line));
    Line *l = &s->lines[at];
    memset(l, 0, sizeof *l);
    l->text = strdup(text);
    l->kind = kind;
    if (key) l->key = strdup(key);
    if (value) l->value = strdup(value);
    if (kind == LINE_KEY) l->group = strdup(group ? group : "");
    s->count++;
    s->dirty = 1;
    return 0;
}

static int find_key(NexSdkSettings *s, const char *group, const char *key)
{
    const char *want = group ? group : "";
    for (int i = 0; i < s->count; i++) {
        const Line *l = &s->lines[i];
        if (l->kind != LINE_KEY) continue;
        if (strcmp(l->group ? l->group : "", want) != 0) continue;
        if (strcmp(l->key ? l->key : "", key) != 0) continue;
        return i;
    }
    return -1;
}

static int find_group(NexSdkSettings *s, const char *group)
{
    if (!group || !*group) return -1;
    for (int i = 0; i < s->count; i++)
        if (s->lines[i].kind == LINE_GROUP && s->lines[i].key && !strcmp(s->lines[i].key, group)) return i;
    return -1;
}

int nexsdk_settings_set(NexSdkSettings *s, const char *group, const char *key, const char *value)
{
    if (!s || !key || !*key) return -1;
    if (!value) value = "";

    int at = find_key(s, group, key);
    if (at >= 0) {
        Line *l = &s->lines[at];
        if (l->value && !strcmp(l->value, value)) return 0;
        free(l->text);
        free(l->value);
        l->value = strdup(value);
        size_t need = strlen(key) + strlen(value) + 4;
        char *text = malloc(need);
        if (!text) return -1;
        snprintf(text, need, "%s=%s", key, value);
        l->text = text;
        s->dirty = 1;
        return 0;
    }

    size_t need = strlen(key) + strlen(value) + 4;
    char *text = malloc(need);
    if (!text) return -1;
    snprintf(text, need, "%s=%s", key, value);

    int group_at = find_group(s, group);
    if (group_at < 0 && group && *group) {
        /* the group does not exist: add it at the end, with the key under it.  A blank line in front of the
         * header, so that the file stays readable in an editor. */
        int at = s->count;
        int rc = 0;
        if (at > 0 && s->lines[at - 1].kind != LINE_BLANK)
            rc = insert_line(s, at++, "", LINE_BLANK, NULL, NULL, NULL);
        char header[160];
        snprintf(header, sizeof header, "[%s]", group);
        if (rc == 0) rc = insert_line(s, at++, header, LINE_GROUP, group, group, NULL);
        if (rc == 0) rc = insert_line(s, at, text, LINE_KEY, group, key, value);
        free(text);
        return rc;
    }

    int last = s->count - 1;
    if (group_at >= 0) {
        last = group_at;
        for (int i = group_at + 1; i < s->count; i++) {
            if (s->lines[i].kind == LINE_GROUP) break;
            last = i;
        }
        /* skip the blank lines at the end of the group: the new key belongs before them */
        while (last > group_at && s->lines[last].kind == LINE_BLANK) last--;
    }
    int rc = insert_line(s, last + 1, text, LINE_KEY, group, key, value);
    (void)line_needs_insert;
    free(text);
    return rc;
}

int nexsdk_settings_set_int(NexSdkSettings *s, const char *group, const char *key, int value)
{
    char buf[32];
    snprintf(buf, sizeof buf, "%d", value);
    return nexsdk_settings_set(s, group, key, buf);
}

int nexsdk_settings_set_bool(NexSdkSettings *s, const char *group, const char *key, int value)
{
    return nexsdk_settings_set(s, group, key, value ? "true" : "false");
}

int nexsdk_settings_remove(NexSdkSettings *s, const char *group, const char *key)
{
    if (!s) return 0;
    int at = find_key(s, group, key);
    if (at < 0) return 0;
    line_clear(&s->lines[at]);
    memmove(&s->lines[at], &s->lines[at + 1], (size_t)(s->count - at - 1) * sizeof(Line));
    s->count--;
    s->dirty = 1;
    return 1;
}

const char *nexsdk_settings_get(NexSdkSettings *s, const char *group, const char *key, const char *fallback)
{
    if (!s || !key) return fallback;
    int at = find_key(s, group, key);
    if (at < 0) return fallback;
    return s->lines[at].value ? s->lines[at].value : fallback;
}

static int parse_bool(const char *v)
{
    if (!v) return 0;
    return !strcmp(v, "true") || !strcmp(v, "1") || !strcmp(v, "yes") || !strcmp(v, "on");
}

int nexsdk_settings_get_bool(NexSdkSettings *s, const char *group, const char *key, int fallback)
{
    const char *v = nexsdk_settings_get(s, group, key, NULL);
    return v ? parse_bool(v) : fallback;
}

int nexsdk_settings_get_int(NexSdkSettings *s, const char *group, const char *key, int fallback)
{
    const char *v = nexsdk_settings_get(s, group, key, NULL);
    if (!v || !*v) return fallback;
    char *end = NULL;
    long n = strtol(v, &end, 10);
    return end && *end == '\0' ? (int)n : fallback;
}

int nexsdk_settings_dirty(const NexSdkSettings *s)
{
    return s ? s->dirty : 0;
}

const char *nexsdk_settings_file(NexSdkSettings *s)
{
    return s ? s->path : NULL;
}

char *nexsdk_settings_to_text(NexSdkSettings *s)
{
    if (!s) return NULL;
    size_t size = 1;
    for (int i = 0; i < s->count; i++) size += strlen(s->lines[i].text ? s->lines[i].text : "") + 1;
    char *out = malloc(size);
    if (!out) return NULL;
    size_t used = 0;
    for (int i = 0; i < s->count; i++) {
        const char *t = s->lines[i].text ? s->lines[i].text : "";
        size_t n = strlen(t);
        memcpy(out + used, t, n);
        used += n;
        out[used++] = '\n';
    }
    out[used] = '\0';
    return out;
}

int nexsdk_settings_save(NexSdkSettings *s)
{
    if (!s) return -1;
    char *text = nexsdk_settings_to_text(s);
    if (!text) return -1;

    size_t need = strlen(s->path) + sizeof ".new";
    char *tmp = malloc(need);
    if (!tmp) {
        free(text);
        return -1;
    }
    snprintf(tmp, need, "%s.new", s->path);
    FILE *fp = fopen(tmp, "w");
    if (!fp) {
        free(tmp);
        free(text);
        return -1;
    }
    size_t len = strlen(text);
    int ok = fwrite(text, 1, len, fp) == len;
    ok = fflush(fp) == 0 && ok;
    ok = fclose(fp) == 0 && ok;
    free(text);
    if (!ok) {
        remove(tmp);
        free(tmp);
        return -1;
    }
    if (rename(tmp, s->path) != 0) {
        remove(tmp);
        free(tmp);
        return -1;
    }
    free(tmp);
    s->dirty = 0;
    s->missing = 0;
    return 0;
}
