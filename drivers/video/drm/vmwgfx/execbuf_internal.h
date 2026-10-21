#ifndef RELIEFNT_DRIVERS_VIDEO_DRM_VMWGFX_EXECBUF_C_INTERNAL_H
#define RELIEFNT_DRIVERS_VIDEO_DRM_VMWGFX_EXECBUF_C_INTERNAL_H

#include "../drm_internal.h"

#include <drm/vmwgfx_drm.h>
#include <linux/errno.h>
#include <reliefnt/heap.h>
#include <reliefnt/lock.h>
#include <reliefnt/futex.h>
#include <reliefnt/panic.h>
#include <reliefnt/sched.h>
#include <reliefnt/time.h>
#include <reliefnt/usercopy.h>

#include "../../svga/svga_protocol.h"
#include "execbuf_parser.h"

#define DRM_VMW_EXECBUF_V1_SHORT_SIZE \
    __builtin_offsetof(struct drm_vmw_execbuf_arg, context_handle)
#define DRM_VMW_FENCE_MAX_GLOBAL 128u
#define DRM_VMW_EXEC_WORK_MAX 32u
#define DRM_VMW_EXEC_WORK_BYTES_MAX (8u * 1024u * 1024u)
#define DRM_VMW_EXEC_ATTACHMENTS_MAX 128u
#define DRM_VMW_EXEC_ATTACHMENT_BYTES_MAX (128u * 1024u * 1024u)
#define DRM_VMW_EXEC_CONTEXTS_MAX 64u
#define DRM_VMW_EXEC_SURFACES_MAX 256u
#define DRM_VMW_EXEC_BOS_MAX 128u
#define DRM_VMW_EXEC_OUTPUT_BYTES_MAX (256u * 1024u)
#define DRM_VMW_EXEC_ATTACHMENT_CACHE_TICKS 100u
#define DRM_VMW_FENCE_WAIT_FAST_POLL_US 500u
#define DRM_VMW_FENCE_WAIT_FAST_POLL_SAMPLES 8192u
#define DRM_VMW_FENCE_WAIT_FAST_CLOCK_RESOLUTION_NS 1000

struct drm_vmwgfx_exec_work;

struct drm_vmwgfx_attachment {
    struct drm_vmwgfx_attachment *next;
    struct drm_bo *bo;
    svga_handle gmr;
    uint64_t idle_since;
    uint64_t last_progress_tick;
    uint32_t bytes;
    uint32_t mobid;
    uint32_t references;
    bool detaching;
    bool ready;
    int error;
};

struct drm_vmwgfx_context_ref {
    struct drm_vmwgfx_context *object;
    uint32_t drm_id;
    uint32_t backend_id;
    svga_handle backend;
};

struct drm_vmwgfx_surface_ref {
    struct drm_vmwgfx_surface *object;
    struct drm_vmwgfx_surface_metadata metadata;
    uint32_t drm_id;
    uint32_t backend_id;
    svga_handle backend;
};

struct drm_vmwgfx_bo_ref {
    struct drm_bo *object;
    struct drm_vmwgfx_attachment *attachment;
    uint32_t drm_id;
};

struct drm_vmwgfx_exec_work {
    struct drm_vmwgfx_exec_work *next;
    struct drm_file *preparing_file;
    uint8_t *snapshot;
    uint8_t *scratch;
    uint32_t *output;
    uint32_t source_bytes;
    uint32_t packet_count;
    uint32_t output_capacity;
    uint32_t context_count, surface_count, bo_count;
    uint32_t allocation_bytes;
    struct drm_vmwgfx_context_ref contexts[DRM_VMW_EXEC_CONTEXTS_MAX];
    struct drm_vmwgfx_surface_ref surfaces[DRM_VMW_EXEC_SURFACES_MAX];
    struct drm_vmwgfx_bo_ref bos[DRM_VMW_EXEC_BOS_MAX];
    struct svga_batch batch;
    struct drm_vmwgfx_fence *fence_object;
    svga_handle fence;
    bool batch_open;
    bool submitted;
    bool reset_pending;
    bool cleanup_queued;
};

enum drm_vmwgfx_continuation_kind {
    DRM_VMW_CONT_FENCE_WAIT = 1,
    DRM_VMW_CONT_SYNCCPU = 2,
    DRM_VMW_CONT_FENCE_REPLY_FAULT = 3,
};

struct drm_vmwgfx_task_continuation {
    struct drm_vmwgfx_task_continuation *next;
    enum drm_vmwgfx_continuation_kind kind;
    struct drm_file *file;
    struct task_file *syscall_file;
    int32_t syscall_fd;
    uint32_t syscall_number;
    uint64_t user_argument;
    uint64_t deadline;
    uint32_t command;
    uint32_t snapshot_bytes;
    union {
        struct drm_vmw_fence_wait_arg wait;
        struct drm_vmw_synccpu_arg synccpu;
    } snapshot;
    struct drm_bo *bo;
    struct drm_vmwgfx_fence *fence_object;
    svga_handle fence;
};

struct drm_vmwgfx_fence {
    struct drm_vmwgfx_fence *next;
    uint32_t references;
    uint32_t handle;
    svga_handle token;
    bool user_linked;
};
















/* Called under exec_lock after changing these owners. Count allocated work
 * as well as queued work, conservatively covering unpublished allocations
 * and every submitted job, cleanup record and cached attachment. */












































































struct drm_vmwgfx_exec_walk {
    struct drm_vmwgfx_exec_work *work;
    struct drm_file *file;
};


extern struct kernel_spinlock drm_vmwgfx_fence_lock;
extern uint32_t drm_vmwgfx_fence_count;
extern struct kernel_spinlock drm_vmwgfx_exec_lock;
extern struct drm_vmwgfx_task_continuation *drm_vmwgfx_continuation_cleanup;
extern uint32_t drm_vmwgfx_exec_work_count;
extern uint32_t drm_vmwgfx_exec_work_bytes;
extern uint32_t drm_vmwgfx_attachment_count;
extern uint64_t drm_vmwgfx_attachment_bytes;

void drm_vmwgfx_exec_publish_maintenance_locked(void);

void drm_vmwgfx_execdiag(
    struct drm_faildiag_context *diagnostics, int error, const char *stage,
    uint32_t source_bytes, uint32_t output_bytes, uint32_t output_capacity,
    uint32_t contexts, uint32_t surfaces, uint32_t bos);

void drm_vmwgfx_attachment_diag(
    struct drm_faildiag_context *diagnostics, int error, const char *stage,
    uint32_t source_bytes, uint32_t output_capacity, uint32_t output_bytes,
    uint32_t bo_refs);

void drm_vmwgfx_fence_diag(
    struct drm_faildiag_context *diagnostics, int error, const char *stage,
    uint32_t source_bytes, uint32_t output_bytes, uint32_t output_capacity);

void drm_vmwgfx_exec_job_enqueue_locked(
    struct drm_vmwgfx_exec_work *work);

void drm_vmwgfx_fence_put(struct drm_vmwgfx_fence *fence);

void drm_vmwgfx_fence_get(struct drm_vmwgfx_fence *fence);

struct drm_vmwgfx_fence *drm_vmwgfx_fence_reserve(
    struct drm_file *file, bool user_handle, const char **failure_stage);

void drm_vmwgfx_fence_publish(struct drm_file *file,
                                     struct drm_vmwgfx_fence *fence);

bool drm_vmwgfx_fence_remove(struct drm_file *file, uint32_t handle);

int drm_vmwgfx_map_svga_status(int status);

struct drm_vmwgfx_surface_ref *drm_vmwgfx_exec_find_surface(
    struct drm_vmwgfx_exec_work *work, uint32_t drm_id);

int drm_vmwgfx_exec_hold_context(struct drm_vmwgfx_exec_work *work,
                                         struct drm_file *file,
                                         uint32_t drm_id,
                                         struct drm_vmwgfx_context_ref **out);

int drm_vmwgfx_exec_hold_surface(struct drm_vmwgfx_exec_work *work,
                                         struct drm_file *file,
                                         uint32_t drm_id,
                                         uint32_t face, uint32_t mip,
                                         struct drm_vmwgfx_surface_ref **out);

int drm_vmwgfx_exec_hold_bo(struct drm_vmwgfx_exec_work *work,
                                    struct drm_file *file, uint32_t drm_id,
                                    struct drm_vmwgfx_bo_ref **out);

bool drm_vmwgfx_exec_has_context(uint32_t command);

int drm_vmwgfx_exec_validate_packet(uint32_t command,
                                            const uint8_t *body,
                                            uint32_t body_bytes,
                                            uint32_t packet_offset,
                                            void *opaque);

int drm_vmwgfx_exec_translate_packet(uint32_t command,
                                              const uint8_t *body,
                                              uint32_t body_bytes,
                                              uint32_t packet_offset,
                                              void *opaque);

int drm_vmwgfx_exec_attachment_get(
    struct drm_bo *bo, struct drm_vmwgfx_attachment **out,
    const char **failure_stage);

void drm_vmwgfx_exec_work_release(struct drm_vmwgfx_exec_work *work);

void drm_vmwgfx_exec_work_fail(
    struct drm_vmwgfx_exec_work *work,
    struct drm_faildiag_context *diagnostics, int error, const char *stage);

void drm_vmwgfx_exec_attachment_fail(
    struct drm_vmwgfx_exec_work *work,
    struct drm_faildiag_context *diagnostics, int error, const char *stage);

struct drm_vmwgfx_exec_work *drm_vmwgfx_exec_work_alloc(
    uint32_t source_bytes, uint32_t packet_count, int *error,
    struct drm_faildiag_context *diagnostics);

void drm_vmwgfx_execbuf_process_retirements(uint32_t budget);

bool drm_vmwgfx_continuation_matches(
    const struct drm_vmwgfx_task_continuation *continuation,
    const struct task *task, const struct task_file *syscall_file,
    const struct drm_file *file, uint32_t command, uint64_t user_argument);

void drm_vmwgfx_continuation_set_key(
    struct drm_vmwgfx_task_continuation *continuation,
    struct task *task, struct drm_file *file, uint32_t command,
    uint64_t user_argument);

void drm_vmwgfx_continuation_release(
    struct drm_vmwgfx_task_continuation *continuation);

void drm_vmwgfx_continuation_queue_release(
    struct drm_vmwgfx_task_continuation *continuation);

int64_t drm_vmwgfx_prepare_ioctl_continuation(
    struct task *task, struct task_file *syscall_file, struct drm_file *file,
    uint32_t command, uint64_t user_argument, void *data, uint32_t data_bytes,
    bool *handled, bool *skip_input);

bool drm_vmwgfx_task_uninterruptible(const struct task *task);

bool drm_vmwgfx_task_signal_busy(const struct task *task);

int drm_vmwgfx_fence_signaled(struct drm_file *file,
                                     struct drm_vmw_fence_signaled_arg *arg);

void drm_vmwgfx_fence_wait_finish(
    struct task *task, struct drm_vmwgfx_task_continuation *continuation);

int64_t drm_vmwgfx_fence_wait(
    struct task *task, struct drm_file *file, uint32_t command,
    struct drm_vmw_fence_wait_arg *arg, uint64_t user_argument,
    struct drm_faildiag_context *diagnostics);

int64_t drm_vmwgfx_execbuf_ioctl(struct task *task, struct drm_file *file,
                                 uint32_t command, void *data,
                                 uint64_t user_argument,
                                 struct drm_faildiag_context *diagnostics);

void drm_vmwgfx_execbuf_file_release(struct drm_file *file);

void drm_vmwgfx_cancel_task_continuation(struct task *task);

#endif
