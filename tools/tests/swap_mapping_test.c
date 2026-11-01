/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <reliefnt/sched.h>
#include <reliefnt/page_cache.h>
#include <reliefnt/syscall.h>

extern int swap_file_check_in_use(struct storage_inode_ref *,const struct storage_node *) __attribute__((weak));
static struct task tasks[2];static struct task_vma vmas[2];static int cached;
/** @brief Borrow one synthetic task by ordinal. @param index Ordinal. @return Task or NULL. */
struct task *sched_task_at(uint32_t index) { return index<2 ? &tasks[index] : NULL; }
/** @brief Report one fixture VMA per task. @param task Task. @return Capacity. */
uint32_t sched_task_vma_capacity(const struct task *task) { (void)task;return 1; }
/** @brief Borrow a task's fixture VMA. @param task Task. @param index Index. @return VMA. */
struct task_vma *sched_task_vma_at(struct task *task,uint32_t index)
{ assert(index==0);return &vmas[task==&tasks[0] ? 0 : 1]; }
/** @brief Supply the cache conflict state. @param node File identity. @return Boolean. */
int page_cache_has_inode(const struct storage_node *node) { assert(node);return cached; }

/** @brief Existing file mappings, cache pages and suspended writers block admission.
 * @return Zero when all conflicts are recognized.
 */
int main(void)
{
    if(!swap_file_check_in_use){fputs("FAIL: mapping/cache admission is absent\n",stderr);return 1;}
    struct storage_inode_ref *inode=(void *)(uintptr_t)1;
    struct storage_node node={.type=RELIEFOS_FS_TYPE_FILE};
    assert(!swap_file_check_in_use(inode,&node));
    cached=1;assert(swap_file_check_in_use(inode,&node)==-EBUSY);cached=0;
    struct task_file writer={.inode=inode};
    tasks[0].syscall_file=&writer;tasks[0].syscall_file_number=LINUX_SYS_WRITE;
    assert(swap_file_check_in_use(inode,&node)==-EBUSY);
    tasks[0].syscall_file_number=LINUX_SYS_READ;
    assert(!swap_file_check_in_use(inode,&node));tasks[0].syscall_file=NULL;
    vmas[1]=(struct task_vma){.used=1,.flags=TASK_VMA_FLAG_FILE|TASK_VMA_FLAG_SHARED,.inode=inode};
    assert(swap_file_check_in_use(inode,&node)==-EBUSY);
    vmas[1].flags=TASK_VMA_FLAG_FILE|TASK_VMA_FLAG_PRIVATE;
    assert(!swap_file_check_in_use(inode,&node));
    vmas[1]=(struct task_vma){0};struct task_file file={.inode=inode};
    tasks[1].regular_io.active=true;tasks[1].syscall_file=&file;
    tasks[1].syscall_file_number=LINUX_SYS_WRITE;
    assert(swap_file_check_in_use(inode,&node)==-EBUSY);
    tasks[1].syscall_file_number=LINUX_SYS_READ;
    assert(!swap_file_check_in_use(inode,&node));
    puts("swap cache, mapping and pending-writer admission: ok");return 0;
}
