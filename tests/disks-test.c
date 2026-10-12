/* tests/disks-test.c — what HDE decides about a disk (src/hde-disks.c) without udisks2: a USB stick, the disk the
 * operating system runs from, an encrypted container, an empty card reader, a loop device behind a snap. Plain C,
 * so `make check-unit` runs it on any machine.
 *   cc -O2 -Wall -Wextra -Wpedantic -std=c11 -Isrc -o build/disks-test tests/disks-test.c src/hde-disks.c
 * Prints PASS/FAIL lines; exit status = number of failures.
 */
#include "hde-disks.h"

#include <stdio.h>
#include <string.h>

static int fails, passes;

#define CHECK(cond, ...) do { \
        if (cond) { passes++; printf("PASS: disks: "); } else { fails++; printf("FAIL: disks: "); } \
        printf(__VA_ARGS__); printf("\n"); \
    } while (0)

#define KB(n) ((uint64_t)(n) * 1000)
#define MB(n) ((uint64_t)(n) * 1000 * 1000)
#define GB(n) ((uint64_t)(n) * 1000 * 1000 * 1000)

/* A plain USB stick with a FAT filesystem on it: the everyday case. */
static HdeDisk usb_stick(void)
{
    HdeDisk d;
    hde_disk_init(&d);
    snprintf(d.object_path, sizeof d.object_path, "/org/freedesktop/UDisks2/block_devices/sdb1");
    snprintf(d.device, sizeof d.device, "/dev/sdb1");
    snprintf(d.label, sizeof d.label, "USB DRIVE");
    snprintf(d.drive_model, sizeof d.drive_model, "SanDisk Cruzer Blade");
    snprintf(d.fstype, sizeof d.fstype, "vfat");
    d.size = GB(31);
    d.removable = 1;
    d.has_filesystem = 1;
    d.hint_auto = 1;
    return d;
}

static void test_sizes(void)
{
    char t[32];
    hde_size_text(0, t, sizeof t);          CHECK(!strcmp(t, ""), "nothing there: no size is shown");
    hde_size_text(1, t, sizeof t);          CHECK(!strcmp(t, "1 byte"), "1 byte is singular");
    hde_size_text(512, t, sizeof t);        CHECK(!strcmp(t, "512 bytes"), "512 bytes");
    hde_size_text(999, t, sizeof t);        CHECK(!strcmp(t, "999 bytes"), "999 bytes: still bytes");
    hde_size_text(KB(1), t, sizeof t);      CHECK(!strcmp(t, "1.0 KB"), "1000 bytes is 1.0 KB");
    hde_size_text(MB(1), t, sizeof t);      CHECK(!strcmp(t, "1.0 MB"), "a megabyte");
    hde_size_text(GB(4) + MB(400), t, sizeof t);
    CHECK(!strcmp(t, "4.4 GB"), "4.4 GB keeps its decimal");
    hde_size_text(GB(31), t, sizeof t);     CHECK(!strcmp(t, "31 GB"), "31 GB");
    hde_size_text(GB(1000), t, sizeof t);   CHECK(!strcmp(t, "1.0 TB"), "a terabyte");
}

static void test_show(void)
{
    HdeDisk d = usb_stick();
    CHECK(hde_disk_should_show(&d), "a USB stick is shown");

    d.hint_ignore = 1;
    CHECK(!hde_disk_should_show(&d), "udisks2's \"do not show this\" is respected");
    d = usb_stick();

    d.loop = 1;
    CHECK(!hde_disk_should_show(&d), "a loop device (a snap, an AppImage) is not a drive");
    d = usb_stick();

    d.has_filesystem = 0;
    CHECK(!hde_disk_should_show(&d), "a partition with no filesystem on it (swap, a partition table) is not shown");
    d = usb_stick();

    d.size = 0;
    CHECK(!hde_disk_should_show(&d), "an empty card reader, nothing in it, is not shown");
    d = usb_stick();

    /* The disk the operating system runs from. */
    HdeDisk sys = usb_stick();
    sys.removable = 0;
    sys.hint_system = 1;
    CHECK(!hde_disk_should_show(&sys), "the machine's own system disk is part of the computer, not a drive");

    /* A second internal disk: nothing was plugged in, but the user still wants to see it. */
    HdeDisk second = usb_stick();
    second.removable = 0;
    second.hint_system = 0;
    CHECK(hde_disk_should_show(&second), "a second internal disk is shown");

    /* An optical drive with no disc in it. */
    HdeDisk cd = usb_stick();
    cd.removable = 0;
    cd.optical = 1;
    cd.has_media = 0;
    CHECK(!hde_disk_should_show(&cd), "a DVD drive with nothing in it is not shown");
    cd.has_media = 1;
    cd.size = GB(4) + MB(400);
    snprintf(cd.fstype, sizeof cd.fstype, "iso9660");
    CHECK(hde_disk_should_show(&cd), "and with a disc in it, it is");
}

static void test_mount(void)
{
    HdeDisk d = usb_stick();
    CHECK(hde_disk_should_mount(&d), "a USB stick is mounted as soon as it is plugged in");

    d.mounted = 1;
    snprintf(d.mount_point, sizeof d.mount_point, "/run/media/user/USB DRIVE");
    CHECK(!hde_disk_should_mount(&d), "it is not mounted twice");
    d = usb_stick();

    d.hint_auto = 0;
    CHECK(!hde_disk_should_mount(&d), "udisks2's \"leave this alone\" is respected");
    CHECK(hde_disk_should_show(&d), "but it is still shown, to be mounted by hand");
    d = usb_stick();

    /* A locked container: mounting it means guessing a password, and we do not. */
    d.encrypted = 1;
    CHECK(!hde_disk_should_mount(&d), "an encrypted disk is never mounted behind the user's back");
    CHECK(hde_disk_should_show(&d), "it is shown, so that Files can ask for the passphrase");
    d = usb_stick();

    /* A second internal disk: it is shown, but it is not mounted just because a session started. */
    d.removable = 0;
    d.hint_system = 0;
    CHECK(hde_disk_should_show(&d), "a second internal disk is shown");
    CHECK(!hde_disk_should_mount(&d), "but not mounted automatically: nobody plugged it in");

    /* The system disk: never, in either sense. */
    HdeDisk sys = usb_stick();
    sys.removable = 0;
    sys.hint_system = 1;
    CHECK(!hde_disk_should_show(&sys) && !hde_disk_should_mount(&sys), "the system disk is neither shown nor mounted");
}

static void test_eject(void)
{
    HdeDisk d = usb_stick();
    CHECK(hde_disk_can_eject(&d), "a USB stick can be safely removed");

    d.removable = 0;
    d.hint_system = 0;
    CHECK(!hde_disk_can_eject(&d), "an internal disk cannot be pulled out");

    HdeDisk cd = usb_stick();
    cd.removable = 0;
    cd.optical = 1;
    cd.has_media = 1;
    CHECK(hde_disk_can_eject(&cd), "a disc can be ejected");

    HdeDisk gone = usb_stick();
    gone.hint_ignore = 1;
    CHECK(!hde_disk_can_eject(&gone), "a disk that is not shown has no \"safely remove\"");
}

static void test_text(void)
{
    char t[256];
    HdeDisk d = usb_stick();

    hde_disk_title(&d, t, sizeof t);
    CHECK(!strcmp(t, "USB DRIVE"), "the label is the name: \"%s\"", t);
    hde_disk_subtitle(&d, t, sizeof t);
    CHECK(!strcmp(t, "31 GB removable drive"), "under it: \"%s\"", t);
    CHECK(!strcmp(hde_disk_icon(&d), "drive-removable-media"), "and it is drawn as removable media");

    d.mounted = 1;
    snprintf(d.mount_point, sizeof d.mount_point, "/run/media/user/USB DRIVE");
    hde_disk_subtitle(&d, t, sizeof t);
    CHECK(!strcmp(t, "31 GB removable drive, mounted"), "a mounted one says so: \"%s\"", t);
    d.mounted = 0;

    /* No label: the drive's own name. */
    d.label[0] = '\0';
    hde_disk_title(&d, t, sizeof t);
    CHECK(!strcmp(t, "SanDisk Cruzer Blade"), "without a label the drive's model is the name: \"%s\"", t);

    /* Neither: the size. */
    d.drive_model[0] = '\0';
    hde_disk_title(&d, t, sizeof t);
    CHECK(!strcmp(t, "31 GB Volume"), "with neither, the size: \"%s\"", t);

    /* Encrypted. */
    d.encrypted = 1;
    hde_disk_subtitle(&d, t, sizeof t);
    CHECK(strstr(t, "password") != NULL, "an encrypted disk says what it needs: \"%s\"", t);
    d.encrypted = 0;

    /* An internal disk and a disc. */
    d.removable = 0;
    hde_disk_subtitle(&d, t, sizeof t);
    CHECK(!strcmp(t, "31 GB drive"), "an internal disk is just a drive: \"%s\"", t);
    CHECK(!strcmp(hde_disk_icon(&d), "drive-harddisk"), "and drawn as one");

    HdeDisk cd = usb_stick();
    cd.removable = 0;
    cd.optical = 1;
    cd.has_media = 1;
    cd.label[0] = '\0';
    cd.drive_model[0] = '\0';
    hde_disk_title(&cd, t, sizeof t);
    CHECK(!strcmp(t, "Disc"), "a disc with no name is \"Disc\"");
    hde_disk_subtitle(&cd, t, sizeof t);
    CHECK(!strcmp(t, "31 GB optical disc"), "and an optical disc: \"%s\"", t);
    CHECK(!strcmp(hde_disk_icon(&cd), "media-optical"), "drawn as an optical disc");
    cd.has_media = 0;
    hde_disk_subtitle(&cd, t, sizeof t);
    CHECK(!strcmp(t, "No disc"), "an empty drive says so: \"%s\"", t);
}

static void test_equal(void)
{
    HdeDisk a = usb_stick(), b = usb_stick();
    CHECK(hde_disk_equal(&a, &b), "two entries for the same disk are equal");

    b.mounted = 1;
    snprintf(b.mount_point, sizeof b.mount_point, "/run/media/user/USB DRIVE");
    CHECK(!hde_disk_equal(&a, &b), "mounting it changes what the user sees");

    b = usb_stick();
    b.size = GB(64);
    CHECK(!hde_disk_equal(&a, &b), "a different size is a different disk");

    b = usb_stick();
    b.label[0] = '\0';
    CHECK(!hde_disk_equal(&a, &b), "taking the label off is a change too");
}

int main(void)
{
    test_sizes();
    test_show();
    test_mount();
    test_eject();
    test_text();
    test_equal();
    printf("\ndisks: %d passed, %d failed\n", passes, fails);
    return fails;
}
