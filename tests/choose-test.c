/* tests/choose-test.c — the rules of "which program for this?" (src/hde-choose.c) on a made-up machine: a PATH with
 * the programs this test puts there, so what is "installed" is decided here and not by the machine that runs it.
 * Built and run by `make check-unit` / `make check-choose`: PASS/FAIL lines, exit status = failures.
 *   cc -O2 -Wall -Wextra -Wpedantic -std=c11 -Isrc -o build/choose-test tests/choose-test.c src/hde-choose.c
 *
 * What it checks: the table (every feature has a question, HDE's own program comes first where HDE has one), finding a
 * feature, looking a program up on PATH, the candidates that are installed (in order, and once each even when two names
 * are the same program — Debian's x-terminal-emulator is a symlink to one of the others), and what the user chose:
 * reading it (the last one in the file wins, like everywhere else in HDE), writing it, and forgetting it again
 * without touching the rest of settings.ini.
 */
#define _POSIX_C_SOURCE 200809L

#include "hde-choose.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails, passes;

#define CHECK(cond, ...) do { \
        if (cond) { passes++; printf("PASS: choose: "); } else { fails++; printf("FAIL: choose: "); } \
        printf(__VA_ARGS__); printf("\n"); \
    } while (0)

static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static char root[192];

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(2); }
    fputs(text, f);
    fclose(f);
}

/* a program this test invented: an empty file with the executable bit */
static void make_program(const char *name)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", root, name);
    write_file(path, "#!/bin/sh\n");
    chmod(path, 0755);
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return dup_str("");
    static char buf[4096];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = '\0';
    fclose(f);
    return dup_str(buf);
}

static const char *cand_names(const HdeChooseCandidate **c, int n, char *buf, size_t len)
{
    buf[0] = '\0';
    for (int i = 0; i < n; i++) {
        if (i) snprintf(buf + strlen(buf), len - strlen(buf), ",");
        snprintf(buf + strlen(buf), len - strlen(buf), "%s", c[i]->program);
    }
    return buf;
}

int main(void)
{
    snprintf(root, sizeof root, "/tmp/hde-choose-test-%ld", (long)getpid());
    char cmd[512];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    if (system(cmd) != 0) { /* nothing to remove */ }
    if (mkdir(root, 0755) != 0) { fprintf(stderr, "cannot make %s\n", root); return 2; }

    /* the settings of this test go to a file of its own: XDG_CONFIG_HOME is what everything in HDE reads */
    setenv("XDG_CONFIG_HOME", root, 1);

    /* ---- the table ---- */
    int n_features = 0;
    const HdeChooseFeature *all = hde_choose_table(&n_features);
    CHECK(all != NULL && n_features == 6, "the table has the six features HDE asks about (%d)", n_features);

    const char *want[] = { "terminal", "files", "monitor", "pictures", "player", "screenshot" };
    for (int i = 0; i < 6 && i < n_features; i++)
        CHECK(!strcmp(all[i].feature, want[i]), "the feature %d is %s (got %s)", i + 1, want[i], all[i].feature);

    int all_named = 1, all_asked = 1, all_offered = 1;
    for (int i = 0; i < n_features; i++) {
        if (!all[i].title || !*all[i].title || all[i].title[strlen(all[i].title) - 1] != '?') all_asked = 0;
        if (!all[i].install || !*all[i].install) all_named = 0;
        if (all[i].n < 2 || !all[i].candidates) all_offered = 0;
    }
    CHECK(all_asked, "every feature says what it is asking (a question, ending in '?')");
    CHECK(all_named, "every feature knows a package to install when nothing on the machine can do it");
    CHECK(all_offered, "every feature offers more than one program (otherwise there would be nothing to ask)");

    /* HDE's own program comes first where HDE has one: that is what HDE is for */
    CHECK(!strcmp(hde_choose_feature("files")->candidates[0].program, "hde-files"), "Hyggshi Files is the first for files");
    CHECK(!strcmp(hde_choose_feature("pictures")->candidates[0].program, "hde-media"), "Hyggshi Media is the first for pictures");
    CHECK(!strcmp(hde_choose_feature("player")->candidates[0].program, "hde-media"), "... and for music and video");
    CHECK(!strcmp(hde_choose_feature("screenshot")->candidates[0].program, "hde-screenshot"), "hde-screenshot is the first for the screen");
    CHECK(hde_choose_feature("files")->candidates[0].note != NULL, "and it is marked as HDE's own (the question says so)");

    /* terminal-emulators that need arguments are marked as such */
    CHECK(hde_choose_feature("files")->candidates[0].args != NULL, "the file managers open the home folder by default");

    CHECK(hde_choose_feature("terminal") == &all[0], "a feature is found by its name");
    CHECK(hde_choose_feature("nonsense") == NULL, "a name nobody knows is not a feature");
    CHECK(hde_choose_feature("") == NULL && hde_choose_feature(NULL) == NULL, "and neither is nothing at all");

    /* ---- what is installed: PATH, or a path ---- */
    char *real_path = dup_str(getenv("PATH") ? getenv("PATH") : "");
    CHECK(hde_choose_have("sh"), "sh is on PATH here");
    CHECK(hde_choose_have("/bin/sh"), "an absolute path is checked itself");
    CHECK(!hde_choose_have("/no/such/program"), "a path that is not there is not a program");
    CHECK(!hde_choose_have("hde-choose-not-installed-anywhere"), "and neither is a name nobody has");
    CHECK(!hde_choose_have("") && !hde_choose_have(NULL), "nothing at all is not a program either");

    /* a machine with two file managers, one terminal — and Debian's x-terminal-emulator pointing at it */
    make_program("hde-files");
    make_program("thunar");
    make_program("xfce4-terminal");
    char link[512];
    snprintf(link, sizeof link, "%s/x-terminal-emulator", root);
    if (symlink("xfce4-terminal", link) != 0) { perror("symlink"); return 2; }
    setenv("PATH", root, 1);

    const HdeChooseCandidate *cands[32];
    int n = hde_choose_installed(hde_choose_feature("files"), cands, 32);
    char buf[512];
    CHECK(n == 2, "two of the file managers are installed here (%d)", n);
    CHECK(n == 2 && !strcmp(cands[0]->program, "hde-files") && !strcmp(cands[1]->program, "thunar"),
          "and they come in the order of the table (%s)", cand_names(cands, n, buf, sizeof buf));

    n = hde_choose_installed(hde_choose_feature("terminal"), cands, 32);
    CHECK(n == 1, "the terminal is one program, not two: x-terminal-emulator is the symlink to xfce4-terminal (%d)", n);
    CHECK(n == 1 && !strcmp(cands[0]->program, "xfce4-terminal"), "... and the real name is the one kept (%s)",
          cand_names(cands, n, buf, sizeof buf));

    n = hde_choose_installed(hde_choose_feature("monitor"), cands, 32);
    CHECK(n == 0, "nothing here can show what the system is doing (%d)", n);

    /* ---- what was remembered ---- */
    char *path = hde_choose_settings_file();
    CHECK(strstr(path, "/hde/settings.ini") != NULL && strstr(path, root) == path,
          "the settings file follows XDG_CONFIG_HOME: %s", path);

    char *value = hde_choose_choice("terminal");
    CHECK(!strcmp(value, ""), "nothing is remembered to start with (got '%s')", value);
    free(value);

    CHECK(hde_choose_set_choice("terminal", "xfce4-terminal") == 0, "a choice can be written down");
    value = hde_choose_choice("terminal");
    CHECK(!strcmp(value, "xfce4-terminal"), "and it comes back (got '%s')", value);
    free(value);

    char *file = read_file(path);
    CHECK(strstr(file, "choice_terminal=xfce4-terminal\n") != NULL, "it is a line of settings.ini (choice_terminal=...)");
    value = hde_choose_choice("files");
    CHECK(!strcmp(value, ""), "the other features are untouched ('%s')", value);
    free(value);
    free(file);

    /* writing it again does not leave the old answer behind (the last one wins, and there is only one) */
    hde_choose_set_choice("terminal", "thunar");
    value = hde_choose_choice("terminal");
    CHECK(!strcmp(value, "thunar"), "a new choice replaces the old one ('%s')", value);
    free(value);
    file = read_file(path);
    int count = 0;
    for (char *p = file; (p = strstr(p, "choice_terminal=")); p++) count++;
    CHECK(count == 1, "and there is exactly one choice_terminal line in the file (%d)", count);
    free(file);

    /* the answer "ask me every time" is a value like any other */
    hde_choose_set_choice("terminal", "ask");
    value = hde_choose_choice("terminal");
    CHECK(!strcmp(value, "ask"), "the answer can be 'ask' ('%s')", value);
    free(value);

    /* forgetting it: --reset writes nothing and leaves the rest of the file alone */
    write_file(path, "# a comment of the user\nwm=nexwm\n\nchoice_terminal=thunar\nchoice_files=hde-files\n");
    CHECK(hde_choose_set_choice("terminal", NULL) == 0, "a choice can be forgotten again");
    value = hde_choose_choice("terminal");
    CHECK(!strcmp(value, ""), "after that there is no answer any more ('%s')", value);
    free(value);
    value = hde_choose_choice("files");
    CHECK(!strcmp(value, "hde-files"), "the other answers are still there ('%s')", value);
    free(value);
    file = read_file(path);
    CHECK(strstr(file, "wm=nexwm\n") != NULL && strstr(file, "# a comment of the user\n") != NULL,
          "and the rest of settings.ini survived: %s", file);
    free(file);

    /* a value written twice (an older HDE, or the Settings window appending): the last one is the answer */
    write_file(path, "choice_terminal=thunar\nchoice_terminal=xfce4-terminal\n");
    value = hde_choose_choice("terminal");
    CHECK(!strcmp(value, "xfce4-terminal"), "the last choice in the file is the one that counts ('%s')", value);
    free(value);
    hde_choose_set_choice("terminal", "xterm");
    file = read_file(path);
    count = 0;
    for (char *p = file; (p = strstr(p, "choice_terminal=")); p++) count++;
    CHECK(count == 1 && strstr(file, "choice_terminal=xterm\n") != NULL, "and writing it down cleans that up: %s", file);
    free(file);

    /* a settings file that is not there yet: no answer, and the first write makes the directory. Removed here
     * without a shell: this test has a PATH of its own, where rm does not live. */
    char dir[512];
    snprintf(dir, sizeof dir, "%s/hde", root);
    unlink(path);
    if (rmdir(dir) != 0) { /* it was not there, which is the point */ }
    value = hde_choose_choice("terminal");
    CHECK(!strcmp(value, ""), "no settings.ini: no answer, and no error ('%s')", value);
    free(value);
    CHECK(hde_choose_set_choice("player", "mpv") == 0, "the first write makes the directory it needs");
    value = hde_choose_choice("player");
    CHECK(!strcmp(value, "mpv"), "and the answer is there ('%s')", value);
    free(value);
    free(path);

    /* leave the machine as it was found */
    setenv("PATH", real_path, 1);
    free(real_path);
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    if (system(cmd) != 0) { /* best effort */ }

    printf("\nchoose-test: %d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
