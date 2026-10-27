#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../block/core/storage_internal.h"

static struct install_disk_state fixture_disks[STORAGE_INSTALL_DISK_INITIAL_CAPACITY];
struct install_disk_state *g_install_disks=fixture_disks;
uint32_t g_install_disk_count;
static int duplicate, absent, nvme;
static const char identity[] = "12345678-1234-4abc-90ab-112233445566";

/** @brief Supply a checked GPT identity at a non-default slot on disk1. */
int storage_disk_partition_uuid(uint32_t disk, uint32_t part, char uuid[37])
{
    if (absent || part != 6 || (!disk && !duplicate)) return -2;
    strcpy(uuid, identity);
    return 0;
}

/** @brief Model ordinary SATA/NVMe block names at the existing metadata boundary. */
int storage_disk_device_name(uint32_t disk, int32_t part, char *out, uint32_t capacity)
{
    if (nvme && disk == 1) snprintf(out, capacity, "/dev/nvme2n1p%u", (unsigned)part + 1);
    else snprintf(out, capacity, "/dev/sd%c%u", 'a' + disk, (unsigned)part + 1);
    return 0;
}

/** @brief Match the resolver's exported device spelling, independent of probe order. */
int storage_parse_block_name(const char *name, uint32_t *disk, int32_t *part)
{
    for (uint32_t d = 0; d < g_install_disk_count; ++d) {
        for (int32_t p = 0; p < (int32_t)RELIEFOS_DISK_MAX_PARTITIONS; ++p) {
            char path[64];
            storage_disk_device_name(d, p, path, sizeof(path));
            if (!strcmp(name, path + 5)) { *disk = d; *part = p; return 0; }
        }
    }
    return -2;
}

/** @brief Host implementation of the storage module's shared string primitive. */
size_t storage_strlen(const char *text) { return strlen(text); }
/** @brief Host implementation of the storage module's bounded string primitive. */
void storage_copy_text(char *out, uint32_t capacity, const char *text)
{ snprintf(out, capacity, "%s", text); }
/** @brief Host implementation of the storage module's exact string comparison. */
int storage_text_eq(const char *a, const char *b) { return !strcmp(a, b); }

/** @brief Check actual root parsing, shared UUID lookup, duplicates and filter scoping. */
int main(void)
{
    g_install_disk_count = 2;
    g_install_disks[0].present = g_install_disks[1].present = true;
    storage_boot_root_configure("root=/ log=screen,serial");
    assert(!storage_boot_root_requested());
    storage_boot_root_configure("debug=1 root=PARTUUID=12345678-1234-4ABC-90AB-112233445566 quiet");
    assert(storage_boot_root_requested());
    assert(storage_boot_root_select() == 1);
    const uint8_t guid[] = {0x78,0x56,0x34,0x12,0x34,0x12,0xbc,0x4a,
                            0x90,0xab,0x11,0x22,0x33,0x44,0x55,0x66};
    const uint8_t other[16] = {1};
    assert(storage_boot_root_accept_guid(other));
    storage_boot_root_filter_enable(true);
    assert(storage_boot_root_accept_guid(guid) && !storage_boot_root_accept_guid(other));
    storage_boot_root_filter_enable(false);
    assert(storage_boot_root_accept_guid(other));
    duplicate = 1;
    assert(storage_boot_root_select() == -76); /* Linux ENOTUNIQ */
    duplicate = 0; absent = 1;
    assert(storage_boot_root_select() == -2);
    absent = 0; nvme = 1;
    assert(storage_boot_root_select() == 1);
    const char *bad[] = {"root=PARTUUID=", "root=PARTUUID=00000000-0000-0000-0000-000000000000",
        "root=PARTUUID=12345678-1234-4abc-90ab-11223344556z", "root=PARTUUID=1234",
        "root=/dev/unsupported", "root=PARTUUID=12345678-1234-4abc-90ab-112233445566extra"};
    for (unsigned i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i) {
        storage_boot_root_configure(bad[i]);
        assert(storage_boot_root_requested() && storage_boot_root_select() == -22);
    }
    storage_boot_root_configure("root=broken root=PARTUUID=12345678-1234-4abc-90ab-112233445566");
    assert(storage_boot_root_select() == 1); /* Linux command line: last root wins. */
    storage_boot_root_configure(NULL);
    assert(!storage_boot_root_requested());
    puts("PASS Linux root=PARTUUID: exact GPT identity, SATA/NVMe, non-default slot, malformed/duplicate/missing refusal and scoped filter");
    return 0;
}
