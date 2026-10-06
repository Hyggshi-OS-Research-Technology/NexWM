# Logos of Linux distributions

Settings > About, the About window and (optionally) the Start button show the logo of the system HDE runs on,
found from `ID` / `ID_LIKE` / `LOGO` of `/etc/os-release`. HDE first uses the logo the system itself installs
(the `LOGO=` icon, `distributor-logo-<id>`, `emblem-<id>`, `/usr/share/pixmaps/<id>-logo.png`, ...); the files
here are only used when the system has none. Each file is named after the `ID=` of its distribution and is drawn by
HDE itself (src/hde-osinfo.c), so no SVG library is needed.

The logos are trademarks of their owners. HDE uses them only to say which system it runs on.

Path data: [Simple Icons](https://simpleicons.org) 16.34.0, CC0-1.0; the colours are the brand colours listed by
Simple Icons. Exceptions with their own license:

| File | Logo | License |
|---|---|---|
| debian.svg | Debian Open Use Logo, © Software in the Public Interest, Inc. and others | CC-BY-SA-3.0 (or LGPL-3.0-or-later) |
| gentoo.svg | Gentoo, © Gentoo Foundation | CC-BY-SA-2.5 |
| rocky.svg | Rocky Linux | CC-BY-SA-4.0 |
| nixos.svg | NixOS | CC-BY-4.0 |

Hyggshi OS (`ID=hyggshios`): drawn in code after `iso-config/branding/Logo.svg` of
[Hyggshi-OS](https://github.com/Hyggshi-OS-Research-Technology/Hyggshi-OS). Hyggshi OS itself installs it as the
`distributor-logo` icon (`LOGO=distributor-logo`), which is used first.

Not included because of their licenses: Fedora (brand guidelines; Fedora installs `fedora-logo-icon` itself),
MX Linux and Garuda Linux (GPL-3.0-only). Systems without any logo get a round badge with the first letter of their
name, in the colour of `ANSI_COLOR`.
