/*
 * tests/store-test.c — Unit tests for the Hyggshi Store (apps/store/hde-store-core.c).
 *
 * No package manager, no display: the parsers are fed with the real output of dpkg-query, apt list,
 * apt-cache search / show / policy, flatpak list and a copyright file, and the Debian version
 * comparison is checked against pairs whose order `dpkg --compare-versions` agrees on.
 */
#include "hde-store-core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int passed = 0;
static int failed = 0;

static void check(int cond, const char *msg)
{
    if (cond) {
        printf("PASS: store: %s\n", msg);
        passed++;
    } else {
        printf("FAIL: store: %s\n", msg);
        failed++;
    }
}

static void check_int(int got, int want, const char *msg)
{
    if (got == want) {
        printf("PASS: store: %s\n", msg);
        passed++;
    } else {
        printf("FAIL: store: %s (got %d, want %d)\n", msg, got, want);
        failed++;
    }
}

static void check_str(const char *got, const char *want, const char *msg)
{
    if (got && want && !strcmp(got, want)) {
        printf("PASS: store: %s\n", msg);
        passed++;
    } else {
        printf("FAIL: store: %s (got \"%s\", want \"%s\")\n", msg, got ? got : "(null)", want);
        failed++;
    }
}

int main(void)
{
    HdeStorePackage p;

    /* ---- 1. Debian version comparison (dpkg's rules) ---- */
    check_int(hde_store_version_compare("1.0", "1.0"), 0, "1.0 = 1.0");
    check_int(hde_store_version_compare("1.0", "1.1"), -1, "1.0 is older than 1.1");
    check_int(hde_store_version_compare("1.10", "1.9"), 1, "1.10 is newer than 1.9 (numbers, not text)");
    check_int(hde_store_version_compare("2:1.0", "1:9.9"), 1, "the epoch wins: 2:1.0 newer than 1:9.9");
    check_int(hde_store_version_compare("1.0~rc1", "1.0"), -1, "1.0~rc1 sorts before 1.0");
    check_int(hde_store_version_compare("1.0~~", "1.0~"), -1, "two tildes sort before one");
    check_int(hde_store_version_compare("1.0-1", "1.0-2"), -1, "the Debian revision decides: -1 before -2");
    check_int(hde_store_version_compare("1.0-1", "1.0"), 1, "1.0-1 is newer than 1.0 (a revision is newer)");
    check_int(hde_store_version_compare("1.0a", "1.0+b1"), -1, "letters sort before other characters");
    check_int(hde_store_version_compare("1.0", "1.0.0"), -1, "1.0 before 1.0.0");
    check_int(hde_store_version_compare("5.2.15-2+b4", "5.2.15-2+b3"), 1, "5.2.15-2+b4 newer than +b3");
    check(hde_store_version_valid("1.0"), "1.0 is a valid version");
    check(!hde_store_version_valid("main"), "a suite name is not a version");
    check(!hde_store_version_valid(""), "an empty string is not a version");
    char upstream[80];
    hde_store_version_upstream("2:1.2-3", upstream, sizeof upstream);
    check_str(upstream, "1.2-3", "the epoch is dropped from the upstream version");

    /* ---- 2. Accent-tolerant folding (searching for "cai dat" finds "Cài đặt") ---- */
    char folded[96];
    hde_store_fold("Cài đặt", folded, sizeof folded);
    check_str(folded, "cai dat", "Vietnamese is folded to ASCII: Cài đặt -> cai dat");
    hde_store_fold("GNOME Weather", folded, sizeof folded);
    check_str(folded, "gnome weather", "Latin names are lower-cased");

    /* ---- 3. dpkg-query -W -f=… (Tab separated, as HDE_STORE_DPKG_FORMAT asks) ---- */
    check_int(hde_store_parse_dpkg_line(
                  "gnome-weather\t44.0-2\tinstall ok installed\tgnome\t1024\t"
                  "Weather app for the GNOME desktop\thttps://wiki.gnome.org/Apps/Weather\tall", &p),
              1, "parses a dpkg-query line");
    check_str(p.name, "gnome-weather", "dpkg: the package name");
    check_str(p.version, "44.0-2", "dpkg: the version");
    check_int(p.installed, 1, "dpkg: 'install ok installed' means installed");
    check_str(p.section, "gnome", "dpkg: the section");
    check_str(p.summary, "Weather app for the GNOME desktop", "dpkg: the summary");
    check_str(p.homepage, "https://wiki.gnome.org/Apps/Weather", "dpkg: the homepage");
    check_str(p.arch, "all", "dpkg: the architecture");
    check(p.size_kb == 1024, "dpkg: Installed-Size is read in KB");
    check_int(p.state, HDE_STORE_STATE_INSTALLED, "dpkg: the state is Installed");

    check_int(hde_store_parse_dpkg_line(
                  "old-thing\t1.0\tdeinstall ok config-files\tutils\t12\tAn old thing\t\tall", &p),
              1, "parses a package whose configuration was left behind");
    check_int(p.state, HDE_STORE_STATE_RESIDUAL, "dpkg: 'config-files' is a residual state");
    check(!p.installed, "dpkg: a removed package with configuration is not installed");

    check_int(hde_store_parse_dpkg_line(
                  "half\t1.0\tinstall ok half-configured\tutils\t12\tHalf way\t\tall", &p),
              1, "parses a half-configured package");
    check_int(p.state, HDE_STORE_STATE_BROKEN, "dpkg: half-configured is a broken state");

    check_int(hde_store_parse_dpkg_line("", &p), 0, "an empty dpkg line is not a package");
    check_int(hde_store_parse_dpkg_line("gnome-weather\t44.0", &p), 0, "a dpkg line without a status is not a package");

    /* ---- 4. apt list ---- */
    check_int(hde_store_parse_apt_list_line("bash/trixie 5.2.15-2+b4 amd64 [upgradable from: 5.2.15-2+b3]", &p),
              1, "parses an `apt list --upgradable` line");
    check_str(p.name, "bash", "apt list: the package name");
    check_int(p.installed, 1, "apt list: an upgradable package is installed");
    check_int(p.upgradable, 1, "apt list: it is upgradable");
    check_str(p.version, "5.2.15-2+b3", "apt list: 'from' is the installed version");
    check_str(p.candidate, "5.2.15-2+b4", "apt list: the offered version is the candidate");
    check_int(p.state, HDE_STORE_STATE_UPGRADABLE, "apt list: the state is Update available");
    check_int(p.verify, HDE_STORE_VERIFY_VERIFIED, "apt list: trixie is a verified repository");

    check_int(hde_store_parse_apt_list_line("adduser/now 3.134 all [installed,local]", &p), 1,
              "parses an installed package with no repository");
    check_int(p.installed, 1, "apt list: [installed,...] means installed");
    check_int(p.verify, HDE_STORE_VERIFY_UNVERIFIED, "apt list: 'now' is not a repository we can check");
    check_int(p.source, HDE_STORE_SOURCE_DPKG_STATUS, "apt list: 'now' comes from the dpkg database alone");

    check_int(hde_store_parse_apt_list_line("htop/trixie 3.2.2-2 amd64 [installed,automatic]", &p), 1,
              "parses an automatic dependency");
    check_int(p.automatic, 1, "apt list: [automatic] is recognised");
    check_int(p.state, HDE_STORE_STATE_INSTALLED, "apt list: an installed package without update");

    check_int(hde_store_parse_apt_list_line("vim/noble,now 2:9.1.0016-1ubuntu7 amd64 [installed]", &p), 1,
              "parses a package installed from a repository (suite,now)");
    check_int(p.installed, 1, "apt list: ',now' also means installed");

    check_int(hde_store_parse_apt_list_line("some-app/jammy 1.2 amd64 [installed]", &p), 1,
              "parses a package from a PPA suite");
    check_int(p.verify, HDE_STORE_VERIFY_VERIFIED, "apt list: an Ubuntu codename counts as the distribution");

    check_int(hde_store_parse_apt_list_line("Listing...", &p), 0, "the 'Listing...' header is not a package");
    check_int(hde_store_parse_apt_list_line("not-a-list-line", &p), 0, "a line without a suite is not a package");

    /* ---- 5. apt-cache search ---- */
    check_int(hde_store_parse_apt_search_line("gnome-weather - Weather app for the GNOME desktop", &p), 1,
              "parses an apt-cache search line");
    check_str(p.name, "gnome-weather", "search: the package name");
    check_str(p.summary, "Weather app for the GNOME desktop", "search: the summary");
    check_int(p.installed, 0, "search: a result is not installed");
    check_int(hde_store_parse_apt_search_line("   ", &p), 0, "an empty search line is not a package");

    /* ---- 6. apt-cache show (control stanza with a long description) ---- */
    const char *show =
        "Package: gnome-weather\n"
        "Version: 44.0-2\n"
        "Priority: optional\n"
        "Section: gnome\n"
        "Installed-Size: 1234\n"
        "Maintainer: Debian GNOME Maintainers <pkg-gnome-maintainers@lists.alioth.debian.org>\n"
        "Architecture: all\n"
        "Homepage: https://wiki.gnome.org/Apps/Weather\n"
        "Description: Weather app for the GNOME desktop\n"
        " A small application to show the current weather and the forecast.\n"
        " It uses the location services of the desktop.\n"
        "\n"
        "Package: something-else\n"
        "Version: 1.0\n";
    check_int(hde_store_parse_control_stanza(show, &p), 1, "parses an apt-cache show stanza");
    check_str(p.name, "gnome-weather", "show: the package name");
    check_str(p.version, "44.0-2", "show: the version");
    check_str(p.summary, "Weather app for the GNOME desktop", "show: the first description line is the summary");
    check(strstr(p.description, "current weather") != NULL, "show: the long description is kept");
    check(strstr(p.description, "something-else") == NULL, "show: only the first stanza is read");
    check_str(p.maintainer, "Debian GNOME Maintainers <pkg-gnome-maintainers@lists.alioth.debian.org>",
              "show: the maintainer");

    /* ---- 7. A local .deb ---- */
    const char *control =
        "Package: hyggshi-hello\n"
        "Version: 1.0.0\n"
        "Section: utils\n"
        "Maintainer: Hyggshi OS <dev@hyggshi.org>\n"
        "Installed-Size: 64\n"
        "Homepage: https://hyggshi.org\n"
        "Description: Hello from Hyggshi\n"
        " A tiny example package.\n";
    check_int(hde_store_from_deb_control(control, "/home/user/Downloads/hyggshi-hello.deb", &p), 1,
              "reads the control fields of a local .deb");
    check_str(p.name, "hyggshi-hello", "deb: the package name");
    check_int(p.source, HDE_STORE_SOURCE_LOCAL, "deb: the source is a local file");
    check_int(p.verify, HDE_STORE_VERIFY_UNVERIFIED, "deb: a local file is unverified");
    check_int(p.from_file, 1, "deb: it is marked as coming from a file");
    check_str(p.origin, "/home/user/Downloads/hyggshi-hello.deb", "deb: the file it came from is kept");

    /* ---- 8. apt-cache policy ---- */
    const char *policy =
        "gnome-weather:\n"
        "  Installed: 44.0-1\n"
        "  Candidate: 44.0-2\n"
        "  Version table:\n"
        " *** 44.0-1 100\n"
        "        100 /var/lib/dpkg/status\n"
        "     44.0-2 500\n"
        "        500 https://deb.debian.org/debian trixie/main amd64 Packages\n";
    char inst[80], cand[80], origin[96];
    check_int(hde_store_parse_apt_policy(policy, inst, sizeof inst, cand, sizeof cand, origin, sizeof origin), 1,
              "parses apt-cache policy");
    check_str(inst, "44.0-1", "policy: the installed version");
    check_str(cand, "44.0-2", "policy: the candidate version");
    check_str(origin, "trixie/main", "policy: where the candidate comes from");
    check_int(hde_store_verify_for(origin, HDE_STORE_SOURCE_DEB), HDE_STORE_VERIFY_VERIFIED,
              "a trixie/main candidate is verified");

    const char *policy_hand_installed =
        "adduser:\n"
        "  Installed: 3.134\n"
        "  Candidate: 3.134\n"
        "  Version table:\n"
        " *** 3.134 100\n"
        "        100 /var/lib/dpkg/status\n";
    check_int(hde_store_parse_apt_policy(policy_hand_installed, inst, sizeof inst, cand, sizeof cand,
                                        origin, sizeof origin), 1,
              "parses a policy with no repository at all");
    check_str(origin, "now", "policy: a package only in the dpkg database comes from 'now'");
    check_int(hde_store_verify_for(origin, HDE_STORE_SOURCE_DEB), HDE_STORE_VERIFY_UNVERIFIED,
              "a package with no repository is unverified");

    const char *policy_ppa =
        "neofetch:\n"
        "  Installed: (none)\n"
        "  Candidate: 7.1.0-1\n"
        "  Version table:\n"
        "     7.1.0-1 500\n"
        "        500 https://ppa.launchpadcontent.net/example/ppa/ubuntu noble/main amd64 Packages\n";
    check_int(hde_store_parse_apt_policy(policy_ppa, inst, sizeof inst, cand, sizeof cand, origin, sizeof origin), 1,
              "parses a policy of a package that is not installed");
    check_str(inst, "", "policy: an uninstalled package has no installed version");
    check_int(hde_store_verify_for("example/ppa", HDE_STORE_SOURCE_DEB), HDE_STORE_VERIFY_THIRD_PARTY,
              "an unknown repository is third party");

    /* ---- 9. Where it comes from ---- */
    check_int(hde_store_verify_for("trixie-security", HDE_STORE_SOURCE_DEB), HDE_STORE_VERIFY_VERIFIED,
              "trixie-security is part of the distribution");
    check_int(hde_store_verify_for("now/local", HDE_STORE_SOURCE_DEB), HDE_STORE_VERIFY_UNVERIFIED,
              "now/local is unverified");
    check_int(hde_store_verify_for("", HDE_STORE_SOURCE_DEB), HDE_STORE_VERIFY_UNKNOWN,
              "no origin at all is unknown");
    check_int(hde_store_verify_for("flathub", HDE_STORE_SOURCE_FLATPAK), HDE_STORE_VERIFY_UNVERIFIED,
              "a Flatpak is outside the .deb trust chain");
    check_str(hde_store_verify_title(HDE_STORE_VERIFY_VERIFIED), "Verified", "the title of a verified package");
    check_str(hde_store_source_title(HDE_STORE_SOURCE_LOCAL), "Local .deb file", "the title of a local .deb");
    char *detail = hde_store_verify_detail(HDE_STORE_VERIFY_THIRD_PARTY, "example/ppa");
    check(detail && strstr(detail, "example/ppa") != NULL, "the detail names the repository");
    free(detail);

    /* ---- 10. License ---- */
    char lic[48];
    const char *dep5 =
        "Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/\n"
        "Upstream-Name: gnome-weather\n"
        "\n"
        "Files: *\n"
        "Copyright: 2011-2024 The GNOME Project\n"
        "License: GPL-3+\n";
    check_int(hde_store_license_from_copyright(dep5, lic, sizeof lic), 1, "reads a DEP-5 License field");
    check_str(lic, "GPL-3+", "the license is GPL-3+");
    check_str(hde_store_license_title(lic), "GNU GPL 3 or later", "the friendly name of GPL-3+");

    const char *prose =
        "This package is free software; you can redistribute it and/or modify\n"
        "it under the terms of the GNU General Public License as published by\n"
        "the Free Software Foundation; version 2 of the License.\n";
    check_int(hde_store_license_from_copyright(prose, lic, sizeof lic), 1, "reads a license from prose");
    check_str(lic, "GPL-2+", "the prose is recognised as GPL-2+");

    const char *mit =
        "License: MIT\n"
        " Permission is hereby granted, free of charge, to any person obtaining a copy\n";
    check_int(hde_store_license_from_copyright(mit, lic, sizeof lic), 1, "reads an MIT copyright file");
    check_str(lic, "MIT", "MIT stays MIT (not counted twice)");

    const char *mixed =
        "License: GPL-2+\n"
        "License: LGPL-2.1+\n"
        "License: MIT\n"
        "License: BSD-3-clause\n";
    check_int(hde_store_license_from_copyright(mixed, lic, sizeof lic), 1, "reads several licenses");
    check_str(lic, "Multiple licenses (4)", "four licenses are summarised");
    check_int(hde_store_license_from_copyright("nothing to see here\n", lic, sizeof lic), 0,
              "a file without a license is not guessed");

    /* ---- 11. Flatpak (read-only) ---- */
    check_int(hde_store_parse_flatpak_line("org.gnome.Weather\t44.0\tflathub\tstable", &p), 1,
              "parses a `flatpak list` line");
    check_str(p.name, "org.gnome.Weather", "flatpak: the application id");
    check_str(p.version, "44.0", "flatpak: the version");
    check_str(p.origin, "flatpak: flathub", "flatpak: the remote is shown");
    check_str(p.branch, "stable", "flatpak: the branch");
    check_int(p.source, HDE_STORE_SOURCE_FLATPAK, "flatpak: the source");
    check_int(hde_store_parse_flatpak_line("application\tversion\torigin\tbranch", &p), 0,
              "the column header of flatpak list is not an application");

    /* ---- 12. Searching and ordering ---- */
    HdeStorePackage weather;
    memset(&weather, 0, sizeof weather);
    strcpy(weather.name, "gnome-weather");
    strcpy(weather.summary, "Weather app for the GNOME desktop");
    strcpy(weather.app_id, "org.gnome.Weather");
    check_int(hde_store_match(&weather, "weather"), 3, "'weather' is in the name");
    check_int(hde_store_match(&weather, "gnome-weather"), 4, "a name prefix is the best match");
    check_int(hde_store_match(&weather, "app"), 2, "a word in the summary matches");
    check_int(hde_store_match(&weather, "org.gnome"), 2, "the application id matches");
    check_int(hde_store_match(&weather, ""), 1, "an empty query matches everything");
    check_int(hde_store_match(&weather, "zzz"), 0, "an unknown word matches nothing");

    HdeStorePackage installing;
    memset(&installing, 0, sizeof installing);
    strcpy(installing.name, "bash");
    installing.installed = 1;
    installing.upgradable = 1;
    HdeStorePackage other;
    memset(&other, 0, sizeof other);
    strcpy(other.name, "aardvark");
    check(hde_store_package_before(&installing, &other) == 1, "a package with an update comes first in the list");
    check(hde_store_package_before(&other, &installing) == 0, "the other way round it comes last");

    /* ---- 12b. The sources of the machine ---- */
    HdeStoreSourceEntry src;
    check_int(hde_store_parse_sources_line("deb http://deb.debian.org/debian trixie main contrib", &src), 1,
              "parses a classic sources.list line");
    check_str(src.types, "deb", "sources: the type");
    check_str(src.uri, "http://deb.debian.org/debian", "sources: the URI");
    check_str(src.suite, "trixie", "sources: the suite");
    check_str(src.components, "main contrib", "sources: the components");
    check_int(src.enabled, 1, "sources: a plain line is enabled");

    check_int(hde_store_parse_sources_line("# deb http://deb.debian.org/debian trixie main", &src), 1,
              "a commented-out source is still shown");
    check_int(src.enabled, 0, "sources: a commented line is disabled");

    check_int(hde_store_parse_sources_line(
                  "deb [signed-by=/usr/share/keyrings/example.gpg arch=amd64] https://repo.example.com/apt stable main",
                  &src), 1, "parses a source with options");
    check_str(src.signed_by, "/usr/share/keyrings/example.gpg", "sources: signed-by is read");
    check_str(src.uri, "https://repo.example.com/apt", "sources: the URI comes after the options");
    check_int(hde_store_parse_sources_line("Types: deb", &src), 0, "a deb822 line is not a sources.list line");
    check_int(hde_store_parse_sources_line("deb", &src), 0, "a line with no URI is not a source");

    const char *deb822 =
        "Types: deb\n"
        "URIs: http://deb.debian.org/debian\n"
        "Suites: trixie trixie-updates\n"
        "Components: main\n"
        "Signed-By: /usr/share/keyrings/debian-archive-keyring.gpg\n"
        "\n"
        "Types: deb-src\n"
        "URIs: http://deb.debian.org/debian\n"
        "Suites: trixie\n"
        "Components: main\n";
    check_int(hde_store_parse_sources_deb822(deb822, 0, &src), 1, "parses the first deb822 stanza");
    check_str(src.types, "deb", "deb822: the types");
    check_str(src.suite, "trixie trixie-updates", "deb822: the suites");
    check_str(src.signed_by, "/usr/share/keyrings/debian-archive-keyring.gpg", "deb822: signed-by");
    check_int(hde_store_parse_sources_deb822(deb822, 1, &src), 1, "parses the second deb822 stanza");
    check_str(src.types, "deb-src", "deb822: the second stanza is the source one");
    check_int(hde_store_parse_sources_deb822(deb822, 2, &src), 0, "there is no third stanza");

    const char *deb822_disabled =
        "Types: deb\n"
        "URIs: https://repo.example.com/apt\n"
        "Suites: stable\n"
        "Components: main\n"
        "Enabled: no\n";
    check_int(hde_store_parse_sources_deb822(deb822_disabled, 0, &src), 1, "parses a disabled deb822 stanza");
    check_int(src.enabled, 0, "deb822: Enabled: no disables the source");

    char label[220];
    HdeStoreSourceEntry flat;
    memset(&flat, 0, sizeof flat);
    strcpy(flat.uri, "https://dl.flathub.org/repo/");
    strcpy(flat.suite, "flathub");
    strcpy(flat.signed_by, "");
    flat.flatpak = 1;
    hde_store_source_label(&flat, label, sizeof label);
    check_str(label, "flatpak: flathub", "a Flatpak remote is labelled as one");

    /* ---- 13. Sizes ---- */
    char size[32];
    hde_store_format_size(0, size, sizeof size);
    check_str(size, "—", "no size is shown as a dash");
    hde_store_format_size(512, size, sizeof size);
    check_str(size, "512 KB", "sizes under a megabyte stay in KB");
    hde_store_format_size(1536, size, sizeof size);
    check_str(size, "1.5 MB", "sizes are shown in MB");
    hde_store_format_size(3ULL * 1024 * 1024, size, sizeof size);
    check_str(size, "3.00 GB", "sizes are shown in GB");

    /* ---- 14. The commands the Store runs, and argv building ---- */
    check_str(HDE_STORE_CMD_INSTALL[0], "pkexec", "installs go through pkexec (authorisation, not root)");
    check_str(HDE_STORE_CMD_INSTALL[1], "apt-get", "the package tool is apt-get");
    check(strstr(HDE_STORE_CMD_DPKG_QUERY[2], "${binary:Package}") != NULL,
          "the dpkg-query format asks for the package name");
    char **argv = hde_store_argv_append(HDE_STORE_CMD_INSTALL, "gnome-weather", NULL);
    check(argv != NULL, "argv is built");
    if (argv) {
        int argc = 0;
        while (argv[argc]) argc++;
        check_int(argc, 6, "the install command gets one more word");
        check_str(argv[5], "gnome-weather", "the package name is the last word");
        check_str(argv[0], "pkexec", "and the first word is still pkexec");
        hde_store_argv_free(argv);
    }

    printf("\nstore-test: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
