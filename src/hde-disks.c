/* hde-disks.c — the decisions about a disk (see src/hde-disks.h) and the lines the user reads about it. Plain C:
 * tests/disks-test.c runs them with nothing but a struct, no udisks2 and no screen.
 *
 * udisks2 hands out a great many facts about every block device on the machine — including the partitions of the
 * disk the operating system runs from, the loop devices behind a snap or an AppImage, and the empty card reader
 * that reports a size of zero. Which of those a person thinks of as "a drive" is a decision, not a fact, and that
 * decision is made here so that it can be tested on its own.
 */
#include "hde-disks.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

void hde_disk_init(HdeDisk *d)
{
    if (!d) return;
    memset(d, 0, sizeof *d);
}

int hde_disk_equal(const HdeDisk *a, const HdeDisk *b)
{
    if (!a || !b) return 0;
    return a->size == b->size &&
           a->removable == b->removable && a->optical == b->optical && a->ejectable == b->ejectable &&
           a->has_media == b->has_media && a->has_filesystem == b->has_filesystem &&
           a->mounted == b->mounted && a->encrypted == b->encrypted && a->loop == b->loop &&
           a->hint_auto == b->hint_auto && a->hint_ignore == b->hint_ignore && a->hint_system == b->hint_system &&
           strcmp(a->object_path, b->object_path) == 0 &&
           strcmp(a->drive_path, b->drive_path) == 0 &&
           strcmp(a->device, b->device) == 0 &&
           strcmp(a->label, b->label) == 0 &&
           strcmp(a->drive_model, b->drive_model) == 0 &&
           strcmp(a->fstype, b->fstype) == 0 &&
           strcmp(a->mount_point, b->mount_point) == 0;
}

int hde_disk_should_show(const HdeDisk *d)
{
    if (!d) return 0;
    if (d->hint_ignore) return 0;               /* udisks2 says: this one is none of the user's business */
    if (d->loop) return 0;                      /* a file mounted as a disk, not something that was plugged in */
    if (!d->has_filesystem) return 0;           /* swap, a partition table, an empty card reader */
    if (d->optical && !d->has_media) return 0;  /* a drive with no disc in it */
    if (d->size == 0 && !d->optical) return 0;  /* nothing there to show */
    /* The disk the operating system runs from is part of the computer, not something the user plugged in:
     * udisks2 marks it HintSystem, and it is left to "Computer" in the sidebar. */
    if (d->hint_system && !d->removable && !d->optical) return 0;
    return 1;
}

int hde_disk_should_mount(const HdeDisk *d)
{
    if (!d || !hde_disk_should_show(d)) return 0;
    if (d->mounted) return 0;
    if (d->encrypted) return 0;                 /* needs a passphrase: Files asks for it, we do not guess */
    if (d->hint_auto == 0) return 0;            /* udisks2 says the user would rather it were left alone */
    /* Only what was plugged in. A second internal disk is mounted from Files (or by /etc/fstab), not the moment
     * the session starts. */
    if (!d->removable && !d->optical) return 0;
    return 1;
}

int hde_disk_can_eject(const HdeDisk *d)
{
    if (!d || !hde_disk_should_show(d)) return 0;
    return d->optical || d->removable || d->ejectable;
}

/* "removable drive", "optical disc", "drive" */
static const char *disk_kind(const HdeDisk *d)
{
    if (d->optical) return "optical disc";
    if (d->removable) return "removable drive";
    return "drive";
}

void hde_disk_title(const HdeDisk *d, char *out, size_t len)
{
    if (!out || !len) return;
    if (!d) { snprintf(out, len, "Drive"); return; }
    if (d->label[0]) { snprintf(out, len, "%s", d->label); return; }
    if (d->drive_model[0]) { snprintf(out, len, "%s", d->drive_model); return; }
    if (d->optical) { snprintf(out, len, "Disc"); return; }
    char size[32];
    hde_size_text(d->size, size, sizeof size);
    if (size[0]) snprintf(out, len, "%s Volume", size);
    else snprintf(out, len, "Drive");
}

void hde_disk_subtitle(const HdeDisk *d, char *out, size_t len)
{
    if (!out || !len) return;
    if (!d) { snprintf(out, len, "Drive"); return; }
    if (d->encrypted) { snprintf(out, len, "Encrypted - a password is needed"); return; }
    if (d->optical && !d->has_media) { snprintf(out, len, "No disc"); return; }

    char size[32];
    hde_size_text(d->size, size, sizeof size);
    const char *kind = disk_kind(d);
    if (size[0]) snprintf(out, len, d->mounted ? "%s %s, mounted" : "%s %s", size, kind);
    else snprintf(out, len, d->mounted ? "%s, mounted" : "%s", kind);
    if (out[0]) out[0] = (char)toupper((unsigned char)out[0]);
}

const char *hde_disk_icon(const HdeDisk *d)
{
    if (!d) return "drive-harddisk";
    if (d->optical) return "media-optical";
    if (d->removable) return "drive-removable-media";
    return "drive-harddisk";
}

void hde_size_text(uint64_t bytes, char *out, size_t len)
{
    if (!out || !len) return;
    if (bytes == 0) { if (len) out[0] = '\0'; return; }
    static const char *const units[] = { "bytes", "KB", "MB", "GB", "TB", "PB" };
    double v = (double)bytes;
    int u = 0;
    while (v >= 1000.0 && u < (int)(sizeof units / sizeof units[0]) - 1) { v /= 1000.0; ++u; }
    if (u == 0) snprintf(out, len, "%llu %s", (unsigned long long)bytes, bytes == 1 ? "byte" : "bytes");
    else if (v < 10.0) snprintf(out, len, "%.1f %s", v, units[u]);
    else snprintf(out, len, "%.0f %s", v, units[u]);
}
