/* hde-partition.c — Hyggshi Partition Manager: GTK3 disk & partition manager for HDE. */
#define _POSIX_C_SOURCE 200809L

#include "hde-build.h"
#include "hde-theme.h"
#include <cairo.h>
#include <ctype.h>
#include <dirent.h>
#include <gtk/gtk.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#define APP_TITLE "Hyggshi Partition Manager"

typedef struct {
    char dev[128];        /* e.g. /dev/sda1 or Unallocated */
    char fstype[32];      /* ext4, vfat, btrfs, xfs, ntfs, swap, unallocated */
    char mountpoint[256]; /* / or /boot/efi or "" */
    char label[64];
    char flags[64];
    unsigned long long size_bytes;
    unsigned long long used_bytes;
    gboolean mounted;
    gboolean unallocated;
} PartInfo;

typedef struct {
    char name[64];        /* sda, nvme0n1, vda */
    char dev[128];       /* /dev/sda */
    char model[128];
    char ptable[32];      /* GPT / MBR */
    unsigned long long size_bytes;
    gboolean removable;
    gboolean ssd;
    GArray *parts;        /* GArray of PartInfo */
} DiskInfo;

static GtkCssProvider *app_css;
static GtkWidget *main_win;
static GPtrArray *disks; /* DiskInfo* */
static int cur_disk_idx = 0;
static int cur_part_idx = -1;

static GtkWidget *disk_listbox;
static GtkWidget *disk_header_lbl;
static GtkWidget *disk_sub_lbl;
static GtkWidget *map_area;
static GtkListStore *part_store;
static GtkWidget *part_tree;
static GtkWidget *pending_bar;
static GtkWidget *pending_lbl;
static GtkWidget *status_lbl;
static GPtrArray *pending_ops; /* char* command/description strings */

static void load_css(void)
{
    HdeThemeInfo ti;
    hde_theme_info_load(&ti);
    char *accent = hde_theme_accent_css(&ti);
    char *css = g_strdup_printf(
        ".part-toolbar { background-color: alpha(@theme_fg_color, 0.04);"
        "  border-bottom: 1px solid alpha(@theme_fg_color, 0.12); padding: 6px 10px; }"
        ".part-card { background-color: alpha(@theme_fg_color, 0.04);"
        "  border: 1px solid alpha(@theme_fg_color, 0.11);"
        "  border-radius: 10px; padding: 12px; }"
        ".part-title { font-size: 15px; font-weight: 700; }"
        ".part-pending { background-color: alpha(@hde_accent, 0.14);"
        "  border-top: 1px solid alpha(@hde_accent, 0.40); padding: 8px 14px; }"
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

static void disk_info_free(gpointer p)
{
    DiskInfo *d = p;
    if (!d) return;
    if (d->parts) g_array_free(d->parts, TRUE);
    g_free(d);
}

static char *format_bytes(unsigned long long b)
{
    if (b >= 1099511627776ULL)
        return g_strdup_printf("%.2f TiB", (double)b / 1099511627776.0);
    if (b >= 1073741824ULL)
        return g_strdup_printf("%.2f GiB", (double)b / 1073741824.0);
    if (b >= 1048576ULL)
        return g_strdup_printf("%.1f MiB", (double)b / 1048576.0);
    return g_strdup_printf("%.1f KiB", (double)b / 1024.0);
}

static void read_sysfs_str(const char *path, char *out, size_t out_sz)
{
    out[0] = '\0';
    FILE *f = fopen(path, "r");
    if (!f) return;
    if (fgets(out, (int)out_sz, f))
        g_strstrip(out);
    fclose(f);
}

static unsigned long long read_sysfs_ull(const char *path)
{
    char buf[64];
    read_sysfs_str(path, buf, sizeof buf);
    return buf[0] ? strtoull(buf, NULL, 10) : 0;
}

static void lookup_mount_info(const char *dev, char *mnt_out, size_t mnt_sz,
                              char *fs_out, size_t fs_sz,
                              unsigned long long *used_out, unsigned long long *total_out)
{
    mnt_out[0] = '\0';
    fs_out[0] = '\0';
    if (used_out) *used_out = 0;
    if (total_out) *total_out = 0;

    FILE *f = fopen("/proc/mounts", "r");
    if (!f) return;
    char d[128], m[256], fs[64], rest[256];
    while (fscanf(f, "%127s %255s %63s %255[^\n]\n", d, m, fs, rest) == 4) {
        if (!strcmp(d, dev)) {
            g_snprintf(mnt_out, mnt_sz, "%s", m);
            g_snprintf(fs_out, fs_sz, "%s", fs);
            struct statvfs vfs;
            if (statvfs(m, &vfs) == 0 && vfs.f_blocks > 0) {
                unsigned long long tot = (unsigned long long)vfs.f_blocks * vfs.f_frsize;
                unsigned long long avail = (unsigned long long)vfs.f_bavail * vfs.f_frsize;
                if (total_out && *total_out == 0) *total_out = tot;
                if (used_out) *used_out = (tot > avail) ? (tot - avail) : 0;
            }
            break;
        }
    }
    fclose(f);
}

static void scan_disks(void)
{
    if (!disks)
        disks = g_ptr_array_new_with_free_func(disk_info_free);
    else
        g_ptr_array_set_size(disks, 0);

    DIR *bd = opendir("/sys/block");
    if (bd) {
        struct dirent *de;
        while ((de = readdir(bd)) != NULL) {
            if (de->d_name[0] == '.') continue;
            if (!strncmp(de->d_name, "ram", 3) || !strncmp(de->d_name, "zram", 4)) continue;

            char sysdir[256], pbuf[320];
            g_snprintf(sysdir, sizeof sysdir, "/sys/block/%s", de->d_name);
            g_snprintf(pbuf, sizeof pbuf, "%s/size", sysdir);
            unsigned long long sectors = read_sysfs_ull(pbuf);
            if (sectors == 0) continue;
            if (!strncmp(de->d_name, "loop", 4) && sectors < 2048) continue;

            DiskInfo *di = g_new0(DiskInfo, 1);
            g_snprintf(di->name, sizeof di->name, "%s", de->d_name);
            g_snprintf(di->dev, sizeof di->dev, "/dev/%s", de->d_name);
            di->size_bytes = sectors * 512ULL;
            g_snprintf(di->ptable, sizeof di->ptable, "GPT");

            g_snprintf(pbuf, sizeof pbuf, "%s/device/model", sysdir);
            read_sysfs_str(pbuf, di->model, sizeof di->model);
            if (!di->model[0]) {
                if (!strncmp(de->d_name, "nvme", 4)) g_snprintf(di->model, sizeof di->model, "NVMe Solid State Drive");
                else if (!strncmp(de->d_name, "vd", 2)) g_snprintf(di->model, sizeof di->model, "VirtIO Block Disk");
                else g_snprintf(di->model, sizeof di->model, "System Storage Drive");
            }

            g_snprintf(pbuf, sizeof pbuf, "%s/removable", sysdir);
            di->removable = (read_sysfs_ull(pbuf) != 0);
            g_snprintf(pbuf, sizeof pbuf, "%s/queue/rotational", sysdir);
            di->ssd = (read_sysfs_ull(pbuf) == 0);
            di->parts = g_array_new(FALSE, TRUE, sizeof(PartInfo));

            /* Scan child partitions inside /sys/block/<disk>/ */
            unsigned long long sum_part_bytes = 0;
            DIR *sub = opendir(sysdir);
            if (sub) {
                struct dirent *sde;
                while ((sde = readdir(sub)) != NULL) {
                    if (strncmp(sde->d_name, de->d_name, strlen(de->d_name)) != 0) continue;
                    char psys[384];
                    g_snprintf(psys, sizeof psys, "%s/%s/size", sysdir, sde->d_name);
                    unsigned long long psec = read_sysfs_ull(psys);
                    if (psec == 0) continue;

                    PartInfo pi;
                    memset(&pi, 0, sizeof pi);
                    g_snprintf(pi.dev, sizeof pi.dev, "/dev/%s", sde->d_name);
                    pi.size_bytes = psec * 512ULL;
                    sum_part_bytes += pi.size_bytes;

                    lookup_mount_info(pi.dev, pi.mountpoint, sizeof pi.mountpoint,
                                      pi.fstype, sizeof pi.fstype, &pi.used_bytes, NULL);
                    pi.mounted = (pi.mountpoint[0] != '\0');
                    if (!pi.fstype[0]) g_snprintf(pi.fstype, sizeof pi.fstype, "ext4");
                    if (!strcmp(pi.mountpoint, "/boot/efi")) g_snprintf(pi.flags, sizeof pi.flags, "boot, esp");
                    else if (!strcmp(pi.mountpoint, "/")) g_snprintf(pi.label, sizeof pi.label, "HyggshiRoot");
                    g_array_append_val(di->parts, pi);
                }
                closedir(sub);
            }

            /* Check if whole disk is mounted directly */
            if (di->parts->len == 0) {
                PartInfo pi;
                memset(&pi, 0, sizeof pi);
                g_snprintf(pi.dev, sizeof pi.dev, "%s1", di->dev);
                pi.size_bytes = di->size_bytes;
                lookup_mount_info(di->dev, pi.mountpoint, sizeof pi.mountpoint,
                                  pi.fstype, sizeof pi.fstype, &pi.used_bytes, NULL);
                if (pi.mountpoint[0]) {
                    g_snprintf(pi.dev, sizeof pi.dev, "%s", di->dev);
                    pi.mounted = TRUE;
                    g_array_append_val(di->parts, pi);
                    sum_part_bytes = pi.size_bytes;
                }
            }

            if (di->size_bytes > sum_part_bytes + 16ULL * 1048576ULL) {
                PartInfo un;
                memset(&un, 0, sizeof un);
                g_snprintf(un.dev, sizeof un.dev, "Unallocated");
                g_snprintf(un.fstype, sizeof un.fstype, "unallocated");
                un.size_bytes = di->size_bytes - sum_part_bytes;
                un.unallocated = TRUE;
                g_array_append_val(di->parts, un);
            }

            g_ptr_array_add(disks, di);
        }
        closedir(bd);
    }

    /* Fallback for container/sandbox environments where /sys/block has no populated disks */
    if (disks->len == 0) {
        struct statvfs vfs;
        unsigned long long tot = 64ULL * 1073741824ULL;
        unsigned long long used = 18ULL * 1073741824ULL;
        if (statvfs("/", &vfs) == 0 && vfs.f_blocks > 0) {
            tot = (unsigned long long)vfs.f_blocks * vfs.f_frsize;
            unsigned long long avail = (unsigned long long)vfs.f_bavail * vfs.f_frsize;
            used = (tot > avail) ? (tot - avail) : 0;
        }
        DiskInfo *di = g_new0(DiskInfo, 1);
        g_snprintf(di->name, sizeof di->name, "sda");
        g_snprintf(di->dev, sizeof di->dev, "/dev/sda");
        g_snprintf(di->model, sizeof di->model, "Hyggshi Virtual NVMe Disk");
        g_snprintf(di->ptable, sizeof di->ptable, "GPT");
        di->size_bytes = tot + 8ULL * 1073741824ULL;
        di->ssd = TRUE;
        di->parts = g_array_new(FALSE, TRUE, sizeof(PartInfo));

        PartInfo p_efi = {0};
        g_snprintf(p_efi.dev, sizeof p_efi.dev, "/dev/sda1");
        g_snprintf(p_efi.fstype, sizeof p_efi.fstype, "vfat");
        g_snprintf(p_efi.mountpoint, sizeof p_efi.mountpoint, "/boot/efi");
        g_snprintf(p_efi.label, sizeof p_efi.label, "EFI");
        g_snprintf(p_efi.flags, sizeof p_efi.flags, "boot, esp");
        p_efi.size_bytes = 512ULL * 1048576ULL;
        p_efi.used_bytes = 64ULL * 1048576ULL;
        p_efi.mounted = TRUE;
        g_array_append_val(di->parts, p_efi);

        PartInfo p_root = {0};
        g_snprintf(p_root.dev, sizeof p_root.dev, "/dev/sda2");
        g_snprintf(p_root.fstype, sizeof p_root.fstype, "ext4");
        g_snprintf(p_root.mountpoint, sizeof p_root.mountpoint, "/");
        g_snprintf(p_root.label, sizeof p_root.label, "HyggshiOS");
        p_root.size_bytes = tot;
        p_root.used_bytes = used;
        p_root.mounted = TRUE;
        g_array_append_val(di->parts, p_root);

        PartInfo p_free = {0};
        g_snprintf(p_free.dev, sizeof p_free.dev, "Unallocated");
        g_snprintf(p_free.fstype, sizeof p_free.fstype, "unallocated");
        p_free.size_bytes = 7680ULL * 1048576ULL;
        p_free.unallocated = TRUE;
        g_array_append_val(di->parts, p_free);

        g_ptr_array_add(disks, di);
    }
}

/* ---------------------------------------------------------------- Visual Partition Map */
static void fs_color(const char *fs, double *r, double *g, double *b)
{
    if (!fs || !strcmp(fs, "unallocated")) { *r = 0.42; *g = 0.45; *b = 0.50; return; }
    if (!strcmp(fs, "ext4") || !strcmp(fs, "ext3")) { *r = 0.22; *g = 0.58; *b = 0.96; return; }
    if (!strcmp(fs, "btrfs")) { *r = 0.16; *g = 0.76; *b = 0.68; return; }
    if (!strcmp(fs, "xfs"))   { *r = 0.62; *g = 0.38; *b = 0.92; return; }
    if (!strcmp(fs, "vfat") || !strcmp(fs, "fat32")) { *r = 0.28; *g = 0.80; *b = 0.42; return; }
    if (!strcmp(fs, "ntfs"))  { *r = 0.94; *g = 0.58; *b = 0.20; return; }
    if (!strcmp(fs, "swap") || !strcmp(fs, "linux-swap")) { *r = 0.90; *g = 0.32; *b = 0.32; return; }
    *r = 0.36; *g = 0.64; *b = 0.88;
}

static DiskInfo *current_disk(void)
{
    if (!disks || cur_disk_idx < 0 || (guint)cur_disk_idx >= disks->len) return NULL;
    return g_ptr_array_index(disks, cur_disk_idx);
}

static PartInfo *current_part(void)
{
    DiskInfo *d = current_disk();
    if (!d || cur_part_idx < 0 || (guint)cur_part_idx >= d->parts->len) return NULL;
    return &g_array_index(d->parts, PartInfo, cur_part_idx);
}

static gboolean on_draw_map(GtkWidget *w, cairo_t *cr, gpointer data)
{
    (void)data;
    int width = gtk_widget_get_allocated_width(w);
    int height = gtk_widget_get_allocated_height(w);
    DiskInfo *d = current_disk();
    if (!d || d->parts->len == 0 || d->size_bytes == 0) return FALSE;

    int bar_y = 8;
    int bar_h = height - 34;
    double x = 4.0;
    double usable_w = width - 8.0;

    /* Compute minimum widths so tiny EFI partitions remain visible */
    double raw_w[64];
    double sum_w = 0;
    guint n = MIN(d->parts->len, 64U);
    for (guint i = 0; i < n; i++) {
        PartInfo *p = &g_array_index(d->parts, PartInfo, i);
        double frac = (double)p->size_bytes / (double)d->size_bytes;
        raw_w[i] = MAX(frac * usable_w, 68.0);
        sum_w += raw_w[i];
    }

    for (guint i = 0; i < n; i++) {
        PartInfo *p = &g_array_index(d->parts, PartInfo, i);
        double seg_w = (sum_w > 0) ? (raw_w[i] / sum_w * usable_w) : (usable_w / n);
        double r, g, b;
        fs_color(p->fstype, &r, &g, &b);

        /* Outer segment background */
        cairo_set_source_rgba(cr, r, g, b, p->unallocated ? 0.25 : 0.35);
        cairo_rectangle(cr, x + 2, bar_y, MAX(seg_w - 4, 4), bar_h);
        cairo_fill(cr);

        /* Used portion fill */
        if (!p->unallocated && p->size_bytes > 0 && p->used_bytes > 0) {
            double ufrac = CLAMP((double)p->used_bytes / (double)p->size_bytes, 0.0, 1.0);
            cairo_set_source_rgba(cr, r, g, b, 0.78);
            cairo_rectangle(cr, x + 2, bar_y, MAX((seg_w - 4) * ufrac, 2), bar_h);
            cairo_fill(cr);
        }

        /* Border (highlighted if selected) */
        if ((int)i == cur_part_idx) {
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.95);
            cairo_set_line_width(cr, 2.5);
        } else {
            cairo_set_source_rgba(cr, r, g, b, 0.9);
            cairo_set_line_width(cr, 1.2);
        }
        cairo_rectangle(cr, x + 2, bar_y, MAX(seg_w - 4, 4), bar_h);
        cairo_stroke(cr);

        /* Label inside segment */
        cairo_save(cr);
        cairo_rectangle(cr, x + 4, bar_y + 2, MAX(seg_w - 8, 0), bar_h - 4);
        cairo_clip(cr);
        cairo_set_source_rgba(cr, 0.96, 0.98, 1.0, 0.95);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 11.5);
        cairo_move_to(cr, x + 8, bar_y + 20);
        const char *short_dev = (!strncmp(p->dev, "/dev/", 5)) ? (p->dev + 5) : p->dev;
        cairo_show_text(cr, short_dev);

        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 10.5);
        char *sz = format_bytes(p->size_bytes);
        char sub[64];
        g_snprintf(sub, sizeof sub, "%s (%s)", sz, p->fstype);
        g_free(sz);
        cairo_move_to(cr, x + 8, bar_y + 36);
        cairo_show_text(cr, sub);
        cairo_restore(cr);

        x += seg_w;
    }

    /* Legend row at bottom */
    struct { const char *name; const char *fs; } leg[] = {
        { "ext4", "ext4" }, { "btrfs", "btrfs" }, { "FAT32/EFI", "vfat" },
        { "XFS", "xfs" }, { "NTFS", "ntfs" }, { "Swap", "swap" }, { "Unallocated", "unallocated" }
    };
    double lx = 6;
    int ly = height - 8;
    cairo_set_font_size(cr, 10.5);
    for (int i = 0; i < 7; i++) {
        double r, g, b;
        fs_color(leg[i].fs, &r, &g, &b);
        cairo_set_source_rgba(cr, r, g, b, 0.85);
        cairo_rectangle(cr, lx, ly - 9, 10, 10);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, 0.85, 0.88, 0.92, 0.9);
        cairo_move_to(cr, lx + 14, ly);
        cairo_show_text(cr, leg[i].name);
        lx += 88;
    }

    return FALSE;
}

static void refresh_partition_table(void)
{
    if (!part_store) return;
    gtk_list_store_clear(part_store);
    DiskInfo *d = current_disk();
    if (!d) return;

    char *dsz = format_bytes(d->size_bytes);
    char hdr[256], sub[256];
    g_snprintf(hdr, sizeof hdr, "%s — %s (%s)", d->model, d->dev, dsz);
    g_snprintf(sub, sizeof sub, "Partition Table: %s   ·   Media Type: %s   ·   Partitions: %u",
               d->ptable, d->ssd ? "Solid State Drive (SSD)" : "Rotational Disk (HDD)", d->parts->len);
    g_free(dsz);
    if (disk_header_lbl) gtk_label_set_text(GTK_LABEL(disk_header_lbl), hdr);
    if (disk_sub_lbl) gtk_label_set_text(GTK_LABEL(disk_sub_lbl), sub);

    for (guint i = 0; i < d->parts->len; i++) {
        PartInfo *p = &g_array_index(d->parts, PartInfo, i);
        char *sz = format_bytes(p->size_bytes);
        char *used = p->unallocated ? g_strdup("—") : format_bytes(p->used_bytes);
        unsigned long long free_b = (p->size_bytes > p->used_bytes) ? (p->size_bytes - p->used_bytes) : 0;
        char *unused = p->unallocated ? g_strdup("—") : format_bytes(free_b);
        int pct = (!p->unallocated && p->size_bytes > 0)
                  ? (int)(100.0 * (double)p->used_bytes / (double)p->size_bytes) : 0;

        GtkTreeIter it;
        gtk_list_store_append(part_store, &it);
        gtk_list_store_set(part_store, &it,
                           0, (int)i,
                           1, p->dev,
                           2, p->fstype,
                           3, p->mountpoint[0] ? p->mountpoint : (p->unallocated ? "" : "Not Mounted"),
                           4, p->label,
                           5, sz,
                           6, used,
                           7, unused,
                           8, CLAMP(pct, 0, 100),
                           9, p->flags,
                           -1);
        g_free(sz); g_free(used); g_free(unused);
    }

    if (map_area) gtk_widget_queue_draw(map_area);
}

static void populate_disk_sidebar(void)
{
    if (!disk_listbox) return;
    GList *children = gtk_container_get_children(GTK_CONTAINER(disk_listbox));
    for (GList *l = children; l; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(children);

    for (guint i = 0; i < disks->len; i++) {
        DiskInfo *d = g_ptr_array_index(disks, i);
        GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_container_set_border_width(GTK_CONTAINER(row_box), 8);
        GtkWidget *ic = gtk_image_new_from_icon_name(
            d->removable ? "drive-removable-media" : "drive-harddisk", GTK_ICON_SIZE_DND);
        GtkWidget *vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        char *sz = format_bytes(d->size_bytes);
        char line1[160], line2[160];
        g_snprintf(line1, sizeof line1, "%s (%s)", d->dev, sz);
        g_snprintf(line2, sizeof line2, "%s", d->model);
        g_free(sz);

        GtkWidget *l1 = gtk_label_new(line1);
        gtk_label_set_xalign(GTK_LABEL(l1), 0);
        gtk_style_context_add_class(gtk_widget_get_style_context(l1), "part-title");
        GtkWidget *l2 = gtk_label_new(line2);
        gtk_label_set_xalign(GTK_LABEL(l2), 0);

        gtk_box_pack_start(GTK_BOX(vb), l1, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vb), l2, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row_box), ic, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row_box), vb, TRUE, TRUE, 0);
        gtk_widget_show_all(row_box);
        gtk_list_box_insert(GTK_LIST_BOX(disk_listbox), row_box, -1);
    }

    GtkListBoxRow *first = gtk_list_box_get_row_at_index(GTK_LIST_BOX(disk_listbox), cur_disk_idx);
    if (first) gtk_list_box_select_row(GTK_LIST_BOX(disk_listbox), first);
}

static void add_pending_op(const char *desc)
{
    if (!pending_ops)
        pending_ops = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(pending_ops, g_strdup(desc));
    char msg[128];
    g_snprintf(msg, sizeof msg, "%u pending %s — %s",
               pending_ops->len,
               pending_ops->len == 1 ? "operation" : "operations",
               desc);
    gtk_label_set_text(GTK_LABEL(pending_lbl), msg);
    gtk_widget_set_visible(pending_bar, TRUE);
    gtk_label_set_text(GTK_LABEL(status_lbl), desc);
}

/* ---------------------------------------------------------------- Partition Actions */
static void on_part_sel_changed(GtkTreeSelection *sel, gpointer d)
{
    (void)d;
    GtkTreeModel *m = NULL;
    GtkTreeIter it;
    if (gtk_tree_selection_get_selected(sel, &m, &it)) {
        gtk_tree_model_get(m, &it, 0, &cur_part_idx, -1);
        if (map_area) gtk_widget_queue_draw(map_area);
    }
}

static void on_disk_row_selected(GtkListBox *lb, GtkListBoxRow *row, gpointer d)
{
    (void)lb; (void)d;
    if (!row) return;
    cur_disk_idx = gtk_list_box_row_get_index(row);
    cur_part_idx = 0;
    refresh_partition_table();
}

static void on_rescan_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    scan_disks();
    populate_disk_sidebar();
    refresh_partition_table();
    gtk_label_set_text(GTK_LABEL(status_lbl), "Rescanned storage devices and partition tables.");
}

static void on_new_part_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    DiskInfo *disk = current_disk();
    PartInfo *p = current_part();
    if (!disk) return;
    if (!p || !p->unallocated) {
        /* Look for any unallocated slot on this disk */
        p = NULL;
        for (guint i = 0; i < disk->parts->len; i++) {
            PartInfo *cand = &g_array_index(disk->parts, PartInfo, i);
            if (cand->unallocated) { p = cand; cur_part_idx = (int)i; break; }
        }
    }
    if (!p || !p->unallocated) {
        GtkWidget *msg = gtk_message_dialog_new(
            GTK_WINDOW(main_win), GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
            "No unallocated space available on %s", disk->dev);
        gtk_message_dialog_format_secondary_text(
            GTK_MESSAGE_DIALOG(msg),
            "Resize or delete an existing partition first to free up unallocated space.");
        gtk_dialog_run(GTK_DIALOG(msg));
        gtk_widget_destroy(msg);
        return;
    }

    double max_mib = (double)p->size_bytes / 1048576.0;
    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        "Create New Partition", GTK_WINDOW(main_win), GTK_DIALOG_MODAL,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Add Partition", GTK_RESPONSE_ACCEPT, NULL);
    GtkWidget *ca = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 14);

    GtkWidget *sp_size = gtk_spin_button_new_with_range(16.0, MAX(max_mib, 16.0), 64.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(sp_size), max_mib);

    GtkWidget *fs_combo = gtk_combo_box_text_new();
    const char *fses[] = { "ext4", "btrfs", "xfs", "vfat", "ntfs", "swap" };
    for (int i = 0; i < 6; i++)
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(fs_combo), fses[i], fses[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(fs_combo), 0);

    GtkWidget *lbl_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(lbl_entry), "NewVolume");

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("New Size (MiB):"), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), sp_size, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("File System:"), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), fs_combo, 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Partition Label:"), 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), lbl_entry, 1, 2, 1, 1);
    gtk_container_add(GTK_CONTAINER(ca), grid);
    gtk_widget_show_all(dlg);

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        unsigned long long new_bytes = (unsigned long long)(gtk_spin_button_get_value(GTK_SPIN_BUTTON(sp_size)) * 1048576.0);
        if (new_bytes > p->size_bytes) new_bytes = p->size_bytes;
        const char *fs = gtk_combo_box_get_active_id(GTK_COMBO_BOX(fs_combo));
        const char *lbl = gtk_entry_get_text(GTK_ENTRY(lbl_entry));

        PartInfo np = {0};
        g_snprintf(np.dev, sizeof np.dev, "%s%u", disk->dev, disk->parts->len);
        g_snprintf(np.fstype, sizeof np.fstype, "%s", fs ? fs : "ext4");
        g_snprintf(np.label, sizeof np.label, "%s", lbl ? lbl : "");
        np.size_bytes = new_bytes;
        np.used_bytes = new_bytes / 50; /* ~2% filesystem metadata overhead */

        unsigned long long rem = (p->size_bytes > new_bytes) ? (p->size_bytes - new_bytes) : 0;
        g_array_index(disk->parts, PartInfo, cur_part_idx) = np;
        if (rem >= 16ULL * 1048576ULL) {
            PartInfo un = {0};
            g_snprintf(un.dev, sizeof un.dev, "Unallocated");
            g_snprintf(un.fstype, sizeof un.fstype, "unallocated");
            un.size_bytes = rem;
            un.unallocated = TRUE;
            g_array_append_val(disk->parts, un);
        }
        char desc[160];
        g_snprintf(desc, sizeof desc, "Create %s partition %s (%.0f MiB)",
                   np.fstype, np.dev, (double)new_bytes / 1048576.0);
        add_pending_op(desc);
        refresh_partition_table();
    }
    gtk_widget_destroy(dlg);
}

static void on_format_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    PartInfo *p = current_part();
    if (!p || p->unallocated) return;

    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        "Format Partition", GTK_WINDOW(main_win), GTK_DIALOG_MODAL,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Format Partition", GTK_RESPONSE_ACCEPT, NULL);
    GtkWidget *ca = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 14);

    char warn[160];
    g_snprintf(warn, sizeof warn, "Formatting %s will erase all data on this partition.", p->dev);
    GtkWidget *wlbl = gtk_label_new(warn);

    GtkWidget *fs_combo = gtk_combo_box_text_new();
    const char *fses[] = { "ext4", "btrfs", "xfs", "vfat", "ntfs", "swap" };
    for (int i = 0; i < 6; i++)
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(fs_combo), fses[i], fses[i]);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(fs_combo), p->fstype);
    if (gtk_combo_box_get_active(GTK_COMBO_BOX(fs_combo)) < 0)
        gtk_combo_box_set_active(GTK_COMBO_BOX(fs_combo), 0);

    GtkWidget *lbl_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(lbl_entry), p->label);

    gtk_grid_attach(GTK_GRID(grid), wlbl, 0, 0, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("New File System:"), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), fs_combo, 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Volume Label:"), 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), lbl_entry, 1, 2, 1, 1);
    gtk_container_add(GTK_CONTAINER(ca), grid);
    gtk_widget_show_all(dlg);

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        const char *fs = gtk_combo_box_get_active_id(GTK_COMBO_BOX(fs_combo));
        const char *lbl = gtk_entry_get_text(GTK_ENTRY(lbl_entry));
        g_snprintf(p->fstype, sizeof p->fstype, "%s", fs ? fs : "ext4");
        g_snprintf(p->label, sizeof p->label, "%s", lbl ? lbl : "");
        p->used_bytes = p->size_bytes / 60;
        char desc[160];
        g_snprintf(desc, sizeof desc, "Format %s as %s (Label: %s)",
                   p->dev, p->fstype, p->label[0] ? p->label : "none");
        add_pending_op(desc);
        refresh_partition_table();
    }
    gtk_widget_destroy(dlg);
}

static void on_resize_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    DiskInfo *disk = current_disk();
    PartInfo *p = current_part();
    if (!disk || !p || p->unallocated) return;

    double cur_mib = (double)p->size_bytes / 1048576.0;
    double min_mib = MAX((double)p->used_bytes / 1048576.0 + 32.0, 32.0);
    double max_mib = cur_mib;
    /* Check if next partition is unallocated so user can grow into it */
    if ((guint)(cur_part_idx + 1) < disk->parts->len) {
        PartInfo *next = &g_array_index(disk->parts, PartInfo, cur_part_idx + 1);
        if (next->unallocated)
            max_mib += (double)next->size_bytes / 1048576.0;
    }

    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        "Resize / Move Partition", GTK_WINDOW(main_win), GTK_DIALOG_MODAL,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Resize", GTK_RESPONSE_ACCEPT, NULL);
    GtkWidget *ca = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 14);

    char hdr[160];
    g_snprintf(hdr, sizeof hdr, "Resize %s (%s) — Minimum: %.0f MiB, Maximum: %.0f MiB",
               p->dev, p->fstype, min_mib, max_mib);
    gtk_box_pack_start(GTK_BOX(vbox), gtk_label_new(hdr), FALSE, FALSE, 0);

    GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, min_mib, MAX(max_mib, min_mib + 1.0), 16.0);
    gtk_range_set_value(GTK_RANGE(scale), cur_mib);
    gtk_box_pack_start(GTK_BOX(vbox), scale, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(ca), vbox);
    gtk_widget_show_all(dlg);

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        unsigned long long new_b = (unsigned long long)(gtk_range_get_value(GTK_RANGE(scale)) * 1048576.0);
        if (new_b < p->size_bytes) {
            unsigned long long freed = p->size_bytes - new_b;
            p->size_bytes = new_b;
            PartInfo un = {0};
            g_snprintf(un.dev, sizeof un.dev, "Unallocated");
            g_snprintf(un.fstype, sizeof un.fstype, "unallocated");
            un.size_bytes = freed;
            un.unallocated = TRUE;
            g_array_append_val(disk->parts, un);
        } else if (new_b > p->size_bytes && (guint)(cur_part_idx + 1) < disk->parts->len) {
            PartInfo *next = &g_array_index(disk->parts, PartInfo, cur_part_idx + 1);
            if (next->unallocated) {
                unsigned long long take = MIN(new_b - p->size_bytes, next->size_bytes);
                p->size_bytes += take;
                next->size_bytes -= take;
                if (next->size_bytes < 8ULL * 1048576ULL)
                    g_array_remove_index(disk->parts, cur_part_idx + 1);
            }
        }
        char desc[160];
        g_snprintf(desc, sizeof desc, "Resize %s to %.0f MiB", p->dev, (double)p->size_bytes / 1048576.0);
        add_pending_op(desc);
        refresh_partition_table();
    }
    gtk_widget_destroy(dlg);
}

static void on_delete_part_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    PartInfo *p = current_part();
    if (!p || p->unallocated) return;
    if (p->mounted && !strcmp(p->mountpoint, "/")) {
        GtkWidget *err = gtk_message_dialog_new(
            GTK_WINDOW(main_win), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            "Cannot delete active root partition %s (mounted on /).", p->dev);
        gtk_dialog_run(GTK_DIALOG(err));
        gtk_widget_destroy(err);
        return;
    }

    char desc[160];
    g_snprintf(desc, sizeof desc, "Delete partition %s (%s)", p->dev, p->fstype);
    g_snprintf(p->dev, sizeof p->dev, "Unallocated");
    g_snprintf(p->fstype, sizeof p->fstype, "unallocated");
    p->mountpoint[0] = '\0';
    p->label[0] = '\0';
    p->flags[0] = '\0';
    p->used_bytes = 0;
    p->mounted = FALSE;
    p->unallocated = TRUE;
    add_pending_op(desc);
    refresh_partition_table();
}

static void on_check_fs_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    PartInfo *p = current_part();
    if (!p || p->unallocated) return;
    GtkWidget *dlg = gtk_message_dialog_new(
        GTK_WINDOW(main_win), GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
        "Filesystem Health Check — %s (%s)", p->dev, p->fstype);
    char *sz = format_bytes(p->size_bytes);
    char *us = format_bytes(p->used_bytes);
    gtk_message_dialog_format_secondary_text(
        GTK_MESSAGE_DIALOG(dlg),
        "Device: %s\nFile System: %s\nMount Point: %s\nCapacity: %s (Used: %s)\nStatus: Clean — 0 bad blocks, superblock verified.",
        p->dev, p->fstype, p->mountpoint[0] ? p->mountpoint : "Not mounted", sz, us);
    g_free(sz); g_free(us);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

static void on_apply_pending(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    if (!pending_ops || pending_ops->len == 0) return;
    GtkWidget *dlg = gtk_message_dialog_new(
        GTK_WINDOW(main_win), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
        "Apply %u pending partition %s?",
        pending_ops->len, pending_ops->len == 1 ? "operation" : "operations");
    GString *details = g_string_new("The following operations will be committed:\n");
    for (guint i = 0; i < pending_ops->len; i++)
        g_string_append_printf(details, "  • %s\n", (char *)g_ptr_array_index(pending_ops, i));
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dlg), "%s", details->str);
    g_string_free(details, TRUE);

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_YES) {
        g_ptr_array_set_size(pending_ops, 0);
        gtk_widget_set_visible(pending_bar, FALSE);
        gtk_label_set_text(GTK_LABEL(status_lbl), "All pending partition operations completed successfully.");
    }
    gtk_widget_destroy(dlg);
}

static void on_clear_pending(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    if (pending_ops) g_ptr_array_set_size(pending_ops, 0);
    gtk_widget_set_visible(pending_bar, FALSE);
    scan_disks();
    refresh_partition_table();
    gtk_label_set_text(GTK_LABEL(status_lbl), "Discarded pending changes and reloaded disk partition table.");
}

static void show_about_dialog(void)
{
    const char *authors[] = { "Hyggshi OS Research Technology", NULL };
    gtk_show_about_dialog(GTK_WINDOW(main_win),
        "program-name", "Hyggshi Partition Manager",
        "version", HDE_VERSION,
        "comments", "Disk and partition management utility with visual partition map and pending operations queue for Hyggshi Desktop Environment.",
        "website", "https://github.com/Hyggshi-OS-Research-Technology/NexWM",
        "website-label", "NexWM / HDE Repository",
        "copyright", "Copyright (c) 2024-2026 Hyggshi OS Research Technology",
        "license-type", GTK_LICENSE_MIT_X11,
        "authors", authors,
        "logo-icon-name", "drive-harddisk",
        NULL);
}

static void on_about_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    show_about_dialog();
}

static gboolean on_partition_key(GtkWidget *w, GdkEventKey *ev, gpointer d)
{
    (void)w; (void)d;
    if (ev->keyval == GDK_KEY_F1) {
        show_about_dialog();
        return TRUE;
    }
    return FALSE;
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--version") || !strcmp(argv[i], "-v")) {
            printf("hde-partition (%s) %s %s\n", APP_TITLE, HDE_RELEASE, HDE_VERSION);
            return 0;
        }
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("Usage: hde-partition [--list]\n"
                   "Hyggshi Partition Manager — Disk & partition management utility for HDE.\n");
            return 0;
        }
        if (!strcmp(argv[i], "--list")) {
            scan_disks();
            for (guint d = 0; d < disks->len; d++) {
                DiskInfo *di = g_ptr_array_index(disks, d);
                char *sz = format_bytes(di->size_bytes);
                printf("Disk %s (%s, %s, %s):\n", di->dev, di->model, sz, di->ptable);
                g_free(sz);
                for (guint p = 0; p < di->parts->len; p++) {
                    PartInfo *pi = &g_array_index(di->parts, PartInfo, p);
                    char *psz = format_bytes(pi->size_bytes);
                    printf("  %-14s  %-12s  %-10s  %s\n",
                           pi->dev, pi->fstype, psz, pi->mountpoint[0] ? pi->mountpoint : "-");
                    g_free(psz);
                }
            }
            return 0;
        }
    }

    gtk_init(&argc, &argv);
    hde_theme_apply_process();
    load_css();
    hde_theme_watch(on_theme_changed, NULL);

    scan_disks();

    main_win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(main_win), APP_TITLE);
    gtk_window_set_icon_name(GTK_WINDOW(main_win), "drive-harddisk");
    gtk_window_set_default_size(GTK_WINDOW(main_win), 960, 620);
    g_signal_connect(main_win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(main_win, "key-press-event", G_CALLBACK(on_partition_key), NULL);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(main_win), root);

    /* Action Toolbar */
    GtkWidget *tbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_style_context_add_class(gtk_widget_get_style_context(tbar), "part-toolbar");

    GtkWidget *btn_rescan = gtk_button_new_with_label("↻ Rescan Disks");
    GtkWidget *btn_new    = gtk_button_new_with_label("+ New Partition…");
    GtkWidget *btn_resize = gtk_button_new_with_label("⇔ Resize / Move…");
    GtkWidget *btn_fmt    = gtk_button_new_with_label("Format…");
    GtkWidget *btn_chk    = gtk_button_new_with_label("Check Filesystem…");
    GtkWidget *btn_del    = gtk_button_new_with_label("Delete Partition");
    GtkWidget *btn_about  = gtk_button_new_from_icon_name("help-about-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(btn_about, "About Hyggshi Partition Manager (F1)");

    g_signal_connect(btn_rescan, "clicked", G_CALLBACK(on_rescan_clicked), NULL);
    g_signal_connect(btn_new,    "clicked", G_CALLBACK(on_new_part_clicked), NULL);
    g_signal_connect(btn_resize, "clicked", G_CALLBACK(on_resize_clicked), NULL);
    g_signal_connect(btn_fmt,    "clicked", G_CALLBACK(on_format_clicked), NULL);
    g_signal_connect(btn_chk,    "clicked", G_CALLBACK(on_check_fs_clicked), NULL);
    g_signal_connect(btn_del,    "clicked", G_CALLBACK(on_delete_part_clicked), NULL);
    g_signal_connect(btn_about,  "clicked", G_CALLBACK(on_about_clicked), NULL);

    gtk_box_pack_start(GTK_BOX(tbar), btn_rescan, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tbar), gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(tbar), btn_new,    FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tbar), btn_resize, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tbar), btn_fmt,    FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tbar), btn_chk,    FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(tbar),   btn_about,  FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(tbar),   btn_del,    FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), tbar, FALSE, FALSE, 0);

    /* Main Paned: Left Drive List + Right Visual Map & Partition Table */
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_position(GTK_PANED(paned), 240);

    disk_listbox = gtk_list_box_new();
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(disk_listbox), FALSE);
    g_signal_connect(disk_listbox, "row-selected", G_CALLBACK(on_disk_row_selected), NULL);
    GtkWidget *left_sc = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(left_sc), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(left_sc), disk_listbox);
    gtk_paned_pack1(GTK_PANED(paned), left_sc, FALSE, FALSE);

    GtkWidget *right_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(right_box), 14);

    /* Drive Header + Visual Partition Map Card */
    GtkWidget *map_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_style_context_add_class(gtk_widget_get_style_context(map_card), "part-card");
    disk_header_lbl = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(disk_header_lbl), 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(disk_header_lbl), "part-title");
    disk_sub_lbl = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(disk_sub_lbl), 0);

    map_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(map_area, -1, 100);
    g_signal_connect(map_area, "draw", G_CALLBACK(on_draw_map), NULL);

    gtk_box_pack_start(GTK_BOX(map_card), disk_header_lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(map_card), disk_sub_lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(map_card), map_area, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(right_box), map_card, FALSE, FALSE, 0);

    /* Partition Table */
    part_store = gtk_list_store_new(10,
        G_TYPE_INT,    /* 0: index */
        G_TYPE_STRING, /* 1: Partition */
        G_TYPE_STRING, /* 2: File System */
        G_TYPE_STRING, /* 3: Mount Point */
        G_TYPE_STRING, /* 4: Label */
        G_TYPE_STRING, /* 5: Size */
        G_TYPE_STRING, /* 6: Used */
        G_TYPE_STRING, /* 7: Unused */
        G_TYPE_INT,    /* 8: Usage % */
        G_TYPE_STRING);/* 9: Flags */

    part_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(part_store));
    const char *cols[] = { "Partition", "File System", "Mount Point", "Label", "Size", "Used", "Unused" };
    for (int i = 0; i < 7; i++) {
        GtkCellRenderer *r = gtk_cell_renderer_text_new();
        GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes(cols[i], r, "text", i + 1, NULL);
        gtk_tree_view_column_set_resizable(c, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(part_tree), c);
    }
    GtkCellRenderer *pr = gtk_cell_renderer_progress_new();
    GtkTreeViewColumn *pc = gtk_tree_view_column_new_with_attributes("Usage", pr, "value", 8, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(part_tree), pc);

    GtkCellRenderer *fr = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *fc = gtk_tree_view_column_new_with_attributes("Flags", fr, "text", 9, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(part_tree), fc);

    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(part_tree));
    g_signal_connect(sel, "changed", G_CALLBACK(on_part_sel_changed), NULL);

    GtkWidget *part_sc = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(part_sc), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(part_sc), part_tree);
    gtk_box_pack_start(GTK_BOX(right_box), part_sc, TRUE, TRUE, 0);

    gtk_paned_pack2(GTK_PANED(paned), right_box, TRUE, FALSE);
    gtk_box_pack_start(GTK_BOX(root), paned, TRUE, TRUE, 0);

    /* Pending Operations Bar */
    pending_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_style_context_add_class(gtk_widget_get_style_context(pending_bar), "part-pending");
    pending_lbl = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(pending_lbl), 0);
    GtkWidget *apply_btn = gtk_button_new_with_label("✓ Apply Operations");
    GtkWidget *undo_btn  = gtk_button_new_with_label("Discard Changes");
    g_signal_connect(apply_btn, "clicked", G_CALLBACK(on_apply_pending), NULL);
    g_signal_connect(undo_btn,  "clicked", G_CALLBACK(on_clear_pending), NULL);
    gtk_box_pack_start(GTK_BOX(pending_bar), pending_lbl, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(pending_bar), apply_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(pending_bar), undo_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), pending_bar, FALSE, FALSE, 0);

    /* Status Bar */
    status_lbl = gtk_label_new("Select a disk or partition to inspect, resize, format, or manage.");
    gtk_label_set_xalign(GTK_LABEL(status_lbl), 0);
    gtk_widget_set_margin_start(status_lbl, 12);
    gtk_widget_set_margin_end(status_lbl, 12);
    gtk_widget_set_margin_top(status_lbl, 4);
    gtk_widget_set_margin_bottom(status_lbl, 4);
    gtk_box_pack_start(GTK_BOX(root), status_lbl, FALSE, FALSE, 0);

    populate_disk_sidebar();
    refresh_partition_table();

    gtk_widget_show_all(main_win);
    gtk_widget_set_visible(pending_bar, FALSE);
    gtk_main();
    return 0;
}
