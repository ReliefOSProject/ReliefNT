#include <reliefnt/graphics_service.h>
#include <reliefnt/console.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <reliefnt/devfs.h>

/* The storage modules are independent translation units; this fixture keeps
 * them in one TU so a plain host build can exercise them together.  The list
 * mirrors the former drivers/bootstrap/storage.c aggregation, in its order. */
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
#ifdef RELIEFNT_FIXTURE_DEVICE_FLUSH
#define ahci_flush_cache storage_reference_hardware_flush
#endif
#include "../../drivers/storage/ahci/storage_ahci.c"
#ifdef RELIEFNT_FIXTURE_DEVICE_FLUSH
#undef ahci_flush_cache
#endif
#include "../../drivers/storage/nvme/storage_nvme.c"
/* Optional ISO-sector hardware boundary for the referenced-backend fixture. */
#ifdef RELIEFNT_FIXTURE_ISO_READER
#define storage_read_iso_blocks storage_reference_hardware_iso_blocks
#endif
#ifdef RELIEFNT_FIXTURE_SECTOR_STATS
#define storage_read_sectors storage_reference_hardware_sectors
#endif
#include "../../block/core/storage_block.c"
#ifdef RELIEFNT_FIXTURE_SECTOR_STATS
#undef storage_read_sectors
#endif
#ifdef RELIEFNT_FIXTURE_ISO_READER
#undef storage_read_iso_blocks
#endif
#include "../../fs/ext_common/format.c"
#include "../../fs/ext_common/checksum.c"
#include "../../fs/ext_common/cache.c"
#include "../../fs/ext_common/alloc.c"
#include "../../fs/ext_common/extent.c"
#include "../../fs/ext_common/ops.c"
#include "../../fs/ext_common/xattr.c"
#ifndef RELIEFNT_FIXTURE_SEPARATE_JOURNAL
#include "../../fs/ext_common/journal.c"
#endif
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
#include "memory_block_fixture.h"

static int test_fail_cache_alloc;
static bool test_drm_registered;
static bool test_drm_render_ready;
static int test_fail_heap_after=-1;
void *kernel_malloc(size_t size)
{
    if(!test_fail_heap_after)return NULL;
    if(test_fail_heap_after>0)--test_fail_heap_after;
    return malloc(size);
}
void kernel_free(void *p) { free(p); }
void pci_unmap_mmio(void *pointer, uint64_t bytes)
{ (void)pointer; (void)bytes; abort(); }
uint64_t mm_alloc_pages_below(uint32_t pages, uint64_t mask)
{ (void)pages; (void)mask; abort(); }
uint64_t mm_alloc_page(void) { return (uintptr_t)aligned_alloc(4096, 4096); }
void *mm_phys_to_virt(uint64_t physical) { return (void *)(uintptr_t)physical; }
void mm_free_page(uint64_t p) { free((void *)(uintptr_t)p); }
uint64_t mm_virt_to_phys(const void *address) { return (uintptr_t)address; }
void mm_free_pages(uint64_t address,uint32_t pages)
{ (void)pages;free((void *)(uintptr_t)address); }
#ifndef RELIEFNT_FIXTURE_UNMAPPED_TRUNCATE
void sched_truncate_file_mappings(const struct storage_node *node, uint64_t size)
{ (void)node; (void)size; abort(); }
#endif
uint64_t mm_alloc_pages(uint32_t pages)
{ return test_fail_cache_alloc ? 0 : (uint64_t)(uintptr_t)calloc(pages, 4096); }
void console_printf(const char *format, ...) { (void)format; }

void console_log(enum console_log_level level, const char *format, ...)
{
    (void)level; (void)format;
}
bool graphics_service_is_registered(void) { return test_drm_registered; }
bool graphics_service_render_node_ready(void) { return test_drm_render_ready; }
int audio_card_snapshot(uint32_t index, uint32_t *out_id,
                        struct audio_card_identity *identity)
{ (void)index; (void)out_id; (void)identity; return -2; }
int audio_device_parse_name(const char *name, uint32_t *card,
                            uint32_t *device,
                            enum audio_direction *direction)
{ (void)name; (void)card; (void)device; (void)direction; return -22; }
int audio_card_pcm_caps(uint32_t card, uint32_t device,
                        enum audio_direction direction,
                        struct audio_caps *caps)
{ (void)card; (void)device; (void)direction; (void)caps; return -19; }
void kernel_execution_lock_irqsave(uint64_t *flags) { *flags = 0; }
void kernel_execution_unlock_irqrestore(uint64_t flags) { (void)flags; }
uint32_t sched_current_pid(void) { return 1; }
struct task *sched_current_task(void) { return NULL; }
#ifndef RELIEFNT_FIXTURE_REAL_DAC
bool task_in_group(const struct task *task, uint32_t gid, bool real)
{ (void)task; (void)gid; (void)real; abort(); }
#endif
void page_cache_invalidate_node(const struct storage_node *node)
{ assert(node && (node->flags & (STORAGE_NODE_FLAG_EXT2 | STORAGE_NODE_FLAG_TMPFS))); }
#ifndef RELIEFNT_FIXTURE_PTY_METADATA
int pty_inode_permissions(const struct storage_node *node,struct reliefos_permissions *value,bool write)
{ (void)node; (void)value; (void)write; abort(); }
#endif
uint32_t smp_cpu_count(void) { return 1; }
int pty_lookup_path(const char *path, struct storage_node *node)
{ (void)path; (void)node; abort(); }
int pty_lookup_vt_path(const char *path, struct storage_node *node)
{ (void)path; (void)node; abort(); }
/* RAM-backed ext2 must never touch the physical controller or other codecs. */
#ifndef RELIEFNT_FIXTURE_OWN_IRQ_LOCKS
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock,uint64_t *flags)
{ assert(!lock->state);lock->state=1;*flags=0; }
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock,uint64_t flags)
{ assert(lock->state && !flags);lock->state=0; }
#endif
void kernel_spin_lock(struct kernel_spinlock *lock)
{
    assert(lock==&storage_transport_lock && !lock->state);lock->state=1;
}
void kernel_spin_unlock(struct kernel_spinlock *lock)
{
    assert(lock==&storage_transport_lock && lock->state);lock->state=0;
}
uint8_t x86_64_inb(uint16_t port) { (void)port; abort(); }
uint16_t x86_64_inw(uint16_t port) { (void)port; abort(); }
void x86_64_outb(uint8_t value, uint16_t port) { (void)value; (void)port; abort(); }
void x86_64_outw(uint16_t value, uint16_t port) { (void)value; (void)port; abort(); }
void x86_64_insw(uint16_t port, void *buffer, size_t count) { (void)port; (void)buffer; (void)count; abort(); }
void x86_64_outsw(uint16_t port, const void *buffer, size_t count) { (void)port; (void)buffer; (void)count; abort(); }
uint64_t time_ticks(void) { abort(); }
static uint64_t test_wall_seconds=1800000000;
int time_wall_clock(struct reliefos_time_info *info)
{ *info = (struct reliefos_time_info){.unix_seconds = test_wall_seconds}; return 0; }
static void load_volume(const char *image, unsigned id)
{
    FILE *file = fopen(image, "rb");
    assert(file && !fseek(file, 0, SEEK_END));
    long size = ftell(file);
    assert(size > 0 && !fseek(file, 0, SEEK_SET));
    struct storage_volume *volume = &g_volumes[id];
    *volume = (struct storage_volume){.volume_id = id, .ext2_sector_count = size / 512};
    uint8_t *bytes=malloc(size);assert(bytes);
    assert(fread(bytes, 1, size, file) == (size_t)size);
    assert(!fclose(file));
    test_memory_bind(volume,bytes,(uint64_t)size);

    g_active_volume = volume;
    assert(ext2_mount() == 0);
    volume->fs_ops=&storage_ext_ops;
    volume->ready = true;
    storage_cache_invalidate();
}

#include "mount_fixture_ops.h"

#ifdef RELIEFNT_FIXTURE_REGISTER_TYPES
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

#endif

int main(int argc, char **argv)
{
#ifdef RELIEFNT_FIXTURE_REGISTER_TYPES
    storage_fixture_register_filesystems();
#endif
    assert(argc == 2);
    load_volume(argv[1], 0);
    struct vfs_namespace ns={0};struct vfs_mount *base=NULL,*devices=NULL;
    struct vfs_path root={0},point={0};
    assert(!storage_create_vfs_mount(&g_volumes[0],NULL,&base));
    assert(!vfs_namespace_init(&ns,base));vfs_mount_put(base);
    assert(!vfs_namespace_root(&ns,&root));
    assert(!vfs_kernel_mkdir(&ns,"/dev",0755));
    assert(!vfs_path_lookup(&ns,&root,&root,"/dev",0,&point));
    assert(!devfs_create_vfs_mount(&devices) && !vfs_mount_attach(&ns,devices,&point));
    vfs_mount_put(devices);vfs_path_put(&point);
    struct storage_node metadata_root;
    assert(!mount_fixture_lookup(&ns,"/", &metadata_root));
    storage_task_io_owner = 2;
    storage_set_io_async_context(true);
    uint64_t mount_flags = UINT64_MAX;
    /* Mount properties are cached metadata, independent of an in-flight
     * asynchronous disk transaction owned by another process. */
    assert(!storage_node_mount_flags(&metadata_root, &mount_flags) && !mount_flags);
    storage_release_task_io(2);
    storage_set_io_async_context(false);
    struct storage_node dri;
    test_drm_registered = true;
    assert(mount_fixture_lookup(&ns,"/dev/dri", &dri) == 0 &&
           dri.type == RELIEFOS_FS_TYPE_DIR &&
           dri.first_cluster == STORAGE_DEV_KIND_DRI_DIR);
    struct storage_node render;
    assert(mount_fixture_lookup(&ns,"/dev/dri/renderD128", &render) == -2);
    uint64_t dri_cursor = 0;
    struct reliefos_dir_entry dri_entry;
    assert(storage_readdir_node(&dri, &dri_cursor, &dri_entry) == 1 &&
           !strcmp(dri_entry.name, "card0") &&
           dri_entry.type == RELIEFOS_FS_TYPE_DEVICE);
    assert(storage_readdir_node(&dri, &dri_cursor, &dri_entry) == 0);
    test_drm_render_ready = true;
    assert(mount_fixture_lookup(&ns,"/dev/dri/renderD128", &render) == 0 &&
           render.type == RELIEFOS_FS_TYPE_DEVICE &&
           render.first_cluster == STORAGE_DEV_KIND_DRM_RENDERD128 &&
           (render.flags & STORAGE_NODE_FLAG_DEV_NODE));
    dri_cursor = 0;
    assert(storage_readdir_node(&dri, &dri_cursor, &dri_entry) == 1 &&
           !strcmp(dri_entry.name, "card0"));
    assert(storage_readdir_node(&dri, &dri_cursor, &dri_entry) == 1 &&
           !strcmp(dri_entry.name, "renderD128") &&
           dri_entry.type == RELIEFOS_FS_TYPE_DEVICE);
    assert(storage_readdir_node(&dri, &dri_cursor, &dri_entry) == 0);
    test_drm_registered = false;
    assert(mount_fixture_lookup(&ns,"/dev/dri", &dri) == -2);
    test_drm_registered = true;
    test_drm_render_ready = false;

    assert(vfs_kernel_mkdir(&ns,"/target",0755) == 0);
    assert(vfs_kernel_mkdir(&ns,"/target",0755) == -17);
    load_volume(argv[1], 1);
    assert(!vfs_path_lookup(&ns,&root,&root,"/target",0,&point));
    assert(!storage_create_vfs_mount(&g_volumes[1],NULL,&base));
    assert(!vfs_mount_attach(&ns,base,&point));vfs_mount_put(base);vfs_path_put(&point);
    /* The update wizard prepares the same target twice: scan, then update. */
    int ret = vfs_kernel_mkdir(&ns,"/target",0755);
    printf("mkdir mounted /target: %d (expected -17/EEXIST)\n", ret);
    fflush(stdout);
    assert(ret == -17);
    assert(vfs_kernel_mkdir(&ns,"/",0755) == -17);
    storage_task_io_owner = 2;
    storage_set_io_async_context(true);
    assert(vfs_kernel_mkdir(&ns,"/target",0755) == -RELIEFOS_EEXIST);
    storage_release_task_io(2);
    storage_set_io_async_context(false);
    assert(vfs_kernel_mkdir(&ns,"/target/boot",0755) == 0);
    load_volume(argv[1], 2);
    assert(!vfs_path_lookup(&ns,&root,&root,"/target/boot",0,&point));
    assert(!storage_create_vfs_mount(&g_volumes[2],NULL,&base));
    assert(!vfs_mount_attach(&ns,base,&point));vfs_mount_put(base);vfs_path_put(&point);
    for (unsigned i = 0; i < 3; ++i) {
        assert(vfs_kernel_mkdir(&ns,"/target",0755) == -17);
        assert(vfs_kernel_mkdir(&ns,"/target/boot",0755) == -17);
        assert(vfs_kernel_mkdir(&ns,"/target/boot/",0755) == -17);
    }
    assert(vfs_kernel_mkdir(&ns,"/target/new",0755) == 0);
    assert(vfs_kernel_mkdir(&ns,"/target/new",0755) == -17);
    assert(vfs_kernel_mkdir(&ns,"/target/missing/child",0755) == -2);
    assert(vfs_kernel_write_file(&ns,"/target/file", "data", 4) == 0);
    assert(vfs_kernel_mkdir(&ns,"/target/file",0755) == -17);
    assert(vfs_kernel_mkdir(&ns,"/target/file/child",0755) == -20);
    struct vfs_lookup_result link_parent={0};struct reliefos_permissions mode={0777,0,0};
    assert(!vfs_kernel_lookup(&ns,"/target/link",VFS_LOOKUP_PARENT,&link_parent));
    assert(!vfs_path_create_node(&link_parent.path,link_parent.last,LINUX_S_IFLNK,&mode,"missing",NULL));
    vfs_path_put(&link_parent.path);
    assert(vfs_kernel_mkdir(&ns,"/target/link",0755) == -17);
    g_active_volume = &g_volumes[0];
    struct storage_node node;
    assert(ext2_lookup_path("/target/new", &node) == -2);
    vfs_path_put(&root);assert(!vfs_namespace_destroy(&ns));
    for(unsigned i=0;i<3;++i) {
        assert(!storage_inode_volume_busy(i) && !g_volumes[i].vfs_mounts);
        assert(!storage_inode_purge_volume(i));storage_ext4_journal_close(&g_volumes[i]);
        test_memory_release(&g_volumes[i]);
    }
    puts("PASS storage mkdir: mounted roots, nested mounts, existing objects and target volume isolation");
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
