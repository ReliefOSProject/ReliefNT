#include "execbuf_internal.h"

static struct drm_vmwgfx_fence *drm_vmwgfx_fence_find_locked(
    struct drm_file *file, uint32_t handle);

static uint32_t drm_vmwgfx_fence_handle_locked(struct drm_file *file);

static struct drm_vmwgfx_fence *drm_vmwgfx_fence_lookup_get(
    struct drm_file *file, uint32_t handle);

/**
 * @brief Remove a fence handle, optionally requiring its retained object.
 * @param file DRM file whose user-visible fence list is searched.
 * @param handle User-visible fence handle to remove.
 * @param expected If non-NULL, the handle must still name this retained fence.
 * @return True if the matching user-visible reference was removed.
 */
static bool drm_vmwgfx_fence_remove_retained(
    struct drm_file *file, uint32_t handle,
    struct drm_vmwgfx_fence *expected);

static uint64_t drm_vmwgfx_timeout_ticks(uint64_t timeout_us);

/**
 * @brief Try a short, unlocked poll for a FENCE_WAIT attempt.
 * @param fence Immutable retained SVGA fence token; no fence object is touched
 *        while execution ownership is suspended.
 * @param signaled Set true when a successful status sample passes. A conclusive
 *        in-flight sample is honored even if its return crosses the budget.
 * @param status Set to the mapped SVGA error if a poll returns a hard error.
 * @param now Updated from the kernel tick clock after execution ownership is
 *        reacquired, before the caller makes timeout or sleep decisions.
 * @return True if the unlocked poll ran and execution ownership was restored;
 *         false if the fine-clock or suspend guard declined it.
 *
 * The 500 us budget limits starting further samples within the polling
 * window; it is not an absolute CPU or wall-time deadline. An in-flight
 * status call, interrupt, or VM deschedule can carry that window past the
 * budget, and a conclusive completion is still accepted. Interrupts remain
 * enabled while global execution ownership is suspended. A finite
 * 8192-sample ceiling also bounds polling if the fine clock stops or moves
 * backward. The suspend token is held while no global execution transaction
 * or SVGA lock is held. A failed resume violates kernel ownership invariants,
 * so shared state must not be touched afterward. Reacquiring the execution
 * transaction can add further contention.
 */
static bool drm_vmwgfx_fence_wait_fast_poll(
    svga_handle fence, bool *signaled, int *status, uint64_t *now);


void drm_vmwgfx_fence_diag(
    struct drm_faildiag_context *diagnostics, int error, const char *stage,
    uint32_t source_bytes, uint32_t output_bytes, uint32_t output_capacity)
{
    if (!drm_faildiag_is_enabled()) return;
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_vmwgfx_exec_lock, &flags);
    uint32_t work_count = drm_vmwgfx_exec_work_count;
    uint32_t work_bytes = drm_vmwgfx_exec_work_bytes;
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_exec_lock, flags);
    kernel_spin_lock_irqsave(&drm_vmwgfx_fence_lock, &flags);
    uint32_t fence_count = drm_vmwgfx_fence_count;
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_fence_lock, flags);
    const struct drm_faildiag_field fields[] = {
        {"source", source_bytes},
        {"output", output_bytes},
        {"capacity", output_capacity},
        {"work_count", work_count},
        {"work_bytes", work_bytes},
        {"fences", fence_count},
    };
    drm_faildiag_record(diagnostics, error, stage, fields,
                        sizeof(fields) / sizeof(fields[0]));
}

static struct drm_vmwgfx_fence *drm_vmwgfx_fence_find_locked(
    struct drm_file *file, uint32_t handle)
{
    for (struct drm_vmwgfx_fence *fence = file
            ? (struct drm_vmwgfx_fence *)file->vmwgfx_fences : NULL;
         fence; fence = fence->next)
        if (fence->handle == handle) return fence;
    return NULL;
}

static uint32_t drm_vmwgfx_fence_handle_locked(struct drm_file *file)
{
    uint32_t first = file->next_fence_handle;
    if (!first) first = 1;
    uint32_t candidate = first;
    do {
        if (!drm_vmwgfx_fence_find_locked(file, candidate)) {
            file->next_fence_handle = candidate + 1u;
            if (!file->next_fence_handle) file->next_fence_handle = 1;
            return candidate;
        }
        ++candidate;
        if (!candidate) candidate = 1;
    } while (candidate != first);
    return 0;
}

void drm_vmwgfx_fence_put(struct drm_vmwgfx_fence *fence)
{
    if (!fence) return;
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_vmwgfx_fence_lock, &flags);
    bool free_fence = fence->references && --fence->references == 0;
    if (free_fence && drm_vmwgfx_fence_count) --drm_vmwgfx_fence_count;
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_fence_lock, flags);
    if (free_fence) kernel_free(fence);
}

void drm_vmwgfx_fence_get(struct drm_vmwgfx_fence *fence)
{
    if (!fence) return;
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_vmwgfx_fence_lock, &flags);
    if (fence->references != UINT32_MAX) ++fence->references;
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_fence_lock, flags);
}

struct drm_vmwgfx_fence *drm_vmwgfx_fence_reserve(
    struct drm_file *file, bool user_handle, const char **failure_stage)
{
    if (failure_stage) *failure_stage = "exec.fence.heap";
    struct drm_vmwgfx_fence *fence = kernel_malloc(sizeof(*fence));
    if (!fence) return NULL;
    *fence = (struct drm_vmwgfx_fence){.references = 1};
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_vmwgfx_fence_lock, &flags);
    if (drm_vmwgfx_fence_count >= DRM_VMW_FENCE_MAX_GLOBAL) {
        kernel_spin_unlock_irqrestore(&drm_vmwgfx_fence_lock, flags);
        kernel_free(fence);
        if (failure_stage) *failure_stage = "exec.fence.quota";
        return NULL;
    }
    if (user_handle) fence->handle = drm_vmwgfx_fence_handle_locked(file);
    if (user_handle && !fence->handle) {
        kernel_spin_unlock_irqrestore(&drm_vmwgfx_fence_lock, flags);
        kernel_free(fence);
        if (failure_stage) *failure_stage = "exec.fence.handle";
        return NULL;
    }
    ++drm_vmwgfx_fence_count;
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_fence_lock, flags);
    if (failure_stage) *failure_stage = NULL;
    return fence;
}

void drm_vmwgfx_fence_publish(struct drm_file *file,
                                     struct drm_vmwgfx_fence *fence)
{
    if (!file || !fence || !fence->handle) return;
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_vmwgfx_fence_lock, &flags);
    fence->references++;
    fence->user_linked = true;
    fence->next = (struct drm_vmwgfx_fence *)file->vmwgfx_fences;
    file->vmwgfx_fences = fence;
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_fence_lock, flags);
}

static struct drm_vmwgfx_fence *drm_vmwgfx_fence_lookup_get(
    struct drm_file *file, uint32_t handle)
{
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_vmwgfx_fence_lock, &flags);
    struct drm_vmwgfx_fence *fence = drm_vmwgfx_fence_find_locked(file, handle);
    if (fence) ++fence->references;
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_fence_lock, flags);
    return fence;
}

/**
 * @brief Remove a fence handle, optionally requiring its retained object.
 * @param file DRM file whose user-visible fence list is searched.
 * @param handle User-visible fence handle to remove.
 * @param expected If non-NULL, the handle must still name this retained fence.
 * @return True if the matching user-visible reference was removed.
 */
static bool drm_vmwgfx_fence_remove_retained(
    struct drm_file *file, uint32_t handle,
    struct drm_vmwgfx_fence *expected)
{
    struct drm_vmwgfx_fence *removed = NULL;
    bool found = false;
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_vmwgfx_fence_lock, &flags);
    struct drm_vmwgfx_fence **link = file
        ? (struct drm_vmwgfx_fence **)&file->vmwgfx_fences : NULL;
    while (link && *link &&
           ((*link)->handle != handle || (expected && *link != expected)))
        link = &(*link)->next;
    if (link && *link) {
        found = true;
        removed = *link;
        *link = removed->next;
        removed->next = NULL;
        removed->user_linked = false;
        if (removed->references && --removed->references == 0) {
            if (drm_vmwgfx_fence_count) --drm_vmwgfx_fence_count;
        } else {
            removed = NULL;
        }
    }
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_fence_lock, flags);
    if (removed) kernel_free(removed);
    return found;
}

bool drm_vmwgfx_fence_remove(struct drm_file *file, uint32_t handle)
{
    return drm_vmwgfx_fence_remove_retained(file, handle, NULL);
}

int drm_vmwgfx_fence_signaled(struct drm_file *file,
                                     struct drm_vmw_fence_signaled_arg *arg)
{
    struct drm_vmwgfx_fence *fence = drm_vmwgfx_fence_lookup_get(file,
                                                                 arg->handle);
    if (!fence) return -LINUX_EINVAL;
    bool signaled = false;
    int status = drm_vmwgfx_map_svga_status(
        svga_fence_status(fence->token, &signaled));
    if (status) { drm_vmwgfx_fence_put(fence); return status; }
    struct svga_fence_progress progress;
    status = drm_vmwgfx_map_svga_status(svga_fence_progress_snapshot(&progress));
    if (status) { drm_vmwgfx_fence_put(fence); return status; }
    arg->signaled = signaled ? 1 : 0;
    arg->passed_seqno = progress.passed_seq;
    arg->signaled_flags = arg->flags;
    drm_vmwgfx_fence_put(fence);
    return 0;
}

static uint64_t drm_vmwgfx_timeout_ticks(uint64_t timeout_us)
{
    uint64_t seconds = timeout_us / 1000000u;
    uint64_t remainder = timeout_us % 1000000u;
    if (seconds > UINT64_MAX / RELIEFNT_TICK_HZ) return UINT64_MAX;
    uint64_t ticks = seconds * RELIEFNT_TICK_HZ;
    uint64_t fraction = (remainder * RELIEFNT_TICK_HZ + 999999u) / 1000000u;
    if (ticks > UINT64_MAX - fraction) return UINT64_MAX;
    return ticks + fraction;
}

/**
 * @brief Try a short, unlocked poll for a FENCE_WAIT attempt.
 * @param fence Immutable retained SVGA fence token; no fence object is touched
 *        while execution ownership is suspended.
 * @param signaled Set true when a successful status sample passes. A conclusive
 *        in-flight sample is honored even if its return crosses the budget.
 * @param status Set to the mapped SVGA error if a poll returns a hard error.
 * @param now Updated from the kernel tick clock after execution ownership is
 *        reacquired, before the caller makes timeout or sleep decisions.
 * @return True if the unlocked poll ran and execution ownership was restored;
 *         false if the fine-clock or suspend guard declined it.
 *
 * The 500 us budget limits starting further samples within the polling
 * window; it is not an absolute CPU or wall-time deadline. An in-flight
 * status call, interrupt, or VM deschedule can carry that window past the
 * budget, and a conclusive completion is still accepted. Interrupts remain
 * enabled while global execution ownership is suspended. A finite
 * 8192-sample ceiling also bounds polling if the fine clock stops or moves
 * backward. The suspend token is held while no global execution transaction
 * or SVGA lock is held. A failed resume violates kernel ownership invariants,
 * so shared state must not be touched afterward. Reacquiring the execution
 * transaction can add further contention.
 */
static bool drm_vmwgfx_fence_wait_fast_poll(
    svga_handle fence, bool *signaled, int *status, uint64_t *now)
{
    if (!signaled || !status || !now) return false;

    struct linux_timespec resolution = {0};
    if (time_clock_getres(LINUX_CLOCK_MONOTONIC, &resolution) != 0 ||
        resolution.tv_sec != 0 || resolution.tv_nsec <= 0 ||
        resolution.tv_nsec > DRM_VMW_FENCE_WAIT_FAST_CLOCK_RESOLUTION_NS)
        return false;

    struct kernel_execution_suspend_token token = {0};
    if (kernel_execution_suspend_irqrestore(&token) != 0) return false;

    const uint64_t started_us = time_uptime_us();
    for (uint32_t sample = 0;
         sample < DRM_VMW_FENCE_WAIT_FAST_POLL_SAMPLES; ++sample) {
        uint64_t before_us = time_uptime_us();
        if (before_us < started_us ||
            before_us - started_us >= DRM_VMW_FENCE_WAIT_FAST_POLL_US)
            break;

        bool sample_signaled = false;
        int sample_status = svga_fence_status(fence, &sample_signaled);
        uint64_t after_us = time_uptime_us();
        bool within_budget = after_us >= started_us &&
            after_us - started_us < DRM_VMW_FENCE_WAIT_FAST_POLL_US;

        if (sample_status && sample_status != SVGA_EBUSY) {
            *status = drm_vmwgfx_map_svga_status(sample_status);
            break;
        }
        if (sample_status == 0 && sample_signaled) {
            *signaled = true;
            break;
        }
        if (!within_budget) break;
        __asm__ volatile("pause" ::: "memory");
    }

    if (kernel_execution_resume_irqsave(&token) != 0)
        panic("drm: failed to resume FENCE_WAIT execution ownership");
    *now = time_ticks();
    return true;
}

void drm_vmwgfx_fence_wait_finish(
    struct task *task, struct drm_vmwgfx_task_continuation *continuation)
{
    if (task && task->drm_vmwgfx_continuation == continuation)
        task->drm_vmwgfx_continuation = NULL;
    drm_vmwgfx_continuation_release(continuation);
}

int64_t drm_vmwgfx_fence_wait(
    struct task *task, struct drm_file *file, uint32_t command,
    struct drm_vmw_fence_wait_arg *arg, uint64_t user_argument,
    struct drm_faildiag_context *diagnostics)
{
    struct drm_vmwgfx_task_continuation *continuation = task
        ? task->drm_vmwgfx_continuation : NULL;
    bool resumed = continuation &&
        continuation->kind == DRM_VMW_CONT_FENCE_WAIT &&
        drm_vmwgfx_continuation_matches(continuation, task,
            task->syscall_file, file, command, user_argument);
    if (continuation && !resumed) {
        drm_vmwgfx_cancel_task_continuation(task);
        continuation = NULL;
    }

    struct drm_vmwgfx_fence *fence = resumed
        ? continuation->fence_object
        : drm_vmwgfx_fence_lookup_get(file, arg->handle);
    if (!fence) return -LINUX_EINVAL;
    uint64_t now = time_ticks();
    uint64_t deadline;
    if (resumed) {
        *arg = continuation->snapshot.wait;
        deadline = continuation->deadline;
    } else if (!arg->cookie_valid) {
        uint64_t delta = drm_vmwgfx_timeout_ticks(arg->timeout_us);
        deadline = delta > UINT64_MAX - now ? UINT64_MAX : now + delta;
        arg->cookie_valid = 1;
        arg->kernel_cookie = deadline;
    } else {
        deadline = arg->kernel_cookie;
    }

    bool signaled = false;
    int status = drm_vmwgfx_map_svga_status(
        svga_fence_status(fence->token, &signaled));
    if (status == -LINUX_EBUSY) {
        /* The backend uses a nonblocking lock; sample contention is not a
         * completed user wait and must not be reported as fence timeout. */
        signaled = false;
        status = 0;
    }
    if (status) {
        if (resumed) drm_vmwgfx_fence_wait_finish(task, continuation);
        else drm_vmwgfx_fence_put(fence);
        return status;
    }
    if (!resumed && !signaled && now < deadline) {
        /* Only the first blocking attempt owns a fresh lookup_get reference;
         * resumed continuations retain the original fence and cookie while
         * following the same lazy/non-lazy scheduling choice below. */
        (void)drm_vmwgfx_fence_wait_fast_poll(
            fence->token, &signaled, &status, &now);
    } else if (resumed && !arg->lazy && !signaled && now < deadline) {
        /* The continuation owns the primary reference. Pin a separate local
         * reference and copy the immutable status token before dropping
         * execution ownership, then validate the original continuation only
         * after ownership is restored. */
        struct drm_vmwgfx_task_continuation *expected_continuation =
            continuation;
        struct drm_vmwgfx_fence *poll_fence = fence;
        drm_vmwgfx_fence_get(poll_fence);
        const svga_handle poll_token = poll_fence->token;
        const struct drm_vmw_fence_wait_arg poll_snapshot = *arg;
        const uint64_t poll_deadline = deadline;
        bool poll_ran = drm_vmwgfx_fence_wait_fast_poll(
            poll_token, &signaled, &status, &now);

        if (poll_ran) {
            bool continuation_valid = task->state != TASK_EXITED &&
                task->drm_vmwgfx_continuation == expected_continuation;
            if (continuation_valid) {
                const struct drm_vmw_fence_wait_arg *saved =
                    &expected_continuation->snapshot.wait;
                continuation_valid =
                    expected_continuation->kind == DRM_VMW_CONT_FENCE_WAIT &&
                    drm_vmwgfx_continuation_matches(expected_continuation,
                        task, task->syscall_file, file, command,
                        user_argument) &&
                    expected_continuation->fence_object == poll_fence &&
                    expected_continuation->fence == poll_token &&
                    expected_continuation->deadline == poll_deadline &&
                    saved->handle == poll_snapshot.handle &&
                    saved->cookie_valid == poll_snapshot.cookie_valid &&
                    saved->kernel_cookie == poll_snapshot.kernel_cookie &&
                    saved->timeout_us == poll_snapshot.timeout_us &&
                    saved->lazy == poll_snapshot.lazy &&
                    saved->flags == poll_snapshot.flags &&
                    saved->wait_options == poll_snapshot.wait_options &&
                    saved->pad64 == poll_snapshot.pad64;
            }
            drm_vmwgfx_fence_put(poll_fence);
            if (!continuation_valid) {
                /* Cancellation or task exit won while unlocked. Leave any
                 * replacement continuation untouched and return through the
                 * normal scheduler boundary; the next syscall attempt follows
                 * the existing restart/cleanup path. */
                sched_request_reschedule_current();
                return KERNEL_SYSCALL_BLOCKED;
            }
        } else {
            drm_vmwgfx_fence_put(poll_fence);
        }
    }
    if (status) {
        if (resumed) drm_vmwgfx_fence_wait_finish(task, continuation);
        else drm_vmwgfx_fence_put(fence);
        return status;
    }
    if (signaled) {
        if (arg->wait_options & DRM_VMW_WAIT_OPTION_UNREF)
            (void)drm_vmwgfx_fence_remove_retained(file, arg->handle, fence);
        if (resumed) drm_vmwgfx_fence_wait_finish(task, continuation);
        else drm_vmwgfx_fence_put(fence);
        return 0;
    }
    if (now >= deadline) {
        if (resumed) drm_vmwgfx_fence_wait_finish(task, continuation);
        else drm_vmwgfx_fence_put(fence);
        return -LINUX_EBUSY;
    }

    if (!resumed) {
        continuation = kernel_malloc(sizeof(*continuation));
        if (!continuation) {
            drm_vmwgfx_fence_put(fence);
            drm_vmwgfx_fence_diag(diagnostics, -LINUX_ENOMEM,
                                  "fence.wait.heap", 0, 0, 0);
            return -LINUX_ENOMEM;
        }
        *continuation = (struct drm_vmwgfx_task_continuation){
            .kind = DRM_VMW_CONT_FENCE_WAIT,
            .fence_object = fence,
            .fence = fence->token,
            .deadline = deadline,
            .snapshot_bytes = sizeof(*arg),
        };
        drm_vmwgfx_continuation_set_key(continuation, task, file, command,
                                         user_argument);
        task->drm_vmwgfx_continuation = continuation;
    }
    continuation->snapshot.wait = *arg;
    continuation->deadline = deadline;
    if (arg->lazy)
        sched_sleep_current_until(now + 1u);
    else
        sched_request_reschedule_current();
    return KERNEL_SYSCALL_BLOCKED;
}
