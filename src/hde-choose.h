/* hde-choose.h — "which program for this?": the same feature, more than one program that can do it.
 *
 * HDE has a program of its own for some of the things a session does (Hyggshi Files for folders, Hyggshi Media for
 * pictures and for music and video, hde-screenshot for the screen), and the machine almost always has another one
 * installed. Where HDE used to take the first one on a list, hde-choose asks the user which one to use — once, and it
 * remembers the answer unless the user says "ask me every time" — and then runs it. This header is the table and the
 * rules; the GTK part (the question, the running of the program) is apps/hde-choose.c, and the unit test of the rules
 * is tests/choose-test.c. It is plain C: no GTK, no GLib, so the tests need no display.
 *
 * The same feature can also be reached on the command line, which is what a session, a key binding or a menu does:
 *
 *   hde-choose terminal [ARGS…]     start a terminal (or whatever the user chose), and write the choice down if asked
 *   hde-choose --list               the features, what is installed for each, and what is remembered
 *   hde-choose --set F PROGRAM      what to use for F from now on (no question asked)
 *   hde-choose --reset F            ask again the next time
 *   HDE_CHOOSE=PROGRAM              override everything for one run (what the tests use)
 */
#ifndef HDE_CHOOSE_H
#define HDE_CHOOSE_H

typedef struct {
    const char *program;     /* looked up on PATH (or an absolute path) */
    const char *args;        /* arguments this program needs here, or NULL. Used only when the caller passed none
                              * of its own, and run through the shell, so "$HOME" and the like are expanded */
    const char *note;        /* shown next to the name in the question, e.g. "HDE's own" */
} HdeChooseCandidate;

typedef struct {
    const char *feature;     /* hde-choose terminal */
    const char *title;       /* what the question asks: "Which program for a terminal?" */
    const char *install;     /* a package to install when nothing here can do it (hde_install_hint) */
    const HdeChooseCandidate *candidates;   /* in the order they are tried: HDE's own program first, if HDE has one */
    int n;
} HdeChooseFeature;

/* The whole table (count written to *count when it is not NULL), and one feature by its name (NULL: nobody knows it). */
const HdeChooseFeature *hde_choose_table(int *count);
const HdeChooseFeature *hde_choose_feature(const char *feature);

/* Is this a program this machine can run? An absolute path is checked itself, a name is looked for on PATH. */
int hde_choose_have(const char *program);

/* The candidates of this feature that are installed, in order, without duplicates (two names that are the same
 * program — Debian's x-terminal-emulator points at one of the others — count once). Returns how many were written. */
int hde_choose_installed(const HdeChooseFeature *f, const HdeChooseCandidate **out, int max);

/* What the user chose for this feature, from ~/.config/hde/settings.ini (choice_<feature>): "" when there is no
 * choice, "ask" when the answer is "ask me every time", or the program name. The caller frees it. */
char *hde_choose_choice(const char *feature);
/* Write it down (NULL or "" removes the key: ask again next time). 0 on success. */
int hde_choose_set_choice(const char *feature, const char *value);

/* ~/.config/hde/settings.ini (or $XDG_CONFIG_HOME/hde/settings.ini) — the file the rest of HDE reads. Caller frees. */
char *hde_choose_settings_file(void);

#endif /* HDE_CHOOSE_H */
