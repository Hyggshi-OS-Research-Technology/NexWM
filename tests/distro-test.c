/* tests/distro-test.c — src/hde-distro.h without those systems: fake os-release files (HDE_OS_RELEASE) for Fedora,
 * Debian, Ubuntu, Hyggshi OS, Nobara, Arch, openSUSE and Alpine. It checks what the user sees when HDE misses
 * something: the right package manager, the right package names ("libXi-devel" on Fedora, not "libxi-dev") and the
 * right advice in Settings > About. Built and run by `make check-unit` (build/distro-test): PASS/FAIL lines,
 * exit status = failures.
 *   cc -Isrc -o build/distro-test tests/distro-test.c
 */
#include "hde-distro.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int fails, passes;

#define CHECK(cond, ...) do { \
        if (cond) { passes++; printf("PASS: distro: "); } else { fails++; printf("FAIL: distro: "); } \
        printf(__VA_ARGS__); printf("\n"); \
    } while (0)

static char *root;

static void write_os_release(const char *name, const char *text)
{
    if (!root) root = strdup("/tmp/hde-distro-test");
    char path[512];
    snprintf(path, sizeof path, "%s/os-release-%s", root, name);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fputs(text, f);
    fclose(f);
    setenv("HDE_OS_RELEASE", path, 1);
}

static const char *FEDORA =
    "NAME=\"Fedora Linux\"\n"
    "VERSION=\"42 (Workstation Edition)\"\n"
    "ID=fedora\n"
    "VERSION_ID=42\n"
    "VERSION_CODENAME=\"\"\n"
    "PRETTY_NAME=\"Fedora Linux 42 (Workstation Edition)\"\n"
    "VARIANT=\"Workstation Edition\"\n"
    "LOGO=fedora-logo-icon\n"
    "CPE_NAME=\"cpe:/o:fedoraproject:fedora:42\"\n"
    "HOME_URL=\"https://fedoraproject.org/\"\n";

static const char *DEBIAN =
    "PRETTY_NAME=\"Debian GNU/Linux 13 (trixie)\"\n"
    "NAME=\"Debian GNU/Linux\"\n"
    "VERSION_ID=\"13\"\n"
    "VERSION=\"13 (trixie)\"\n"
    "VERSION_CODENAME=trixie\n"
    "ID=debian\n";

static const char *HYGGSHI =
    "PRETTY_NAME=\"Hyggshi OS 1.0 \\\"Sen Vang\\\" (dua tren Debian 13)\"\n"
    "NAME=\"Hyggshi OS\"\n"
    "VERSION_ID=\"1.0\"\n"
    "VERSION_CODENAME=\"Sen Vang\"\n"
    "ID=hyggshios\n"
    "ID_LIKE=debian\n";

static const char *NOBARA =
    "NAME=\"Nobara Linux\"\n"
    "VERSION=\"41\"\n"
    "ID=nobara\n"
    "ID_LIKE=\"fedora\"\n"
    "PRETTY_NAME=\"Nobara Linux 41 (KDE Plasma)\"\n"
    "VERSION_ID=41\n";

static const char *ARCH =
    "NAME=\"Arch Linux\"\n"
    "PRETTY_NAME=\"Arch Linux\"\n"
    "ID=arch\n"
    "BUILD_ID=rolling\n";

static const char *OPENSUSE =
    "NAME=\"openSUSE Tumbleweed\"\n"
    "ID=\"opensuse-tumbleweed\"\n"
    "ID_LIKE=\"suse opensuse\"\n"
    "PRETTY_NAME=\"openSUSE Tumbleweed\"\n"
    "VERSION_ID=\"20250920\"\n";

static const char *ALPINE =
    "NAME=\"Alpine Linux\"\n"
    "ID=alpine\n"
    "VERSION_ID=3.22.0\n"
    "PRETTY_NAME=\"Alpine Linux v3.22\"\n";

/* one system: the family, the package manager and the translations HDE's messages use */
static void check_system(const char *what, const char *text, const char *id, int family, const char *pkgcmd)
{
    write_os_release(what, text);
    char *have_id = hde_distro_id();
    CHECK(!strcmp(have_id, id), "%s: ID is '%s' (got '%s')", what, id, have_id);
    free(have_id);
    int fam = hde_distro_family();
    CHECK(fam == family, "%s: family is %s (got %s)", what, hde_distro_family_name(family), hde_distro_family_name(fam));
    const char *pc = hde_distro_pkgcmd();
    CHECK(!strcmp(pc, pkgcmd), "%s: packages are installed with '%s' (got '%s')", what, pkgcmd, *pc ? pc : "nothing");
}

int main(void)
{
    char buf[512];
    mkdir("/tmp/hde-distro-test", 0755);

    /* ---- Fedora: dnf, Fedora names, Fedora advice */
    check_system("Fedora", FEDORA, "fedora", HDE_FAMILY_RPM, "dnf");
    char *name = hde_distro_name();
    CHECK(strstr(name, "Fedora") != NULL, "Fedora: the name for messages is '%s'", name);
    free(name);
    char *ver = hde_distro_version();
    CHECK(!strcmp(ver, "42"), "Fedora: version 42 (got '%s')", ver);
    free(ver);
    CHECK(!strcmp(hde_distro_pkg("libxi-dev"), "libXi-devel"),
          "Fedora: Debian's libxi-dev is libXi-devel (got %s)", hde_distro_pkg("libxi-dev"));
    CHECK(!strcmp(hde_distro_pkg("libwnck-3-dev"), "libwnck3-devel"),
          "Fedora: libwnck3-devel (got %s)", hde_distro_pkg("libwnck-3-dev"));
    CHECK(!strcmp(hde_distro_pkg("libgtk-layer-shell-dev"), "gtk-layer-shell-devel"),
          "Fedora: gtk-layer-shell-devel (got %s)", hde_distro_pkg("libgtk-layer-shell-dev"));
    CHECK(!strcmp(hde_distro_pkg("network-manager"), "NetworkManager"),
          "Fedora: NetworkManager (got %s)", hde_distro_pkg("network-manager"));
    CHECK(!strcmp(hde_distro_pkg("policykit-1-gnome"), "polkit-gnome"),
          "Fedora: polkit-gnome (got %s)", hde_distro_pkg("policykit-1-gnome"));
    CHECK(!strcmp(hde_distro_pkg("metacity"), "metacity"), "Fedora: metacity keeps its name");
    char *hint = hde_install_hint("playerctl");
    CHECK(hint && !strcmp(hint, "sudo dnf install playerctl"), "Fedora: '%s'", hint ? hint : "(null)");
    free(hint);
    hint = hde_install_hint("network-manager");
    CHECK(hint && !strcmp(hint, "sudo dnf install NetworkManager"),
          "Fedora: a missing network manager is reported as '%s'", hint ? hint : "(null)");
    free(hint);
    hint = hde_deps_hint("build");
    CHECK(hint && strstr(hint, "gtk3-devel") && strstr(hint, "libwnck3-devel") && !strstr(hint, "libgtk-3-dev"),
          "Fedora: the build dependencies are one dnf command without -dev packages (%s)", hint ? hint : "(null)");
    CHECK(hint && !strncmp(hint, "sudo dnf install ", 17), "Fedora: build hint starts with 'sudo dnf install'");
    free(hint);
    hint = hde_distro_ram_advice();
    CHECK(hint && strstr(hint, "Fedora") && !strstr(hint, "Debian"),
          "Fedora: Settings > About talks about Fedora, not Debian (%s)", hint ? hint : "(null)");
    free(hint);

    /* ---- Debian, Ubuntu, Hyggshi OS: apt, the names HDE's documentation uses */
    check_system("Debian", DEBIAN, "debian", HDE_FAMILY_DEB, "apt");
    CHECK(!strcmp(hde_distro_pkg("libxi-dev"), "libxi-dev"), "Debian: libxi-dev stays libxi-dev");
    hint = hde_install_hint("network-manager");
    CHECK(hint && !strcmp(hint, "sudo apt install network-manager"), "Debian: '%s'", hint ? hint : "(null)");
    free(hint);
    hint = hde_install_hint("libgtk-3-dev");
    CHECK(hint && !strcmp(hint, "sudo apt install libgtk-3-dev"), "Debian: '%s'", hint ? hint : "(null)");
    free(hint);
    hint = hde_deps_hint("build");
    CHECK(hint && !strncmp(hint, "sudo apt install build-essential", 32), "Debian: build hint starts with apt (%s)",
          hint ? hint : "(null)");
    free(hint);
    hint = hde_distro_ram_advice();
    CHECK(hint && strstr(hint, "Debian"), "Debian: the advice stays Debian's (%s)", hint ? hint : "(null)");
    free(hint);

    write_os_release("ubuntu", "NAME=\"Ubuntu\"\nID=ubuntu\nID_LIKE=debian\nPRETTY_NAME=\"Ubuntu 24.04.3 LTS\"\n"
                               "VERSION_ID=\"24.04\"\n");
    CHECK(hde_distro_family() == HDE_FAMILY_DEB, "Ubuntu: ID_LIKE=debian -> the Debian family");
    hint = hde_distro_ram_advice();
    CHECK(hint && strstr(hint, "Ubuntu"), "Ubuntu: the advice is Ubuntu's (%s)", hint ? hint : "(null)");
    free(hint);

    check_system("Hyggshi OS", HYGGSHI, "hyggshios", HDE_FAMILY_DEB, "apt");
    CHECK(hde_distro_is("debian"), "Hyggshi OS: based on Debian");
    name = hde_distro_name();
    CHECK(strstr(name, "Hyggshi OS") != NULL, "Hyggshi OS: the name is '%s'", name);
    free(name);

    /* ---- other Fedora-family systems, like Fedora-based distributions */
    write_os_release("nobara", NOBARA);
    CHECK(hde_distro_family() == HDE_FAMILY_RPM && !strcmp(hde_distro_pkgcmd(), "dnf"),
          "Nobara: ID_LIKE=fedora -> the RPM family with dnf");
    CHECK(!strcmp(hde_distro_pkg("libxi-dev"), "libXi-devel"), "Nobara: Fedora's package names");

    /* ---- Arch and openSUSE */
    check_system("Arch", ARCH, "arch", HDE_FAMILY_ARCH, "pacman");
    hint = hde_install_hint("libgtk-3-dev");
    CHECK(hint && !strcmp(hint, "sudo pacman -S gtk3"), "Arch: '%s'", hint ? hint : "(null)");
    free(hint);
    check_system("openSUSE", OPENSUSE, "opensuse-tumbleweed", HDE_FAMILY_SUSE, "zypper");
    hint = hde_install_hint("libxi-dev");
    CHECK(hint && !strcmp(hint, "sudo zypper install libXi-devel"), "openSUSE: '%s'", hint ? hint : "(null)");
    free(hint);

    /* ---- something HDE does not know, and no os-release at all */
    check_system("Alpine", ALPINE, "alpine", HDE_FAMILY_UNKNOWN, "");
    hint = hde_install_hint("metacity");
    CHECK(hint && !strncmp(hint, "install ", 8), "Alpine: no package manager detected -> '%s'",
          hint ? hint : "(null)");
    free(hint);
    hint = hde_distro_ram_advice();
    CHECK(hint && !strstr(hint, "Debian") && !strstr(hint, "Fedora"),
          "Alpine: the advice does not name another system (%s)", hint ? hint : "(null)");
    free(hint);

    setenv("HDE_OS_RELEASE", "/tmp/hde-distro-test/does-not-exist", 1);
    char *id = hde_distro_id();
    CHECK(!strcmp(id, ""), "no os-release: no id (got '%s')", id);
    free(id);
    name = hde_distro_name();
    CHECK(!strcmp(name, "this system"), "no os-release: 'this system' for messages (got '%s')", name);
    free(name);
    CHECK(hde_distro_family() == HDE_FAMILY_UNKNOWN, "no os-release: the family is unknown");

    /* a quoted value with spaces and escapes (Hyggshi OS) is read correctly */
    write_os_release("quotes", "NAME=\"Foo OS\"\nPRETTY_NAME=\"Foo OS 2 \\\"Bar\\\"\"\nID=foo\nANSI_COLOR=\"0;35\"\n");
    name = hde_distro_name();
    CHECK(strstr(name, "Foo OS 2") != NULL, "quoted PRETTY_NAME with spaces: '%s'", name);
    free(name);

    unsetenv("HDE_OS_RELEASE");
    (void)buf;
    printf("\ndistro-test: %d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
