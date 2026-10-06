/* hde-sysinfo.h — "About this computer" for Settings > About and `hde-settings --about`: the computer, the system,
 * and how much memory (RAM) the desktop itself uses right now. GLib only (no GTK, no X), reads /proc and /sys. */
#ifndef HDE_SYSINFO_H
#define HDE_SYSINFO_H

#include <glib.h>

typedef struct {
    char *hostname;
    char *model;            /* "Lenovo ThinkPad T480", "VirtualBox virtual machine" */
    char *virt;             /* hypervisor ("VirtualBox", "QEMU/KVM", "Hyper-V", ...), NULL on real hardware */
    char *cpu;              /* "Intel Core i5-8250U @ 1.60GHz" */
    int cpu_threads;
    char *gpu;              /* "Intel Iris Xe Graphics", "VMware SVGA II Adapter" */
    char *os;               /* PRETTY_NAME of /etc/os-release */
    char *kernel;           /* "Linux 6.12.38" */
    char *arch;             /* "x86_64" */
    guint64 mem_total, mem_available, swap_total, swap_free;     /* bytes */
    guint64 disk_total, disk_free;                               /* the / file system */
    double uptime;          /* seconds */
} HdeSysInfo;

void hde_sysinfo_load(HdeSysInfo *si);
void hde_sysinfo_clear(HdeSysInfo *si);

typedef struct {
    int pid;
    char name[32];          /* process name (hde-panel, metacity, ...) */
    const char *role;       /* "Panel, Start menu and notifications", "Window manager", ... */
    guint64 pss, rss;       /* bytes; PSS = its fair share of memory shared with other processes */
} HdeProcMem;

/* The desktop's processes of this user: the HDE programs, the window manager and the polkit agent HDE started.
 * Sorted by PSS, largest first. Returns how many were found (at most max). The PSS total is what the desktop
 * really costs (shared libraries counted once); the RSS total counts them in every process. */
int hde_sysinfo_desktop_memory(HdeProcMem *out, int max, guint64 *total_pss, guint64 *total_rss);

char *hde_format_bytes(guint64 bytes);              /* "86 MB", "3.8 GB" (g_free) */

/* Plain text for the "Copy system info" button and `hde-settings --about`. extra: lines added by the caller
 * (window manager, display server, screens), may be NULL. */
char *hde_sysinfo_report(const HdeSysInfo *si, const char *extra);

#endif
