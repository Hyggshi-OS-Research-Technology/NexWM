/* NexSDK — the toolkit for building applications for Hyggshi OS and HDE.
 *
 * Why it exists: HDE is a desktop (a session, a panel, a set of settings in ~/.config/hde/settings.ini) and
 * Hyggshi OS is the system under it.  A program that wants to say something to the user, read what the
 * system is, or follow the desktop's settings used to have to read HDE's sources.  NexSDK is that, as three
 * small APIs and one packaging tool:
 *
 *   nexsdk/system.h    what this machine is: OS, kernel, session, desktop, memory, uptime
 *   nexsdk/notify.h    a notification, without a shell and without a toolkit
 *   nexsdk/settings.h  ~/.config/hde/settings.ini, read and written without losing comments
 *   nexsdk/pkg.h       checking a .deb before it is published (hde-pkg-check)
 *
 * There is no dependency beyond the C library and POSIX: an application already using GTK can use NexSDK
 * as well, and a command line tool that has no toolkit at all can use the same calls.  Nothing here starts
 * a D-Bus connection of its own, opens a display, or writes outside the user's own files.
 *
 *   #include <nexsdk/nexsdk.h>
 *
 *   NexSdkSystem sys;
 *   nexsdk_system_read(&sys);
 *   printf("%s on %s\n", sys.os_pretty, sys.kernel);
 *
 * Build: see nexsdk/README.md (`pkg-config --cflags --libs nexsdk`).
 */
#ifndef NEXSDK_NEXSDK_H
#define NEXSDK_NEXSDK_H

#include "nexsdk/system.h"
#include "nexsdk/notify.h"
#include "nexsdk/settings.h"
#include "nexsdk/pkg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NEXSDK_VERSION "1.0"
#define NEXSDK_VERSION_MAJOR 1
#define NEXSDK_VERSION_MINOR 0

/* The NexSDK release this program was built against, e.g. "1.0". */
const char *nexsdk_version(void);

/* True when this program runs inside an HDE session (HDE_SESSION_PID is set or an HDE component is running). */
int nexsdk_in_hde_session(void);

/* The Hyggshi OS release NexSDK was built for, e.g. "1.0"; "" when it cannot be read.  nex_sdk does not
 * allocate: the string lives as long as the program. */
const char *nexsdk_os_release(void);

#ifdef __cplusplus
}
#endif
#endif
