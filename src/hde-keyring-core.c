/* hde-keyring-core.c — reading what `gnome-keyring-daemon --start` prints, and only what the session may be given
 * (see src/hde-keyring-core.h). Plain C: tests/keyring-test.c runs it without a keyring on the machine.
 */
#include "hde-keyring-core.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static const char *const var_names[HDE_KEYRING_N_VARS] = {
    "GNOME_KEYRING_CONTROL",
    "SSH_AUTH_SOCK",
    "GPG_AGENT_INFO",
};

const char *hde_keyring_var_name(HdeKeyringVar v)
{
    if (v < 0 || v >= HDE_KEYRING_N_VARS) return "";
    return var_names[v];
}

int hde_keyring_wants_export(const char *name)
{
    if (!name) return 0;
    for (int i = 0; i < HDE_KEYRING_N_VARS; ++i)
        if (strcmp(name, var_names[i]) == 0) return 1;
    return 0;
}

static int is_name_char(char c, int first)
{
    if (c == '_') return 1;
    if (c >= 'A' && c <= 'Z') return 1;
    if (c >= 'a' && c <= 'z') return 1;
    return !first && c >= '0' && c <= '9';
}

/* The value may be quoted (the daemon does not quote, a hand-written file might) and may have a stray CR in
 * front of the line end. */
static void clean_value(const char *src, char *dst, size_t len)
{
    if (!len) return;
    size_t n = strlen(src);
    while (n > 0 && (src[n - 1] == '\r' || src[n - 1] == '\n' || src[n - 1] == ' ' || src[n - 1] == '\t')) n--;
    /* `SSH_AUTH_SOCK = /run/user/...`: the space after the = belongs to the line, not to the path. */
    while (n > 0 && (*src == ' ' || *src == '\t')) { src++; n--; }
    if (n >= 2 && ((src[0] == '"' && src[n - 1] == '"') || (src[0] == '\'' && src[n - 1] == '\''))) {
        src++;
        n -= 2;
    }
    if (n >= len) n = len - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* One line of the daemon's output into names[slot]/values[slot]. Returns 1 when it held something usable. */
static int parse_line(char *line, char *name, size_t name_len, char *value, size_t value_len)
{
    if (!line) return 0;
    char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '#' || *p == '\0' || *p == '\n' || *p == '\r') return 0;

    /* `export NAME=value` is accepted: the daemon's output is meant to be pasted into a shell. */
    static const char export_word[] = "export";
    size_t w = sizeof export_word - 1;
    if (strncmp(p, export_word, w) == 0 && (p[w] == ' ' || p[w] == '\t')) {
        p += w;
        while (*p == ' ' || *p == '\t') p++;
    }

    if (!is_name_char(*p, 1)) return 0;
    char *eq = strchr(p, '=');
    if (!eq) return 0;
    char *end = eq;
    while (end > p && (end[-1] == ' ' || end[-1] == '\t')) end--;
    size_t n = (size_t)(end - p);
    if (n == 0 || n >= name_len) return 0;
    for (size_t i = 0; i < n; ++i)
        if (!is_name_char(p[i], i == 0)) return 0;
    memcpy(name, p, n);
    name[n] = '\0';

    clean_value(eq + 1, value, value_len);
    return 1;
}

int hde_keyring_parse_env(const char *text,
                          char names[HDE_KEYRING_MAX_VARS][HDE_KEYRING_NAME_MAX],
                          char values[HDE_KEYRING_MAX_VARS][HDE_KEYRING_VALUE_MAX])
{
    if (!text || !names || !values) return 0;
    int found = 0;
    const char *p = text;
    char line[HDE_KEYRING_NAME_MAX + HDE_KEYRING_VALUE_MAX + 8];
    size_t line_len = sizeof line;

    while (*p && found < HDE_KEYRING_MAX_VARS) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len >= line_len) len = line_len - 1;
        memcpy(line, p, len);
        line[len] = '\0';
        p += nl ? len + 1 : len;

        char name[HDE_KEYRING_NAME_MAX], value[HDE_KEYRING_VALUE_MAX];
        if (!parse_line(line, name, sizeof name, value, sizeof value)) continue;
        if (!hde_keyring_wants_export(name)) continue;      /* nothing else reaches the session */
        int again = 0;
        for (int i = 0; i < found; ++i)
            if (strcmp(names[i], name) == 0) again = 1;
        if (again) continue;                                /* the first one wins */
        snprintf(names[found], HDE_KEYRING_NAME_MAX, "%s", name);
        snprintf(values[found], HDE_KEYRING_VALUE_MAX, "%s", value);
        found++;
    }
    return found;
}

const char *hde_keyring_lookup(const char *text, const char *name, char *out, size_t len)
{
    if (out && len) out[0] = '\0';
    if (!text || !name) return out;
    char names[HDE_KEYRING_MAX_VARS][HDE_KEYRING_NAME_MAX];
    char values[HDE_KEYRING_MAX_VARS][HDE_KEYRING_VALUE_MAX];
    int n = hde_keyring_parse_env(text, names, values);
    for (int i = 0; i < n; ++i) {
        if (strcmp(names[i], name) == 0) {
            if (out && len) snprintf(out, len, "%s", values[i]);
            return out;
        }
    }
    return out;
}
