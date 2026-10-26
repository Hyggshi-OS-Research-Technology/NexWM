# NexSDK — building applications for Hyggshi OS and HDE

Hyggshi OS has a desktop (HDE), a window manager (NexWM) and one settings file that everything reads.
NexSDK is the small C library that lets any program talk to all of that **without reading HDE's sources**:
what the system is, a notification the desktop will show, the settings of the desktop (read *and* written,
without losing the comments), and a check for the `.deb` you are about to publish.

Everything is plain C11 and POSIX. There is no GTK in the library, no GLib, no D-Bus code of its own, and
nothing is written outside the user's own files — so the same calls work in a GTK application, in a
command line tool, in a background service and in the unit test.

```
nexsdk/
├── include/nexsdk/
│   ├── nexsdk.h       the umbrella header (everything below)
│   ├── system.h       OS, kernel, session, desktop, memory, uptime
│   ├── notify.h       a notification without a shell
│   ├── settings.h     ~/.config/hde/settings.ini, read and written safely
│   └── pkg.h          the engine of hde-pkg-check
├── src/               the implementation (one file per header, plus hde-core.c)
├── tools/             hde-pkg-check: check a .deb before it is published
├── examples/          four small programs that show each part
├── templates/         start a project: hello-c, gtk-app, library
└── nexsdk.pc.in       pkg-config (installed as nexsdk.pc)
```

## Building against it

After `sudo make install` in this repository:

```sh
cc $(pkg-config --cflags nexsdk) myapp.c $(pkg-config --libs nexsdk) -o myapp
```

Without installing anything, from the source tree:

```sh
cc -I nexsdk/include myapp.c build/libnexsdk.a -o myapp
```

CMake:

```cmake
find_package(PkgConfig REQUIRED)
pkg_check_modules(NEXSDK REQUIRED nexsdk)
target_include_directories(myapp PRIVATE ${NEXSDK_INCLUDE_DIRS})
target_link_directories(myapp PRIVATE ${NEXSDK_LIBRARY_DIRS})
target_link_libraries(myapp PRIVATE ${NEXSDK_LIBRARIES})
```

## 1. What this machine is

```c
#include <nexsdk/nexsdk.h>

NexSdkSystem sys;
nexsdk_system_read(&sys);

printf("%s, Linux %s, %s session, HDE %s\n", sys.os_pretty, sys.kernel,
       nexsdk_session_name(sys.session), sys.hde_version[0] ? sys.hde_version : "not installed");

printf("%llu MB of memory, up %ld hours\n", sys.mem_total_kb / 1024, sys.uptime_seconds / 3600);

char *line = nexsdk_system_summary(&sys);   /* "Hyggshi OS 1.0 · Linux 6.12.38 · wayland · HDE" */
free(line);
```

Everything comes from files and the environment (`/etc/os-release`, `uname(2)`, `/proc/meminfo`,
`/proc/uptime`, `XDG_SESSION_TYPE`, `XDG_CURRENT_DESKTOP`, `/usr/share/hde/version`). A field that cannot be
read stays empty; nothing is guessed. The parsers are public as well (`nexsdk_parse_os_release`,
`nexsdk_parse_meminfo`, `nexsdk_parse_uptime`) — that is what the unit test uses.

## 2. A notification

```c
NexSdkNotification n = NEXSDK_NOTIFICATION_INIT;
n.summary = "Backup finished";
n.body = "1,204 files copied to the external drive.";
n.icon = "drive-harddisk-symbolic";
n.app_name = "Hyggshi Backup";
n.urgency = NEXSDK_URGENCY_NORMAL;
n.timeout_ms = 6000;
nexsdk_notify_send(&n);
```

The notification goes to the desktop's own notification service, through `notify-send` run with an **argv
array** (`posix_spawn`), never through a shell. A summary like `It's done; rm -rf ~/ -- $(whoami)` is
delivered as text: there is nothing in NexSDK that could turn it into a command.

`nexsdk_notify_available()` says whether this session has a notification service at all (a bare TTY does
not), and `nexsdk_notify_argv()` builds the argument list if you would rather spawn the program yourself.

## 3. The settings of the desktop

HDE keeps one file: `${XDG_CONFIG_HOME:-~/.config}/hde/settings.ini`.

```c
NexSdkSettings *cfg = nexsdk_settings_open();          /* creates the directory when needed */
int style = nexsdk_settings_get_int(cfg, "settings", "theme_index", 0);   /* 1 light, 2 dark */
const char *accent = nexsdk_settings_get(cfg, "settings", "accent", "automatic");

nexsdk_settings_set(cfg, "myapp", "last_folder", "/home/me/Pictures");
nexsdk_settings_set_bool(cfg, "myapp", "remember", 1);
nexsdk_settings_save(cfg);                              /* free() writes too, so nothing is lost */
```

What it promises:

* **your comments survive** — the file is edited line by line, so an unrelated `# 1 = Light, 2 = Dark`
  stays where it is;
* **nobody's change is lost** — the file is read again before it is written, and the write goes to
  `settings.ini.new`, then a rename: an interrupted program cannot leave half a file;
* **the format is the one HDE uses** — `key=value`, `[groups]`, `# comments`, inline comments, quotes around
  a value, `true`/`false`.

Use your own group (`[myapp]`) for your own keys; `[settings]`, `[panel]`, `[desktop]` and `[hotkeys]`
belong to HDE.

## 4. Checking a `.deb` before publishing it

```sh
hde-pkg-check myapp_1.0-1_amd64.deb
hde-pkg-check --strict build/…            # warnings fail the build
hde-pkg-check --json build/…              # for a CI job
```

Exit status: `0` all good, `1` warnings, `2` errors, `3` the file cannot be read.

It checks the control fields (name, version with a Debian revision, architecture, a maintainer with an
address, a description with more than a synopsis, Depends for a binary, a Homepage that is a URL), the files
(nothing in `/usr/local`, no setuid unless it is the point, `/usr/share/doc/<package>/copyright` present) and
every `.desktop` file (Type, Name, and whether `Exec` and `Icon` name something the package actually
installs — the package is unpacked into a temporary directory to answer that).

In `debian/rules`, the last line of the build:

```make
override_dh_builddeb:
	dh_builddeb
	hde-pkg-check --strict ../myapp_*.deb
```

## 5. Templates

```sh
sudo cp -r /usr/share/nexsdk/templates/gtk-app ~/myapp     # or hello-c, or library
cd ~/myapp
cmake -S . -B build && cmake --build build
debuild -us -uc
hde-pkg-check ../gtk-app_1.0-1_amd64.deb
```

Each template is a complete project: sources, `CMakeLists.txt`, `debian/` (control with the right Depends,
rules that run `hde-pkg-check`, changelog, copyright, source format), a `.desktop` entry and, for the GTK
one, an icon that is installed where an icon belongs.

## 6. Examples

| program | what it shows |
| --- | --- |
| `examples/system-info.c` | the 20 lines every application starts with |
| `examples/hello-notify.c` | a notification; try it with a summary full of semicolons and quotes |
| `examples/settings-theme.c` | reading the theme, remembering a folder of your own |
| `examples/gtk-hello.c` | a GTK3 window and the same three calls |

```sh
cd nexsdk/examples && make FROM_TREE=1 && ./system-info
```

## The rules NexSDK follows

1. **Plain C, no toolkit.** A library that needs GTK cannot be used by a service or a test.
2. **No shell.** Anything that is run is run with `execvp`/`posix_spawn` and an argv array.
3. **Nothing is written outside the user's files**, and nothing is written at all unless asked.
4. **A missing fact is empty, not invented.** `nexsdk_system_read()` fills what it could read.
5. **The file belongs to the user.** Settings are edited in place, comments and order kept.

## Tests

```sh
make build/nexsdk-test && ./build/nexsdk-test
```

164 checks: the os-release/meminfo/uptime parsers, the notification argv (including that nothing is executed),
the settings file (comments kept, one key changed, new group at the end, atomic write, a missing file), and
the `.deb` checks (control rules, file list, `.desktop` files, a file that is not a package).

## License

MIT, like the rest of Hyggshi OS — see `LICENSE` in the repository root.
