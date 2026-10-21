#include "execbuf_internal.h"

static void drm_vmwgfx_exec_submit_diag(
    struct drm_faildiag_context *diagnostics, int error, const char *stage,
    uint32_t source_bytes, uint32_t output_bytes, uint32_t output_capacity,
    const struct svga_batch_submit_failure *failure);

static void drm_vmwgfx_exec_fence_fail(
    struct drm_vmwgfx_exec_work *work,
    struct drm_faildiag_context *diagnostics, int error, const char *stage);

static void drm_vmwgfx_exec_submit_fail(
    struct drm_vmwgfx_exec_work *work,
    struct drm_faildiag_context *diagnostics, int error, const char *stage);

static int drm_vmwgfx_execbuf(struct task *task, struct drm_file *file,
                              uint32_t command, struct drm_vmw_execbuf_arg *arg,
                              uint64_t user_argument,
                              struct drm_faildiag_context *diagnostics);

static int64_t drm_vmwgfx_synccpu(
    struct task *task, struct drm_file *file, uint32_t command,
    struct drm_vmw_synccpu_arg *arg, uint64_t user_argument);

struct kernel_spinlock drm_vmwgfx_fence_lock = KERNEL_SPINLOCK_INIT;

uint32_t drm_vmwgfx_fence_count;

struct kernel_spinlock drm_vmwgfx_exec_lock = KERNEL_SPINLOCK_INIT;

struct drm_vmwgfx_task_continuation *drm_vmwgfx_continuation_cleanup;

uint32_t drm_vmwgfx_exec_work_count;

uint32_t drm_vmwgfx_exec_work_bytes;

uint32_t drm_vmwgfx_attachment_count;

uint64_t drm_vmwgfx_attachment_bytes;

void drm_vmwgfx_execdiag(
    struct drm_faildiag_context *diagnostics, int error, const char *stage,
    uint32_t source_bytes, uint32_t output_bytes, uint32_t output_capacity,
    uint32_t contexts, uint32_t surfaces, uint32_t bos)
{
    if (!drm_faildiag_is_enabled()) return;
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_vmwgfx_exec_lock, &flags);
    uint32_t work_count = drm_vmwgfx_exec_work_count;
    uint32_t work_bytes = drm_vmwgfx_exec_work_bytes;
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_exec_lock, flags);
    const struct drm_faildiag_field fields[] = {
        {"source", source_bytes},
        {"output", output_bytes},
        {"capacity", output_capacity},
        {"work_count", work_count},
        {"work_bytes", work_bytes},
        {"contexts", contexts},
        {"surfaces", surfaces},
        {"bos", bos},
    };
    drm_faildiag_record(diagnostics, error, stage, fields,
                        sizeof(fields) / sizeof(fields[0]));
}

static void drm_vmwgfx_exec_submit_diag(
    struct drm_faildiag_context *diagnostics, int error, const char *stage,
    uint32_t source_bytes, uint32_t output_bytes, uint32_t output_capacity,
    const struct svga_batch_submit_failure *failure)
{
    if (!drm_faildiag_is_enabled()) return;
    const struct drm_faildiag_field fields[] = {
        {"source", source_bytes},
        {"output", output_bytes},
        {"capacity", output_capacity},
        {"reason", failure ? failure->reason : 0},
        {"index", failure ? failure->index : SVGA_BATCH_SUBMIT_INDEX_NONE},
        {"detail0", failure ? failure->detail0 : 0},
        {"detail1", failure ? failure->detail1 : 0},
        {"detail2", failure ? failure->detail2 : 0},
    };
    drm_faildiag_record(diagnostics, error, stage, fields,
                        sizeof(fields) / sizeof(fields[0]));
}

void drm_vmwgfx_attachment_diag(
    struct drm_faildiag_context *diagnostics, int error, const char *stage,
    uint32_t source_bytes, uint32_t output_capacity, uint32_t output_bytes,
    uint32_t bo_refs)
{
    if (!drm_faildiag_is_enabled()) return;
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_vmwgfx_exec_lock, &flags);
    uint32_t work_count = drm_vmwgfx_exec_work_count;
    uint32_t work_bytes = drm_vmwgfx_exec_work_bytes;
    uint32_t attachment_count = drm_vmwgfx_attachment_count;
    uint64_t attachment_bytes = drm_vmwgfx_attachment_bytes;
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_exec_lock, flags);
    const struct drm_faildiag_field fields[] = {
        {"source", source_bytes},
        {"output_capacity", output_capacity},
        {"output", output_bytes},
        {"work_count", work_count},
        {"work_bytes", work_bytes},
        {"attachments", attachment_count},
        {"attachment_bytes", attachment_bytes},
        {"bo_refs", bo_refs},
    };
    drm_faildiag_record(diagnostics, error, stage, fields,
                        sizeof(fields) / sizeof(fields[0]));
}

int drm_vmwgfx_map_svga_status(int status)
{
    switch (status) {
    case 0: return 0;
    case SVGA_EBUSY: return -LINUX_EBUSY;
    case SVGA_ENOMEM: return -LINUX_ENOMEM;
    case SVGA_ENODEV: return -LINUX_ENODEV;
    case SVGA_ENOTSUP: return -LINUX_EOPNOTSUPP;
    case SVGA_ETIMEDOUT: return -LINUX_ETIMEDOUT;
    case SVGA_EINVAL: return -LINUX_EINVAL;
    default: return -LINUX_EIO;
    }
}

static void drm_vmwgfx_exec_fence_fail(
    struct drm_vmwgfx_exec_work *work,
    struct drm_faildiag_context *diagnostics, int error, const char *stage)
{
    if (!work) return;
    uint32_t source_bytes = work->source_bytes;
    uint32_t output_bytes = work->batch.bytes;
    uint32_t output_capacity = work->output_capacity;
    drm_vmwgfx_exec_work_release(work);
    drm_vmwgfx_fence_diag(diagnostics, error, stage, source_bytes,
        output_bytes, output_capacity);
}

static void drm_vmwgfx_exec_submit_fail(
    struct drm_vmwgfx_exec_work *work,
    struct drm_faildiag_context *diagnostics, int error, const char *stage)
{
    if (!work) return;
    uint32_t source_bytes = work->source_bytes;
    uint32_t output_bytes = work->batch.bytes;
    uint32_t output_capacity = work->output_capacity;
    struct svga_batch_submit_failure failure = work->batch.submit_failure;
    drm_vmwgfx_exec_work_release(work);
    drm_vmwgfx_exec_submit_diag(diagnostics, error, stage, source_bytes,
        output_bytes, output_capacity, &failure);
}

static int drm_vmwgfx_execbuf(struct task *task, struct drm_file *file,
                              uint32_t command, struct drm_vmw_execbuf_arg *arg,
                              uint64_t user_argument,
                              struct drm_faildiag_context *diagnostics)
{
    uint32_t arg_size = _IOC_SIZE(command);
    if (_IOC_DIR(command) != _IOC_WRITE ||
        (arg_size != sizeof(*arg) && arg_size != DRM_VMW_EXECBUF_V1_SHORT_SIZE)) {
        const struct drm_faildiag_field fields[] = {
            {"argument_bytes", arg_size},
            {"source", arg->command_size},
        };
        drm_faildiag_record(diagnostics, -LINUX_EINVAL, "exec.argument",
                            fields, sizeof(fields) / sizeof(fields[0]));
        return -LINUX_EINVAL;
    }
    if (arg->version != 1u || arg->flags != 0u ||
        arg->command_size > DRM_VMW_EXECBUF_MAX_COMMAND_BYTES ||
        (arg->command_size && !arg->commands)) {
        const struct drm_faildiag_field fields[] = {
            {"argument_bytes", arg_size},
            {"source", arg->command_size},
            {"source_limit", DRM_VMW_EXECBUF_MAX_COMMAND_BYTES},
        };
        drm_faildiag_record(diagnostics, -LINUX_EINVAL, "exec.argument",
                            fields, sizeof(fields) / sizeof(fields[0]));
        return -LINUX_EINVAL;
    }

    /* Linux 6.12 ignores this field for every version-1 payload, including
     * the full-size encoding. Keep the local copy canonical as well. */
    arg->context_handle = SVGA3D_INVALID_ID;
    int allocation_error = 0;
    struct drm_vmwgfx_exec_work *work = drm_vmwgfx_exec_work_alloc(
        arg->command_size, 0, &allocation_error, diagnostics);
    if (!work) return allocation_error;
    if (arg->command_size &&
        user_copy_from_task(task, work->snapshot, arg->commands,
                            arg->command_size) < 0) {
        drm_vmwgfx_exec_work_fail(work, diagnostics, -LINUX_EFAULT,
                                  "exec.input_copy");
        return -LINUX_EFAULT;
    }

    int status = 0;
    struct drm_vmwgfx_exec_walk walk = {.work = work, .file = file};
    if (arg->command_size) {
        status = drm_vmwgfx_validate_stream(work->snapshot,
                                             arg->command_size,
                                             &work->packet_count);
        if (status) {
            drm_vmwgfx_exec_work_fail(work, diagnostics, status,
                                      "exec.stream");
            return status;
        }
        status = drm_vmwgfx_walk_stream(work->snapshot,
            arg->command_size, &work->packet_count,
            drm_vmwgfx_exec_validate_packet, &walk);
        if (status) {
            const char *stage = status == -LINUX_ENOMEM
                ? "exec.references" : "exec.validation";
            drm_vmwgfx_exec_work_fail(work, diagnostics, status, stage);
            return status;
        }

        for (uint32_t i = 0; i < work->bo_count; ++i) {
            const char *failure_stage = NULL;
            status = drm_vmwgfx_exec_attachment_get(work->bos[i].object,
                &work->bos[i].attachment, &failure_stage);
            if (status) {
                drm_vmwgfx_exec_attachment_fail(work, diagnostics, status,
                    failure_stage ? failure_stage : "exec.attach.backend");
                return status;
            }
        }

        struct svga_info info;
        svga_get_info(&info);
        if (!info.available || !info.fifo_ready) {
            drm_vmwgfx_exec_work_fail(work, diagnostics, -LINUX_ENODEV,
                                      "exec.device");
            return -LINUX_ENODEV;
        }
        status = drm_vmwgfx_map_svga_status(svga_batch_init_sized(
            &work->batch, info.generation, work->output,
            work->output_capacity, work->source_bytes));
        if (status) {
            drm_vmwgfx_exec_work_fail(work, diagnostics, status,
                                      "exec.batch_init");
            return status;
        }
        work->batch_open = true;
        for (uint32_t i = 0; i < work->context_count && !status; ++i)
            status = drm_vmwgfx_map_svga_status(svga_batch_retain_context(
                &work->batch, work->contexts[i].backend));
        for (uint32_t i = 0; i < work->surface_count && !status; ++i)
            status = drm_vmwgfx_map_svga_status(svga_batch_retain_surface(
                &work->batch, work->surfaces[i].backend));
        for (uint32_t i = 0; i < work->bo_count && !status; ++i) {
            if (!work->bos[i].attachment || !work->bos[i].attachment->ready)
                status = -LINUX_EBUSY;
            else
                status = drm_vmwgfx_map_svga_status(svga_batch_retain_gmr(
                    &work->batch, work->bos[i].attachment->gmr));
        }
        if (status) {
            drm_vmwgfx_exec_work_fail(work, diagnostics, status,
                                      "exec.batch_retain");
            return status;
        }
        status = drm_vmwgfx_walk_stream(work->snapshot,
            arg->command_size, &work->packet_count,
            drm_vmwgfx_exec_translate_packet, &walk);
        if (status) {
            drm_vmwgfx_exec_work_fail(work, diagnostics, status,
                                      "exec.translation");
            return status;
        }
    }

    const char *fence_failure_stage = NULL;
    struct drm_vmwgfx_fence *fence = drm_vmwgfx_fence_reserve(
        file, arg->fence_rep != 0, &fence_failure_stage);
    if (!fence) {
        drm_vmwgfx_exec_fence_fail(work, diagnostics, -LINUX_ENOMEM,
            fence_failure_stage ? fence_failure_stage : "exec.fence.heap");
        return -LINUX_ENOMEM;
    }
    work->fence_object = fence;
    struct drm_vmwgfx_task_continuation *fault_wait = NULL;
    if (arg->fence_rep) {
        fault_wait = kernel_malloc(sizeof(*fault_wait));
        if (!fault_wait) {
            drm_vmwgfx_exec_fence_fail(work, diagnostics, -LINUX_ENOMEM,
                                      "exec.fence.reply_heap");
            return -LINUX_ENOMEM;
        }
        *fault_wait = (struct drm_vmwgfx_task_continuation){0};
    }

    struct svga_fence_progress before = {0};
    (void)svga_fence_progress_snapshot(&before);
    svga_handle token = 0;
    status = arg->command_size
        ? drm_vmwgfx_map_svga_status(svga_batch_submit(&work->batch, &token))
        : drm_vmwgfx_map_svga_status(svga_fence_try_insert(&token));
    if (status) {
        kernel_free(fault_wait);
        if (arg->command_size)
            drm_vmwgfx_exec_submit_fail(work, diagnostics, status,
                                        "exec.fifo_submit");
        else
            drm_vmwgfx_exec_fence_fail(work, diagnostics, status,
                                       "exec.fifo_submit");
        return status;
    }
    work->batch_open = false;
    work->submitted = true;
    work->fence = token;
    fence->token = token;
    drm_vmwgfx_fence_publish(file, fence);
    uint64_t list_flags;
    kernel_spin_lock_irqsave(&drm_vmwgfx_exec_lock, &list_flags);
    drm_vmwgfx_exec_job_enqueue_locked(work);
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_exec_lock, list_flags);

    if (!arg->fence_rep) return 0;
    struct svga_fence_progress progress = before;
    if (!svga_fence_progress_snapshot(&progress)) {
        /* A successful snapshot is actual progress; if the try-lock is busy,
         * the pre-submit snapshot remains a conservative valid sequence. */
    }
    struct drm_vmw_fence_rep result = {
        .handle = fence->handle,
        .mask = 0,
        .seqno = (uint32_t)token,
        .passed_seqno = progress.passed_seq,
        .fd = -1,
        .error = 0,
    };
    if (user_copy_to_task(task, arg->fence_rep, &result, sizeof(result)) < 0) {
        (void)drm_vmwgfx_fence_remove(file, fence->handle);
        drm_vmwgfx_fence_get(fence);
        fault_wait->kind = DRM_VMW_CONT_FENCE_REPLY_FAULT;
        fault_wait->fence = token;
        fault_wait->fence_object = fence;
        fault_wait->deadline = time_ticks() +
                               5u * RELIEFNT_TICK_HZ;
        drm_vmwgfx_continuation_set_key(fault_wait, task, file, command,
                                         user_argument);
        task->drm_vmwgfx_continuation = fault_wait;
        sched_sleep_current_until(time_ticks() + 1u);
        return KERNEL_SYSCALL_BLOCKED;
    }
    kernel_free(fault_wait);
    return 0;
}

static int64_t drm_vmwgfx_synccpu(
    struct task *task, struct drm_file *file, uint32_t command,
    struct drm_vmw_synccpu_arg *arg, uint64_t user_argument)
{
    if (arg->op == drm_vmw_synccpu_release)
        return drm_bo_synccpu_ioctl(file, arg);

    struct drm_vmwgfx_task_continuation *continuation = task
        ? task->drm_vmwgfx_continuation : NULL;
    bool resumed = continuation &&
        continuation->kind == DRM_VMW_CONT_SYNCCPU &&
        drm_vmwgfx_continuation_matches(continuation, task,
            task->syscall_file, file, command, user_argument);
    if (continuation && !resumed) {
        drm_vmwgfx_cancel_task_continuation(task);
        continuation = NULL;
    }

    struct drm_bo *bo = NULL;
    if (resumed) {
        *arg = continuation->snapshot.synccpu;
        bo = continuation->bo;
    } else {
        int status = drm_bo_synccpu_lookup_get(file, arg->handle,
                                                arg->flags, &bo);
        if (status) return status;
    }

    bool busy = drm_bo_gpu_busy(bo);
    if (!busy) {
        int status = drm_bo_synccpu_grab_retained(file, arg->handle, bo,
                                                  arg->flags);
        if (!status) {
            if (resumed) drm_vmwgfx_fence_wait_finish(task, continuation);
            else drm_bo_put(bo);
            return 0;
        }
        if (status != -LINUX_EBUSY) {
            if (resumed) drm_vmwgfx_fence_wait_finish(task, continuation);
            else drm_bo_put(bo);
            return status;
        }
        busy = true;
    }
    if (arg->flags & drm_vmw_synccpu_dontblock) {
        if (resumed) drm_vmwgfx_fence_wait_finish(task, continuation);
        else drm_bo_put(bo);
        return -LINUX_EBUSY;
    }

    if (!resumed) {
        continuation = kernel_malloc(sizeof(*continuation));
        if (!continuation) {
            drm_bo_put(bo);
            return -LINUX_ENOMEM;
        }
        *continuation = (struct drm_vmwgfx_task_continuation){
            .kind = DRM_VMW_CONT_SYNCCPU,
            .bo = bo,
            .snapshot_bytes = sizeof(*arg),
        };
        drm_vmwgfx_continuation_set_key(continuation, task, file, command,
                                         user_argument);
        task->drm_vmwgfx_continuation = continuation;
    }
    continuation->snapshot.synccpu = *arg;
    sched_sleep_current_until(time_ticks() + 1u);
    return KERNEL_SYSCALL_BLOCKED;
}

int64_t drm_vmwgfx_execbuf_ioctl(struct task *task, struct drm_file *file,
                                 uint32_t command, void *data,
                                 uint64_t user_argument,
                                 struct drm_faildiag_context *diagnostics)
{
    if (!task || !file || !data) return -LINUX_EFAULT;
    if (_IOC_NR(command) < DRM_COMMAND_BASE) return -LINUX_EINVAL;
    switch (_IOC_NR(command) - DRM_COMMAND_BASE) {
    case DRM_VMW_EXECBUF:
        return drm_vmwgfx_execbuf(task, file, command, data, user_argument,
                                  diagnostics);
    case DRM_VMW_FENCE_SIGNALED:
        if (_IOC_DIR(command) != (_IOC_READ | _IOC_WRITE) ||
            _IOC_SIZE(command) != sizeof(struct drm_vmw_fence_signaled_arg))
            return -LINUX_EINVAL;
        return drm_vmwgfx_fence_signaled(file, data);
    case DRM_VMW_FENCE_WAIT:
        if (_IOC_DIR(command) != (_IOC_READ | _IOC_WRITE) ||
            _IOC_SIZE(command) != sizeof(struct drm_vmw_fence_wait_arg))
            return -LINUX_EINVAL;
        return drm_vmwgfx_fence_wait(task, file, command, data,
                                     user_argument, diagnostics);
    case DRM_VMW_FENCE_UNREF: {
        struct drm_vmw_fence_arg *arg = data;
        if (_IOC_DIR(command) != _IOC_WRITE ||
            _IOC_SIZE(command) != sizeof(*arg)) return -LINUX_EINVAL;
        return drm_vmwgfx_fence_remove(file, arg->handle) ? 0 : -LINUX_EINVAL;
    }
    case DRM_VMW_SYNCCPU:
        if (_IOC_DIR(command) != _IOC_WRITE ||
            _IOC_SIZE(command) != sizeof(struct drm_vmw_synccpu_arg))
            return -LINUX_EINVAL;
        return drm_vmwgfx_synccpu(task, file, command, data, user_argument);
    default:
        return -LINUX_EINVAL;
    }
}

void drm_vmwgfx_execbuf_file_release(struct drm_file *file)
{
    if (!file) return;
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_vmwgfx_fence_lock, &flags);
    struct drm_vmwgfx_fence *fences =
        (struct drm_vmwgfx_fence *)file->vmwgfx_fences;
    file->vmwgfx_fences = NULL;
    struct drm_vmwgfx_fence *free_list = NULL;
    while (fences) {
        struct drm_vmwgfx_fence *next = fences->next;
        fences->next = NULL;
        fences->user_linked = false;
        if (fences->references && --fences->references == 0) {
            if (drm_vmwgfx_fence_count) --drm_vmwgfx_fence_count;
            fences->next = free_list;
            free_list = fences;
        }
        fences = next;
    }
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_fence_lock, flags);
    while (free_list) {
        struct drm_vmwgfx_fence *next = free_list->next;
        kernel_free(free_list);
        free_list = next;
    }
}

void drm_vmwgfx_cancel_task_continuation(struct task *task)
{
    if (!task) return;
    struct drm_vmwgfx_task_continuation *continuation =
        task->drm_vmwgfx_continuation;
    if (!continuation) return;
    task->drm_vmwgfx_continuation = NULL;
    drm_vmwgfx_continuation_queue_release(continuation);
}
