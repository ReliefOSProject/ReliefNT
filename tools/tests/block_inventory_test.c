/* SPDX-License-Identifier: Apache-2.0 */
#include "../../block/core/storage_core_api.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static unsigned allocations;
static bool fail_allocation;
int storage_acquire_task_io(void){return 0;}
int storage_install_identify(struct install_disk_state *disk,uint64_t *sectors){(void)disk;(void)sectors;abort();}
void *kernel_malloc(size_t n){if(fail_allocation)return NULL;void *p=calloc(1,n);if(p)++allocations;return p;}
void kernel_free(void *p){if(p){assert(allocations);--allocations;free(p);}}
void storage_memzero(void *p,size_t n){memset(p,0,n);}
void storage_copy_text(char *p,uint32_t n,const char *s){snprintf(p,n,"%s",s);}
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock,uint64_t *flags)
{assert(!lock->state);lock->state=1;*flags=0;}
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock,uint64_t flags)
{assert(lock->state && !flags);lock->state=0;}
static int no_read(void *context,uint64_t first,uint32_t count,void *bytes)
{(void)context;(void)first;(void)count;(void)bytes;abort();}
static int no_write(void *context,uint64_t first,uint32_t count,const void *bytes)
{(void)context;(void)first;(void)count;(void)bytes;abort();}
static const struct block_device_operations ops={.read=no_read,.write=no_write};
int main(void)
{
    struct block_device_registration root={.info={.device=block_device_allocate_number(false),.blocks=4096,
        .block_size=512,.max_blocks=64,.flags=BLOCK_DEVICE_WRITE_THROUGH,.transport=STORAGE_TRANSPORT_AHCI,
        .bus=0,.slot=31,.function=2,.port=0},.operations=&ops};
    assert(!register_block_device(&root));storage_refresh_disks();
    assert(g_install_disk_count==1 && g_install_disks[0].device==root.info.device && g_install_disks[0].name_index==0);
    uint64_t previous=0;uint32_t old_slot=0;
    for(unsigned cycle=0;cycle<64;++cycle) {
        struct block_device_registration disk={.info={.device=block_device_allocate_number(false),.blocks=4096,
            .block_size=512,.max_blocks=128,.flags=BLOCK_DEVICE_WRITE_THROUGH,.transport=STORAGE_TRANSPORT_IDE_PIO,
            .bus=0,.slot=3,.port=0},.operations=&ops};
        assert(disk.info.device!=previous && !register_block_device(&disk));
        if(cycle==15) {
            struct install_disk_state *old=g_install_disks;
            fail_allocation=true;
            assert(storage_refresh_disks()==-12 && g_install_disk_count==16 && g_install_disks==old && g_install_disks[0].present);
            fail_allocation=false;
        }
        assert(!storage_refresh_disks());
        uint32_t slot=1;while(slot<g_install_disk_count && g_install_disks[slot].device!=disk.info.device)++slot;
        assert(slot<g_install_disk_count && g_install_disks[slot].present);
        uint32_t current_count=0;struct reliefos_install_disk current[2];
        assert(!storage_install_list_disks(current,2,&current_count) && current_count==2 &&
            current[0].id==0 && current[1].id==slot);
        assert(g_install_disks[slot].name_index==1); /* Same physical slot keeps /dev/sdb after reload. */
        if(previous) {
            assert(storage_block_rdev(STORAGE_BLOCK_VOLUME_ID(old_slot,-1))==previous);
            struct block_device_ref stale={0};assert(storage_disk_block_get(old_slot,&stale)==-19 && !stale.device);
        }
        struct block_device_ref open={0};assert(!storage_disk_block_get(slot,&open));
        assert(unregister_block_device(&disk)==-16);block_device_put(&open);
        assert(!unregister_block_device(&disk));storage_refresh_disks();
        assert(!g_install_disks[slot].present && g_install_disks[0].present);
        previous=disk.info.device;old_slot=slot;
    }
    struct block_device_registration ram={.info={.device=block_device_make_number(1,0),.blocks=4096,
        .block_size=512,.max_blocks=128,.flags=BLOCK_DEVICE_WRITE_THROUGH,.transport=STORAGE_TRANSPORT_MEMORY},.operations=&ops};
    assert(!register_block_device(&ram) && !storage_refresh_disks());
    uint32_t all_count=0,install_count=0;struct reliefos_install_disk all[2],install[2];
    assert(!storage_list_block_disks(all,2,&all_count) && all_count==2 && all[1].id==65);
    assert(!storage_install_list_disks(install,2,&install_count) && install_count==1 && install[0].id==0);
    assert(g_install_disks[65].name_index==0 && storage_block_rdev(STORAGE_BLOCK_VOLUME_ID(65,-1))==0x100);
    struct block_device_registration next=root;next.info.device=block_device_allocate_number(false);next.info.port=1;
    assert(!register_block_device(&next) && !storage_refresh_disks());
    assert(g_install_disks[66].name_index==2); /* ram0 did not consume an sd name. */
    assert(!unregister_block_device(&next) && !unregister_block_device(&ram));
    assert(!unregister_block_device(&root));storage_refresh_disks();
    uint32_t remaining=99;assert(!storage_install_list_disks(NULL,0,&remaining) && !remaining);
    assert(g_install_disk_count==67 && allocations==1); /* One permanent metadata array, no registry leak. */
    puts("PASS actual inventory64 reloads: same physical name, new dev_t, stale IDs never alias and no sixteen-cycle exhaustion");
}
