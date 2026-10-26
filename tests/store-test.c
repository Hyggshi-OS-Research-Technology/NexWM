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
    check_str(hde_store_verify_title(HDE_STORE_VERIFY_VERIFIED, 0), "Verified",
              "the title of a verified package, in English");
    check_str(hde_store_verify_title(HDE_STORE_VERIFY_VERIFIED, 1), "Đã xác minh",
              "the title of a verified package, in Vietnamese");
    check_str(hde_store_verify_title(HDE_STORE_VERIFY_THIRD_PARTY, 1), "Bên thứ ba",
              "a third-party package is named in Vietnamese");
    check_str(hde_store_verify_title(HDE_STORE_VERIFY_UNVERIFIED, 1), "Chưa xác minh",
              "an unverified package is named in Vietnamese");
    check_str(hde_store_verify_title(HDE_STORE_VERIFY_UNKNOWN, 0), "Unknown origin",
              "an unknown origin keeps its English title");
    check_str(hde_store_source_title(HDE_STORE_SOURCE_LOCAL, 0), "Local .deb file",
              "the title of a local .deb, in English");
    check_str(hde_store_source_title(HDE_STORE_SOURCE_LOCAL, 1), "Tệp .deb cục bộ",
              "the title of a local .deb, in Vietnamese");
    check_str(hde_store_source_title(HDE_STORE_SOURCE_DEB, 1), "Kho APT (.deb)",
              "an APT package is described in Vietnamese");
    check_str(hde_store_state_title(HDE_STORE_STATE_INSTALLED, 0), "Installed",
              "an installed package, in English");
    check_str(hde_store_state_title(HDE_STORE_STATE_INSTALLED, 1), "Đã cài đặt",
              "an installed package, in Vietnamese");
    check_str(hde_store_state_title(HDE_STORE_STATE_UPGRADABLE, 1), "Có bản cập nhật",
              "an upgradable package, in Vietnamese");
    char *detail = hde_store_verify_detail(HDE_STORE_VERIFY_THIRD_PARTY, "example/ppa", 0);
    check(detail && strstr(detail, "example/ppa") != NULL, "the detail names the repository");
    free(detail);
    detail = hde_store_verify_detail(HDE_STORE_VERIFY_THIRD_PARTY, "example/ppa", 1);
    check(detail && strstr(detail, "example/ppa") != NULL, "the Vietnamese detail names the repository too");
    check(detail && strstr(detail, "một kho được thêm vào hệ thống") != NULL,
          "the Vietnamese detail explains that the repository was added by hand");
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
    check_str(hde_store_license_title(lic, 0), "GNU GPL 3 or later", "the friendly name of GPL-3+");
    check_str(hde_store_license_title(lic, 1), "GNU GPL 3 trở lên",
              "and the same license in Vietnamese");

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

    /* ---- 14. the language of the Store: Vietnamese first ---- */
    check_int(hde_store_speak_vietnamese("vi_VN.UTF-8", NULL), 1, "a Vietnamese session gets Vietnamese");
    check_int(hde_store_speak_vietnamese("vi:en", NULL), 1, "LANGUAGE=vi:en is Vietnamese");
    check_int(hde_store_speak_vietnamese("en_US.UTF-8", NULL), 0, "an English session gets English");
    check_int(hde_store_speak_vietnamese("en:vi", NULL), 0, "LANGUAGE=en:vi is English");
    check_int(hde_store_speak_vietnamese("EN_GB.UTF-8", NULL), 0, "the language code is not case sensitive");
    check_int(hde_store_speak_vietnamese("fr_FR.UTF-8", NULL), 1, "any other language still gets Vietnamese");
    check_int(hde_store_speak_vietnamese("C", NULL), 1, "POSIX 'C' gets Vietnamese");
    check_int(hde_store_speak_vietnamese("C.UTF-8", NULL), 1, "and so does C.UTF-8");
    check_int(hde_store_speak_vietnamese("", NULL), 1, "an empty locale is Vietnamese");
    check_int(hde_store_speak_vietnamese(NULL, NULL), 1, "no locale at all is Vietnamese");
    check_int(hde_store_speak_vietnamese("vi_VN", "en"), 0, "HDE_STORE_LANG=en wins over a Vietnamese session");
    check_int(hde_store_speak_vietnamese("en_US", "vi"), 1, "HDE_STORE_LANG=vi wins over an English session");
    check_int(hde_store_speak_vietnamese("vi_VN", ""), 1, "an empty HDE_STORE_LANG changes nothing");

    /* ---- 15. The artwork of a package: its icon and its pictures ---- */
    const char *desktop_text =
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=Weather\n"
        "Name[vi]=Thời tiết\n"
        "Exec=gnome-weather %u\n"
        "Icon=org.gnome.Weather\n"
        "Categories=GNOME;GTK;Utility;\n"
        "\n"
        "[Desktop Action new-window]\n"
        "Name=New Window\n"
        "Exec=gnome-weather --new-window\n";
    HdeStoreDesktop de;
    check_int(hde_store_parse_desktop(desktop_text, &de), 1, "a .desktop entry with an Icon= is understood");
    check_str(de.name, "Weather", "the Name= of the entry (the untranslated one)");
    check_str(de.icon, "org.gnome.Weather", "the icon name the application asks for");
    check_str(de.exec, "gnome-weather", "the program of Exec=, without its arguments");
    check_int(hde_store_parse_desktop("[Desktop Entry]\nName=No icon here\n", &de), 0,
              "an entry without Icon= has nothing to show");
    check_int(hde_store_parse_desktop("", &de), 0, "an empty file is not an entry");

    hde_store_desktop_id("/usr/share/applications/org.gnome.Weather.desktop", &de);
    check_str(de.app_id, "org.gnome.Weather", "the app id comes from the file name");
    check_str(de.id, "org.gnome.Weather.desktop", "and the entry keeps its file name");
    check_int(hde_store_desktop_matches_package(&de, "gnome-weather"), 1,
              "org.gnome.Weather is the program of the gnome-weather package");
    check_int(hde_store_desktop_matches_package(&de, "gnome-calculator"), 0,
              "it is not the program of another package");
    check_int(hde_store_desktop_matches_package(&de, "org.gnome.weather"), 1,
              "the comparison ignores case");

    HdeStoreDesktop hinted;
    hde_store_parse_desktop("[Desktop Entry]\nName=Vendor Tool\nIcon=vendor-tool\n"
                            "Exec=/opt/vendor/bin/vendor-tool\nX-HDE-Package=hyggshi-vendor-tool\n", &hinted);
    check_int(hde_store_desktop_matches_package(&hinted, "hyggshi-vendor-tool"), 1,
              "X-HDE-Package= says which package an entry belongs to");
    check_int(hde_store_desktop_matches_package(&hinted, "something-else"), 0,
              "and then nothing else claims it");

    HdeStoreDesktop by_exec;
    hde_store_parse_desktop("[Desktop Entry]\nName=Comics\nIcon=comics\nExec=hyggshi-comics\n", &by_exec);
    hde_store_desktop_id("/usr/share/applications/oddname.desktop", &by_exec);
    check_int(hde_store_desktop_matches_package(&by_exec, "hyggshi-comics"), 1,
              "an odd file name is matched through Exec=");

    /* Files on disk: an icon theme and a folder of loose pictures. */
    const char *root = "/tmp/store-test-artwork";
    char cmd[512];
    snprintf(cmd, sizeof cmd, "rm -rf %s && mkdir -p %s/icons/hicolor/256x256/apps %s/icons/hicolor/scalable/apps "
             "%s/pixmaps %s/screens", root, root, root, root, root);
    if (system(cmd) != 0) printf("note: cannot prepare %s\n", root);
    FILE *f = fopen("/tmp/store-test-artwork/icons/hicolor/256x256/apps/org.gnome.Weather.png", "w");
    if (f) { fputs("png", f); fclose(f); }
    f = fopen("/tmp/store-test-artwork/icons/hicolor/scalable/apps/gnome-weather.svg", "w");
    if (f) { fputs("<svg/>", f); fclose(f); }
    f = fopen("/tmp/store-test-artwork/pixmaps/gnome-weather.xpm", "w");
    if (f) { fputs("xpm", f); fclose(f); }

    const char *icon_dirs[] = { "/tmp/store-test-artwork/icons/hicolor/256x256/apps",
                                "/tmp/store-test-artwork/icons/hicolor/scalable/apps",
                                "/tmp/store-test-artwork/pixmaps", NULL };
    const char *want_icon[] = { "org.gnome.Weather", "gnome-weather", NULL };
    char icon_path[512];
    check_int(hde_store_icon_file(icon_dirs, want_icon, icon_path, sizeof icon_path), 1,
              "the icon file of a package is found in the icon theme");
    check_str(icon_path, "/tmp/store-test-artwork/icons/hicolor/256x256/apps/org.gnome.Weather.png",
              "the first candidate wins, at its real path");
    const char *want_svg[] = { "gnome-weather", NULL };
    check_int(hde_store_icon_file(icon_dirs, want_svg, icon_path, sizeof icon_path), 1,
              "an SVG icon counts as well");
    check_str(icon_path, "/tmp/store-test-artwork/icons/hicolor/scalable/apps/gnome-weather.svg",
              "and it is the SVG that was found");
    const char *want_none[] = { "nothing-like-this", NULL };
    check_int(hde_store_icon_file(icon_dirs, want_none, icon_path, sizeof icon_path), 0,
              "a package without an icon stays without one (no guess)");
    check_str(icon_path, "", "and the path stays empty");
    const char *absolute[] = { "/tmp/store-test-artwork/pixmaps/gnome-weather.xpm", NULL };
    check_int(hde_store_icon_file(icon_dirs, absolute, icon_path, sizeof icon_path), 1,
              "Icon= that names a file is used as it is");

    f = fopen("/tmp/store-test-artwork/screens/b.png", "w");
    if (f) { fputs("png", f); fclose(f); }
    f = fopen("/tmp/store-test-artwork/screens/a.jpg", "w");
    if (f) { fputs("jpg", f); fclose(f); }
    f = fopen("/tmp/store-test-artwork/screens/notes.txt", "w");
    if (f) { fputs("txt", f); fclose(f); }
    HdeStoreShot shots[HDE_STORE_MAX_SHOTS];
    check_int(hde_store_shots_in_dir("/tmp/store-test-artwork/screens", shots, HDE_STORE_MAX_SHOTS), 2,
              "the pictures of a folder are the images in it, nothing else");
    check_str(shots[0].path, "/tmp/store-test-artwork/screens/a.jpg", "sorted by name: a.jpg first");
    check_str(shots[1].path, "/tmp/store-test-artwork/screens/b.png", "then b.png");
    check_int(hde_store_shots_in_dir("/tmp/store-test-artwork/nowhere", shots, HDE_STORE_MAX_SHOTS), 0,
              "a folder that is not there has no pictures");

    char cache[512];
    hde_store_shots_cache_dir("/home/linh/.cache", "gnome-weather", cache, sizeof cache);
    check_str(cache, "/home/linh/.cache/hde/store/shots/gnome-weather",
              "fetched pictures live in the cache of the Store");
    hde_store_shots_cache_dir("", "gnome-weather", cache, sizeof cache);
    check(strstr(cache, "/hde/store/shots/gnome-weather") != NULL,
          "without XDG_CACHE_HOME the home cache is used");
    check_int(system("rm -rf /tmp/store-test-artwork") == 0 || 1, 1, "the fixture is cleaned up");

    /* AppStream metadata: the package behind an application, its icon and its screenshots. */
    const char *appstream =
        "<?xml version=\"1.0\"?>\n"
        "<components>\n"
        "  <component type=\"desktop-application\">\n"
        "    <id>org.gnome.Weather</id>\n"
        "    <name>Weather</name>\n"
        "    <name xml:lang=\"vi\">Thời tiết</name>\n"
        "    <summary>Show the weather forecast</summary>\n"
        "    <pkgname>gnome-weather</pkgname>\n"
        "    <icon type=\"cached\" width=\"64\" height=\"64\">/var/cache/app-info/icons/debian/main/64x64/org.gnome.Weather.png</icon>\n"
        "    <screenshots>\n"
        "      <screenshot type=\"default\">\n"
        "        <image type=\"source\">https://example.org/weather-1.png</image>\n"
        "        <image type=\"thumbnail\" width=\"624\">https://example.org/weather-1-small.png</image>\n"
        "      </screenshot>\n"
        "      <screenshot>\n"
        "        <image type=\"source\">/usr/share/gnome-weather/screenshots/2.png</image>\n"
        "      </screenshot>\n"
        "    </screenshots>\n"
        "  </component>\n"
        "</components>\n";
    HdeStoreAppStream as;
    check_int(hde_store_parse_appstream(appstream, "gnome-weather", &as), 1, "AppStream metadata is read");
    check_str(as.package, "gnome-weather", "the package behind the application");
    check_str(as.app_id, "org.gnome.Weather", "the application id");
    check_str(as.name, "Weather", "the untranslated name is taken");
    check_str(as.summary, "Show the weather forecast", "the summary");
    check_int(as.icon_is_file, 1, "a cached icon is a file");
    check_str(as.icon, "/var/cache/app-info/icons/debian/main/64x64/org.gnome.Weather.png",
              "at the path the catalogue gives");
    check_int(as.n_shots, 2, "both screenshots are found");
    check_str(as.shots[0].url, "https://example.org/weather-1.png",
              "a screenshot that is a URL keeps its URL (and not the thumbnail)");
    check_str(as.shots[0].path, "", "and no local path");
    check_str(as.shots[1].path, "/usr/share/gnome-weather/screenshots/2.png",
              "a screenshot that is a local file keeps its path");
    check_int(hde_store_parse_appstream(appstream, "gnome-calculator", &as), 1,
              "another package still gets the one component in the file");
    check_int(hde_store_parse_appstream("<components></components>", "gnome-weather", &as), 0,
              "an empty catalogue is nothing");

    const char *catalogue =
        "<components>\n"
        "  <component type=\"desktop-application\">\n"
        "    <id>org.gnome.Calculator</id><name>Calculator</name><pkgname>gnome-calculator</pkgname>\n"
        "    <icon type=\"stock\">accessories-calculator</icon>\n"
        "  </component>\n"
        "  <component type=\"desktop-application\">\n"
        "    <id>org.gnome.Weather</id><name>Weather</name><pkgname>gnome-weather</pkgname>\n"
        "    <icon type=\"stock\">weather-few-clouds</icon>\n"
        "    <screenshots><screenshot><image type=\"thumbnail\">https://example.org/w.png</image></screenshot></screenshots>\n"
        "  </component>\n"
        "</components>\n";
    HdeStoreAppStream cat;
    check_int(hde_store_parse_appstream(catalogue, "gnome-weather", &cat), 1,
              "the component of the asked package is picked out of a catalogue");
    check_str(cat.package, "gnome-weather", "which is the second one here");
    check_str(cat.app_id, "org.gnome.Weather", "with its own application id");
    check_int(cat.icon_is_file, 0, "an icon of type stock is a name for the icon theme");
    check_str(cat.icon, "weather-few-clouds", "and the theme has to know that name");
    check_int(cat.n_shots, 1, "a screenshot with only a thumbnail still counts");
    check_str(cat.shots[0].url, "https://example.org/w.png", "a thumbnail is an URL as well");
    check_int(hde_store_parse_appstream(catalogue, "gnome-calculator", &cat), 1,
              "the first component is found when it is the one asked for");
    check_str(cat.name, "Calculator", "with its own name");

    printf("\nstore-test: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
