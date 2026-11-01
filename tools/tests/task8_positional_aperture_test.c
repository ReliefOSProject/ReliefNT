/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Exercise positional admission before the real regular/block engine.
 * Provider callbacks fail on first use, exposing any premature operation.
 */
#define main task8_inherited_regular_main
#include "regular_file_io_test.c"
#undef main
#include "../../kernel/syscall/io/legacy_io.c"

static struct task current;
static struct task_file admitted;
struct task *sched_current_task(void) { return &current; }
struct task_file *task_file_for_io(struct task *task,int fd)
{ assert(task==&current); return fd==3 ? &admitted : NULL; }
int file_can_read(const struct task_file *file) { (void)file; return 1; }
int file_can_write(const struct task_file *file) { (void)file; return 1; }
#ifdef CONFIG_SWAP
int swap_prepare_user_range(struct task *task,uint64_t address,uint64_t length,bool writing)
{ (void)task;(void)address;(void)length;(void)writing;abort(); }
#endif

int main(void)
{
    struct storage_node root={.type=RELIEFOS_FS_TYPE_DIR};
    struct vfs_mount *mount=NULL;
    current.pid=10;current.kind=TASK_KIND_KERNEL;
    admitted=(struct task_file){.used=1,.flags=RELIEFOS_O_RDWR,
        .node={.type=RELIEFOS_FS_TYPE_FILE,.first_cluster=2}};
    assert(!vfs_mount_new(&provider_ops,disk,NULL,&root,&mount));
    assert(!vfs_path_anonymous(mount,&admitted.node,&admitted.path_ref));
    vfs_mount_put(mount);expected_cursor=&admitted.read_cursor;
    const uint64_t bases[]={RELIEFNT_USER_TOP-32768u,RELIEFNT_USER_TOP-0x7ffff000u,UINT64_MAX-3};
    const uint64_t sizes[]={32769u,0x7ffff001u,8u};
    for(unsigned block=0;block<2;++block) {
        if(block) {
            admitted.flags|=TASK_FILE_FLAG_DEV_BLOCK|TASK_FILE_FLAG_DEV_NODE;
            admitted.node.flags=STORAGE_NODE_FLAG_DEV_BLOCK|STORAGE_NODE_FLAG_DEV_NODE;
            admitted.node.first_cluster=STORAGE_DEV_KIND_DISK;
        }
        for(unsigned writing=0;writing<2;++writing)for(unsigned i=0;i<3;++i) {
            error_offset=0;injected_error=-LINUX_EAGAIN;
            unsigned reads=provider_reads,writes=provider_writes;
            int64_t result=syscall_legacy_io(writing?LINUX_SYS_PWRITE64:LINUX_SYS_PREAD64,
                                             3,bases[i],sizes[i],0,0,0);
            assert(result==-LINUX_EFAULT);
            assert(error_offset==0 && provider_reads==reads && provider_writes==writes);
        }
    }
    vfs_path_put(&admitted.path_ref);assert(!provider_pins);
    puts("PASS positional regular/block original aperture and no provider effect");
    return 0;
}
