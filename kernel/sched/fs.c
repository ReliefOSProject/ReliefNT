/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

/** @brief Attach initial shared-namespace root/cwd to an unpublished task.
 * @param task New task under execution ownership, not yet runnable.
 * @param ns Published namespace, borrowed and never retained as a bare pointer.
 * @return Zero, EINVAL for partial state or ENODEV without a namespace. Existing
 * fully inherited paths remain unchanged. Only mount/dentry reference counts
 * change: no allocation, I/O, provider callback or scheduler-lock inversion.
 */
int sched_task_fs_start(struct task *task,struct vfs_namespace *ns)
{
    if(!task)return -22;
    struct task_fs_paths *paths=sched_task_paths(task);
    bool root=vfs_path_node(&paths->root)!=NULL,cwd=vfs_path_node(&paths->cwd)!=NULL;
    if(root && cwd)return 0;
    if(root || cwd)return -22;
    if(!ns || !ns->root)return -19;
    struct vfs_path initial={0};int rc=vfs_namespace_root(ns,&initial);if(rc)return rc;
    paths->root=initial;vfs_path_get(&initial,&paths->cwd);return 0;
}

/** @brief Retain independent root/cwd objects, including covered or removed dirs.
 * @param task New task with private filesystem state; execution-owned.
 * @param parent Stable source task, borrowed, including shared CLONE_FS state.
 * Acquires both pins before releasing old destination handles. No allocation;
 * source lifetime remains independent of child creation or later rollback.
 */
void sched_task_fs_inherit(struct task *task,const struct task *parent)
{
    struct task_fs_paths next={0},*source=sched_task_paths(parent),*target=sched_task_paths(task);
    vfs_path_get(&source->root,&next.root);vfs_path_get(&source->cwd,&next.cwd);
    struct task_fs_paths old=*target;*target=next;
    vfs_path_put(&old.root);vfs_path_put(&old.cwd);
}

/** @brief Check whether a separate process can change a task's CLONE_FS state.
 * @param task Current task under exclusive execution ownership; required.
 * @return True if any filesystem-state reference is held outside the task's
 * thread group. Borrows the scheduler task entries under execution ownership.
 */
bool sched_task_fs_shared_outside_group(const struct task *task)
{
    const struct task_fs_state *shared = task->shared_fs;
    if (!shared || shared->references == 1) return false;

    uint32_t group = sched_task_tgid(task);
    for (uint32_t i = 0; i < task_count; ++i) {
        const struct task *peer = tasks[i];
        if (peer->shared_fs == shared && sched_task_tgid(peer) != group)
            return true;
    }
    return false;
}

/** @brief Release the task's root/cwd ownership exactly once, honoring CLONE_FS.
 * @param task Quiescent task under execution ownership, or NULL.
 * Detaches state before dropping final backend paths, which may perform I/O.
 * Nonfinal shared holders only drop their state reference; no paths are freed.
 */
void sched_task_fs_release(struct task *task)
{
    if(!task)return;
    struct task_fs_state *shared=task->shared_fs;task->shared_fs=NULL;
    if(shared && __atomic_sub_fetch(&shared->references,1,__ATOMIC_SEQ_CST))return;
    struct task_fs_paths *paths=shared ? &shared->paths : &task->fs_paths;
    struct task_fs_paths old=*paths;*paths=(struct task_fs_paths){0};
    vfs_path_put(&old.root);vfs_path_put(&old.cwd);
    if(shared)kernel_free(shared);
}
