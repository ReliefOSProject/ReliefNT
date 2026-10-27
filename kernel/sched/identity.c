/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

/* Invalidate the target before its task record can be recycled. Keeping this
 * allocation alive, rather than comparing timestamps or a wrapping PID, lets
 * an old proc inode distinguish a dead process from a reused task slot. */
struct task_identity {struct task *task;uint64_t references;};

/** @brief Acquire an independent stable PID-object reference for proc lookup.
 * @param task Borrowed live task or waitable zombie under execution ownership.
 * @param out Empty destination receiving one reference, empty on failure.
 * @return Zero or negative EINVAL/ESRCH/ENOMEM. Lazy allocation runs without
 * scheduler_lock, and publishes only a completely initialized identity.
 */
int sched_task_identity_get(struct task *task,struct task_identity **out)
{
    if(!out)return -EINVAL;
    *out=NULL;
    if(!task || !task->pid || (task->state==TASK_EXITED &&
        !(task->flags&TASK_FLAG_WAITABLE_CHILD)))return -ESRCH;
    if(!task->identity_token) {
        struct task_identity *identity=kernel_malloc(sizeof(*identity));
        if(!identity)return -ENOMEM;
        *identity=(struct task_identity){.task=task,.references=1};task->identity_token=identity;
    }
    *out=task->identity_token;++(*out)->references;return 0;
}

/** @brief Resolve a retained PID object while task records are serialized.
 * @param identity Borrowed retained identity, nullable.
 * @return Borrowed task or NULL after identity invalidation; no new references.
 */
struct task *sched_task_identity_task(const struct task_identity *identity)
{return identity ? identity->task : NULL;}

/** @brief Drop one owned identity reference, freeing only the final owner.
 * @param identity Owned identity or NULL, serialized by execution ownership
 * or scheduler_lock. Performs no backend calls or allocation.
 */
void sched_task_identity_put(struct task_identity *identity)
{if(identity && !--identity->references)kernel_free(identity);}

/** @brief Disconnect a recycled/reaped task from every previously held proc inode.
 * @param task Stable task under execution ownership or scheduler_lock, nullable.
 * Clears ownership and the target before releasing the task's reference. No I/O.
 */
void sched_task_identity_detach(struct task *task)
{
    if(!task || !task->identity_token)return;
    struct task_identity *identity=task->identity_token;task->identity_token=NULL;
    identity->task=NULL;sched_task_identity_put(identity);
}

/** @brief Exchange identity targets with the PIDs of a de-threading exec.
 * @param left Stable task under scheduler_lock, required.
 * @param right Stable task under scheduler_lock, required.
 * No references are created/dropped; existing holders follow their same PID.
 */
void sched_task_identity_swap(struct task *left,struct task *right)
{
    struct task_identity *old=left->identity_token;
    left->identity_token=right->identity_token;right->identity_token=old;
    if(left->identity_token)left->identity_token->task=left;
    if(right->identity_token)right->identity_token->task=right;
}
