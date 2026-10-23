#include "execbuf_internal.h"

static void drm_vmwgfx_exec_attachment_owner_put(void *opaque);

static int drm_vmwgfx_exec_attachment_progress(
    struct drm_vmwgfx_attachment *attachment);

static void drm_vmwgfx_exec_queue_cleanup(struct drm_vmwgfx_exec_work *work);

static void drm_vmwgfx_exec_attachment_put(
    struct drm_vmwgfx_attachment *attachment);

static void drm_vmwgfx_exec_work_alloc_diag(
    struct drm_faildiag_context *diagnostics, int error, const char *stage,
    uint32_t source_bytes, uint32_t output_capacity, uint64_t requested);

static struct drm_vmwgfx_attachment *drm_vmwgfx_attachments;
static struct drm_vmwgfx_exec_work *drm_vmwgfx_exec_jobs;
static struct drm_vmwgfx_exec_work *drm_vmwgfx_exec_jobs_tail;
static struct drm_vmwgfx_exec_work *drm_vmwgfx_exec_cleanup;
static uint32_t drm_vmwgfx_attachment_progress_cursor;
static bool drm_vmwgfx_exec_maintenance_pending;
void drm_vmwgfx_exec_publish_maintenance_locked(void)
{
    bool pending = drm_vmwgfx_exec_work_count || drm_vmwgfx_attachment_count ||
                   drm_vmwgfx_continuation_cleanup;
    __atomic_store_n(&drm_vmwgfx_exec_maintenance_pending, pending,
                     __ATOMIC_RELEASE);
}

void drm_vmwgfx_exec_job_enqueue_locked(
    struct drm_vmwgfx_exec_work *work)
{
    work->next = NULL;
    if (drm_vmwgfx_exec_jobs_tail)
        drm_vmwgfx_exec_jobs_tail->next = work;
    else
        drm_vmwgfx_exec_jobs = work;
    drm_vmwgfx_exec_jobs_tail = work;
}

static void drm_vmwgfx_exec_attachment_owner_put(void *opaque)
{
    struct drm_vmwgfx_attachment *attachment = opaque;
    if (!attachment) return;
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_vmwgfx_exec_lock, &flags);
    struct drm_vmwgfx_attachment **link = &drm_vmwgfx_attachments;
    while (*link && *link != attachment) link = &(*link)->next;
    if (*link == attachment) {
        *link = attachment->next;
        if (drm_vmwgfx_attachment_count) --drm_vmwgfx_attachment_count;
        drm_vmwgfx_exec_publish_maintenance_locked();
        if (drm_vmwgfx_attachment_bytes >= attachment->bytes)
            drm_vmwgfx_attachment_bytes -= attachment->bytes;
        else
            drm_vmwgfx_attachment_bytes = 0;
    }
    struct drm_bo *bo = attachment->bo;
    attachment->bo = NULL;
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_exec_lock, flags);
    if (bo) drm_bo_put(bo);
    kernel_free(attachment);
}

static int drm_vmwgfx_exec_attachment_progress(
    struct drm_vmwgfx_attachment *attachment)
{
    if (!attachment || !attachment->bo || attachment->detaching)
        return -LINUX_EINVAL;
    if (attachment->error) return attachment->error;
    if (attachment->ready) return 0;
    uint64_t phys = drm_bo_page_physical(attachment->bo, 0);
    uint64_t size = drm_bo_size(attachment->bo);
    if (!phys || (phys & 4095u) || !size || size > UINT32_MAX)
        return -LINUX_EINVAL;
    int status = svga_gmr_borrowed_attach(phys, (uint32_t)size, attachment,
        drm_vmwgfx_exec_attachment_owner_put, &attachment->gmr);
    if (status == SVGA_EBUSY) return -LINUX_EBUSY;
    if (status) {
        attachment->error = drm_vmwgfx_map_svga_status(status);
        return attachment->error;
    }
    status = svga_gmr_borrowed_mob_id(attachment->gmr,
                                      &attachment->mobid);
    if (status) {
        if (status == SVGA_EBUSY) return -LINUX_EBUSY;
        attachment->error = drm_vmwgfx_map_svga_status(status);
        return attachment->error;
    }
    attachment->ready = true;
    return 0;
}

static void drm_vmwgfx_exec_queue_cleanup(struct drm_vmwgfx_exec_work *work)
{
    if (!work || work->cleanup_queued) return;
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_vmwgfx_exec_lock, &flags);
    if (!work->cleanup_queued) {
        work->cleanup_queued = true;
        work->next = drm_vmwgfx_exec_cleanup;
        drm_vmwgfx_exec_cleanup = work;
    }
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_exec_lock, flags);
}

static void drm_vmwgfx_exec_attachment_put(
    struct drm_vmwgfx_attachment *attachment)
{
    if (!attachment) return;
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_vmwgfx_exec_lock, &flags);
    if (attachment->references) --attachment->references;
    if (!attachment->references) attachment->idle_since = time_ticks();
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_exec_lock, flags);
}

int drm_vmwgfx_exec_attachment_get(
    struct drm_bo *bo, struct drm_vmwgfx_attachment **out,
    const char **failure_stage)
{
    if (failure_stage) *failure_stage = "exec.attach.validate";
    if (!bo || !out) return -LINUX_EINVAL;
    *out = NULL;
    uint64_t bytes = drm_bo_size(bo);
    if (!bytes || bytes > DRM_VMW_EXEC_ATTACHMENT_BYTES_MAX)
        return -LINUX_EINVAL;

    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_vmwgfx_exec_lock, &flags);
    for (struct drm_vmwgfx_attachment *attachment = drm_vmwgfx_attachments;
         attachment; attachment = attachment->next) {
        if (attachment->bo != bo || attachment->detaching) continue;
        if (attachment->references == UINT32_MAX) {
            kernel_spin_unlock_irqrestore(&drm_vmwgfx_exec_lock, flags);
            if (failure_stage) *failure_stage = "exec.attach.ref_limit";
            return -LINUX_ENOMEM;
        }
        ++attachment->references;
        *out = attachment;
        kernel_spin_unlock_irqrestore(&drm_vmwgfx_exec_lock, flags);
        if (failure_stage) *failure_stage = "exec.attach.backend";
        return drm_vmwgfx_exec_attachment_progress(attachment);
    }
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_exec_lock, flags);

    if (failure_stage) *failure_stage = "exec.attach.heap";
    struct drm_vmwgfx_attachment *candidate = kernel_malloc(sizeof(*candidate));
    if (!candidate) return -LINUX_ENOMEM;
    *candidate = (struct drm_vmwgfx_attachment){.bo = bo, .bytes = (uint32_t)bytes,
        .references = 1, .idle_since = time_ticks()};
    drm_bo_get(bo); /* transferred to the attachment's backend owner */

    kernel_spin_lock_irqsave(&drm_vmwgfx_exec_lock, &flags);
    for (struct drm_vmwgfx_attachment *attachment = drm_vmwgfx_attachments;
         attachment; attachment = attachment->next) {
        if (attachment->bo != bo || attachment->detaching) continue;
        if (attachment->references == UINT32_MAX) {
            kernel_spin_unlock_irqrestore(&drm_vmwgfx_exec_lock, flags);
            drm_bo_put(bo);
            kernel_free(candidate);
            if (failure_stage) *failure_stage = "exec.attach.ref_limit";
            return -LINUX_ENOMEM;
        }
        ++attachment->references;
        *out = attachment;
        kernel_spin_unlock_irqrestore(&drm_vmwgfx_exec_lock, flags);
        drm_bo_put(bo);
        kernel_free(candidate);
        if (failure_stage) *failure_stage = "exec.attach.backend";
        return drm_vmwgfx_exec_attachment_progress(attachment);
    }
    if (drm_vmwgfx_attachment_count >= DRM_VMW_EXEC_ATTACHMENTS_MAX ||
        bytes > DRM_VMW_EXEC_ATTACHMENT_BYTES_MAX - drm_vmwgfx_attachment_bytes) {
        kernel_spin_unlock_irqrestore(&drm_vmwgfx_exec_lock, flags);
        drm_bo_put(bo);
        kernel_free(candidate);
        if (failure_stage) *failure_stage = "exec.attach.quota";
        return -LINUX_ENOMEM;
    }
    candidate->next = drm_vmwgfx_attachments;
    drm_vmwgfx_attachments = candidate;
    ++drm_vmwgfx_attachment_count;
    drm_vmwgfx_exec_publish_maintenance_locked();
    drm_vmwgfx_attachment_bytes += bytes;
    *out = candidate;
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_exec_lock, flags);
    if (failure_stage) *failure_stage = "exec.attach.backend";
    return drm_vmwgfx_exec_attachment_progress(candidate);
}

void drm_vmwgfx_exec_work_release(struct drm_vmwgfx_exec_work *work)
{
    if (!work) return;
    if (work->batch_open) {
        int ret = drm_vmwgfx_map_svga_status(svga_batch_abort(&work->batch));
        if (ret) {
            drm_vmwgfx_exec_queue_cleanup(work);
            return;
        }
        work->batch_open = false;
    }
    for (uint32_t i = 0; i < work->context_count; ++i)
        drm_vmwgfx_context_put(work->contexts[i].object);
    for (uint32_t i = 0; i < work->surface_count; ++i)
        drm_vmwgfx_surface_put(work->surfaces[i].object);
    for (uint32_t i = 0; i < work->bo_count; ++i) {
        drm_vmwgfx_exec_attachment_put(work->bos[i].attachment);
        drm_bo_gpu_release(work->bos[i].object);
        drm_bo_put(work->bos[i].object);
    }
    drm_vmwgfx_fence_put(work->fence_object);
    kernel_free(work->snapshot);
    kernel_free(work->scratch);
    kernel_free(work->output);
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_vmwgfx_exec_lock, &flags);
    if (drm_vmwgfx_exec_work_count) --drm_vmwgfx_exec_work_count;
    drm_vmwgfx_exec_publish_maintenance_locked();
    if (drm_vmwgfx_exec_work_bytes >= work->allocation_bytes)
        drm_vmwgfx_exec_work_bytes -= work->allocation_bytes;
    else
        drm_vmwgfx_exec_work_bytes = 0;
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_exec_lock, flags);
    kernel_free(work);
}

void drm_vmwgfx_exec_work_fail(
    struct drm_vmwgfx_exec_work *work,
    struct drm_faildiag_context *diagnostics, int error, const char *stage)
{
    if (!work) return;
    uint32_t source_bytes = work->source_bytes;
    uint32_t output_bytes = work->batch.bytes;
    uint32_t output_capacity = work->output_capacity;
    uint32_t contexts = work->context_count;
    uint32_t surfaces = work->surface_count;
    uint32_t bos = work->bo_count;
    drm_vmwgfx_exec_work_release(work);
    drm_vmwgfx_execdiag(diagnostics, error, stage, source_bytes,
        output_bytes, output_capacity, contexts, surfaces, bos);
}

void drm_vmwgfx_exec_attachment_fail(
    struct drm_vmwgfx_exec_work *work,
    struct drm_faildiag_context *diagnostics, int error, const char *stage)
{
    if (!work) return;
    uint32_t source_bytes = work->source_bytes;
    uint32_t output_capacity = work->output_capacity;
    uint32_t bo_refs = work->bo_count;
    drm_vmwgfx_exec_work_release(work);
    drm_vmwgfx_attachment_diag(diagnostics, error, stage, source_bytes,
        output_capacity, 0, bo_refs);
}

static void drm_vmwgfx_exec_work_alloc_diag(
    struct drm_faildiag_context *diagnostics, int error, const char *stage,
    uint32_t source_bytes, uint32_t output_capacity, uint64_t requested)
{
    if (!drm_faildiag_is_enabled()) return;
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_vmwgfx_exec_lock, &flags);
    uint32_t work_count = drm_vmwgfx_exec_work_count;
    uint32_t work_bytes = drm_vmwgfx_exec_work_bytes;
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_exec_lock, flags);
    const struct drm_faildiag_field fields[] = {
        {"source", source_bytes},
        {"output_capacity", output_capacity},
        {"requested", requested},
        {"work_count", work_count},
        {"work_bytes", work_bytes},
        {"work_limit", DRM_VMW_EXEC_WORK_BYTES_MAX},
    };
    drm_faildiag_record(diagnostics, error, stage, fields,
                        sizeof(fields) / sizeof(fields[0]));
}

struct drm_vmwgfx_exec_work *drm_vmwgfx_exec_work_alloc(
    uint32_t source_bytes, uint32_t packet_count, int *error,
    struct drm_faildiag_context *diagnostics)
{
    uint64_t output_capacity64 = source_bytes
        ? (uint64_t)source_bytes * 4u + 1024u : 0;
    if (output_capacity64 < 4096u && source_bytes) output_capacity64 = 4096u;
    if (output_capacity64 > DRM_VMW_EXEC_OUTPUT_BYTES_MAX)
        output_capacity64 = DRM_VMW_EXEC_OUTPUT_BYTES_MAX;
    uint32_t output_capacity = (uint32_t)output_capacity64;
    uint64_t allocation = sizeof(struct drm_vmwgfx_exec_work) +
        (uint64_t)source_bytes * 2u + output_capacity;
    if (allocation > UINT32_MAX || allocation > DRM_VMW_EXEC_WORK_BYTES_MAX) {
        if (error) *error = -LINUX_ENOMEM;
        drm_vmwgfx_exec_work_alloc_diag(diagnostics, -LINUX_ENOMEM,
            "exec.work.bytes", source_bytes, output_capacity, allocation);
        return NULL;
    }

    uint64_t flags;
    bool retried_retirement = false;
retry_quota:
    kernel_spin_lock_irqsave(&drm_vmwgfx_exec_lock, &flags);
    if (drm_vmwgfx_exec_work_count >= DRM_VMW_EXEC_WORK_MAX ||
        allocation > DRM_VMW_EXEC_WORK_BYTES_MAX - drm_vmwgfx_exec_work_bytes) {
        kernel_spin_unlock_irqrestore(&drm_vmwgfx_exec_lock, flags);
        /* Completed jobs may still be awaiting a maintenance pass. Reap a
         * bounded number outside exec_lock before charging new work. Pending
         * fences keep their full charge; transient pressure is retryable,
         * not an allocation failure that makes Mesa abort the X server. */
        if (!retried_retirement) {
            retried_retirement = true;
            drm_vmwgfx_execbuf_process_retirements(8u);
            goto retry_quota;
        }
        if (error) *error = -LINUX_EBUSY;
        return NULL;
    }
    ++drm_vmwgfx_exec_work_count;
    drm_vmwgfx_exec_publish_maintenance_locked();
    drm_vmwgfx_exec_work_bytes += (uint32_t)allocation;
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_exec_lock, flags);

    struct drm_vmwgfx_exec_work *work = kernel_malloc(sizeof(*work));
    if (!work) goto no_memory;
    *work = (struct drm_vmwgfx_exec_work){.source_bytes = source_bytes,
        .packet_count = packet_count, .output_capacity = output_capacity,
        .allocation_bytes = (uint32_t)allocation};
    if (source_bytes) {
        work->snapshot = kernel_malloc(source_bytes);
        work->scratch = kernel_malloc(source_bytes);
        work->output = kernel_malloc(output_capacity);
        if (!work->snapshot || !work->scratch || !work->output) {
            kernel_free(work->snapshot);
            kernel_free(work->scratch);
            kernel_free(work->output);
            kernel_free(work);
            goto no_memory;
        }
    }
    if (error) *error = 0;
    return work;

no_memory:
    kernel_spin_lock_irqsave(&drm_vmwgfx_exec_lock, &flags);
    if (drm_vmwgfx_exec_work_count) --drm_vmwgfx_exec_work_count;
    drm_vmwgfx_exec_publish_maintenance_locked();
    if (drm_vmwgfx_exec_work_bytes >= allocation)
        drm_vmwgfx_exec_work_bytes -= (uint32_t)allocation;
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_exec_lock, flags);
    if (error) *error = -LINUX_ENOMEM;
    drm_vmwgfx_exec_work_alloc_diag(diagnostics, -LINUX_ENOMEM,
        "exec.work.heap", source_bytes, output_capacity, allocation);
    return NULL;
}

void drm_vmwgfx_execbuf_process_retirements(uint32_t budget)
{
    /* Work published after this empty observation is handled on the next
     * syscall or idle-service pass. No queue entry is consumed by this hint. */
    if (!__atomic_load_n(&drm_vmwgfx_exec_maintenance_pending, __ATOMIC_ACQUIRE))
        return;
    uint64_t now = time_ticks();
    if (budget > 8u) budget = 8u;
    for (uint32_t i = 0; i < budget; ++i) {
        uint64_t flags;
        kernel_spin_lock_irqsave(&drm_vmwgfx_exec_lock, &flags);
        struct drm_vmwgfx_task_continuation *continuation =
            drm_vmwgfx_continuation_cleanup;
        if (continuation) {
            drm_vmwgfx_continuation_cleanup = continuation->next;
            continuation->next = NULL;
            drm_vmwgfx_exec_publish_maintenance_locked();
        }
        struct drm_vmwgfx_exec_work *work = drm_vmwgfx_exec_cleanup;
        if (work) {
            drm_vmwgfx_exec_cleanup = work->next;
            work->next = NULL;
            work->cleanup_queued = false;
        } else {
            work = drm_vmwgfx_exec_jobs;
            if (work) {
                drm_vmwgfx_exec_jobs = work->next;
                if (!drm_vmwgfx_exec_jobs)
                    drm_vmwgfx_exec_jobs_tail = NULL;
                work->next = NULL;
            }
        }
        kernel_spin_unlock_irqrestore(&drm_vmwgfx_exec_lock, flags);
        drm_vmwgfx_continuation_release(continuation);
        if (work) {
            if (!work->submitted) {
                drm_vmwgfx_exec_work_release(work);
            } else {
                bool signaled = false;
                int status = drm_vmwgfx_map_svga_status(
                    svga_fence_status(work->fence, &signaled));
                bool quiesced = false;
                if (status == -LINUX_EIO) {
                    work->reset_pending = true;
                    quiesced = svga_fence_is_quiesced(work->fence);
                }
                if ((!status && signaled) ||
                    (work->reset_pending && quiesced)) {
                    drm_vmwgfx_exec_work_release(work);
                } else {
                    kernel_spin_lock_irqsave(&drm_vmwgfx_exec_lock, &flags);
                    drm_vmwgfx_exec_job_enqueue_locked(work);
                    kernel_spin_unlock_irqrestore(&drm_vmwgfx_exec_lock, flags);
                }
            }
        }

        struct drm_vmwgfx_attachment *attachment = NULL;
        bool detach = false;
        kernel_spin_lock_irqsave(&drm_vmwgfx_exec_lock, &flags);
        struct drm_vmwgfx_attachment *cursor = drm_vmwgfx_attachments;
        if (cursor) {
            uint32_t skip = drm_vmwgfx_attachment_progress_cursor++ %
                            (drm_vmwgfx_attachment_count
                                ? drm_vmwgfx_attachment_count : 1u);
            while (skip-- && cursor) cursor = cursor->next;
        }
        if (cursor && cursor->last_progress_tick != now) {
            cursor->last_progress_tick = now;
            attachment = cursor;
            if (!cursor->references && now >= cursor->idle_since &&
                now - cursor->idle_since >= DRM_VMW_EXEC_ATTACHMENT_CACHE_TICKS) {
                cursor->detaching = true;
                detach = true;
            }
        }
        kernel_spin_unlock_irqrestore(&drm_vmwgfx_exec_lock, flags);
        if (!attachment) continue;
        if (detach) {
            if (!attachment->gmr) {
                drm_vmwgfx_exec_attachment_owner_put(attachment);
            } else {
                /* A busy detach may already have queued host destruction.
                 * Keep this cache entry unavailable until the owner callback
                 * unlinks it after the backend retirement fence completes. */
                (void)svga_gmr_borrowed_detach(attachment->gmr);
            }
        } else if (!attachment->ready && !attachment->error) {
            (void)drm_vmwgfx_exec_attachment_progress(attachment);
        }
    }
}
