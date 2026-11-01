/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <reliefnt/blkdev.h>
#include <reliefnt/console.h>
#include <reliefnt/devfs.h>
#include <reliefnt/lock.h>
#include <reliefnt/memory_pressure.h>
#include <reliefnt/mount.h>
#include <reliefnt/page_cache.h>
#include <reliefnt/permissions.h>
#include <reliefnt/sched.h>
#include <reliefnt/swap.h>
#include <reliefnt/swap_internal.h>
#include <reliefnt/syscall.h>
#include <reliefnt/syscall_internal.h>
#include <linux/capability.h>
#include <linux/fcntl.h>
#include <linux/stat.h>
#include "../../mm/swap/internal.h"
/** @brief Resolve and activate a backing. @param path User pointer.
 * @param flags Linux flags. @return Zero or negative errno. */
extern int swap_activate_user(uint64_t path,uint32_t flags) __attribute__((weak));
/** @brief Resolve and deactivate a backing. @param path User pointer.
 * @return Zero or negative errno. */
extern int swap_deactivate_user(uint64_t path) __attribute__((weak));
static struct task task={.kind=TASK_KIND_USER,.pid=3,.cap_effective=1ULL<<CAP_SYS_ADMIN};
static struct fake_file { struct storage_node node; bool claimed,unwritten[4];uint64_t physical;unsigned writers; } files[3];
static uint8_t disk[512*512];
static unsigned allocations,refs,paths,opens,suspends,flushes,conversions;
static int deny_access,fail_sync,hole,fail_checkpoint,no_convert,fail_writer,fail_open;
static uint64_t raw_count=64;static unsigned raw_calls;
static struct swap_storage_claim *claims;
static struct swap_storage_token *active_token;
#ifdef TASK11_SWAPOFF_RECOVERY
static unsigned console_warnings;
#endif

/** @brief Accept fixture diagnostics. @param level Severity. @param fmt Format. */
void console_log(enum console_log_level level,const char *fmt,...)
{
    (void)level; (void)fmt;
#ifdef TASK11_SWAPOFF_RECOVERY
    ++console_warnings;
#endif
}

#ifndef TASK11_SWAPOFF_RECOVERY
/* This activation-only fixture models no ordinary frames or reclaim path. */
__attribute__((weak)) int kernel_signal_fatal_pending(const struct task *owner)
{ (void)owner;return 0; }
uint64_t mm_total_memory_kib(void) { return 0; }
uint64_t mm_free_memory_kib(void) { return 0; }
uint64_t mm_emergency_reserve_pages(void) { return 0; }
int memory_reclaim_swapoff_step(struct pressure_cursor *cursor,uint64_t pages,
                                uint32_t limit,uint32_t *released)
{ (void)cursor;(void)pages;(void)limit;(void)released;abort(); }
#endif

/** @brief Allocate fixture metadata. @param n Bytes. @return Memory. */
void *kernel_malloc(size_t n) { void *p=malloc(n);if(p)++allocations;return p; }
/** @brief Model an already-resident path before the activation reservation.
 * @param dst Destination. @param cap Capacity. @param user_ptr Fixture text.
 * @return Zero after copying the pathname.
 */
#ifndef TASK8_SWAP_REAL_PATH
int copy_user_path(char *dst,uint32_t cap,uint64_t user_ptr)
{ const char *text=(const char *)(uintptr_t)user_ptr;assert(strlen(text)<cap);strcpy(dst,text);return 0; }
int task_lookup_user_text(struct task *owner,uint64_t address,uint32_t length,bool allow_null,struct task_user_text *out)
{ (void)owner;(void)address;(void)length;(void)allow_null;(void)out;abort(); }
#endif
/** @brief Free fixture metadata. @param p Owned memory. */
void kernel_free(void *p) { if(p){assert(allocations);--allocations;free(p);} }
/** @brief Current task. @return Fixture task. */
struct task *sched_current_task(void) { return &task; }
/** @brief Current task ID. @return PID. */
uint32_t sched_current_pid(void) { return task.pid; }
/** @brief Enumerate tasks. @param i Index. @return Task or NULL. */
struct task *sched_task_at(uint32_t i) { return i ? NULL : &task; }
/** @brief The activation fixture has no modeled user MM or swapped leaves. */
bool kernel_execution_is_outermost(void) { return true; }
#ifndef TASK9_SWAPOFF_REAL_MM
uint64_t mm_context_generation_existing(const struct address_space *as)
{ (void)as; return 0; }
bool mm_swap_record_next(struct address_space *as,uint64_t start,uint64_t *page,uint64_t *token)
{ (void)as;(void)start;(void)page;(void)token;abort(); }
int swap_restore_leaf(struct task *owner,uint64_t address)
{ (void)owner;(void)address;abort(); }
#endif
/** @brief VMA capacity. @param t Task. @return Zero. */
uint32_t sched_task_vma_capacity(const struct task *t)
{
    (void)t;
#ifdef TASK9_SWAPOFF_REAL_MM
    return SCHED_TASK_VMA_MAX;
#else
    return 0;
#endif
}
/** @brief VMA lookup. @param t Task. @param i Index. @return NULL. */
struct task_vma *sched_task_vma_at(struct task *t,uint32_t i)
{
#ifdef TASK9_SWAPOFF_REAL_MM
    assert(t && i < SCHED_TASK_VMA_MAX); return &sched_task_mm(t)->vmas[i];
#else
    (void)t; (void)i; return NULL;
#endif
}
/** @brief Cache occupancy. @param n Node. @return Zero. */
int page_cache_has_inode(const struct storage_node *n) { (void)n;return 0; }
/** @brief Lock. @param l Lock. @param f Saved flags. */
void kernel_spin_lock_irqsave(struct kernel_spinlock *l,uint64_t *f) { (void)l;*f=0; }
/** @brief Unlock. @param l Lock. @param f Saved flags. */
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *l,uint64_t f) { (void)l;(void)f; }
/** @brief Enter a reentrant execution transaction. @param flags Saved flags. */
void kernel_execution_lock_irqsave(uint64_t *flags) { *flags=0; }
/** @brief Exit a reentrant execution transaction. @param flags Saved flags. */
void kernel_execution_unlock_irqrestore(uint64_t flags) { (void)flags; }
/** @brief Suspend with physical claims still present. @param t Token. @return Status. */
int kernel_execution_suspend_irqrestore(struct kernel_execution_suspend_token *t)
{ assert(!active_token && t && !t->active && claims);if(fail_checkpoint)return -EINTR;t->active=1;++suspends;return 0; }
/** @brief Resume execution. @param t Token. @return Zero. */
int kernel_execution_resume_irqsave(struct kernel_execution_suspend_token *t)
{ assert(t && t->active);t->active=0;return 0; }
/** @brief Fail invariant. @param message Diagnostic. */
__attribute__((noreturn)) void panic(const char *message) { fprintf(stderr,"%s\n",message);abort(); }
/** @brief Shared namespace. @return Non-NULL namespace. */
struct vfs_namespace *vfs_system_namespace(void) { return (void *)(uintptr_t)1; }
/** @brief Resolve and retain a test path. @param ns Namespace. @param t Task.
 * @param dirfd Directory. @param user User string. @param empty Empty policy.
 * @param ids Real ID policy. @param flags Lookup flags. @param out Result. @return Status. */
int resolve_kernel_object_at_flags(struct vfs_namespace *ns,struct task *t,int32_t dirfd,
    const char *name,bool empty,bool ids,uint32_t flags,struct vfs_lookup_result *out)
{
    assert(ns && t==&task && dirfd==LINUX_AT_FDCWD && !empty && !ids && flags==VFS_LOOKUP_FOLLOW);
    if(!name)return -EFAULT;
    int i=!strcmp(name,"one") || !strcmp(name,"alias") ? 0 : !strcmp(name,"two") ? 1 :
        !strcmp(name,"raw") ? 2 : -1;
    if(i<0)return -ENOENT;*out=(struct vfs_lookup_result){0};out->path.mount=(void *)(uintptr_t)1;
    out->path.dentry=(void *)&files[i];++paths;return 0;
}
/** @brief Path node. @param p Path. @return Node. */
const struct storage_node *vfs_path_node(const struct vfs_path *p)
{ return p && p->dentry ? &((struct fake_file *)p->dentry)->node : NULL; }
/** @brief Release path. @param p Owned path. */
void vfs_path_put(struct vfs_path *p)
{ if(p && p->dentry){assert(paths);--paths;*p=(struct vfs_path){0};} }
/** @brief Compare identity. @param a First. @param b Second. @return Same inode. */
bool vfs_path_same_inode(const struct vfs_path *a,const struct vfs_path *b) { return a->dentry==b->dentry; }
/** @brief Mount policy. @param p Path. @return Flags. */
uint64_t vfs_path_mount_flags(const struct vfs_path *p) { assert(p && p->dentry);return 0; }
/** @brief Apply DAC. @param t Task. @param p Path. @param a Access. @param real Real IDs. @return Status. */
int fs_permissions_check_path(const struct task *t,const struct vfs_path *p,uint32_t a,bool real)
{ assert(t && p && a==(FS_ACCESS_READ|FS_ACCESS_WRITE) && !real);return deny_access ? -EACCES : 0; }
/** @brief Capture open. @param p Path. @param flags Flags. @param out State. @return Zero. */
int vfs_file_open(const struct vfs_path *p,uint32_t flags,struct vfs_open_file **out)
{ assert(p && flags==LINUX_O_RDWR);if(fail_open)return -ENOMEM;*out=(void *)(uintptr_t)1;++opens;return 0; }
/** @brief Close state. @param f Owned state. */
void vfs_file_close(struct vfs_open_file *f) { if(f){assert(opens);--opens;} }
/** @brief Stat file. @param p Path. @param out Stat. @return Zero. */
int vfs_path_stat(const struct vfs_path *p,struct linux_stat_abi *out)
{ assert(p && p->dentry);*out=(struct linux_stat_abi){.st_size=4*4096};return 0; }
/** @brief Read header. @param p Path. @param off Offset. @param out Bytes.
 * @param n Length. @param done Output count. @return Zero. */
int vfs_path_read(const struct vfs_path *p,uint64_t off,void *out,uint32_t n,uint32_t *done)
{ struct fake_file *f=(void *)p->dentry;assert(off==0 && n==4096);memcpy(out,disk+f->physical*512,n);*done=n;return 0; }
/** @brief Sync file. @param p Path. @param fs Filesystem. @param data Data only. @return Status. */
int vfs_path_sync(const struct vfs_path *p,bool fs,bool data)
{ assert(p && !fs && !data);return fail_sync ? -EIO : 0; }
/** @brief Unsupported native rdev. @param type Type. @param dev Device. @param out Node. @return Error. */
int devfs_lookup_rdev(uint32_t type,uint64_t dev,struct storage_node *out)
{ (void)type;(void)dev;(void)out;return -EINVAL; }
/** @brief Retain inode. @param n Node. @param out Inode. @return Zero. */
int storage_inode_get(const struct storage_node *n,struct storage_inode_ref **out)
{ assert(n->first_cluster>=1 && n->first_cluster<=2);*out=(void *)&files[n->first_cluster-1];return 0; }
/** @brief Release inode. @param inode Inode. @return Zero. */
int storage_inode_put(struct storage_inode_ref *inode) { assert(inode);return 0; }
/** @brief Admit a normal inode writer. @param inode Inode. @return Zero. */
int storage_inode_write_get(struct storage_inode_ref *inode)
{ struct fake_file *f=(void *)inode;if(fail_writer)return -ETXTBSY;++f->writers;return 0; }
/** @brief Release the captured inode writer. @param inode Inode. */
void storage_inode_write_put(struct storage_inode_ref *inode)
{ struct fake_file *f=(void *)inode;assert(f->writers);--f->writers; }
/** @brief Claim inode. @param inode Inode. @return Status. */
int storage_inode_swap_claim(struct storage_inode_ref *inode)
{ struct fake_file *f=(void *)inode;if(f->claimed)return -EBUSY;assert(f->writers);f->claimed=true;return 0; }
/** @brief Release inode claim. @param inode Inode. */
void storage_inode_swap_release(struct storage_inode_ref *inode)
{ struct fake_file *f=(void *)inode;assert(f->claimed);f->claimed=false; }
/** @brief Enter sync phase. @param token Token. @return Status. */
int swap_storage_begin(struct swap_storage_token *token)
{ if(active_token)return -EBUSY;assert(token && !token->active);token->active=true;active_token=token;return 0; }
/** @brief This fixture has no competing task transport owner. @return False. */
bool swap_storage_other_owner(void) { return false; }
/** @brief Exit sync phase. @param token Token. */
void swap_storage_end(struct swap_storage_token *token)
{ if(token && token->active){assert(active_token==token);token->active=false;active_token=NULL;} }
/** @brief Pin device. @param dev Device. @param out Token. @return Zero. */
int block_device_get(uint64_t dev,struct block_device_ref *out)
{ assert(dev==42 && out && !out->device);out->device=(void *)(uintptr_t)1;++refs;return 0; }
/** @brief Drop device. @param ref Token. */
void block_device_put(struct block_device_ref *ref)
{ assert(ref && ref->device && refs);ref->device=NULL;--refs; }
/** @brief Get geometry. @param ref Device. @param out Info. @return Zero. */
int block_device_info(const struct block_device_ref *ref,struct block_device_info *out)
{ assert(ref && ref->device);*out=(struct block_device_info){.device=42,.blocks=512,.block_size=512,
    .max_blocks=3,.flags=BLOCK_DEVICE_SWAP_SAFE};return 0; }
/** @brief Pin filesystem device. @param n File. @param out Device. @param first Start.
 * @param count Length. @return Zero. */
int swap_storage_file_device(const struct storage_node *n,struct block_device_ref *out,uint64_t *first,uint64_t *count)
{ assert(files[n->first_cluster-1].claimed);*first=32;*count=224;return block_device_get(42,out); }
/** @brief Obtain a real-style extent. @param n File. @param off Offset. @param out Extent. @return Status. */
int swap_storage_file_extent(const struct storage_node *n,uint64_t off,struct swap_storage_extent *out)
{
    struct fake_file *f=&files[n->first_cluster-1];unsigned page=(unsigned)(off/4096);
    assert(f->claimed && !(off%4096) && page<4);if(hole && page==2)return -EINVAL;
    *out=(struct swap_storage_extent){.logical=off/512,.physical=f->physical+page*8,
                                     .length=8,.unwritten=f->unwritten[page]};return 0;
}
/** @brief Convert an unwritten run. @param n File. @param off Offset. @param len Bytes. @return Zero. */
int swap_storage_file_initialize(const struct storage_node *n,uint64_t off,uint64_t len)
{ struct fake_file *f=&files[n->first_cluster-1];unsigned p=(unsigned)(off/4096);
  assert(f->claimed && len==4096 && f->unwritten[p]);++conversions;
  if(no_convert)return conversions>3 ? -ELOOP : 0;f->unwritten[p]=false;return 0; }
/** @brief Claim a range. @param c Record. @param r Device. @param first Start.
 * @param count Length. @param file Filesystem claim. @return Status. */
int swap_storage_claim_range(struct swap_storage_claim *c,const struct block_device_ref *r,
                             uint64_t first,uint64_t count,bool file)
{
    for(struct swap_storage_claim *p=claims;p;p=p->next)
        if(p->device==r->device && (!file || !p->file) && first<p->first+p->count && p->first<first+count)return -EBUSY;
    *c=(struct swap_storage_claim){.next=claims,.device=r->device,.first=first,.count=count,.file=file,.active=true};
    claims=c;return 0;
}
/** @brief Release range. @param c Claim. */
void swap_storage_release_range(struct swap_storage_claim *c)
{ if(c->active){struct swap_storage_claim **p=&claims;while(*p && *p!=c)p=&(*p)->next;
    assert(*p==c);*p=c->next;*c=(struct swap_storage_claim){0};} }
/** @brief Pin disk. @param id Disk. @param out Ref. @return Status. */
int storage_disk_block_get(uint32_t id,struct block_device_ref *out)
{ assert(id==0);return block_device_get(42,out); }
/** @brief Partition geometry. @param id Disk. @param part Partition. @param first Start.
 * @param count Length. @return Zero. */
int storage_disk_block_info(uint32_t id,int32_t part,uint64_t *first,uint64_t *count)
{ assert(id==0 && part==0);*first=300;*count=raw_count;return 0; }
/** @brief No mounted raw partition. @param id Disk. @param part Partition. @return Zero. */
int storage_disk_block_busy(uint32_t id,int32_t part) { assert(!id && !part);return 0; }
/** @brief Complete fixed command. @param token Token. @param ref Device. @param first Sector.
 * @param count Length. @param bytes Buffer. @param write Direction. @return Zero. */
int swap_storage_transfer(const struct swap_storage_token *token,const struct block_device_ref *ref,
                          uint64_t first,uint32_t count,void *bytes,bool write)
{ assert(token==active_token && ref->device && count && count<=3 && first+count<=512);
  if(first>=300)++raw_calls;
  if(write)memcpy(disk+first*512,bytes,count*512);else memcpy(bytes,disk+first*512,count*512);return 0; }
/** @brief Flush device. @param token Token. @param ref Device. @return Zero. */
int swap_storage_flush(const struct swap_storage_token *token,const struct block_device_ref *ref)
{ assert(token==active_token && ref->device);++flushes;return 0; }
/** @brief Translate test physical page. @param physical Address. @return Pointer. */
void *mm_phys_to_virt(uint64_t physical) { return (void *)(uintptr_t)physical; }
/** @brief Encode little-endian. @param p Bytes. @param n Value. */
static void word(uint8_t *p,uint32_t n) { p[0]=n;p[1]=n>>8;p[2]=n>>16;p[3]=n>>24; }
/** @brief Make swap header. @param sector Sector. */
static void header(unsigned sector)
{ uint8_t *h=disk+sector*512;memset(h,0,4096);word(h+1024,1);word(h+1028,3);memcpy(h+4086,"SWAPSPACE2",10); }
/** @brief Ensure ownership is empty. */
static void empty(void)
{ assert(!claims && !refs && !paths && !opens && !allocations && !active_token);
  assert(!files[0].claimed && !files[1].claimed && !files[0].writers && !files[1].writers); }

/** @brief Exercise activation, rollback, priority, data and raw ownership. @return Zero. */
int main(void)
{
    if (!swap_activate_user || !swap_deactivate_user) {
        fputs("FAIL: swap activation/deactivation ownership is absent\n",stderr);return 1;
    }
    files[0].node=(struct storage_node){.type=RELIEFOS_FS_TYPE_FILE,.flags=STORAGE_NODE_FLAG_EXT2,.first_cluster=1};
    files[1].node=(struct storage_node){.type=RELIEFOS_FS_TYPE_FILE,.flags=STORAGE_NODE_FLAG_EXT2,.first_cluster=2};
    files[0].physical=64;files[1].physical=128;
    files[2].node=(struct storage_node){.flags=STORAGE_NODE_FLAG_DEV_NODE|STORAGE_NODE_FLAG_DEV_BLOCK,
        .first_cluster=STORAGE_DEV_KIND_DISK,.volume_id=0x10000};
    header(64);header(128);header(300);
    deny_access=1;assert(syscall_mm_swapon((uintptr_t)"one",0)==-EACCES);deny_access=0;empty();
    fail_writer=1;assert(syscall_mm_swapon((uintptr_t)"one",0)==-ETXTBSY);fail_writer=0;empty();
    fail_open=1;assert(syscall_mm_swapon((uintptr_t)"one",0)==-ENOMEM);fail_open=0;empty();
    hole=1;assert(syscall_mm_swapon((uintptr_t)"one",0)==-EINVAL);hole=0;empty();
    fail_sync=1;assert(syscall_mm_swapon((uintptr_t)"one",0)==-EIO);fail_sync=0;empty();
    fail_checkpoint=1;assert(syscall_mm_swapon((uintptr_t)"one",0)==-EINTR);fail_checkpoint=0;empty();
    files[1].unwritten[2]=true;no_convert=1;conversions=0;
    assert(syscall_mm_swapon((uintptr_t)"two",0)==-EIO);no_convert=0;conversions=0;empty();
    files[0].writers=1; /* An old writable OFD remains open throughout activation. */
    assert(!syscall_mm_swapon((uintptr_t)"one",0x8005));
    assert(files[0].writers==2);
    assert(syscall_mm_swapon((uintptr_t)"alias",0)==-EBUSY);
    files[1].unwritten[2]=true;assert(!syscall_mm_swapon((uintptr_t)"two",0x8009));
    assert(conversions==1 && suspends && flushes>=2);
    struct swap_stats stats;swap_get_stats(&stats);assert(stats.total==6 && stats.free==6);
    uint64_t slot=swap_slot_alloc();assert(slot);
    assert(syscall_mm_swapoff((uintptr_t)"two")==-EBUSY);
    void *page=aligned_alloc(4096,4096);assert(page);memset(page,0x5a,4096);
    assert(!swap_slot_submit_write(slot,(uintptr_t)page));memset(page,0,4096);
    assert(!swap_slot_submit_read(slot,(uintptr_t)page));
    for(unsigned i=0;i<4096;++i)assert(((uint8_t *)page)[i]==0x5a);
    swap_slot_put(slot);free(page);
    assert(!syscall_mm_swapoff((uintptr_t)"two"));
    assert(!syscall_mm_swapoff((uintptr_t)"alias"));
    assert(files[0].writers==1);storage_inode_write_put((void *)&files[0]);empty();
    raw_count=4;raw_calls=0;assert(syscall_mm_swapon((uintptr_t)"raw",0)==-EINVAL && !raw_calls);
    raw_count=64;empty();assert(!syscall_mm_swapon((uintptr_t)"raw",0));
    assert(syscall_mm_swapon((uintptr_t)"raw",0)==-EBUSY);
    assert(!syscall_mm_swapoff((uintptr_t)"raw"));empty();
    puts("swap activation, rollback, priority, coexistence and raw ownership: ok");return 0;
}
