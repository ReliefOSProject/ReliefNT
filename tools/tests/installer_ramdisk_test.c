/* Host regression for the installer RAM-root module's physical-address contract. */
#include <reliefnt/graphics_service.h>
#include <reliefnt/console.h>
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../block/core/storage_internal.h"
#include <reliefnt/tmpfs.h>
#include "../../block/core/storage_state.c"
#include "../../block/core/device_number.c"
#include "../../block/core/storage_state_data.c"
#include "../../block/core/volume.c"
#include "../../block/device/registry.c"
#include "../../block/device/inventory.c"
#include "../../block/device/optical.c"
#include "../../block/device/storage_devlinks.c"
#include "../../fs/ext_common/ext2_cache.c"
#include "../../drivers/storage/ide/storage_ide.c"
#include "../../drivers/storage/ahci/storage_ahci.c"
#include "../../drivers/storage/nvme/storage_nvme.c"
#include "../../block/core/storage_block.c"
#include "../../fs/ext_common/format.c"
#include "../../fs/ext_common/checksum.c"
#include "../../fs/ext_common/cache.c"
#include "../../fs/ext_common/alloc.c"
#include "../../fs/ext_common/extent.c"
#include "../../fs/ext_common/ops.c"
#include "../../fs/ext_common/xattr.c"
#include "../../fs/ext_common/journal.c"
#include "../../fs/ext_common/dir.c"
#include "../../block/core/storage_sync.c"
#include "../../fs/iso9660/iso.c"
#include "../../fs/iso9660/native.c"
#include "modules/fat32.h"
#include "../../fs/vfs/inode.c"
#include "../../fs/ext_common/vfs.c"
#include "../../fs/ext_common/ext2.c"
#include "modules/exfat.h"
#include "../../fs/ext_common/mount.c"
#include "../../fs/ext_common/sync.c"
#include "../../fs/ext_common/native.c"
#include "../../fs/vfs/mount.c"
#include "../../sound/core/storage_audio.c"
#include "modules/vfs.h"
#include "../../fs/vfs/mounts.c"
#include "../../fs/tmpfs/core.c"
#include "../../fs/tmpfs/tmpfs.c"
#include "../../fs/tmpfs/native.c"
#include "../../fs/security/permissions.c"
#include "../../fs/security/sidecar.c"
#include "../../fs/vfs/statfs.c"
#include "../../block/install/storage_installer.c"
#include "../../fs/fat/mkfs.c"
#include "../../fs/exfat/mkfs.c"
#include "../../fs/ext_common/mkfs.c"
#include "modules/disk.h"
#include "../../block/partition/storage_identity.c"
#include "../../block/partition/boot_root.c"
#include "../../lib/text_utf16.c"

enum { TEST_RAMDISK_PHYS = 0x12345000u };

static uint8_t *test_ramdisk_bytes;
static uint64_t test_ramdisk_length;
static unsigned test_mm_mapping_calls;
static unsigned test_direct_mapping_calls;
static unsigned test_direct_range_calls;
static uint64_t test_last_direct_physical;
static bool test_direct_map_reject;

/** @brief Provide allocator storage for the actual storage implementation. */
void *kernel_malloc(size_t size) { return malloc(size); }
/** @brief Release allocator storage for the actual storage implementation. */
void kernel_free(void *pointer) { free(pointer); }
/** @brief Allocate one host page for code paths retained by the storage fixture. */
void pci_unmap_mmio(void *pointer, uint64_t bytes)
{ (void)pointer; (void)bytes; abort(); }
void pci_config_write16(uint8_t bus,uint8_t slot,uint8_t function,uint8_t offset,uint16_t value)
{ (void)bus;(void)slot;(void)function;(void)offset;(void)value;abort(); }
uint64_t mm_alloc_pages_below(uint32_t pages, uint64_t mask)
{ (void)pages; (void)mask; abort(); }
uint64_t mm_alloc_page(void) { return (uintptr_t)aligned_alloc(4096, 4096); }
/** @brief Map physical memory to a deliberately nonidentity host alias. */
void *mm_phys_to_virt(uint64_t physical)
{
    if (physical == TEST_RAMDISK_PHYS) {
        ++test_mm_mapping_calls;
        return (void *)(uintptr_t)(UINT64_C(0xffff800000000000) | physical);
    }
    return (void *)(uintptr_t)physical;
}
/** @brief Free a host page allocated by the storage fixture. */
void mm_free_page(uint64_t physical) { free((void *)(uintptr_t)physical); }
/** @brief Allocate contiguous host pages for storage cache paths. */
uint64_t mm_alloc_pages(uint32_t pages)
{ return (uint64_t)(uintptr_t)calloc(pages, 4096); }
/** @brief Free contiguous host pages allocated by the storage fixture. */
void mm_free_pages(uint64_t physical, uint32_t pages)
{ (void)pages; free((void *)(uintptr_t)physical); }
/** @brief Invert only owned host page mappings, never the retained boot image. */
uint64_t mm_virt_to_phys(const void *address)
{ assert(address!=test_ramdisk_bytes);return (uintptr_t)address; }

/** @brief Reject file-mapping truncation outside this boot-time host fixture. */
void sched_truncate_file_mappings(const struct storage_node *node, uint64_t size)
{ (void)node; (void)size; abort(); }
/** @brief Suppress formatted console output during the fixture. */
void console_printf(const char *format, ...) { (void)format; }
/** @brief Suppress console logging during the fixture. */
void console_log(enum console_log_level level, const char *format, ...)
{ (void)level; (void)format; }
/** @brief Report no DRM render device in the storage fixture. */
bool graphics_service_is_registered(void) { return false; }
/** @brief Report no DRM render node in the storage fixture. */
bool graphics_service_render_node_ready(void) { return false; }
/** @brief Report no audio cards in the storage fixture. */
int audio_card_snapshot(uint32_t index, uint32_t *out_id,
                        struct audio_card_identity *identity)
{ (void)index; (void)out_id; (void)identity; return -2; }
/** @brief Reject audio device name parsing in the storage fixture. */
int audio_device_parse_name(const char *name, uint32_t *card,
                            uint32_t *device,
                            enum audio_direction *direction)
{ (void)name; (void)card; (void)device; (void)direction; return -22; }
/** @brief Report no audio PCM capabilities in the storage fixture. */
int audio_card_pcm_caps(uint32_t card, uint32_t device,
                        enum audio_direction direction,
                        struct audio_caps *caps)
{ (void)card; (void)device; (void)direction; (void)caps; return -19; }
/** @brief Stub the kernel execution lock, which is uncontended in host tests. */
void kernel_execution_lock_irqsave(uint64_t *flags) { *flags = 0; }
/** @brief Stub kernel execution lock release in the single-threaded fixture. */
void kernel_execution_unlock_irqrestore(uint64_t flags) { (void)flags; }
/** @brief Report a single host CPU. */
uint32_t smp_cpu_count(void) { return 1; }
/** @brief Provide no scheduled task to boot-time storage code. */
struct task *sched_current_task(void) { return NULL; }
/** @brief Provide the host fixture's synthetic current pid. */
uint32_t sched_current_pid(void) { return 1; }
/** @brief Ignore page-cache invalidation for non-page-backed test nodes. */
void page_cache_invalidate_node(const struct storage_node *node) { (void)node; }
/** @brief Reject PTY lookup in this ramdisk-only fixture. */
int pty_lookup_path(const char *path, struct storage_node *node)
{ (void)path; (void)node; return -2; }
/** @brief Reject unrelated terminal metadata callbacks in the RAM-root fixture. */
int pty_inode_permissions(const struct storage_node *node,struct reliefos_permissions *value,bool write)
{ (void)node; (void)value; (void)write; abort(); }
/** @brief Reject VT lookup in this ramdisk-only fixture. */
int pty_lookup_vt_path(const char *path, struct storage_node *node)
{ (void)path; (void)node; return -2; }
/** @brief Reject unexpected transport locking for the memory-backed root. */
/** @brief Emulate short registry IRQ critical sections on this single-thread host. */
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock,uint64_t *flags)
{ assert(!lock->state);lock->state=1;*flags=0; }
/** @brief Restore the host fixture's emulated registry IRQ state. */
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock,uint64_t flags)
{ assert(lock->state && !flags);lock->state=0; }
void kernel_spin_lock(struct kernel_spinlock *lock) { assert(!lock->state);lock->state=1; }
/** @brief Reject unexpected transport unlock for the memory-backed root. */
void kernel_spin_unlock(struct kernel_spinlock *lock) { assert(lock->state);lock->state=0; }
/** @brief Reject port I/O because no physical controller is present. */
uint8_t x86_64_inb(uint16_t port) { (void)port; abort(); }
/** @brief Reject port I/O because no physical controller is present. */
uint16_t x86_64_inw(uint16_t port) { (void)port; abort(); }
/** @brief Reject port I/O because no physical controller is present. */
void x86_64_outb(uint8_t value, uint16_t port) { (void)value; (void)port; abort(); }
/** @brief Reject port I/O because no physical controller is present. */
void x86_64_outw(uint16_t value, uint16_t port) { (void)value; (void)port; abort(); }
void x86_64_insw(uint16_t port, void *buffer, size_t count) { (void)port; (void)buffer; (void)count; abort(); }
void x86_64_outsw(uint16_t port, const void *buffer, size_t count) { (void)port; (void)buffer; (void)count; abort(); }
/** @brief Reject unexpected controller polling in the memory-backed fixture. */
uint64_t time_ticks(void) { abort(); }
/** @brief Supply a stable wall clock for retained metadata helpers. */
int time_wall_clock(struct reliefos_time_info *info)
{ *info = (struct reliefos_time_info){.unix_seconds = 1800000000}; return 0; }
/** @brief Model an empty PCI bus during boot-root storage discovery. */
uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t function,
                           uint8_t offset)
{ (void)bus; (void)slot; (void)function; (void)offset; return 0xffffu; }
/** @brief Model absent PCI devices during boot-root storage discovery. */
uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t function,
                           uint8_t offset)
{ (void)bus; (void)slot; (void)function; (void)offset; return 0; }
/** @brief Refuse MMIO mappings because this fixture contains no devices. */
bool paging_mmio_uncached(uint64_t physical, uint64_t length)
{ (void)physical; (void)length; return false; }
/** @brief Validate the module's physical range without dereferencing it. */
bool paging_kernel_direct_map_range(uint64_t physical, uint64_t length)
{
    ++test_direct_range_calls;
    return !test_direct_map_reject && physical == TEST_RAMDISK_PHYS &&
           length <= test_ramdisk_length;
}
/** @brief Map the physical module base to its retained host backing. */
void *paging_kernel_direct_map(uint64_t physical)
{
    ++test_direct_mapping_calls;
    test_last_direct_physical = physical;
    return !test_direct_map_reject && physical == TEST_RAMDISK_PHYS
               ? test_ramdisk_bytes : NULL;
}

#include "mount_fixture_ops.h"
/** @brief Read one node through a temporary actual root and retire every path.
 * @param name Borrowed absolute test name. @param node Required node output.
 * @return Common lookup status. The fixture owns the RAM image throughout;
 * namespace destruction completes before either boot-root path is repeated.
 */
static int installer_lookup(const char *name,struct storage_node *node)
{
    struct vfs_namespace ns={0};struct vfs_path root={0};
    int rc=mount_fixture_begin(&ns,&g_volumes[STORAGE_VOLUME_ROOT],&root);
    if(rc)return rc;
    rc=mount_fixture_lookup(&ns,name,node);vfs_path_put(&root);
    assert(!vfs_namespace_destroy(&ns));return rc;
}

/** @brief Read the test image into the RAM backing retained by the mount. */
static void load_test_image(const char *path)
{
    FILE *file = fopen(path, "rb");
    assert(file && !fseek(file, 0, SEEK_END));
    long size = ftell(file);
    assert(size >= 4096 && !fseek(file, 0, SEEK_SET));
    test_ramdisk_bytes = aligned_alloc(4096, (size_t)size);
    assert(test_ramdisk_bytes && fread(test_ramdisk_bytes, 1, (size_t)size, file) ==
                                  (size_t)size);
    assert(!fclose(file));
    test_ramdisk_length = (uint64_t)size;
}

/** @brief Build one Multiboot list with an ignored module before the root. */
static struct boot_info test_boot_info(const char *root_name)
{
    struct boot_info boot = {0};
    boot.module_count = 2;
    boot.modules[0] = (struct boot_module){
        .start = TEST_RAMDISK_PHYS + 0x100000u,
        .end = TEST_RAMDISK_PHYS + 0x101000u,
        .name = "unrelated-module",
    };
    boot.modules[1] = (struct boot_module){
        .start = TEST_RAMDISK_PHYS,
        .end = TEST_RAMDISK_PHYS + test_ramdisk_length,
        .name = (char *)root_name,
    };
    return boot;
}

/** @brief Check both installer root entry points retain the direct-map image. */
static void test_installer_ramdisk_mount_paths(void)
{
    struct boot_info boot = test_boot_info("reliefos-installer-root");
    struct boot_info legacy_boot;
    struct storage_node node;
    unsigned prior_direct_calls;

    test_direct_map_reject = false;
    test_mm_mapping_calls = 0;
    test_direct_mapping_calls = 0;
    storage_mount_boot_root(&boot, true);
    assert(storage_installer_root_active() && storage_ready());
    assert(g_volumes[STORAGE_VOLUME_ROOT].transport==STORAGE_TRANSPORT_MEMORY);
    assert(g_volumes[STORAGE_VOLUME_ROOT].device_ref.device);
    assert(storage_volume_device_number(&g_volumes[STORAGE_VOLUME_ROOT]) == 0x100);
    char name[32];uint32_t ram_disk=g_volumes[STORAGE_VOLUME_ROOT].source_disk_id;
    assert(!storage_disk_device_name(ram_disk,-1,name,sizeof(name)) && !strcmp(name,"/dev/ram0"));
    struct vfs_mount *first=NULL,*second=NULL,*rejected=NULL;
    assert(storage_new_block_mount(ram_disk,UINT32_MAX,NULL,0,&rejected)==-22 && !rejected);
    assert(storage_new_block_mount(ram_disk,UINT32_MAX,"vfat",0,&rejected)==-22 && !rejected);
    assert(storage_new_block_mount(ram_disk,UINT32_MAX,"ext4",MS_RDONLY,&rejected)==-16 && !rejected);
    assert(!storage_new_block_mount(ram_disk,UINT32_MAX,"ext4",0,&first));
    assert(!storage_new_block_mount(ram_disk,UINT32_MAX,"ext4",0,&second));
    assert(g_volumes[STORAGE_VOLUME_ROOT].vfs_mounts==2);
    vfs_mount_put(first);vfs_mount_put(second);
    assert(!storage_ready());
    storage_init_installer_root(&boot);
    assert(test_last_direct_physical == TEST_RAMDISK_PHYS);
    assert(!test_mm_mapping_calls);
    assert(installer_lookup("/lost+found", &node) == 0);
    assert(node.type == RELIEFOS_FS_TYPE_DIR);
    prior_direct_calls = test_direct_mapping_calls;

    storage_init_installer_root(&boot);
    assert(storage_installer_root_active() && storage_ready());
    assert(g_active_volume == &g_volumes[STORAGE_VOLUME_ROOT]);
    assert(g_volumes[STORAGE_VOLUME_ROOT].device_ref.device && storage_volume_device_number(&g_volumes[STORAGE_VOLUME_ROOT])==0x100);
    assert(test_last_direct_physical == TEST_RAMDISK_PHYS);
    assert(test_direct_mapping_calls == prior_direct_calls);
    assert(!test_mm_mapping_calls);
    uint8_t sector[512];assert(!storage_read_device(&g_volumes[STORAGE_VOLUME_ROOT],2,1,sector));
    assert(!memcmp(test_ramdisk_bytes+1024,sector,sizeof(sector)));
    assert(installer_lookup("/lost+found", &node) == 0);
    assert(node.type == RELIEFOS_FS_TYPE_DIR);
    /* The temporary graph was the final native membership. Its context and
     * provider reference retire; reserved Multiboot backing is still intact. */
    assert(!storage_ready() && !g_volumes[STORAGE_VOLUME_ROOT].fs_type.type);

    legacy_boot = test_boot_info("leonos-installer-root");
    prior_direct_calls = test_direct_mapping_calls;
    storage_init_installer_root(&legacy_boot);
    assert(storage_installer_root_active() && storage_ready());
    assert(g_volumes[STORAGE_VOLUME_ROOT].device_ref.device && storage_volume_device_number(&g_volumes[STORAGE_VOLUME_ROOT])==0x100);
    assert(test_last_direct_physical == TEST_RAMDISK_PHYS);
    assert(test_direct_mapping_calls == prior_direct_calls);
    assert(!test_mm_mapping_calls);
}

/** @brief Check missing and unmappable modules leave no stale mounted root. */
static void test_installer_ramdisk_failure_cleanup(void)
{
    struct boot_info boot = test_boot_info("wrong-root-name");

    g_volumes[STORAGE_VOLUME_ROOT].ready = true;
    g_volumes[STORAGE_VOLUME_BOOT].ready = true;
    g_active_volume = &g_volumes[STORAGE_VOLUME_BOOT];
    g_installer_root_active = 1;
    storage_init_installer_root(&boot);
    assert(!storage_ready() && !storage_installer_root_active());
    assert(g_active_volume == &g_volumes[STORAGE_VOLUME_ROOT]);
    assert(!g_volumes[STORAGE_VOLUME_ROOT].ready && !g_volumes[STORAGE_VOLUME_BOOT].ready);

    g_volumes[STORAGE_VOLUME_ROOT].ready = true;
    g_volumes[STORAGE_VOLUME_BOOT].ready = true;
    g_active_volume = &g_volumes[STORAGE_VOLUME_BOOT];
    storage_mount_boot_root(&boot, true);
    assert(!storage_ready() && !storage_installer_root_active());
    assert(g_active_volume == &g_volumes[STORAGE_VOLUME_ROOT]);
    assert(!g_volumes[STORAGE_VOLUME_ROOT].ready && !g_volumes[STORAGE_VOLUME_BOOT].ready);
    test_direct_map_reject = false;
}

/** @brief Exercise installer module mapping, failure cleanup, and image lifetime. */
#include "../../fs/fat/type.c"
#include "../../fs/exfat/type.c"
#include "../../fs/ext4/type.c"
#include "../../fs/ext2/type.c"
#include "../../fs/iso9660/type.c"
/* Real registration is explicit in the image-construction fixtures; codec-only
 * fixtures can keep raw parsers independently linked. */
static void storage_fixture_register_filesystems(void)
{
    assert(!fat_register_filesystem());assert(!exfat_register_filesystem());
    assert(!ext2_register_filesystem());assert(!ext4_register_filesystem());assert(!iso_register_filesystem());
}

int main(int argc, char **argv)
{
    storage_fixture_register_filesystems();
    assert(argc == 2);
    load_test_image(argv[1]);
    struct boot_info rejected=test_boot_info("reliefos-installer-root");
    test_direct_map_reject=true;
    storage_init_installer_root(&rejected);
    assert(!storage_ready() && !storage_installer_root_active() && test_direct_range_calls==1);
    test_direct_map_reject=false;
    test_installer_ramdisk_mount_paths();
    test_installer_ramdisk_failure_cleanup();
    puts("PASS installer ramdisk module mapping, boot-root paths, cleanup, and lifetime");
    return 0;
}

/* This filesystem fixture keeps the real sound namespace parsers, with
 * hardware inventory deliberately absent. Admission/lifetime is covered by the
 * service suites; these boundary adapters preserve the original parser calls. */
int sound_service_timer_node(const char *name,struct storage_node *out)
{return storage_audio_timer_node(name,out);}
int sound_service_control_node(const char *name,struct storage_node *out)
{return storage_audio_control_node(name,out);}
int sound_service_pcm_node(const char *name,struct storage_node *out)
{return storage_audio_pcm_node(name,out);}
int sound_service_snd_readdir(uint64_t *cursor,struct reliefos_dir_entry *entry)
{return storage_audio_snd_readdir(cursor,entry);}

/* Permanent proc inventory and real sound namespace share this fixture's
 * deliberately empty hardware-card inventory; no sound implementation is faked. */
int sound_service_card_snapshot(uint32_t index,uint32_t *out_id,struct audio_card_identity *identity)
{return audio_card_snapshot(index,out_id,identity);}
