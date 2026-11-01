/* SPDX-License-Identifier: Apache-2.0 */
/** @file @brief Real DEVICE admission and ordinary positioned file I/O scaling.
 * CPU, TLB and physical aliases are host models; mapping, ownership, VFS,
 * vector dispatch, bounded file stages and the cache registry are production.
 */
#define TASK8_FIX3_REUSED_BACKING 1
#define TASK8_FIX5_FRONT 1
#include "task8_fix2_device_mmap_test.c"
#include <linux/uio.h>
#include <reliefnt/mount.h>

extern uint64_t device_mapping_test_visits;
extern uint64_t device_mapping_test_tree_visits __attribute__((weak));
int64_t syscall_legacy_io(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
static struct iovec vectors[1024];
static unsigned file_reads, drain_calls, alias_count;
static bool stall_next,pending_submitted,pending_drained;
static void *pending_buffer;
static uint32_t pending_length;
static uint64_t pending_position;
static uint8_t *device_bytes;
static uint64_t device_length,backing_length;

/** @brief Give staged DEVICE pages writable host aliases. @param physical
 * Indexed fixture backing page. @param flags Admitted cache flags. @return
 * The same backing bytes while one modeled supervisor alias is held. */
void *__wrap_paging_ioremap_device_page(uint64_t physical,uint64_t flags)
{
    assert(!flags && physical>=device_base && physical<device_base+backing_length);
    ++alias_count;return (void *)(uintptr_t)physical;
}
/** @brief Retire a modeled supervisor alias. @param alias Original page.
 * @param length Page length. @return True after consuming one alias. */
bool __wrap_paging_iounmap(void *alias,uint64_t length)
{
    assert((uintptr_t)alias>=device_base && (uintptr_t)alias<device_base+backing_length);
    assert(length==4096 && alias_count && (!pending_submitted || pending_drained));
    --alias_count;return true;
}
/** @brief Permit the host iovec snapshot plus the admitted modeled VMA.
 * @param address Start byte. @param length Byte count. @return Range validity. */
bool __wrap_user_range_ok(uint64_t address,uint64_t length)
{
    if(address==(uintptr_t)vectors && length<=sizeof(vectors))return true;
    return user_range_ok(address,length);
}
/** @brief Bind the legacy dispatcher to the fixture task. @return Current task. */
struct task *__wrap_sched_current_task(void) { return current; }
/** @brief Model monotonic clock with no yield during a synchronous call.
 * @return Constant timestamp. */
uint64_t time_uptime_us(void) { return 0; }
/** @brief Model release of a completed provider request. @param pid Caller. */
void storage_release_task_io(uint32_t pid) { assert(pid==current->pid); }
/** @brief Model synchronous provider drain. @param pid Caller. */
void storage_drain_task_io(uint32_t pid)
{
    assert(pid==current->pid);++drain_calls;
    if(pending_submitted) {
        struct regular_io_stage *stage=regular_stage_find(current);
        assert(!pending_drained && stage && pending_buffer==stage->bytes &&
               pending_length==32768 && pending_position==17);
        pending_drained=true;pending_buffer=NULL;
    }
}
/** @brief Return a deterministic byte independent of the device address.
 * @param offset Provider file offset. @return Pattern byte. */
static uint8_t byte_at(uint64_t offset) { return (uint8_t)(offset*13u+(offset>>8)); }
/** @brief Fill an ordinary VFS read immediately. @param context Provider cookie.
 * @param node Inode. @param offset File position. @param buffer Stage bytes.
 * @param length Requested bytes. @param done Accepted bytes. @param flags Mount flags.
 * @return Zero after full synchronous transfer. */
static int read_now(void *context,const struct storage_node *node,uint64_t offset,
                    void *buffer,uint32_t length,uint32_t *done,uint64_t flags)
{
    assert(context==&file_reads && node);(void)flags;++file_reads;
    if(stall_next) {
        stall_next=false;pending_submitted=true;pending_drained=false;
        pending_buffer=buffer;pending_length=length;pending_position=offset;
        *done=0;return -LINUX_EAGAIN;
    }
    for(uint32_t i=0;i<length;++i)((uint8_t *)buffer)[i]=byte_at(offset+i);
    *done=length;return 0;
}
/** @brief Retain a modeled ordinary provider node. @param context Cookie.
 * @param node Inode. @param pin Receives no extra pin. @return Zero. */
static int file_retain(void *context,const struct storage_node *node,void **pin)
{ assert(context==&file_reads && node && pin);*pin=NULL;return 0; }
/** @brief Retire a modeled ordinary provider node. @param context Cookie.
 * @param node Inode. @param pin Empty modeled pin. */
static void file_release(void *context,const struct storage_node *node,void *pin)
{ assert(context==&file_reads && node && !pin); }
/** @brief Compare modeled inode identities. @param context Cookie.
 * @param a First inode. @param b Second inode. @return True for equal identity. */
static bool file_same(void *context,const struct storage_node *a,const struct storage_node *b)
{ assert(context==&file_reads);return a->first_cluster==b->first_cluster; }
/** @brief Reject lookup on the anonymous fixture inode. @param context Cookie.
 * @param parent Directory. @param name Component. @param out Unused output.
 * @return ENOENT; the fixture opens its inode directly. */
static int file_lookup(void *context,const struct storage_node *parent,const char *name,struct storage_node *out)
{ (void)context;(void)parent;(void)name;(void)out;return -LINUX_ENOENT; }
static const struct vfs_backend_ops file_ops={.lookup=file_lookup,.read=read_now,.retain=file_retain,
    .release=file_release,.same_node=file_same};

/** @brief Exercise real PREAD64 and PREADV from the legacy front door.
 * @param argc Argument count. @param argv First argument is view page count.
 * @return Zero after contents, work bounds and owner retirement are checked. */
int main(int argc,char **argv)
{
    assert(argc==2);uint64_t pages_count=strtoull(argv[1],NULL,10);
    assert(pages_count==256 || pages_count==32768);
    device_length=pages_count*4096;backing_length=(pages_count==256 ? 32768ULL : 65536ULL)*4096;
    device_bytes=mmap(NULL,backing_length,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(device_bytes!=MAP_FAILED);device_base=(uintptr_t)device_bytes;
    kernel_pml4[511]=(uintptr_t)kernel_shared_pdpt|3;mmio_dynamic_tables_ready=true;
    setup(1);clear_setup_vmas();current->kind=TASK_KIND_USER;current->pid=37;
    current->stack_top=RELIEFNT_USER_TOP-4096;
    sched_task_limits(current)->as.rlim_cur=LINUX_RLIM_INFINITY;
    struct task_file device_file={.used=1,.flags=LINUX_O_RDWR};fix2_device_file=&device_file;
    assert(!character_device_register(&registration));
    assert(!character_file_open((uint64_t)221<<8,current,&device_file,&device_file.character_file));
    const uint64_t user=0x40000000ULL;fix2_device_user_start=user;fix2_device_user_end=user+device_length;
    device_mapping_test_visits=0;
    if(&device_mapping_test_tree_visits)device_mapping_test_tree_visits=0;
    assert(syscall_mm_mmap(user,device_length,3,LINUX_MAP_SHARED|LINUX_MAP_FIXED,3,0)==(int64_t)user);
    assert(policy_count()==pages_count); /* Do not reserve the unmapped backing tail. */
    uint64_t setup_visits=device_mapping_test_visits;
    uint64_t setup_tree=&device_mapping_test_tree_visits ? device_mapping_test_tree_visits : 0;
    struct task_file file={.used=1,.flags=LINUX_O_RDWR,
        .node={.type=RELIEFOS_FS_TYPE_FILE,.first_cluster=2}};
    struct storage_node root={.type=RELIEFOS_FS_TYPE_DIR};struct vfs_mount *mount=NULL;
    assert(!vfs_mount_new(&file_ops,&file_reads,NULL,&root,&mount));
    assert(!vfs_path_anonymous(mount,&file.node,&file.path_ref));vfs_mount_put(mount);
    fix2_device_file=&file;current->syscall_file=&file;
    current->syscall_file_number=LINUX_SYS_PREAD64;
    device_mapping_test_visits=0;
    int64_t full=syscall_legacy_io(LINUX_SYS_PREAD64,3,user,device_length,0,0,0);
    uint64_t full_visits=device_mapping_test_visits;
    assert(full==(int64_t)device_length && !alias_count && !current->regular_io.active);
    for(uint64_t i=0;i<device_length;++i)assert(device_bytes[i]==byte_at(i));
    unsigned full_reads=file_reads;assert(full_reads==device_length/32768);
    for(unsigned i=0;i<1024;++i)vectors[i]=(struct iovec){
        .iov_base=(void *)(uintptr_t)(user+(i&1 ? device_length-4096 : 0)),.iov_len=1};
    current->syscall_file_number=LINUX_SYS_PREADV;device_mapping_test_visits=0;
    int64_t vector=syscall_legacy_io(LINUX_SYS_PREADV,3,(uintptr_t)vectors,1024,0,0,0);
    uint64_t vector_visits=device_mapping_test_visits;
    assert(vector==1024 && file_reads==full_reads+1024 && !alias_count && !current->vector_io);
    assert(device_bytes[0]==byte_at(1022) && device_bytes[device_length-4096]==byte_at(1023));
    fprintf(stderr,"pages=%llu full_bytes=%lld full_reads=%u full_view_visits=%llu vector_bytes=%lld vector_reads=%u vector_view_visits=%llu setup_view_visits=%llu setup_index_visits=%llu\n",
        (unsigned long long)pages_count,(long long)full,full_reads,(unsigned long long)full_visits,
        (long long)vector,file_reads-full_reads,(unsigned long long)vector_visits,
        (unsigned long long)setup_visits,(unsigned long long)setup_tree);
    assert(full_visits<pages_count*80 && vector_visits<1024*80);
    if(&device_mapping_test_tree_visits)assert(setup_tree<pages_count*80);
    /* Submit a real pending ordinary read against the last eight indexed
     * pages. Model an unmap while the stage owns those aliases. */
    uint8_t pending_before[32768];memcpy(pending_before,device_bytes+device_length-32768,sizeof(pending_before));
    current->syscall_file_number=LINUX_SYS_PREAD64;stall_next=true;
    assert(syscall_legacy_io(LINUX_SYS_PREAD64,3,user+device_length-32768,32768,17,0,0)==-LINUX_EAGAIN);
    assert(pending_submitted && !pending_drained && alias_count==8 && current->regular_io.active);
    assert(regular_stage_find(current) && regular_stage_find(current)->pages==8);
    fix2_device_file=&device_file;device_mapping_test_visits=0;
    if(&device_mapping_test_tree_visits)device_mapping_test_tree_visits=0;
    assert(!syscall_mm_munmap(user,device_length));
    uint64_t retire_tree=&device_mapping_test_tree_visits ? device_mapping_test_tree_visits : 0;
    fprintf(stderr,"pages=%llu unmap_view_visits=%llu unmap_index_visits=%llu pending_claims=%u\n",
        (unsigned long long)pages_count,(unsigned long long)device_mapping_test_visits,
        (unsigned long long)retire_tree,policy_count());
    if(&device_mapping_test_tree_visits)assert(retire_tree<pages_count*80);
    assert(policy_count()==8 && alias_count==8 && !provider_releases);
    storage_drain_task_io(current->pid);task_regular_io_stage_cancel(current);
    assert(pending_drained && !alias_count && provider_releases==1 && !cache_claims);
    assert(!memcmp(pending_before,device_bytes+device_length-32768,sizeof(pending_before)));
    pending_submitted=false;current->regular_io.active=false;file.io_owner=0;
    vfs_path_put(&file.path_ref);
    character_file_close(device_file.character_file,&device_file);device_file.character_file=NULL;
    assert(!character_device_unregister(&registration));fix2_device_file=NULL;
    teardown();assert(!cache_claims);assert(!munmap(device_bytes,backing_length));
    puts("PASS positioned ordinary file and vector stage bounds, contents and final ownership");
    return 0;
}
