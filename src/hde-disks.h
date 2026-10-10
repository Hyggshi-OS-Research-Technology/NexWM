/* hde-disks.h — what HDE knows about a disk, and what it decides about it: is this something to show in the Files
 * sidebar, and is this something to mount the moment it is plugged in? Plain C (src/hde-disks.c) so that
 * tests/disks-test.c can check every rule without udisks2, without a D-Bus daemon and without a screen.
 *
 * The D-Bus half lives in src/hde-udisks.c (it fills these in from org.freedesktop.UDisks2); the decisions live
 * here, and they are the ones a user notices: an encrypted container is never mounted behind their back, the disk
 * the operating system runs from is not offered as a USB stick, and a file mounted as a loop device is not shown
 * as a drive at all.
 */
#ifndef HDE_DISKS_H
#define HDE_DISKS_H

#include <stddef.h>
#include <stdint.h>

#define HDE_DISK_PATH_MAX   160      /* the udisks2 D-Bus object path: /org/freedesktop/UDisks2/block_devices/sdb1 */
#define HDE_DISK_DEVICE_MAX  64      /* /dev/sdb1 */
#define HDE_DISK_NAME_MAX   128      /* a label or a drive model */
#define HDE_DISK_UUID_MAX    64
#define HDE_DISK_FSTYPE_MAX  32      /* vfat, exfat, ntfs, ext4, iso9660 ... */
#define HDE_DISK_MOUNT_MAX  512      /* /run/media/user/USB DRIVE */

typedef struct {
    char     object_path[HDE_DISK_PATH_MAX];
    /* The drive this belongs to (an SD card is not the reader it is in); "" when udisks2 does not say. */
    char     drive_path[HDE_DISK_PATH_MAX];
    char     device[HDE_DISK_DEVICE_MAX];       /* /dev/sdb1 */
    char     label[HDE_DISK_NAME_MAX];          /* the name written on the filesystem ("USB DRIVE") */
    char     drive_model[HDE_DISK_NAME_MAX];    /* what the drive calls itself ("SanDisk Cruzer Blade") */
    char     uuid[HDE_DISK_UUID_MAX];
    char     fstype[HDE_DISK_FSTYPE_MAX];
    char     mount_point[HDE_DISK_MOUNT_MAX];   /* empty when it is not mounted */
    uint64_t size;                              /* bytes, 0 when unknown (an empty card reader) */
    int      removable;             /* a USB stick, an SD card, an external disk */
    int      optical;               /* a CD, a DVD, a Blu-ray */
    int      ejectable;             /* the drive can be ejected (udisks2 Drive:Ejectable) */
    int      has_media;             /* there is a disc in the drive (optical only) */
    int      has_filesystem;        /* udisks2 offers a Filesystem interface for it */
    int      mounted;
    int      encrypted;             /* a locked container (crypto_LUKS) that needs a passphrase to open */
    int      loop;                  /* /dev/loop*: a file mounted as a disk (a snap, an AppImage) */
    int      hint_auto;             /* udisks2: "the user probably wants this mounted" */
    int      hint_ignore;           /* udisks2: "do not show this to the user" */
    int      hint_system;           /* udisks2: part of the machine the operating system runs from */
} HdeDisk;

void hde_disk_init(HdeDisk *d);
/* Two entries for the same disk: what the user sees has not changed. Used to keep from announcing a disk twice. */
int  hde_disk_equal(const HdeDisk *a, const HdeDisk *b);

/* ---- the decisions ---- */
/* Show it in the sidebar of Hyggshi Files and in the Devices list? */
int  hde_disk_should_show(const HdeDisk *d);
/* Mount it the moment it appears (hde-automount)? Only something the user plugged in, never the machine's own disk,
 * never a locked container - those are mounted from Files, where the passphrase can be asked for. */
int  hde_disk_should_mount(const HdeDisk *d);
/* Offer "Safely remove"? Only for something that can be pulled out. */
int  hde_disk_can_eject(const HdeDisk *d);

/* ---- what the user reads ---- */
/* The name of the disk: its label, or the drive's model, or "31 GB Volume". */
void hde_disk_title(const HdeDisk *d, char *out, size_t len);
/* Under it: "31 GB removable drive, mounted", "Encrypted", "No disc". */
void hde_disk_subtitle(const HdeDisk *d, char *out, size_t len);
/* The icon for the sidebar: drive-removable-media, media-optical, drive-harddisk. */
const char *hde_disk_icon(const HdeDisk *d);
/* "1 byte", "4.4 GB", "931 GB" (decimal, the way disks are sold). */
void hde_size_text(uint64_t bytes, char *out, size_t len);

#endif /* HDE_DISKS_H */
