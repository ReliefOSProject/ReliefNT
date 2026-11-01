/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Integrate the real staged read, VFS provider and COW page tables.
 * Physical pages and the execution owner are represented by the inherited
 * host fixture; no hardware, concurrent CPU or guest scheduling is modeled.
 */
#define vfs_path_node fixture_anon_vfs_path_node
#define vfs_path_read fixture_anon_vfs_path_read
#define main task8_inherited_paging_main
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type"
#include "private_anon_fault_test.c"
#pragma clang diagnostic pop
#undef main
#undef vfs_path_read
#undef vfs_path_node
#include <reliefnt/mount.h>
#include <reliefnt/module.h>
#include <reliefnt/swap.h>

#ifdef CONFIG_SWAP
/** @brief Keep this provider/fork fixture's resident pages available. */
int swap_prepare_user_range(struct task *task,uint64_t address,uint64_t size,bool writing)
{ assert(task && address && size<=32768 && writing);return 0; }
/** @brief Trap swap-leaf ownership in the resident-only page model. */
int swap_slot_get(uint64_t token) { (void)token;abort(); }
/** @brief Trap swap-leaf release in the resident-only page model. */
void swap_slot_put(uint64_t token) { (void)token;abort(); }
#endif

static unsigned calls, provider_refs;
uint32_t mm_page_refcount(uint64_t physical)
{
    for(unsigned i=0;i<sizeof(host_page_refs)/sizeof(host_page_refs[0]);++i)
        if(host_page_refs[i].physical==physical)return host_page_refs[i].references;
    return 0;
}
static int provider_lookup(void *context,const struct storage_node *parent,
                           const char *name,struct storage_node *out)
{ (void)context;(void)parent;(void)name;(void)out;abort(); }
static int provider_retain(void *context,const struct storage_node *node,void **pin)
{ (void)context;(void)node;*pin=NULL;++provider_refs;return 0; }
static void provider_release(void *context,const struct storage_node *node,void *pin)
{ (void)context;(void)node;assert(!pin && provider_refs);--provider_refs; }
static bool provider_same(void *context,const struct storage_node *a,const struct storage_node *b)
{ (void)context;return a->first_cluster==b->first_cluster; }
static int provider_read(void *context,const struct storage_node *node,uint64_t offset,
                         void *buffer,uint32_t capacity,uint32_t *read,uint64_t flags)
{
    (void)context;(void)node;(void)flags;assert(offset==0 && capacity==64 && provider_refs);
    *read=0;
    if(++calls==1)return -LINUX_EAGAIN;
    memset(buffer,0x71,capacity);*read=capacity;return 0;
}
static const struct vfs_backend_ops provider={.lookup=provider_lookup,.retain=provider_retain,
    .release=provider_release,.same_node=provider_same,.read=provider_read};

uint64_t time_uptime_us(void) { return 0; }
void storage_release_task_io(uint32_t pid) { assert(pid==10); }
void storage_drain_task_io(uint32_t pid) { assert(pid==10); }
int storage_disk_block_read(uint32_t disk,int32_t part,uint64_t off,void *buf,uint32_t len,uint32_t *done)
{ (void)disk;(void)part;(void)off;(void)buf;(void)len;(void)done;abort(); }
int storage_disk_block_write(uint32_t disk,int32_t part,uint64_t off,const void *buf,uint32_t len,uint32_t *done)
{ (void)disk;(void)part;(void)off;(void)buf;(void)len;(void)done;abort(); }
int storage_sync_disk(uint32_t disk) { (void)disk;abort(); }

int main(void)
{
    struct task *owner=calloc(1,sizeof(*owner)),*peer=calloc(1,sizeof(*peer));
    assert(owner && peer);owner->pid=10;owner->kind=TASK_KIND_USER;
    peer->pid=11;peer->kind=TASK_KIND_USER;peer->shared_mm=&owner->address_space;
    struct address_space child={0};const uint64_t address=RELIEFNT_USER_BASE+0x30000;
    current_task=owner;nx_enabled=true;
    assert(address_space_create(sched_task_as(owner)));
    uint64_t physical=mm_alloc_page();assert(physical);
    memset(mm_phys_to_virt(physical),0x25,4096);
    assert(address_space_map_user_page(sched_task_as(owner),address,physical,RELIEFNT_PAGE_WRITABLE));
    struct task_file file={.used=1,.flags=RELIEFOS_O_RDWR,
        .node={.type=RELIEFOS_FS_TYPE_FILE,.first_cluster=4}};
    struct storage_node root={.type=RELIEFOS_FS_TYPE_DIR};struct vfs_mount *mount=NULL;
    assert(!vfs_mount_new(&provider,NULL,NULL,&root,&mount));
    assert(!vfs_path_anonymous(mount,&file.node,&file.path_ref));vfs_mount_put(mount);
    owner->syscall_file=&file;owner->syscall_file_number=LINUX_SYS_READ;
    assert(syscall_regular_io(owner,&file,address,64,0,false,false)==-LINUX_EAGAIN);
    assert(calls==1 && mm_page_refcount(physical)==2);
    current_task=peer;
    /* Fail each allocation in turn until the complete clone succeeds. Every
     * unsuccessful child must be destroyed without changing the source. */
    uint64_t before=*address_space_pte(sched_task_as(peer),address);
    unsigned live=allocated,failures=0;
    for(unsigned step=1;step<32;++step) {
        struct address_space probe={0};fail_after_allocation=step;
        if(address_space_clone_cow(sched_task_as(peer),&probe)) {
            fail_after_allocation=0;address_space_destroy(&probe);break;
        }
        fail_after_allocation=0;++failures;
        assert(*address_space_pte(sched_task_as(peer),address)==before && allocated==live);
    }
    assert(failures>=4 && *address_space_pte(sched_task_as(peer),address)==before);
    assert(address_space_clone_cow(sched_task_as(peer),&child));
    current_task=owner;
    assert(syscall_regular_io(owner,&file,address,64,0,false,false)==64 && calls==2);
    uint64_t child_phys=address_space_user_page_phys(&child,address);
    uint64_t parent_phys=address_space_user_page_phys(sched_task_as(owner),address);
    assert(child_phys && parent_phys);
    for(unsigned i=0;i<64;++i) {
        assert(((uint8_t *)mm_phys_to_virt(parent_phys))[i]==0x71);
        assert(((uint8_t *)mm_phys_to_virt(child_phys))[i]==0x25);
    }
    task_regular_io_stage_cancel(owner);address_space_destroy(&child);
    /* A drained cancellation must unregister the alias so an ordinary later
     * fork shares and write-protects this frame as usual. */
    calls=0;file.offset=0;
    assert(syscall_regular_io(owner,&file,address,64,0,false,false)==-LINUX_EAGAIN);
    assert(mm_page_refcount(physical)==2);
    storage_drain_task_io(owner->pid);task_regular_io_stage_cancel(owner);
    owner->regular_io.active=false;file.io_owner=0;
    assert(mm_page_refcount(physical)==1);
    current_task=peer;assert(address_space_clone_cow(sched_task_as(peer),&child));
    assert(address_space_user_page_phys(&child,address)==physical);
    assert(!address_space_user_page_writable(sched_task_as(peer),address));
    address_space_destroy(&child);current_task=owner;
    address_space_destroy(sched_task_as(owner));vfs_path_put(&file.path_ref);
    assert(!provider_refs && !allocated);free(peer);free(owner);
    puts("PASS staged provider read preserves fork child COW snapshot");
    return 0;
}
