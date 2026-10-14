/* hde-resources.c — Hyggshi Resources: System resource monitor & application/process controller for HDE. */
#define _POSIX_C_SOURCE 200809L

#include "hde-build.h"
#include "hde-theme.h"
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <cairo.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <gdk/gdkx.h>
#include <gtk/gtk.h>
#include <math.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>

#define APP_TITLE "Hyggshi Resources"
#define HIST_LEN  60
#define MAX_CORES 64

typedef struct {
    unsigned long long user, nice, sys, idle, iowait, irq, softirq, steal;
} CpuTick;

typedef struct {
    pid_t pid;
    uid_t uid;
    char user[32];
    char name[64];
    char cmdline[256];
    char state;
    int nice_val;
    unsigned long long utime_ticks;
    unsigned long long stime_ticks;
    double cpu_pct;
    unsigned long rss_kb;
} ProcEntry;

static GtkCssProvider *app_css;
static GtkWidget *main_win;

/* Rolling history (0.0 .. 100.0 for CPU/Mem, KB/s for Net) */
static double cpu_hist[HIST_LEN];
static double mem_hist[HIST_LEN];
static double net_rx_hist[HIST_LEN];
static double net_tx_hist[HIST_LEN];
static double core_pct[MAX_CORES];
static int num_cores = 1;

static CpuTick prev_total_cpu;
static CpuTick prev_core_cpu[MAX_CORES];
static unsigned long long prev_net_rx_bytes;
static unsigned long long prev_net_tx_bytes;
static gint64 prev_sample_us;
static GHashTable *prev_proc_table; /* pid -> guint64 ticks */

static double cur_cpu_pct = 0.0;
static double cur_mem_pct = 0.0;
static double cur_swap_pct = 0.0;
static unsigned long long mem_total_kb = 0, mem_used_kb = 0, mem_avail_kb = 0;
static unsigned long long swap_total_kb = 0, swap_used_kb = 0;
static double cur_rx_kbs = 0.0, cur_tx_kbs = 0.0;
static unsigned long long total_rx_bytes = 0, total_tx_bytes = 0;

/* Overview widgets */
static GtkWidget *cpu_graph_area;
static GtkWidget *mem_graph_area;
static GtkWidget *net_graph_area;
static GtkWidget *cpu_summary_lbl;
static GtkWidget *mem_summary_lbl;
static GtkWidget *net_summary_lbl;
static GtkListStore *mounts_store;

/* Applications tab widgets */
static GtkListStore *apps_store;
static GtkWidget *apps_tree;
static GtkWidget *apps_count_lbl;

/* Processes tab widgets */
static GtkListStore *proc_store;
static GtkTreeModel *proc_filter_model;
static GtkTreeModel *proc_sort_model;
static GtkWidget *proc_tree;
static GtkWidget *proc_search_entry;
static GtkWidget *proc_only_mine_chk;
static GtkWidget *proc_pause_toggle;
static GtkWidget *proc_status_lbl;

static void load_css(void)
{
    HdeThemeInfo ti;
    hde_theme_info_load(&ti);
    char *accent = hde_theme_accent_css(&ti);
    char *css = g_strdup_printf(
        ".res-card { background-color: alpha(@theme_fg_color, 0.04);"
        "  border: 1px solid alpha(@theme_fg_color, 0.11);"
        "  border-radius: 10px; padding: 12px; }"
        ".res-title { font-size: 14px; font-weight: 700; }"
        ".res-metric { font-family: monospace; font-size: 13px; }"
        "%s",
        accent);
    if (!app_css) {
        app_css = gtk_css_provider_new();
        GdkScreen *scr = gdk_screen_get_default();
        if (scr)
            gtk_style_context_add_provider_for_screen(scr, GTK_STYLE_PROVIDER(app_css),
                                                      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    gtk_css_provider_load_from_data(app_css, css, -1, NULL);
    g_free(css);
    g_free(accent);
    hde_theme_info_clear(&ti);
}

static void on_theme_changed(gpointer d) { (void)d; load_css(); }

/* ---------------------------------------------------------------- Metrics Sampling */
static double cpu_tick_delta_pct(const CpuTick *prev, const CpuTick *cur, unsigned long long *total_delta_out)
{
    unsigned long long prev_idle = prev->idle + prev->iowait;
    unsigned long long cur_idle = cur->idle + cur->iowait;
    unsigned long long prev_total = prev_idle + prev->user + prev->nice + prev->sys + prev->irq + prev->softirq + prev->steal;
    unsigned long long cur_total = cur_idle + cur->user + cur->nice + cur->sys + cur->irq + cur->softirq + cur->steal;
    unsigned long long diff_total = (cur_total > prev_total) ? (cur_total - prev_total) : 0;
    unsigned long long diff_idle = (cur_idle > prev_idle) ? (cur_idle - prev_idle) : 0;
    if (total_delta_out) *total_delta_out = diff_total;
    if (diff_total == 0) return 0.0;
    double pct = 100.0 * (double)(diff_total - diff_idle) / (double)diff_total;
    return CLAMP(pct, 0.0, 100.0);
}

static unsigned long long sample_cpu(void)
{
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return 100;
    char line[256];
    unsigned long long total_delta = 100;
    int cores_seen = 0;

    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "cpu ", 4)) {
            CpuTick cur = {0};
            sscanf(line + 4, "%llu %llu %llu %llu %llu %llu %llu %llu",
                   &cur.user, &cur.nice, &cur.sys, &cur.idle,
                   &cur.iowait, &cur.irq, &cur.softirq, &cur.steal);
            cur_cpu_pct = cpu_tick_delta_pct(&prev_total_cpu, &cur, &total_delta);
            prev_total_cpu = cur;
        } else if (!strncmp(line, "cpu", 3) && isdigit((unsigned char)line[3])) {
            int idx = atoi(line + 3);
            if (idx >= 0 && idx < MAX_CORES) {
                CpuTick cur = {0};
                char tag[16];
                sscanf(line, "%15s %llu %llu %llu %llu %llu %llu %llu %llu",
                       tag, &cur.user, &cur.nice, &cur.sys, &cur.idle,
                       &cur.iowait, &cur.irq, &cur.softirq, &cur.steal);
                core_pct[idx] = cpu_tick_delta_pct(&prev_core_cpu[idx], &cur, NULL);
                prev_core_cpu[idx] = cur;
                if (idx + 1 > cores_seen) cores_seen = idx + 1;
            }
        }
    }
    fclose(f);
    if (cores_seen > 0) num_cores = cores_seen;

    memmove(&cpu_hist[0], &cpu_hist[1], sizeof(double) * (HIST_LEN - 1));
    cpu_hist[HIST_LEN - 1] = cur_cpu_pct;
    return total_delta > 0 ? total_delta : 100;
}

static void sample_memory(void)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return;
    char line[128];
    unsigned long long mem_free = 0, buffers = 0, cached = 0, swap_free = 0;
    mem_total_kb = mem_avail_kb = swap_total_kb = 0;

    while (fgets(line, sizeof line, f)) {
        unsigned long long v = 0;
        if (sscanf(line, "MemTotal: %llu kB", &v) == 1) mem_total_kb = v;
        else if (sscanf(line, "MemFree: %llu kB", &v) == 1) mem_free = v;
        else if (sscanf(line, "MemAvailable: %llu kB", &v) == 1) mem_avail_kb = v;
        else if (sscanf(line, "Buffers: %llu kB", &v) == 1) buffers = v;
        else if (sscanf(line, "Cached: %llu kB", &v) == 1) cached = v;
        else if (sscanf(line, "SwapTotal: %llu kB", &v) == 1) swap_total_kb = v;
        else if (sscanf(line, "SwapFree: %llu kB", &v) == 1) swap_free = v;
    }
    fclose(f);

    if (mem_avail_kb == 0) mem_avail_kb = mem_free + buffers + cached;
    mem_used_kb = (mem_total_kb > mem_avail_kb) ? (mem_total_kb - mem_avail_kb) : 0;
    swap_used_kb = (swap_total_kb > swap_free) ? (swap_total_kb - swap_free) : 0;

    cur_mem_pct = mem_total_kb ? (100.0 * (double)mem_used_kb / (double)mem_total_kb) : 0.0;
    cur_swap_pct = swap_total_kb ? (100.0 * (double)swap_used_kb / (double)swap_total_kb) : 0.0;

    memmove(&mem_hist[0], &mem_hist[1], sizeof(double) * (HIST_LEN - 1));
    mem_hist[HIST_LEN - 1] = cur_mem_pct;
}

static void sample_network(void)
{
    FILE *f = fopen("/proc/net/dev", "r");
    if (!f) return;
    char line[256];
    unsigned long long rx_sum = 0, tx_sum = 0;
    while (fgets(line, sizeof line, f)) {
        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = '\0';
        char *ifname = g_strstrip(line);
        if (!strcmp(ifname, "lo")) continue;
        unsigned long long rx = 0, tx = 0, dummy = 0;
        if (sscanf(colon + 1, "%llu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &rx, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &tx) >= 9) {
            rx_sum += rx;
            tx_sum += tx;
        }
    }
    fclose(f);

    gint64 now_us = g_get_monotonic_time();
    if (prev_sample_us > 0 && now_us > prev_sample_us) {
        double dt = (double)(now_us - prev_sample_us) / 1e6;
        cur_rx_kbs = (rx_sum >= prev_net_rx_bytes) ? ((double)(rx_sum - prev_net_rx_bytes) / 1024.0 / dt) : 0.0;
        cur_tx_kbs = (tx_sum >= prev_net_tx_bytes) ? ((double)(tx_sum - prev_net_tx_bytes) / 1024.0 / dt) : 0.0;
    }
    prev_net_rx_bytes = total_rx_bytes = rx_sum;
    prev_net_tx_bytes = total_tx_bytes = tx_sum;
    prev_sample_us = now_us;

    memmove(&net_rx_hist[0], &net_rx_hist[1], sizeof(double) * (HIST_LEN - 1));
    memmove(&net_tx_hist[0], &net_tx_hist[1], sizeof(double) * (HIST_LEN - 1));
    net_rx_hist[HIST_LEN - 1] = cur_rx_kbs;
    net_tx_hist[HIST_LEN - 1] = cur_tx_kbs;
}

static void refresh_mounts(void)
{
    if (!mounts_store) return;
    gtk_list_store_clear(mounts_store);
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) return;
    char dev[128], mnt[256], fstype[64], rest[256];
    while (fscanf(f, "%127s %255s %63s %255[^\n]\n", dev, mnt, fstype, rest) == 4) {
        if (strncmp(dev, "/dev/", 5) != 0 && strcmp(mnt, "/") != 0) continue;
        if (!strncmp(dev, "/dev/loop", 9)) continue;
        struct statvfs vfs;
        if (statvfs(mnt, &vfs) != 0 || vfs.f_blocks == 0) continue;
        unsigned long long total_b = (unsigned long long)vfs.f_blocks * vfs.f_frsize;
        unsigned long long avail_b = (unsigned long long)vfs.f_bavail * vfs.f_frsize;
        unsigned long long used_b = (total_b > avail_b) ? (total_b - avail_b) : 0;
        int pct = (int)(100.0 * (double)used_b / (double)total_b);

        char size_str[64];
        g_snprintf(size_str, sizeof size_str, "%.1f GiB / %.1f GiB (%d%%)",
                   (double)used_b / (1024.0 * 1024.0 * 1024.0),
                   (double)total_b / (1024.0 * 1024.0 * 1024.0), pct);

        GtkTreeIter it;
        gtk_list_store_append(mounts_store, &it);
        gtk_list_store_set(mounts_store, &it,
                           0, dev, 1, mnt, 2, fstype, 3, size_str, 4, pct, -1);
    }
    fclose(f);
}

static GArray *scan_processes(unsigned long long total_cpu_delta)
{
    if (!prev_proc_table)
        prev_proc_table = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);

    GArray *arr = g_array_new(FALSE, TRUE, sizeof(ProcEntry));
    DIR *d = opendir("/proc");
    if (!d) return arr;

    long page_kb = sysconf(_SC_PAGESIZE) / 1024;
    if (page_kb <= 0) page_kb = 4;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!isdigit((unsigned char)de->d_name[0])) continue;
        pid_t pid = atoi(de->d_name);
        if (pid <= 0) continue;

        char path[128], buf[1024];
        g_snprintf(path, sizeof path, "/proc/%d/stat", pid);
        FILE *sf = fopen(path, "r");
        if (!sf) continue;
        if (!fgets(buf, sizeof buf, sf)) { fclose(sf); continue; }
        fclose(sf);

        char *lparen = strchr(buf, '(');
        char *rparen = strrchr(buf, ')');
        if (!lparen || !rparen || rparen <= lparen) continue;

        ProcEntry pe;
        memset(&pe, 0, sizeof pe);
        pe.pid = pid;
        size_t nlen = (size_t)(rparen - lparen - 1);
        if (nlen >= sizeof pe.name) nlen = sizeof pe.name - 1;
        memcpy(pe.name, lparen + 1, nlen);
        pe.name[nlen] = '\0';

        unsigned long utime = 0, stime = 0;
        long nice_v = 0, rss_pages = 0;
        char state = 'S';
        /* Fields after ') ': state(3) ppid(4) pgrp(5) session(6) tty(7) tpgid(8) flags(9)
           minflt(10) cminflt(11) majflt(12) cmajflt(13) utime(14) stime(15) cutime(16) cstime(17)
           priority(18) nice(19) num_threads(20) itrealvalue(21) starttime(22) vsize(23) rss(24) */
        int matched = sscanf(rparen + 2,
                             "%c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu %*d %*d %*d %ld %*d %*d %*u %*u %ld",
                             &state, &utime, &stime, &nice_v, &rss_pages);
        if (matched < 5) continue;

        pe.state = state;
        pe.nice_val = (int)nice_v;
        pe.utime_ticks = utime;
        pe.stime_ticks = stime;
        pe.rss_kb = (rss_pages > 0) ? (unsigned long)rss_pages * (unsigned long)page_kb : 0;

        unsigned long long total_p = utime + stime;
        guint64 *prev_p = g_hash_table_lookup(prev_proc_table, GINT_TO_POINTER(pid));
        if (prev_p && total_p >= *prev_p && total_cpu_delta > 0) {
            pe.cpu_pct = 100.0 * (double)num_cores * (double)(total_p - *prev_p) / (double)total_cpu_delta;
            pe.cpu_pct = CLAMP(pe.cpu_pct, 0.0, 100.0 * num_cores);
        }
        guint64 *new_p = g_new(guint64, 1);
        *new_p = total_p;
        g_hash_table_replace(prev_proc_table, GINT_TO_POINTER(pid), new_p);

        /* UID & User */
        g_snprintf(path, sizeof path, "/proc/%d/status", pid);
        FILE *stf = fopen(path, "r");
        if (stf) {
            char sline[128];
            while (fgets(sline, sizeof sline, stf)) {
                unsigned int uid = 0;
                if (sscanf(sline, "Uid: %u", &uid) == 1) {
                    pe.uid = (uid_t)uid;
                    break;
                }
            }
            fclose(stf);
        }
        struct passwd *pw = getpwuid(pe.uid);
        g_snprintf(pe.user, sizeof pe.user, "%s", pw ? pw->pw_name : "root");

        /* Command line */
        g_snprintf(path, sizeof path, "/proc/%d/cmdline", pid);
        FILE *cf = fopen(path, "r");
        if (cf) {
            size_t r = fread(pe.cmdline, 1, sizeof pe.cmdline - 1, cf);
            fclose(cf);
            for (size_t i = 0; i < r; i++)
                if (pe.cmdline[i] == '\0') pe.cmdline[i] = ' ';
            pe.cmdline[r] = '\0';
            g_strstrip(pe.cmdline);
        }
        if (!pe.cmdline[0])
            g_snprintf(pe.cmdline, sizeof pe.cmdline, "[%s]", pe.name);

        g_array_append_val(arr, pe);
    }
    closedir(d);
    return arr;
}

/* ---------------------------------------------------------------- Cairo Graphs */
static void draw_graph_card(cairo_t *cr, int w, int h, const double *series, double max_val,
                            double r, double g, double b, const char *label_top)
{
    cairo_set_source_rgba(cr, 0.08, 0.10, 0.14, 0.85);
    cairo_rectangle(cr, 0, 0, w, h);
    cairo_fill(cr);

    /* Subtle grid lines */
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.08);
    cairo_set_line_width(cr, 1.0);
    for (int i = 1; i < 4; i++) {
        double y = (double)h * i / 4.0;
        cairo_move_to(cr, 0, y);
        cairo_line_to(cr, w, y);
    }
    cairo_stroke(cr);

    if (max_val <= 0.001) max_val = 1.0;

    /* Area fill */
    cairo_move_to(cr, 0, h);
    for (int i = 0; i < HIST_LEN; i++) {
        double x = (double)w * i / (double)(HIST_LEN - 1);
        double norm = CLAMP(series[i] / max_val, 0.0, 1.0);
        double y = h - norm * (h - 16) - 4;
        cairo_line_to(cr, x, y);
    }
    cairo_line_to(cr, w, h);
    cairo_close_path(cr);
    cairo_set_source_rgba(cr, r, g, b, 0.25);
    cairo_fill(cr);

    /* Stroke line */
    for (int i = 0; i < HIST_LEN; i++) {
        double x = (double)w * i / (double)(HIST_LEN - 1);
        double norm = CLAMP(series[i] / max_val, 0.0, 1.0);
        double y = h - norm * (h - 16) - 4;
        if (i == 0) cairo_move_to(cr, x, y);
        else cairo_line_to(cr, x, y);
    }
    cairo_set_source_rgba(cr, r, g, b, 0.95);
    cairo_set_line_width(cr, 2.2);
    cairo_stroke(cr);

    if (label_top && *label_top) {
        cairo_set_source_rgba(cr, 0.9, 0.94, 0.98, 0.9);
        cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 11.5);
        cairo_move_to(cr, 8, 16);
        cairo_show_text(cr, label_top);
    }
}

static gboolean on_draw_cpu_graph(GtkWidget *w, cairo_t *cr, gpointer d)
{
    (void)d;
    int width = gtk_widget_get_allocated_width(w);
    int height = gtk_widget_get_allocated_height(w);
    char top[64];
    g_snprintf(top, sizeof top, "Total CPU: %.1f%% (%d logical cores)", cur_cpu_pct, num_cores);
    draw_graph_card(cr, width, height, cpu_hist, 100.0, 0.22, 0.65, 0.98, top);
    return FALSE;
}

static gboolean on_draw_mem_graph(GtkWidget *w, cairo_t *cr, gpointer d)
{
    (void)d;
    int width = gtk_widget_get_allocated_width(w);
    int height = gtk_widget_get_allocated_height(w);
    char top[96];
    g_snprintf(top, sizeof top, "RAM: %.1f%% (%.2f / %.2f GiB)",
               cur_mem_pct, (double)mem_used_kb / 1048576.0, (double)mem_total_kb / 1048576.0);
    draw_graph_card(cr, width, height, mem_hist, 100.0, 0.25, 0.84, 0.52, top);
    return FALSE;
}

static gboolean on_draw_net_graph(GtkWidget *w, cairo_t *cr, gpointer d)
{
    (void)d;
    int width = gtk_widget_get_allocated_width(w);
    int height = gtk_widget_get_allocated_height(w);
    double peak = 64.0;
    for (int i = 0; i < HIST_LEN; i++) {
        if (net_rx_hist[i] > peak) peak = net_rx_hist[i];
        if (net_tx_hist[i] > peak) peak = net_tx_hist[i];
    }
    char top[96];
    g_snprintf(top, sizeof top, "Down: %.1f KB/s   Up: %.1f KB/s", cur_rx_kbs, cur_tx_kbs);
    draw_graph_card(cr, width, height, net_rx_hist, peak, 0.96, 0.62, 0.20, top);
    return FALSE;
}

/* ---------------------------------------------------------------- Applications Tab (X11 + /proc) */
static void refresh_applications_tab(GArray *procs)
{
    if (!apps_store) return;
    gtk_list_store_clear(apps_store);
    int app_count = 0;

    GdkDisplay *gdisp = gdk_display_get_default();
    if (gdisp && GDK_IS_X11_DISPLAY(gdisp)) {
        Display *dpy = gdk_x11_display_get_xdisplay(gdisp);
        Window root = DefaultRootWindow(dpy);
        Atom a_list = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
        Atom a_pid = XInternAtom(dpy, "_NET_WM_PID", False);
        Atom a_name = XInternAtom(dpy, "_NET_WM_NAME", False);
        Atom a_utf8 = XInternAtom(dpy, "UTF8_STRING", False);

        Atom actual_type;
        int actual_fmt;
        unsigned long nitems = 0, bytes_after = 0;
        unsigned char *data = NULL;

        gdk_x11_display_error_trap_push(gdisp);
        if (XGetWindowProperty(dpy, root, a_list, 0, 1024, False, XA_WINDOW,
                               &actual_type, &actual_fmt, &nitems, &bytes_after, &data) == Success && data) {
            Window *wins = (Window *)data;
            for (unsigned long i = 0; i < nitems; i++) {
                Window w = wins[i];
                unsigned char *pdata = NULL;
                Atom pt; int pf; unsigned long pn = 0, pb = 0;
                pid_t wpid = 0;
                if (XGetWindowProperty(dpy, w, a_pid, 0, 1, False, XA_CARDINAL,
                                       &pt, &pf, &pn, &pb, &pdata) == Success && pdata && pn >= 1) {
                    wpid = (pid_t)(*(unsigned long *)pdata);
                }
                if (pdata) XFree(pdata);

                char title[128] = "Application Window";
                unsigned char *ndata = NULL;
                if (XGetWindowProperty(dpy, w, a_name, 0, 64, False, a_utf8,
                                       &pt, &pf, &pn, &pb, &ndata) == Success && ndata && pn > 0) {
                    g_snprintf(title, sizeof title, "%s", (char *)ndata);
                }
                if (ndata) XFree(ndata);

                double cpu = 0.0;
                unsigned long rss = 0;
                char proc_name[64] = "";
                for (guint j = 0; j < procs->len; j++) {
                    ProcEntry *pe = &g_array_index(procs, ProcEntry, j);
                    if (pe->pid == wpid) {
                        cpu = pe->cpu_pct;
                        rss = pe->rss_kb;
                        g_snprintf(proc_name, sizeof proc_name, "%s", pe->name);
                        break;
                    }
                }
                char mem_s[32], cpu_s[32];
                g_snprintf(mem_s, sizeof mem_s, "%.1f MiB", (double)rss / 1024.0);
                g_snprintf(cpu_s, sizeof cpu_s, "%.1f%%", cpu);

                GtkTreeIter it;
                gtk_list_store_append(apps_store, &it);
                gtk_list_store_set(apps_store, &it,
                                   0, title,
                                   1, proc_name[0] ? proc_name : "X11 Client",
                                   2, (int)wpid,
                                   3, cpu_s,
                                   4, mem_s,
                                   5, (guint64)w,
                                   -1);
                app_count++;
            }
            XFree(data);
        }
        gdk_x11_display_error_trap_pop_ignored(gdisp);
    }

    /* Also list user GUI/desktop processes if no X11 windows matched */
    if (app_count == 0) {
        uid_t my_uid = getuid();
        for (guint j = 0; j < procs->len; j++) {
            ProcEntry *pe = &g_array_index(procs, ProcEntry, j);
            if (pe->uid != my_uid) continue;
            if (!strncmp(pe->name, "hde-", 4) || !strcmp(pe->name, "nexwm")) {
                char mem_s[32], cpu_s[32];
                g_snprintf(mem_s, sizeof mem_s, "%.1f MiB", (double)pe->rss_kb / 1024.0);
                g_snprintf(cpu_s, sizeof cpu_s, "%.1f%%", pe->cpu_pct);
                GtkTreeIter it;
                gtk_list_store_append(apps_store, &it);
                gtk_list_store_set(apps_store, &it,
                                   0, pe->cmdline, 1, pe->name, 2, (int)pe->pid,
                                   3, cpu_s, 4, mem_s, 5, (guint64)0, -1);
                app_count++;
            }
        }
    }

    if (apps_count_lbl) {
        char buf[64];
        g_snprintf(buf, sizeof buf, "%d running %s", app_count, app_count == 1 ? "application" : "applications");
        gtk_label_set_text(GTK_LABEL(apps_count_lbl), buf);
    }
}

static gboolean get_selected_app(int *pid_out, guint64 *xid_out, char **title_out)
{
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(apps_tree));
    GtkTreeModel *model = NULL;
    GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(sel, &model, &it)) return FALSE;
    gtk_tree_model_get(model, &it, 0, title_out, 2, pid_out, 5, xid_out, -1);
    return TRUE;
}

static void on_app_switch_to(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    int pid = 0; guint64 xid = 0; char *title = NULL;
    if (!get_selected_app(&pid, &xid, &title)) return;
    g_free(title);
    GdkDisplay *gdisp = gdk_display_get_default();
    if (xid && gdisp && GDK_IS_X11_DISPLAY(gdisp)) {
        Display *dpy = gdk_x11_display_get_xdisplay(gdisp);
        XEvent ev;
        memset(&ev, 0, sizeof ev);
        ev.xclient.type = ClientMessage;
        ev.xclient.window = (Window)xid;
        ev.xclient.message_type = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = 2;
        ev.xclient.data.l[1] = CurrentTime;
        XSendEvent(dpy, DefaultRootWindow(dpy), False,
                   SubstructureRedirectMask | SubstructureNotifyMask, &ev);
        XFlush(dpy);
    }
}

static void on_app_close_graceful(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    int pid = 0; guint64 xid = 0; char *title = NULL;
    if (!get_selected_app(&pid, &xid, &title)) return;
    g_free(title);
    GdkDisplay *gdisp = gdk_display_get_default();
    if (xid && gdisp && GDK_IS_X11_DISPLAY(gdisp)) {
        Display *dpy = gdk_x11_display_get_xdisplay(gdisp);
        XEvent ev;
        memset(&ev, 0, sizeof ev);
        ev.xclient.type = ClientMessage;
        ev.xclient.window = (Window)xid;
        ev.xclient.message_type = XInternAtom(dpy, "_NET_CLOSE_WINDOW", False);
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = CurrentTime;
        ev.xclient.data.l[1] = 2;
        XSendEvent(dpy, DefaultRootWindow(dpy), False,
                   SubstructureRedirectMask | SubstructureNotifyMask, &ev);
        XFlush(dpy);
    } else if (pid > 1) {
        kill(pid, SIGTERM);
    }
}

static void on_app_force_quit(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    int pid = 0; guint64 xid = 0; char *title = NULL;
    if (!get_selected_app(&pid, &xid, &title)) return;
    GtkWidget *dlg = gtk_message_dialog_new(
        GTK_WINDOW(main_win), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE,
        "Force quit “%s” (PID %d)?", title ? title : "Application", pid);
    gtk_message_dialog_format_secondary_text(
        GTK_MESSAGE_DIALOG(dlg),
        "Unsaved data in this application will be lost immediately.");
    gtk_dialog_add_buttons(GTK_DIALOG(dlg),
                           "_Cancel", GTK_RESPONSE_CANCEL,
                           "_Force Quit", GTK_RESPONSE_ACCEPT, NULL);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        if (pid > 1) kill(pid, SIGKILL);
    }
    gtk_widget_destroy(dlg);
    g_free(title);
}

/* ---------------------------------------------------------------- Processes Tab */
static const char *state_label(char s)
{
    switch (s) {
    case 'R': return "Running";
    case 'S': return "Sleeping";
    case 'D': return "Disk Wait";
    case 'T': return "Stopped";
    case 'Z': return "Zombie";
    case 'I': return "Idle";
    default:  return "Sleeping";
    }
}

static gboolean get_selected_proc(int *pid_out, char **name_out)
{
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(proc_tree));
    GtkTreeModel *model = NULL;
    GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(sel, &model, &it)) return FALSE;
    gtk_tree_model_get(model, &it, 0, pid_out, 1, name_out, -1);
    return TRUE;
}

static void refresh_processes_tab(GArray *procs)
{
    if (!proc_store) return;
    int sel_pid = -1;
    char *sel_name = NULL;
    get_selected_proc(&sel_pid, &sel_name);
    g_free(sel_name);

    gtk_list_store_clear(proc_store);
    for (guint i = 0; i < procs->len; i++) {
        ProcEntry *pe = &g_array_index(procs, ProcEntry, i);
        char cpu_s[32], mem_s[32];
        g_snprintf(cpu_s, sizeof cpu_s, "%.1f%%", pe->cpu_pct);
        g_snprintf(mem_s, sizeof mem_s, "%.1f MiB", (double)pe->rss_kb / 1024.0);

        GtkTreeIter it;
        gtk_list_store_append(proc_store, &it);
        gtk_list_store_set(proc_store, &it,
                           0, (int)pe->pid,
                           1, pe->name,
                           2, pe->user,
                           3, state_label(pe->state),
                           4, cpu_s,
                           5, mem_s,
                           6, pe->nice_val,
                           7, pe->cmdline,
                           8, pe->cpu_pct,
                           9, (guint64)pe->rss_kb,
                           10, (guint)pe->uid,
                           -1);
    }

    /* Restore selection by PID */
    if (sel_pid > 0 && proc_sort_model) {
        GtkTreeIter it;
        if (gtk_tree_model_get_iter_first(proc_sort_model, &it)) {
            do {
                int p = 0;
                gtk_tree_model_get(proc_sort_model, &it, 0, &p, -1);
                if (p == sel_pid) {
                    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(proc_tree));
                    gtk_tree_selection_select_iter(sel, &it);
                    break;
                }
            } while (gtk_tree_model_iter_next(proc_sort_model, &it));
        }
    }
}

static gboolean proc_visible_func(GtkTreeModel *model, GtkTreeIter *iter, gpointer d)
{
    (void)d;
    int pid = 0;
    char *name = NULL, *cmd = NULL, *user = NULL;
    guint uid = 0;
    gtk_tree_model_get(model, iter, 0, &pid, 1, &name, 2, &user, 7, &cmd, 10, &uid, -1);

    gboolean vis = TRUE;
    if (proc_only_mine_chk && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(proc_only_mine_chk))) {
        if ((uid_t)uid != getuid()) vis = FALSE;
    }
    if (vis && proc_search_entry) {
        const char *q = gtk_entry_get_text(GTK_ENTRY(proc_search_entry));
        if (q && *q) {
            char *ql = g_ascii_strdown(q, -1);
            char *nl = g_ascii_strdown(name ? name : "", -1);
            char *cl = g_ascii_strdown(cmd ? cmd : "", -1);
            char pid_s[32];
            g_snprintf(pid_s, sizeof pid_s, "%d", pid);
            if (!strstr(nl, ql) && !strstr(cl, ql) && !strstr(pid_s, ql))
                vis = FALSE;
            g_free(ql); g_free(nl); g_free(cl);
        }
    }
    g_free(name); g_free(user); g_free(cmd);
    return vis;
}

static void send_proc_signal(int sig, const char *verb)
{
    int pid = 0;
    char *name = NULL;
    if (!get_selected_proc(&pid, &name)) return;
    if (sig == SIGKILL) {
        GtkWidget *dlg = gtk_message_dialog_new(
            GTK_WINDOW(main_win), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE,
            "Force kill process “%s” (PID %d)?", name ? name : "", pid);
        gtk_message_dialog_format_secondary_text(
            GTK_MESSAGE_DIALOG(dlg),
            "Killing a process immediately can cause data loss or system instability.");
        gtk_dialog_add_buttons(GTK_DIALOG(dlg), "_Cancel", GTK_RESPONSE_CANCEL, "_Kill Process", GTK_RESPONSE_ACCEPT, NULL);
        int r = gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
        if (r != GTK_RESPONSE_ACCEPT) { g_free(name); return; }
    }
    char msg[128];
    if (kill(pid, sig) == 0)
        g_snprintf(msg, sizeof msg, "%s sent to %s (PID %d)", verb, name ? name : "process", pid);
    else
        g_snprintf(msg, sizeof msg, "Failed to control PID %d: %s", pid, strerror(errno));
    gtk_label_set_text(GTK_LABEL(proc_status_lbl), msg);
    g_free(name);
}

static void on_proc_end(GtkButton *b, gpointer d)  { (void)b; (void)d; send_proc_signal(SIGTERM, "SIGTERM (End)"); }
static void on_proc_kill(GtkButton *b, gpointer d) { (void)b; (void)d; send_proc_signal(SIGKILL, "SIGKILL (Force Kill)"); }
static void on_proc_stop(GtkButton *b, gpointer d) { (void)b; (void)d; send_proc_signal(SIGSTOP, "SIGSTOP (Paused)"); }
static void on_proc_cont(GtkButton *b, gpointer d) { (void)b; (void)d; send_proc_signal(SIGCONT, "SIGCONT (Resumed)"); }

static void on_proc_renice(GtkComboBox *cb, gpointer d)
{
    (void)d;
    int pid = 0; char *name = NULL;
    if (!get_selected_proc(&pid, &name)) return;
    const char *id = gtk_combo_box_get_active_id(cb);
    if (!id) { g_free(name); return; }
    int prio = atoi(id);
    char msg[128];
    if (setpriority(PRIO_PROCESS, (id_t)pid, prio) == 0)
        g_snprintf(msg, sizeof msg, "Set priority (nice %d) for %s (PID %d)", prio, name, pid);
    else
        g_snprintf(msg, sizeof msg, "Could not change priority for PID %d: %s", pid, strerror(errno));
    gtk_label_set_text(GTK_LABEL(proc_status_lbl), msg);
    g_free(name);
}

static void on_proc_filter_changed(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    if (proc_filter_model)
        gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(proc_filter_model));
}

/* ---------------------------------------------------------------- Periodic Tick */
static gboolean refresh_all(gpointer d)
{
    (void)d;
    unsigned long long cpu_delta = sample_cpu();
    sample_memory();
    sample_network();
    refresh_mounts();

    GArray *procs = scan_processes(cpu_delta);

    /* Update Overview labels */
    if (cpu_summary_lbl) {
        double l1 = 0, l5 = 0, l15 = 0;
        FILE *lf = fopen("/proc/loadavg", "r");
        if (lf) { fscanf(lf, "%lf %lf %lf", &l1, &l5, &l15); fclose(lf); }
        double up_sec = 0;
        FILE *uf = fopen("/proc/uptime", "r");
        if (uf) { fscanf(uf, "%lf", &up_sec); fclose(uf); }
        int up_h = (int)(up_sec / 3600);
        int up_m = ((int)up_sec % 3600) / 60;

        GString *cs = g_string_new("");
        g_string_printf(cs, "Load Average: %.2f, %.2f, %.2f   ·   Processes: %u   ·   Uptime: %dh %dm\n",
                        l1, l5, l15, procs->len, up_h, up_m);
        int show_cores = MIN(num_cores, 8);
        for (int i = 0; i < show_cores; i++) {
            g_string_append_printf(cs, "Core %d: %4.1f%%   ", i, core_pct[i]);
        }
        gtk_label_set_text(GTK_LABEL(cpu_summary_lbl), cs->str);
        g_string_free(cs, TRUE);
    }

    if (mem_summary_lbl) {
        char mbuf[160];
        g_snprintf(mbuf, sizeof mbuf,
                   "Used RAM: %.2f GiB / %.2f GiB (Available: %.2f GiB)   ·   Swap: %.2f GiB / %.2f GiB (%.1f%%)",
                   (double)mem_used_kb / 1048576.0, (double)mem_total_kb / 1048576.0,
                   (double)mem_avail_kb / 1048576.0,
                   (double)swap_used_kb / 1048576.0, (double)swap_total_kb / 1048576.0, cur_swap_pct);
        gtk_label_set_text(GTK_LABEL(mem_summary_lbl), mbuf);
    }

    if (net_summary_lbl) {
        char nbuf[160];
        g_snprintf(nbuf, sizeof nbuf,
                   "Receiving: %.1f KB/s (Total: %.2f MiB)   ·   Sending: %.1f KB/s (Total: %.2f MiB)",
                   cur_rx_kbs, (double)total_rx_bytes / 1048576.0,
                   cur_tx_kbs, (double)total_tx_bytes / 1048576.0);
        gtk_label_set_text(GTK_LABEL(net_summary_lbl), nbuf);
    }

    if (cpu_graph_area) gtk_widget_queue_draw(cpu_graph_area);
    if (mem_graph_area) gtk_widget_queue_draw(mem_graph_area);
    if (net_graph_area) gtk_widget_queue_draw(net_graph_area);

    refresh_applications_tab(procs);
    if (!proc_pause_toggle || !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(proc_pause_toggle)))
        refresh_processes_tab(procs);

    g_array_free(procs, TRUE);
    return G_SOURCE_CONTINUE;
}

/* ---------------------------------------------------------------- UI Builders */
static GtkWidget *build_overview_page(void)
{
    GtkWidget *sc = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(box), 16);

    /* CPU Card */
    GtkWidget *cpu_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_style_context_add_class(gtk_widget_get_style_context(cpu_card), "res-card");
    GtkWidget *ct = gtk_label_new("Processor (CPU)");
    gtk_label_set_xalign(GTK_LABEL(ct), 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(ct), "res-title");
    cpu_graph_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(cpu_graph_area, -1, 110);
    g_signal_connect(cpu_graph_area, "draw", G_CALLBACK(on_draw_cpu_graph), NULL);
    cpu_summary_lbl = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(cpu_summary_lbl), 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(cpu_summary_lbl), "res-metric");
    gtk_box_pack_start(GTK_BOX(cpu_card), ct, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(cpu_card), cpu_graph_area, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(cpu_card), cpu_summary_lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), cpu_card, FALSE, FALSE, 0);

    /* Memory Card */
    GtkWidget *mem_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_style_context_add_class(gtk_widget_get_style_context(mem_card), "res-card");
    GtkWidget *mt = gtk_label_new("Memory & Swap");
    gtk_label_set_xalign(GTK_LABEL(mt), 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(mt), "res-title");
    mem_graph_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(mem_graph_area, -1, 100);
    g_signal_connect(mem_graph_area, "draw", G_CALLBACK(on_draw_mem_graph), NULL);
    mem_summary_lbl = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(mem_summary_lbl), 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(mem_summary_lbl), "res-metric");
    gtk_box_pack_start(GTK_BOX(mem_card), mt, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(mem_card), mem_graph_area, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(mem_card), mem_summary_lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), mem_card, FALSE, FALSE, 0);

    /* Network Card */
    GtkWidget *net_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_style_context_add_class(gtk_widget_get_style_context(net_card), "res-card");
    GtkWidget *nt = gtk_label_new("Network Traffic");
    gtk_label_set_xalign(GTK_LABEL(nt), 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(nt), "res-title");
    net_graph_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(net_graph_area, -1, 90);
    g_signal_connect(net_graph_area, "draw", G_CALLBACK(on_draw_net_graph), NULL);
    net_summary_lbl = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(net_summary_lbl), 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(net_summary_lbl), "res-metric");
    gtk_box_pack_start(GTK_BOX(net_card), nt, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(net_card), net_graph_area, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(net_card), net_summary_lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), net_card, FALSE, FALSE, 0);

    /* Storage / Mounted Filesystems Card */
    GtkWidget *dsk_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_style_context_add_class(gtk_widget_get_style_context(dsk_card), "res-card");
    GtkWidget *dt = gtk_label_new("Storage & Mounted Filesystems");
    gtk_label_set_xalign(GTK_LABEL(dt), 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(dt), "res-title");

    mounts_store = gtk_list_store_new(5, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT);
    GtkWidget *mtree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(mounts_store));
    const char *mcols[] = { "Device", "Mount Point", "Type", "Used / Total" };
    for (int i = 0; i < 4; i++) {
        GtkCellRenderer *r = gtk_cell_renderer_text_new();
        GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes(mcols[i], r, "text", i, NULL);
        gtk_tree_view_column_set_resizable(c, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(mtree), c);
    }
    GtkCellRenderer *pr = gtk_cell_renderer_progress_new();
    GtkTreeViewColumn *pc = gtk_tree_view_column_new_with_attributes("Usage", pr, "value", 4, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(mtree), pc);

    gtk_box_pack_start(GTK_BOX(dsk_card), dt, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(dsk_card), mtree, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), dsk_card, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(sc), box);
    return sc;
}

static GtkWidget *build_applications_page(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(box), 16);

    GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *title = gtk_label_new("Running Applications");
    gtk_style_context_add_class(gtk_widget_get_style_context(title), "res-title");
    apps_count_lbl = gtk_label_new("0 running applications");
    gtk_box_pack_start(GTK_BOX(hdr), title, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(hdr), apps_count_lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), hdr, FALSE, FALSE, 0);

    apps_store = gtk_list_store_new(6, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT,
                                    G_TYPE_STRING, G_TYPE_STRING, G_TYPE_UINT64);
    apps_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(apps_store));
    const char *cols[] = { "Application / Window Title", "Process", "PID", "CPU %", "Memory" };
    for (int i = 0; i < 5; i++) {
        GtkCellRenderer *r = gtk_cell_renderer_text_new();
        GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes(cols[i], r, "text", i, NULL);
        gtk_tree_view_column_set_resizable(c, TRUE);
        if (i == 0) gtk_tree_view_column_set_expand(c, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(apps_tree), c);
    }

    GtkWidget *sc = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(sc), apps_tree);
    gtk_box_pack_start(GTK_BOX(box), sc, TRUE, TRUE, 0);

    GtkWidget *abar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *sw_btn = gtk_button_new_with_label("Switch To Window");
    GtkWidget *cl_btn = gtk_button_new_with_label("Close Application");
    GtkWidget *fq_btn = gtk_button_new_with_label("Force Quit Application…");
    g_signal_connect(sw_btn, "clicked", G_CALLBACK(on_app_switch_to), NULL);
    g_signal_connect(cl_btn, "clicked", G_CALLBACK(on_app_close_graceful), NULL);
    g_signal_connect(fq_btn, "clicked", G_CALLBACK(on_app_force_quit), NULL);
    gtk_box_pack_start(GTK_BOX(abar), sw_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(abar), cl_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(abar), fq_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), abar, FALSE, FALSE, 0);

    return box;
}

static GtkWidget *build_processes_page(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(box), 16);

    /* Filter Bar */
    GtkWidget *fbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    proc_search_entry = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(proc_search_entry), "Filter by process name, command, or PID…");
    g_signal_connect(proc_search_entry, "search-changed", G_CALLBACK(on_proc_filter_changed), NULL);

    proc_only_mine_chk = gtk_check_button_new_with_label("My Processes Only");
    g_signal_connect(proc_only_mine_chk, "toggled", G_CALLBACK(on_proc_filter_changed), NULL);

    proc_pause_toggle = gtk_toggle_button_new_with_label("Pause Updates");

    gtk_box_pack_start(GTK_BOX(fbar), proc_search_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(fbar), proc_only_mine_chk, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(fbar), proc_pause_toggle, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), fbar, FALSE, FALSE, 0);

    /* Process Table */
    proc_store = gtk_list_store_new(11,
        G_TYPE_INT,    /* 0: PID */
        G_TYPE_STRING, /* 1: Name */
        G_TYPE_STRING, /* 2: User */
        G_TYPE_STRING, /* 3: State */
        G_TYPE_STRING, /* 4: CPU % str */
        G_TYPE_STRING, /* 5: Mem str */
        G_TYPE_INT,    /* 6: Nice */
        G_TYPE_STRING, /* 7: Command line */
        G_TYPE_DOUBLE, /* 8: CPU % numeric sort */
        G_TYPE_UINT64, /* 9: RSS numeric sort */
        G_TYPE_UINT);  /* 10: UID */

    proc_filter_model = gtk_tree_model_filter_new(GTK_TREE_MODEL(proc_store), NULL);
    gtk_tree_model_filter_set_visible_func(GTK_TREE_MODEL_FILTER(proc_filter_model),
                                           proc_visible_func, NULL, NULL);
    proc_sort_model = gtk_tree_model_sort_new_with_model(proc_filter_model);
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(proc_sort_model), 8, GTK_SORT_DESCENDING);

    proc_tree = gtk_tree_view_new_with_model(proc_sort_model);
    struct { const char *title; int text_col; int sort_col; } cols[] = {
        { "PID",          0, 0 },
        { "Process Name", 1, 1 },
        { "User",         2, 2 },
        { "Status",       3, 3 },
        { "CPU %",        4, 8 },
        { "Memory",       5, 9 },
        { "Nice",         6, 6 },
        { "Command Line", 7, 7 },
    };
    for (int i = 0; i < 8; i++) {
        GtkCellRenderer *r = gtk_cell_renderer_text_new();
        GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes(cols[i].title, r, "text", cols[i].text_col, NULL);
        gtk_tree_view_column_set_sort_column_id(c, cols[i].sort_col);
        gtk_tree_view_column_set_resizable(c, TRUE);
        if (i == 7) gtk_tree_view_column_set_expand(c, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(proc_tree), c);
    }

    GtkWidget *sc = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(sc), proc_tree);
    gtk_box_pack_start(GTK_BOX(box), sc, TRUE, TRUE, 0);

    /* Action Bar */
    GtkWidget *abar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *end_btn = gtk_button_new_with_label("End Process (SIGTERM)");
    GtkWidget *kill_btn = gtk_button_new_with_label("Force Kill (SIGKILL)…");
    GtkWidget *stop_btn = gtk_button_new_with_label("Pause (SIGSTOP)");
    GtkWidget *cont_btn = gtk_button_new_with_label("Resume (SIGCONT)");

    GtkWidget *prio_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(prio_combo), "-5", "High Priority (-5)");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(prio_combo), "0",  "Normal Priority (0)");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(prio_combo), "10", "Low Priority (+10)");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(prio_combo), "0");

    g_signal_connect(end_btn, "clicked", G_CALLBACK(on_proc_end), NULL);
    g_signal_connect(kill_btn, "clicked", G_CALLBACK(on_proc_kill), NULL);
    g_signal_connect(stop_btn, "clicked", G_CALLBACK(on_proc_stop), NULL);
    g_signal_connect(cont_btn, "clicked", G_CALLBACK(on_proc_cont), NULL);
    g_signal_connect(prio_combo, "changed", G_CALLBACK(on_proc_renice), NULL);

    proc_status_lbl = gtk_label_new("Select a process to control it");
    gtk_label_set_xalign(GTK_LABEL(proc_status_lbl), 0);

    gtk_box_pack_start(GTK_BOX(abar), end_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(abar), kill_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(abar), stop_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(abar), cont_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(abar), prio_combo, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(abar), proc_status_lbl, TRUE, TRUE, 4);
    gtk_box_pack_start(GTK_BOX(box), abar, FALSE, FALSE, 0);

    return box;
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--version") || !strcmp(argv[i], "-v")) {
            printf("hde-resources (%s) %s %s\n", APP_TITLE, HDE_RELEASE, HDE_VERSION);
            return 0;
        }
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("Usage: hde-resources [--snapshot]\n"
                   "Hyggshi Resources — System resource monitor & application/process controller.\n");
            return 0;
        }
        if (!strcmp(argv[i], "--snapshot")) {
            unsigned long long d = sample_cpu();
            sample_memory();
            sample_network();
            GArray *procs = scan_processes(d);
            printf("CPU: %.1f%% (%d cores) | RAM: %llu / %llu kB (%.1f%%) | Processes: %u\n",
                   cur_cpu_pct, num_cores, mem_used_kb, mem_total_kb, cur_mem_pct, procs->len);
            g_array_free(procs, TRUE);
            return 0;
        }
    }

    gtk_init(&argc, &argv);
    hde_theme_apply_process();
    load_css();
    hde_theme_watch(on_theme_changed, NULL);

    main_win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(main_win), APP_TITLE);
    gtk_window_set_icon_name(GTK_WINDOW(main_win), "utilities-system-monitor");
    gtk_window_set_default_size(GTK_WINDOW(main_win), 920, 640);
    g_signal_connect(main_win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(main_win), root);

    GtkWidget *top_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(top_bar), 10);
    GtkWidget *stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
    gtk_stack_set_transition_duration(GTK_STACK(stack), hde_theme_animation_duration_ms());

    GtkWidget *sw = gtk_stack_switcher_new();
    gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(sw), GTK_STACK(stack));
    gtk_box_pack_start(GTK_BOX(top_bar), sw, TRUE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), top_bar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);

    gtk_stack_add_titled(GTK_STACK(stack), build_overview_page(), "overview", "System Resources");
    gtk_stack_add_titled(GTK_STACK(stack), build_applications_page(), "apps", "Applications");
    gtk_stack_add_titled(GTK_STACK(stack), build_processes_page(), "procs", "Processes");
    gtk_box_pack_start(GTK_BOX(root), stack, TRUE, TRUE, 0);

    /* Initial sample + 1500ms refresh timer */
    refresh_all(NULL);
    g_timeout_add(1500, refresh_all, NULL);

    gtk_widget_show_all(main_win);
    gtk_main();
    return 0;
}
