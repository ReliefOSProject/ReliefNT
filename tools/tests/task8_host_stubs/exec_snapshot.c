/* SPDX-License-Identifier: Apache-2.0 */
/* No provider, wait queue or process_vm state is owned by this exec import. */
#include <reliefnt/syscall_internal.h>
#include <reliefnt/sched.h>
#include <reliefnt/mount.h>
#include <stdlib.h>
#include <assert.h>
#include <reliefnt/usercopy.h>
#include <linux/errno.h>
int user_range_readable(uint64_t address,uint64_t length)
{ return user_range_ok(address,length) ? 0 : -EFAULT; }
const struct storage_node *vfs_path_node(const struct vfs_path *path)
{ (void)path;abort(); }
void vfs_path_get(const struct vfs_path *path,struct vfs_path *out)
{ (void)path;(void)out;abort(); }
void storage_drain_task_io(uint32_t pid){assert(pid==42);}
void task_process_vm_scope_release(void *state){(void)state;abort();}
void kernel_wait_queue_remove(struct kernel_wait_queue *q,struct task *t){(void)q;(void)t;abort();}
void vfs_walk_cancel(struct vfs_walk **w){assert(!*w);}
void vfs_path_put(struct vfs_path *p){assert(!p->mount);}
void storage_inode_allow_write(struct storage_inode_ref *i){(void)i;abort();}
int storage_inode_put(struct storage_inode_ref *i){(void)i;abort();}
