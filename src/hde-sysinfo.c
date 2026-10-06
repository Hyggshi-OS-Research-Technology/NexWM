/* hde-sysinfo.c — see hde-sysinfo.h */
#define _DEFAULT_SOURCE
#include "hde-sysinfo.h"
#include "hde-build.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>

static char *read_line(const char *path)
{
    char *s = NULL;
    if (!g_file_get_contents(path, &s, NULL, NULL)) return NULL;
    g_strstrip(s);
    char *nl = strchr(s, '\n');
    if (nl) *nl = '\0';
    if (!*s) { g_free(s); return NULL; }
    return s;
}

static gboolean junk(const char *s)
{
    static const char *const bad[] = { "To Be Filled", "To be filled", "System Product Name", "System manufacturer",
                                       "Default string", "Not Applicable", "Not Specified", "None", "O.E.M.",
                                       "Type1ProductConfigId", "System Version", "x.x" };
    if (!s || !*s) return TRUE;
    for (guint i = 0; i < G_N_ELEMENTS(bad); i++)
        if (strstr(s, bad[i])) return TRUE;
    return FALSE;
}

static const char *short_vendor(const char *v)
{
    static const struct { const char *match, *name; } map[] = {
        { "LENOVO", "Lenovo" }, { "ASUSTeK", "ASUS" }, { "Hewlett-Packard", "HP" }, { "HP", "HP" },
        { "Dell", "Dell" }, { "Acer", "Acer" }, { "Micro-Star", "MSI" }, { "Gigabyte", "Gigabyte" },
        { "GIGABYTE", "Gigabyte" }, { "Apple", "Apple" }, { "SAMSUNG", "Samsung" }, { "TOSHIBA", "Toshiba" },
        { "Dynabook", "Dynabook" }, { "HUAWEI", "Huawei" }, { "Microsoft", "Microsoft" }, { "Framework", "Framework" },
        { "System76", "System76" }, { "TUXEDO", "TUXEDO" }, { "Razer", "Razer" }, { "XIAOMI", "Xiaomi" },
        { "Intel", "Intel" }, { "ASRock", "ASRock" }, { "Fujitsu", "Fujitsu" }, { "FUJITSU", "Fujitsu" },
    };
    for (guint i = 0; i < G_N_ELEMENTS(map); i++)
        if (g_str_has_prefix(v, map[i].match)) return map[i].name;
    return v;
}

static void load_model(HdeSysInfo *si)
{
    char *vendor = read_line("/sys/class/dmi/id/sys_vendor");
    char *product = read_line("/sys/class/dmi/id/product_name");
    char *version = read_line("/sys/class/dmi/id/product_version");
    char *board = read_line("/sys/class/dmi/id/board_name");
    char *bvendor = read_line("/sys/class/dmi/id/board_vendor");
    gboolean hv = FALSE;
    char *cpuinfo = NULL;
    if (g_file_get_contents("/proc/cpuinfo", &cpuinfo, NULL, NULL)) {
        hv = strstr(cpuinfo, " hypervisor") != NULL;
        g_free(cpuinfo);
    }
    const char *v = vendor ? vendor : "", *p = product ? product : "";
    if (strstr(v, "innotek") || strstr(p, "VirtualBox")) si->virt = g_strdup("VirtualBox");
    else if (strstr(v, "QEMU") || strstr(p, "KVM") || strstr(v, "Red Hat")) si->virt = g_strdup("QEMU/KVM");
    else if (strstr(v, "VMware")) si->virt = g_strdup("VMware");
    else if (strstr(v, "Microsoft") && strstr(p, "Virtual")) si->virt = g_strdup("Hyper-V");
    else if (strstr(v, "Xen") || strstr(p, "HVM domU")) si->virt = g_strdup("Xen");
    else if (strstr(v, "Parallels")) si->virt = g_strdup("Parallels");
    if (si->virt) {
        si->model = g_strdup_printf("%s virtual machine", si->virt);
    } else if (hv) {
        si->virt = g_strdup("unknown hypervisor");
        si->model = g_strdup("Virtual machine");
    } else {
        const char *name = !junk(p) ? p : NULL;
        /* Lenovo keeps the marketing name ("ThinkPad T480") in product_version */
        if (vendor && !strcmp(vendor, "LENOVO") && !junk(version)) name = version;
        if (!name && !junk(board)) name = board;
        const char *ven = !junk(v) ? v : !junk(bvendor) ? bvendor : NULL;
        if (ven && name) {
            const char *sv = short_vendor(ven);
            si->model = g_str_has_prefix(name, sv) ? g_strdup(name) : g_strdup_printf("%s %s", sv, name);
        } else if (name) {
            si->model = g_strdup(name);
        } else if (ven) {
            si->model = g_strdup(short_vendor(ven));
        }
    }
    g_free(vendor); g_free(product); g_free(version); g_free(board); g_free(bvendor);
}

static void load_cpu(HdeSysInfo *si)
{
    char *s = NULL;
    si->cpu_threads = (int)g_get_num_processors();
    if (!g_file_get_contents("/proc/cpuinfo", &s, NULL, NULL)) return;
    char **lines = g_strsplit(s, "\n", -1);
    for (int i = 0; lines[i] && !si->cpu; i++) {
        if (!g_str_has_prefix(lines[i], "model name") && !g_str_has_prefix(lines[i], "Hardware") &&
            !g_str_has_prefix(lines[i], "Model\t") && !g_str_has_prefix(lines[i], "cpu model")) continue;
        char *c = strchr(lines[i], ':');
        if (!c) continue;
        GString *g = g_string_new(g_strstrip(c + 1));
        static const char *const drop[] = { "(R)", "(r)", "(TM)", "(tm)", " CPU", " Processor" };
        for (guint k = 0; k < G_N_ELEMENTS(drop); k++) {
            char *f;
            while ((f = strstr(g->str, drop[k]))) g_string_erase(g, f - g->str, (gssize)strlen(drop[k]));
        }
        while (strstr(g->str, "  ")) {
            char *f = strstr(g->str, "  ");
            g_string_erase(g, f - g->str, 1);
        }
        si->cpu = g_string_free(g, FALSE);
        g_strstrip(si->cpu);
    }
    g_strfreev(lines);
    g_free(s);
}

static guint64 meminfo(const char *data, const char *key)
{
    const char *p = strstr(data, key);
    if (!p) return 0;
    return g_ascii_strtoull(p + strlen(key), NULL, 10) * 1024;
}

static void load_memory(HdeSysInfo *si)
{
    char *s = NULL;
    if (!g_file_get_contents("/proc/meminfo", &s, NULL, NULL)) return;
    si->mem_total = meminfo(s, "MemTotal:");
    si->mem_available = meminfo(s, "MemAvailable:");
    si->swap_total = meminfo(s, "SwapTotal:");
    si->swap_free = meminfo(s, "SwapFree:");
    g_free(s);
}

/* ---- graphics: PCI display controllers, named through pci.ids when it is installed ---- */
static char *pci_ids_lookup(unsigned vendor, unsigned device, char **vendor_name)
{
    static const char *const files[] = { "/usr/share/misc/pci.ids", "/usr/share/hwdata/pci.ids", "/usr/share/pci.ids" };
    char *data = NULL;
    for (guint i = 0; i < G_N_ELEMENTS(files) && !data; i++)
        if (!g_file_get_contents(files[i], &data, NULL, NULL)) data = NULL;
    if (!data) return NULL;
    char vid[8], did[8];
    g_snprintf(vid, sizeof vid, "%04x", vendor);
    g_snprintf(did, sizeof did, "%04x", device);
    char *res = NULL;
    gboolean in_vendor = FALSE;
    for (char *line = data, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = '\0';
        if (line[0] == '#' || !line[0]) continue;
        if (line[0] != '\t') {
            if (in_vendor) break;
            if (!g_ascii_strncasecmp(line, vid, 4) && line[4] == ' ') {
                in_vendor = TRUE;
                if (vendor_name) *vendor_name = g_strdup(g_strstrip(line + 4));
            }
            continue;
        }
        if (in_vendor && line[1] != '\t' && !g_ascii_strncasecmp(line + 1, did, 4) && line[5] == ' ') {
            res = g_strdup(g_strstrip(line + 5));
            break;
        }
    }
    g_free(data);
    return res;
}

static const char *vendor_short(unsigned id)
{
    switch (id) {
    case 0x8086: return "Intel";
    case 0x1002: return "AMD";
    case 0x10de: return "NVIDIA";
    case 0x15ad: return "VMware";
    case 0x80ee: return "VirtualBox";
    case 0x1af4: return "Virtio";
    case 0x1234: return "QEMU";
    case 0x1b36: return "QEMU (QXL)";
    case 0x1414: return "Microsoft Hyper-V";
    case 0x1a03: return "ASPEED";
    case 0x5143: return "Qualcomm";
    default: return NULL;
    }
}

static char *gpu_name(const char *dev, unsigned vendor, unsigned device)
{
    char *vname = NULL;
    char *dname = pci_ids_lookup(vendor, device, &vname);
    const char *vs = vendor_short(vendor);
    char *res = NULL;
    if (dname) {
        /* "Alder Lake-P GT2 [Iris Xe Graphics]" -> "Iris Xe Graphics" */
        char *lb = strrchr(dname, '['), *rb = lb ? strchr(lb, ']') : NULL;
        if (lb && rb && rb > lb + 1) {
            *rb = '\0';
            char *inner = g_strdup(lb + 1);
            g_free(dname);
            dname = inner;
        }
        const char *v = vs ? vs : vname ? vname : "";
        res = *v && !g_str_has_prefix(dname, v) ? g_strdup_printf("%s %s", v, dname) : g_strdup(dname);
    } else {
        char *drv_link = g_build_filename(dev, "driver", NULL);
        char *target = g_file_read_link(drv_link, NULL);
        char *drv = target ? g_path_get_basename(target) : NULL;
        res = g_strdup_printf("%s graphics%s%s%s", vs ? vs : vname ? vname : "Unknown", drv ? " (" : "", drv ? drv : "",
                              drv ? " driver)" : "");
        g_free(drv);
        g_free(target);
        g_free(drv_link);
    }
    g_free(dname);
    g_free(vname);
    return res;
}

/* Graphics without a PCI display controller (Hyper-V, ARM boards, firmware framebuffer): name from the DRM driver
 * or the framebuffer ("hyperv_drm", "hyperv_fb", "simpledrmdrmfb", "EFI VGA"). */
static char *gpu_from_driver(const char *drv)
{
    static const struct { const char *match, *name; } map[] = {
        { "hyperv", "Microsoft Hyper-V synthetic video" }, { "virtio", "Virtio GPU" }, { "vmwgfx", "VMware SVGA" },
        { "svga", "VMware SVGA" }, { "qxl", "QXL (QEMU/SPICE)" }, { "bochs", "QEMU standard VGA" },
        { "vboxvideo", "VirtualBox Graphics Adapter" }, { "cirrus", "Cirrus Logic (QEMU)" },
        { "i915", "Intel graphics" }, { "xe", "Intel graphics" }, { "amdgpu", "AMD graphics" },
        { "radeon", "AMD Radeon graphics" }, { "nouveau", "NVIDIA graphics (nouveau)" }, { "nvidia", "NVIDIA graphics" },
        { "vc4", "Broadcom VideoCore (Raspberry Pi)" }, { "v3d", "Broadcom VideoCore (Raspberry Pi)" },
        { "msm", "Qualcomm Adreno" }, { "panfrost", "Arm Mali" }, { "lima", "Arm Mali" }, { "etnaviv", "Vivante" },
        { "simpledrm", "Basic framebuffer (no graphics driver loaded)" }, { "simplefb", "Basic framebuffer (no graphics driver loaded)" },
        { "efifb", "Basic framebuffer (no graphics driver loaded)" }, { "EFI VGA", "Basic framebuffer (no graphics driver loaded)" },
        { "vesa", "VESA framebuffer (no graphics driver loaded)" }, { "VESA", "VESA framebuffer (no graphics driver loaded)" },
    };
    for (guint i = 0; i < G_N_ELEMENTS(map); i++)
        if (strstr(drv, map[i].match)) return g_strdup(map[i].name);
    return g_strdup_printf("%s driver", drv);
}

static void load_gpu(HdeSysInfo *si)
{
    GDir *d = g_dir_open("/sys/bus/pci/devices", 0, NULL);
    GString *all = g_string_new(NULL);
    const char *n;
    while (d && (n = g_dir_read_name(d))) {
        char *dev = g_build_filename("/sys/bus/pci/devices", n, NULL);
        char *cp = g_build_filename(dev, "class", NULL), *vp = g_build_filename(dev, "vendor", NULL);
        char *dp = g_build_filename(dev, "device", NULL);
        char *cls = read_line(cp), *ven = read_line(vp), *did = read_line(dp);
        if (cls && ven && did && (g_ascii_strtoull(cls, NULL, 16) >> 16) == 0x03) {
            char *g = gpu_name(dev, (unsigned)g_ascii_strtoull(ven, NULL, 16), (unsigned)g_ascii_strtoull(did, NULL, 16));
            g_string_append_printf(all, "%s%s", all->len ? " + " : "", g);
            g_free(g);
        }
        g_free(cls); g_free(ven); g_free(did); g_free(cp); g_free(vp); g_free(dp); g_free(dev);
    }
    if (d) g_dir_close(d);
    if (!all->len) {
        for (int c = 0; c < 4 && !all->len; c++) {
            char *link = g_strdup_printf("/sys/class/drm/card%d/device/driver", c);
            char *t = g_file_read_link(link, NULL);
            if (t) {
                char *b = g_path_get_basename(t), *g = gpu_from_driver(b);
                g_string_append(all, g);
                g_free(g);
                g_free(b);
                g_free(t);
            }
            g_free(link);
        }
    }
    if (!all->len) {
        char *fb = read_line("/proc/fb");             /* "0 hyperv_fb" */
        const char *name = fb ? strchr(fb, ' ') : NULL;
        if (name && *(name + 1)) {
            char *g = gpu_from_driver(name + 1);
            g_string_append(all, g);
            g_free(g);
        }
        g_free(fb);
    }
    si->gpu = all->len ? g_string_free(all, FALSE) : (g_string_free(all, TRUE), NULL);
}

void hde_sysinfo_load(HdeSysInfo *si)
{
    memset(si, 0, sizeof *si);
    si->hostname = g_strdup(g_get_host_name());
    load_model(si);
    load_cpu(si);
    load_memory(si);
    load_gpu(si);
    si->os = g_get_os_info(G_OS_INFO_KEY_PRETTY_NAME);
    struct utsname u;
    if (uname(&u) == 0) {
        si->kernel = g_strdup_printf("%s %s", u.sysname, u.release);
        si->arch = g_strdup(u.machine);
    }
    struct statvfs fs;
    if (statvfs("/", &fs) == 0) {
        si->disk_total = (guint64)fs.f_blocks * fs.f_frsize;
        si->disk_free = (guint64)fs.f_bavail * fs.f_frsize;
    }
    char *up = read_line("/proc/uptime");
    if (up) {
        si->uptime = g_ascii_strtod(up, NULL);
        g_free(up);
    }
}

void hde_sysinfo_clear(HdeSysInfo *si)
{
    g_free(si->hostname); g_free(si->model); g_free(si->virt); g_free(si->cpu); g_free(si->gpu);
    g_free(si->os); g_free(si->kernel); g_free(si->arch);
    memset(si, 0, sizeof *si);
}

char *hde_format_bytes(guint64 b)
{
    double mb = b / (1024.0 * 1024.0);
    if (mb < 10) return g_strdup_printf("%.1f MB", mb);
    if (mb < 1000) return g_strdup_printf("%.0f MB", mb);
    return g_strdup_printf("%.1f GB", mb / 1024.0);
}

/* ---- memory of the desktop's processes ---- */
static const struct { const char *name; const char *role; } known[] = {
    { "hde-session", "Session manager" },
    { "hde-panel", "Panel, Start menu, notifications" },
    { "hde-desktop", "Desktop icons and wallpaper" },
    { "hde-xsettings", "Settings service (theme, touchpad, screens)" },
    { "hde-hotkeys", "Keyboard shortcuts" },
    { "hde-settings", "Settings (this window)" },
    { "hde-screenshot", "Screenshot" },
    { "hde-start", "Session start script" },
    { "metacity", "Window manager (Metacity)" }, { "marco", "Window manager (Marco)" },
    { "mutter", "Window manager (Mutter)" }, { "muffin", "Window manager (Muffin)" },
    { "xfwm4", "Window manager (Xfwm4)" }, { "openbox", "Window manager (Openbox)" },
    { "icewm", "Window manager (IceWM)" }, { "fluxbox", "Window manager (Fluxbox)" },
    { "nexwm", "Window manager (NexWM)" },
    { "lxpolkit", "Password prompts (polkit agent)" }, { "polkit-gnome-au", "Password prompts (polkit agent)" },
    { "polkit-mate-aut", "Password prompts (polkit agent)" }, { "xfce-polkit", "Password prompts (polkit agent)" },
    { "lxqt-policykit-", "Password prompts (polkit agent)" }, { "mate-polkit", "Password prompts (polkit agent)" },
};

static guint64 status_kb(const char *data, const char *key)
{
    const char *p = strstr(data, key);
    return p ? g_ascii_strtoull(p + strlen(key), NULL, 10) * 1024 : 0;
}

static int cmp_pss(const void *a, const void *b)
{
    const HdeProcMem *x = a, *y = b;
    return x->pss < y->pss ? 1 : x->pss > y->pss ? -1 : 0;
}

static gboolean exclude_self;
void hde_sysinfo_exclude_self(gboolean exclude) { exclude_self = exclude; }

int hde_sysinfo_desktop_memory(HdeProcMem *out, int max, guint64 *total_pss, guint64 *total_rss)
{
    GDir *d = g_dir_open("/proc", 0, NULL);
    int n = 0;
    uid_t me = getuid();
    guint64 tp = 0, tr = 0;
    const char *e;
    while (d && (e = g_dir_read_name(d)) && n < max) {
        if (!isdigit((unsigned char)e[0])) continue;
        if (exclude_self && atoi(e) == (int)getpid()) continue;
        char path[64];
        struct stat st;
        g_snprintf(path, sizeof path, "/proc/%s", e);
        if (stat(path, &st) != 0 || st.st_uid != me) continue;
        g_snprintf(path, sizeof path, "/proc/%s/comm", e);
        char *comm = read_line(path);
        if (!comm) continue;
        const char *role = NULL;
        for (guint k = 0; k < G_N_ELEMENTS(known) && !role; k++)
            if (!strcmp(comm, known[k].name)) role = known[k].role;
        if (!role) { g_free(comm); continue; }
        HdeProcMem *m = &out[n];
        memset(m, 0, sizeof *m);
        m->pid = atoi(e);
        g_strlcpy(m->name, comm, sizeof m->name);
        m->role = role;
        g_free(comm);
        char *data = NULL;
        g_snprintf(path, sizeof path, "/proc/%s/status", e);
        if (g_file_get_contents(path, &data, NULL, NULL)) {
            m->rss = status_kb(data, "VmRSS:");
            g_free(data);
        }
        g_snprintf(path, sizeof path, "/proc/%s/smaps_rollup", e);
        if (g_file_get_contents(path, &data, NULL, NULL)) {
            m->pss = status_kb(data, "\nPss:");
            if (!m->pss) m->pss = status_kb(data, "Pss:");
            g_free(data);
        }
        if (!m->pss) m->pss = m->rss;              /* kernel without smaps_rollup (< 4.14) */
        if (!m->rss) continue;                     /* a zombie */
        tp += m->pss;
        tr += m->rss;
        n++;
    }
    if (d) g_dir_close(d);
    qsort(out, (size_t)n, sizeof out[0], cmp_pss);
    if (total_pss) *total_pss = tp;
    if (total_rss) *total_rss = tr;
    return n;
}

char *hde_sysinfo_report(const HdeSysInfo *si, const char *extra)
{
    GString *g = g_string_new(NULL);
    char *mt = hde_format_bytes(si->mem_total), *mu = hde_format_bytes(si->mem_total - si->mem_available);
    char *dt = hde_format_bytes(si->disk_total), *df = hde_format_bytes(si->disk_free);
    g_string_append_printf(g, "Hyggshi Desktop Environment %s (build %s)\n", HDE_RELEASE, HDE_VERSION);
    g_string_append_printf(g, "Device name: %s\n", si->hostname ? si->hostname : "?");
    g_string_append_printf(g, "Model: %s\n", si->model ? si->model : "unknown");
    g_string_append_printf(g, "Processor: %s (%d threads)\n", si->cpu ? si->cpu : "unknown", si->cpu_threads);
    g_string_append_printf(g, "Memory (RAM): %s, %s in use\n", mt, mu);
    g_string_append_printf(g, "Graphics: %s\n", si->gpu ? si->gpu : "unknown");
    g_string_append_printf(g, "Storage (/): %s, %s free\n", dt, df);
    g_string_append_printf(g, "Operating system: %s\n", si->os ? si->os : "unknown");
    g_string_append_printf(g, "Kernel: %s (%s)\n", si->kernel ? si->kernel : "?", si->arch ? si->arch : "?");
    if (extra && *extra) g_string_append(g, extra);
    HdeProcMem procs[48];
    guint64 pss = 0, rss = 0;
    int n = hde_sysinfo_desktop_memory(procs, G_N_ELEMENTS(procs), &pss, &rss);
    char *ps = hde_format_bytes(pss), *rs = hde_format_bytes(rss);
    g_string_append_printf(g, "Desktop memory now: %s (PSS: shared memory counted once; %s RSS) in %d processes\n",
                           ps, rs, n);
    g_free(rs);
    for (int i = 0; i < n; i++) {
        char *b = hde_format_bytes(procs[i].pss);
        g_string_append_printf(g, "  %-15s %8s  %s\n", procs[i].name, b, procs[i].role);
        g_free(b);
    }
    g_free(ps);
    g_free(mt); g_free(mu); g_free(dt); g_free(df);
    return g_string_free(g, FALSE);
}
