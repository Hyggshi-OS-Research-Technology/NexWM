/* hde-udisks.h — the removable disks of the machine, read from udisks2 (org.freedesktop.UDisks2) over D-Bus.
 * Used twice: by hde-automount (src/hde-automount.c), which mounts what is plugged in, and by the sidebar of
 * Hyggshi Files (hde-files/src/disks.c), which shows it.
 *
 * There is no GVolumeMonitor and no GVfs in this: HDE talks to udisks2 itself, so the sidebar shows a USB stick
 * on a machine where GVfs is not installed, and the daemon does not depend on a volume monitor being started.
 *
 * What is done with a disk once it is known — should it be shown, should it be mounted the moment it appears,
 * what is it called — is not decided here but in src/hde-disks.c, which is plain C and tested without udisks2.
 * This file is only the D-Bus: it fills in HdeDisk structs and keeps them up to date.
 */
#ifndef HDE_UDISKS_H
#define HDE_UDISKS_H

#include <gio/gio.h>

#include "hde-disks.h"

typedef struct HdeDisks HdeDisks;

/* Something changed: a disk was plugged in, taken out, mounted or unmounted. */
typedef void (*HdeDisksChanged)(HdeDisks *disks, gpointer data);

/* The answer to a mount / unmount / eject. message is NULL when it worked; mount_point is the path it was
 * mounted on, or NULL for an unmount and an eject. */
typedef void (*HdeDisksDone)(HdeDisks *disks, const char *object_path, gboolean ok, const char *message,
                            const char *mount_point, gpointer data);

HdeDisks  *hde_disks_new(HdeDisksChanged changed, gpointer data);
void       hde_disks_free(HdeDisks *disks);

/* The disks, newest first? no: in the order udisks2 lists them. The HdeDisk structs belong to the monitor and
 * stay valid until the next "changed"; copy one if it has to outlive the call. Empty while udisks2 is not
 * running (hde_disks_online() says whether it has answered at all). */
GPtrArray *hde_disks_list(HdeDisks *disks);
gboolean   hde_disks_online(HdeDisks *disks);
/* One disk by its object path ("" and NULL are the same here: nothing found). */
HdeDisk   *hde_disks_find(HdeDisks *disks, const char *object_path);

void       hde_disks_mount_async(HdeDisks *disks, const char *object_path, HdeDisksDone cb, gpointer data);
void       hde_disks_unmount_async(HdeDisks *disks, const char *object_path, HdeDisksDone cb, gpointer data);
/* "Safely remove": unmount and then spin the drive down, so that the stick can be pulled out. */
void       hde_disks_eject_async(HdeDisks *disks, const char *object_path, HdeDisksDone cb, gpointer data);

#endif /* HDE_UDISKS_H */
