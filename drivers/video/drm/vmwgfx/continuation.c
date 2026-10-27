#include "../memory.h"
#include "execbuf_internal.h"

static int64_t drm_vmwgfx_resume_continuation(
    struct task *task, struct task_file *syscall_file, struct drm_file *file,
    uint32_t command, uint64_t user_argument, void *data, uint32_t data_bytes,
    bool *handled, bool *skip_input);


bool drm_vmwgfx_continuation_matches(
    const struct drm_vmwgfx_task_continuation *continuation,
    const struct task *task, const struct task_file *syscall_file,
    const struct drm_file *file, uint32_t command, uint64_t user_argument)
{
    return continuation && task && continuation->syscall_file == syscall_file &&
        continuation->syscall_fd == task->syscall_fd &&
        continuation->syscall_number == task->syscall_file_number &&
        continuation->file == file && continuation->command == command &&
        continuation->user_argument == user_argument;
}

void drm_vmwgfx_continuation_set_key(
    struct drm_vmwgfx_task_continuation *continuation,
    struct task *task, struct drm_file *file, uint32_t command,
    uint64_t user_argument)
{
    continuation->file = file;
    continuation->syscall_file = task->syscall_file;
    continuation->syscall_fd = task->syscall_fd;
    continuation->syscall_number = task->syscall_file_number;
    continuation->command = command;
    continuation->user_argument = user_argument;
}

/**
 * @brief Consume a detached continuation after dropping its retained BO and fence.
 * @param continuation Owned asynchronous allocation or NULL.
 * Execution-owned task context under independent file/service pin; no queue lock spans final backing, heap or module release.
 */
void drm_vmwgfx_continuation_release(
    struct drm_vmwgfx_task_continuation *continuation)
{
    if (!continuation) return;
    drm_bo_put(continuation->bo);
    drm_vmwgfx_fence_put(continuation->fence_object);
    drm_async_free(continuation);
}

void drm_vmwgfx_continuation_queue_release(
    struct drm_vmwgfx_task_continuation *continuation)
{
    if (!continuation) return;
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_vmwgfx_exec_lock, &flags);
    continuation->next = drm_vmwgfx_continuation_cleanup;
    drm_vmwgfx_continuation_cleanup = continuation;
    drm_vmwgfx_exec_publish_maintenance_locked();
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_exec_lock, flags);
}

static int64_t drm_vmwgfx_resume_continuation(
    struct task *task, struct task_file *syscall_file, struct drm_file *file,
    uint32_t command, uint64_t user_argument, void *data, uint32_t data_bytes,
    bool *handled, bool *skip_input)
{
    if (handled) *handled = false;
    if (skip_input) *skip_input = false;
    struct drm_vmwgfx_task_continuation *continuation = task
        ? task->drm_vmwgfx_continuation : NULL;
    if (!continuation) return 0;
    if (!drm_vmwgfx_continuation_matches(continuation, task, syscall_file,
            file, command, user_argument)) {
        drm_vmwgfx_cancel_task_continuation(task);
        return 0;
    }
    if (continuation->kind == DRM_VMW_CONT_FENCE_REPLY_FAULT) {
        bool signaled = false;
        int status = drm_vmwgfx_map_svga_status(
            svga_fence_status(continuation->fence, &signaled));
        if (status == -LINUX_EBUSY) status = 0;
        if (status == -LINUX_EIO) {
            struct svga_fence_progress progress;
            if (!svga_fence_progress_snapshot(&progress) &&
                progress.generation != (uint32_t)(continuation->fence >> 32))
                signaled = true;
        }
        if (signaled || status < 0 || time_ticks() >= continuation->deadline) {
            task->drm_vmwgfx_continuation = NULL;
            drm_vmwgfx_continuation_release(continuation);
            if (handled) *handled = true;
            return -LINUX_EFAULT;
        }
        sched_sleep_current_until(time_ticks() + 1u);
        if (handled) *handled = true;
        return KERNEL_SYSCALL_BLOCKED;
    }
    if (!data || !skip_input || continuation->snapshot_bytes > data_bytes) {
        drm_vmwgfx_cancel_task_continuation(task);
        if (handled) *handled = true;
        return -LINUX_EFAULT;
    }
    if (continuation->kind == DRM_VMW_CONT_FENCE_WAIT)
        __builtin_memcpy(data, &continuation->snapshot.wait,
                         sizeof(continuation->snapshot.wait));
    else if (continuation->kind == DRM_VMW_CONT_SYNCCPU)
        __builtin_memcpy(data, &continuation->snapshot.synccpu,
                         sizeof(continuation->snapshot.synccpu));
    else {
        drm_vmwgfx_cancel_task_continuation(task);
        if (handled) *handled = true;
        return -LINUX_EINVAL;
    }
    *skip_input = true;
    return 0;
}

int64_t drm_vmwgfx_prepare_ioctl_continuation(
    struct task *task, struct task_file *syscall_file, struct drm_file *file,
    uint32_t command, uint64_t user_argument, void *data, uint32_t data_bytes,
    bool *handled, bool *skip_input)
{
    return drm_vmwgfx_resume_continuation(task, syscall_file, file, command,
        user_argument, data, data_bytes, handled, skip_input);
}

bool drm_vmwgfx_task_uninterruptible(const struct task *task)
{
    const struct drm_vmwgfx_task_continuation *continuation = task
        ? task->drm_vmwgfx_continuation : NULL;
    return continuation &&
        continuation->kind == DRM_VMW_CONT_FENCE_REPLY_FAULT;
}

bool drm_vmwgfx_task_signal_busy(const struct task *task)
{
    const struct drm_vmwgfx_task_continuation *continuation = task
        ? task->drm_vmwgfx_continuation : NULL;
    return continuation && continuation->kind == DRM_VMW_CONT_SYNCCPU;
}
