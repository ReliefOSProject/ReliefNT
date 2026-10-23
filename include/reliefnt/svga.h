/* Trusted Ring-0 legacy SVGA3D API. No pointers in this API are user pointers. */
#ifndef RELIEFNT_SVGA_H
#define RELIEFNT_SVGA_H
#include <reliefnt/types.h>
#include <reliefos/gpu_abi.h>

typedef uint64_t svga_handle;
#define SVGA_INVALID_HANDLE 0ULL
#define SVGA_OK 0
#define SVGA_EIO (-5)
#define SVGA_ENOMEM (-12)
#define SVGA_EBUSY (-16)
#define SVGA_ENODEV (-19)
#define SVGA_EINVAL (-22)
#define SVGA_ENOTSUP (-95)
#define SVGA_ETIMEDOUT (-110)
#define SVGA_BATCH_MAX_INPUT_BYTES 65536u
#define SVGA_BATCH_MAX_OUTPUT_BYTES 262144u
/* Source-compatible name for the original command-buffer limit. */
#define SVGA_BATCH_MAX_BYTES SVGA_BATCH_MAX_INPUT_BYTES
#define SVGA_BATCH_MAX_REFS 32u /* Deprecated; typed limits below are authoritative. */
#define SVGA_BATCH_MAX_CONTEXT_REFS 64u
#define SVGA_BATCH_MAX_SURFACE_REFS 256u
#define SVGA_BATCH_MAX_GMR_REFS 128u
#define SVGA_BATCH_MAX_QUERY_OPS (SVGA_BATCH_MAX_INPUT_BYTES / 8u)
#define SVGA_BATCH_SUBMIT_FAILURE_SNAPSHOT 1u
#define SVGA_BATCH_SUBMIT_INDEX_NONE UINT32_MAX

enum svga_batch_submit_reason {
    SVGA_BATCH_SUBMIT_REASON_NONE = 0,
    SVGA_BATCH_SUBMIT_REASON_BATCH_SHAPE,
    SVGA_BATCH_SUBMIT_REASON_GENERATION,
    SVGA_BATCH_SUBMIT_REASON_RESOURCE,
    SVGA_BATCH_SUBMIT_REASON_BINDING,
    SVGA_BATCH_SUBMIT_REASON_SHADER,
    SVGA_BATCH_SUBMIT_REASON_QUERY,
    SVGA_BATCH_SUBMIT_REASON_FIFO_ARGUMENT,
    SVGA_BATCH_SUBMIT_REASON_FIFO_RING,
    SVGA_BATCH_SUBMIT_REASON_FIFO_STATIC_FIT,
    SVGA_BATCH_SUBMIT_REASON_FIFO_TRANSIENT,
    SVGA_BATCH_SUBMIT_REASON_DEVICE,
};

/* detail0 is one of these numeric checks; detail1/detail2 carry the relevant
 * observed and expected values or resource IDs for that check. */
enum svga_batch_submit_detail {
    SVGA_BATCH_SUBMIT_DETAIL_NONE = 0,
    SVGA_BATCH_SUBMIT_DETAIL_BATCH_NULL,
    SVGA_BATCH_SUBMIT_DETAIL_FENCE_NULL,
    SVGA_BATCH_SUBMIT_DETAIL_BATCH_SUBMITTED,
    SVGA_BATCH_SUBMIT_DETAIL_BATCH_EMPTY,
    SVGA_BATCH_SUBMIT_DETAIL_BATCH_METADATA,
    SVGA_BATCH_SUBMIT_DETAIL_CONTEXT_COUNT,
    SVGA_BATCH_SUBMIT_DETAIL_SURFACE_COUNT,
    SVGA_BATCH_SUBMIT_DETAIL_GMR_COUNT,
    SVGA_BATCH_SUBMIT_DETAIL_SHADER_OP_COUNT,
    SVGA_BATCH_SUBMIT_DETAIL_BINDING_OP_COUNT,
    SVGA_BATCH_SUBMIT_DETAIL_QUERY_OP_COUNT,
    SVGA_BATCH_SUBMIT_DETAIL_OPERATION_COUNT,
    SVGA_BATCH_SUBMIT_DETAIL_GENERATION,
    SVGA_BATCH_SUBMIT_DETAIL_CONTEXT_MISSING,
    SVGA_BATCH_SUBMIT_DETAIL_CONTEXT_RETIRING,
    SVGA_BATCH_SUBMIT_DETAIL_SURFACE_MISSING,
    SVGA_BATCH_SUBMIT_DETAIL_SURFACE_RETIRING,
    SVGA_BATCH_SUBMIT_DETAIL_GMR_MISSING,
    SVGA_BATCH_SUBMIT_DETAIL_GMR_RETIRING,
    SVGA_BATCH_SUBMIT_DETAIL_GMR_MAP_INFLIGHT,
    SVGA_BATCH_SUBMIT_DETAIL_GMR_MOB_UNDEFINED,
    SVGA_BATCH_SUBMIT_DETAIL_GMR_PAGES_INCOMPLETE,
    SVGA_BATCH_SUBMIT_DETAIL_BINDING_METADATA,
    SVGA_BATCH_SUBMIT_DETAIL_BINDING_CONTEXT_MISSING,
    SVGA_BATCH_SUBMIT_DETAIL_BINDING_TRANSACTION,
    SVGA_BATCH_SUBMIT_DETAIL_BINDING_CONTEXT_OWNER,
    SVGA_BATCH_SUBMIT_DETAIL_BINDING_CONTEXT_RETIRING,
    SVGA_BATCH_SUBMIT_DETAIL_BINDING_INDEX,
    SVGA_BATCH_SUBMIT_DETAIL_BINDING_KIND,
    SVGA_BATCH_SUBMIT_DETAIL_BINDING_CLEAR_FIELDS,
    SVGA_BATCH_SUBMIT_DETAIL_BINDING_SURFACE_MISSING,
    SVGA_BATCH_SUBMIT_DETAIL_BINDING_SURFACE_RETIRING,
    SVGA_BATCH_SUBMIT_DETAIL_BINDING_FACE_RANGE,
    SVGA_BATCH_SUBMIT_DETAIL_BINDING_MIP_RANGE,
    SVGA_BATCH_SUBMIT_DETAIL_SHADER_CONTEXT_MISSING,
    SVGA_BATCH_SUBMIT_DETAIL_SHADER_TRANSACTION,
    SVGA_BATCH_SUBMIT_DETAIL_SHADER_CONTEXT_OWNER,
    SVGA_BATCH_SUBMIT_DETAIL_SHADER_TYPE,
    SVGA_BATCH_SUBMIT_DETAIL_SHADER_UNBIND_LOCAL_ID,
    SVGA_BATCH_SUBMIT_DETAIL_SHADER_KIND,
    SVGA_BATCH_SUBMIT_DETAIL_SHADER_SLOT,
    SVGA_BATCH_SUBMIT_DETAIL_SHADER_DEFINE_RESERVED,
    SVGA_BATCH_SUBMIT_DETAIL_SHADER_DEFINE_OWNER,
    SVGA_BATCH_SUBMIT_DETAIL_SHADER_DEFINE_CONTEXT,
    SVGA_BATCH_SUBMIT_DETAIL_SHADER_DEFINE_LOCAL_ID,
    SVGA_BATCH_SUBMIT_DETAIL_SHADER_DEFINE_TYPE,
    SVGA_BATCH_SUBMIT_DETAIL_SHADER_DEFINE_CODE,
    SVGA_BATCH_SUBMIT_DETAIL_SHADER_RESOURCE_STATE,
    SVGA_BATCH_SUBMIT_DETAIL_SHADER_RESOURCE_RETIRING,
    SVGA_BATCH_SUBMIT_DETAIL_SHADER_RESOURCE_CONTEXT,
    SVGA_BATCH_SUBMIT_DETAIL_SHADER_RESOURCE_LOCAL_ID,
    SVGA_BATCH_SUBMIT_DETAIL_SHADER_RESOURCE_TYPE,
    SVGA_BATCH_SUBMIT_DETAIL_SHADER_RESOURCE_OWNER,
    SVGA_BATCH_SUBMIT_DETAIL_SHADER_DEFINE_COUNT,
    SVGA_BATCH_SUBMIT_DETAIL_QUERY_METADATA,
    SVGA_BATCH_SUBMIT_DETAIL_QUERY_CONTEXT_RANGE,
    SVGA_BATCH_SUBMIT_DETAIL_QUERY_CONTEXT_MISSING,
    SVGA_BATCH_SUBMIT_DETAIL_QUERY_KIND,
    SVGA_BATCH_SUBMIT_DETAIL_QUERY_BEGIN_ACTIVE,
    SVGA_BATCH_SUBMIT_DETAIL_QUERY_END_NOT_ACTIVE,
    SVGA_BATCH_SUBMIT_DETAIL_QUERY_END_GMR_RANGE,
    SVGA_BATCH_SUBMIT_DETAIL_QUERY_END_GMR_KIND,
    SVGA_BATCH_SUBMIT_DETAIL_QUERY_END_GMR_RETIRING,
    SVGA_BATCH_SUBMIT_DETAIL_QUERY_END_GMR_MOB,
    SVGA_BATCH_SUBMIT_DETAIL_QUERY_END_GMR_MAP,
    SVGA_BATCH_SUBMIT_DETAIL_QUERY_END_GMR_PAGES,
    SVGA_BATCH_SUBMIT_DETAIL_QUERY_END_OFFSET,
    SVGA_BATCH_SUBMIT_DETAIL_QUERY_END_RESULT_RANGE,
    SVGA_BATCH_SUBMIT_DETAIL_QUERY_END_CAPACITY,
    SVGA_BATCH_SUBMIT_DETAIL_QUERY_END_PENDING_CAPACITY,
    SVGA_BATCH_SUBMIT_DETAIL_QUERY_END_KEY,
    SVGA_BATCH_SUBMIT_DETAIL_QUERY_END_STAGED_COUNT,
    SVGA_BATCH_SUBMIT_DETAIL_QUERY_WAIT_GMR_RANGE,
    SVGA_BATCH_SUBMIT_DETAIL_QUERY_WAIT_GMR_KIND,
    SVGA_BATCH_SUBMIT_DETAIL_QUERY_WAIT_GMR_RETIRING,
    SVGA_BATCH_SUBMIT_DETAIL_QUERY_WAIT_GMR_MOB,
    SVGA_BATCH_SUBMIT_DETAIL_QUERY_WAIT_GMR_MAP,
    SVGA_BATCH_SUBMIT_DETAIL_QUERY_WAIT_GMR_PAGES,
    SVGA_BATCH_SUBMIT_DETAIL_QUERY_WAIT_OFFSET,
    SVGA_BATCH_SUBMIT_DETAIL_QUERY_WAIT_RESULT_RANGE,
    SVGA_BATCH_SUBMIT_DETAIL_QUERY_WAIT_KEY,
    SVGA_BATCH_SUBMIT_DETAIL_QUERY_WAIT_NO_MATCH,
    SVGA_BATCH_SUBMIT_DETAIL_FIFO_ARGUMENT_WORDS,
    SVGA_BATCH_SUBMIT_DETAIL_FIFO_ARGUMENT_ALIGNMENT,
    SVGA_BATCH_SUBMIT_DETAIL_FIFO_ARGUMENT_FENCE,
    SVGA_BATCH_SUBMIT_DETAIL_FIFO_ARGUMENT_SIZE,
    SVGA_BATCH_SUBMIT_DETAIL_FIFO_FENCE_UNSUPPORTED,
    SVGA_BATCH_SUBMIT_DETAIL_FIFO_RING_NOT_READY,
    SVGA_BATCH_SUBMIT_DETAIL_FIFO_RING_NULL_STORAGE,
    SVGA_BATCH_SUBMIT_DETAIL_FIFO_RING_CONFIG_MIN,
    SVGA_BATCH_SUBMIT_DETAIL_FIFO_RING_STORAGE_SIZE,
    SVGA_BATCH_SUBMIT_DETAIL_FIFO_RING_MIN_REGISTER,
    SVGA_BATCH_SUBMIT_DETAIL_FIFO_RING_MAX_REGISTER,
    SVGA_BATCH_SUBMIT_DETAIL_FIFO_RING_ALIGNMENT,
    SVGA_BATCH_SUBMIT_DETAIL_FIFO_RING_NEXT_LOWER_BOUND,
    SVGA_BATCH_SUBMIT_DETAIL_FIFO_RING_STOP_LOWER_BOUND,
    SVGA_BATCH_SUBMIT_DETAIL_FIFO_RING_NEXT_UPPER_BOUND,
    SVGA_BATCH_SUBMIT_DETAIL_FIFO_RING_STOP_UPPER_BOUND,
    SVGA_BATCH_SUBMIT_DETAIL_FIFO_SIZE_OVERFLOW,
    SVGA_BATCH_SUBMIT_DETAIL_FIFO_STATIC_FIT,
    SVGA_BATCH_SUBMIT_DETAIL_FIFO_FENCE_WINDOW,
    SVGA_BATCH_SUBMIT_DETAIL_FIFO_FREE_BYTES,
    SVGA_BATCH_SUBMIT_DETAIL_DEVICE_UNAVAILABLE,
};

/** Numeric failure state owned by one batch; it contains no pointers or text. */
struct svga_batch_submit_failure {
    uint32_t reason, index;
    uint64_t detail0, detail1, detail2;
};

/* Trusted diagnostic snapshot of the last extended-FIFO probe, before rollback.
 * stage points to a static kernel string and must never enter the user ABI. */
struct svga_probe_info {
    const char *stage;
    int status;
    uint32_t fifo_caps, fifo_min, mem_regs, enable;
    uint32_t host_legacy, host_revised, devcap_3d;
    bool gb_objects;
};

struct svga_info {
    uint32_t device_caps, fifo_caps, host_version, guest_version;
    uint32_t generation, contexts, surfaces, gmrs, guest_pages;
    uint64_t surface_bytes, surface_limit_bytes;
    uint64_t compat_surface_limit_bytes;
    bool fifo_ready, available;
    struct svga_probe_info probe;
};

/* All faces have the same mip count; cube maps have six square faces.
 * Dimensions are in texels, except BUFFER width which is in bytes. */
struct svga_surface_desc {
    uint32_t format, flags, width, height, depth, mip_levels;
};

struct svga_dma_box {
    uint32_t x, y, z, w, h, d, srcx, srcy, srcz;
};
struct svga_present_rect { uint32_t x, y, w, h, srcx, srcy; };

struct svga_batch_query_op {
    uint32_t kind, context, gmr, offset;
    uint32_t match_kind, match_index, commit_slot;
};
enum svga_batch_shader_op_kind {
    SVGA_BATCH_SHADER_BIND = 1,
    SVGA_BATCH_SHADER_DEFINE = 2,
    SVGA_BATCH_SHADER_DESTROY = 3,
};
struct svga_batch_shader_op {
    svga_handle context;
    uint32_t kind, local_id, type, slot;
};
enum svga_batch_binding_kind {
    SVGA_BATCH_BINDING_RENDER_TARGET = 1,
    SVGA_BATCH_BINDING_TEXTURE = 2,
};
struct svga_batch_binding_op {
    svga_handle context, surface;
    uint32_t kind, index, face, mipmap;
};
struct svga_batch_metadata;
struct svga_fence_progress { uint32_t generation, issued_seq, passed_seq; };

/** Kernel-owned batch. Append copies every packet immediately; submit publishes
 * all packets plus its fence atomically. An EBUSY batch can be retried as-is.
 */
struct svga_batch {
    uint32_t generation, bytes, capacity, source_bytes;
    bool submitted;
    uint32_t *words;
    uint32_t context_count, surface_count, gmr_count;
    uint32_t query_op_count, shader_op_count, binding_op_count, operation_count;
    struct svga_batch_metadata *metadata;
    struct svga_batch_submit_failure submit_failure;
};

/** @brief Snapshot device capabilities and live resource counters into non-NULL out. */
void svga_get_info(struct svga_info *out);
/** @brief Read a host capability; return ENOTSUP when that index was not advertised. */
int svga3d_capability(uint32_t index, uint32_t *out);
/** @brief Return implemented host-positive VGPU9 format operations and host restrictions.
 * Positive operations are intersected with the local implementation. The
 * NOFILTER, NOALPHABLEND, and NOTEXCOORDWRAPNORMIP host restriction bits are
 * preserved so UAPI capability filtering cannot advertise forbidden use.
 * BUFFER is a special byte resource and returns an empty operation mask.
 */
int svga3d_format_operations(uint32_t format, uint32_t *out);
/** @brief Initialize extended FIFO after mm and kernel paging; 2D survives absent 3D. */
int svga3d_init(void);
/** @brief Drain and destroy all resources; on failure preserve them for a retry. */
int svga3d_shutdown(void);
/**
 * @brief Allocate a context and, for GB devices, its pinned state backing.
 * @param out Non-NULL handle storage. A nonzero handle on failure owns a partial
 * context that must be destroyed after the host recovers; it cannot render.
 * @return Zero or negative SVGA error; ownership persists until destroy succeeds.
 */
int svga3d_context_create(svga_handle *out);
/** @brief Destroy a context and its shaders/queries after outstanding work completes. */
int svga3d_context_destroy(svga_handle context);
/**
 * @brief Define all surface faces/mips and GB backing within host memory limits.
 * @param desc Trusted non-NULL dimensions, format and usage flags.
 * @param out Non-NULL handle storage. A nonzero handle on failure must be passed
 * to svga3d_surface_destroy after the host recovers; it cannot render.
 * @return Zero or negative SVGA error; unfinished retirement retains ownership.
 */
int svga3d_surface_create(const struct svga_surface_desc *desc, svga_handle *out);
/** @brief Unbind and destroy a surface; a failure retains ownership for a retry. */
int svga3d_surface_destroy(svga_handle surface);
/** @brief Allocate a DMA buffer, pin its pages and define a GMR; out receives ownership.
 * On a timeout while retiring a partially mapped GMR, out may contain a handle
 * that must be passed to svga_gmr_destroy() after the host responds.
 */
int svga_gmr_create(uint32_t bytes, svga_handle *out);
/** @brief Wait for all DMA, undefine the GMR, then free its pages. */
int svga_gmr_destroy(svga_handle buffer);
/** @brief Synchronize then copy bytes to or from a managed GMR; validates offset and size. */
int svga_gmr_transfer(svga_handle buffer, uint32_t offset, void *data,
                      uint32_t bytes, bool write);
/** @brief Attach an already-pinned, physically contiguous BO without copying it.
 * Call with *out=0 to start, then retry with the returned handle until zero is
 * returned. A nonzero handle transfers one retained owner reference to the
 * backend, including while the operation returns EBUSY. The put callback runs
 * only after GMR undefine, MOB destroy, and their real FIFO fence complete.
 * The attachment holds a mode-preservation lease until detach completes.
 * Detach/retry and the callback run in task context outside svga_lock; callers
 * must hold kernel execution ownership around that path. The callback must not
 * reacquire an ownership lock held by the caller.
 */
int svga_gmr_borrowed_attach(uint64_t phys, uint32_t bytes, void *owner,
                             void (*owner_put)(void *owner), svga_handle *out);
/** @brief Return the GB MOB ID for a fully mapped borrowed attachment. */
int svga_gmr_borrowed_mob_id(svga_handle buffer, uint32_t *mobid);
/** @brief Begin/retry nonblocking borrowed GMR and MOB retirement. */
int svga_gmr_borrowed_detach(svga_handle buffer);
/** @brief Copy image boxes between GMR and host surface; direction is SVGA3dTransferType. */
int svga3d_surface_dma(svga_handle surface, uint32_t face, uint32_t mip,
                       svga_handle buffer, uint32_t offset, uint32_t pitch,
                       uint32_t direction, const struct svga_dma_box *boxes,
                       uint32_t count);
/** @brief Bind a surface as a context render target. */
int svga3d_set_render_target(svga_handle context, uint32_t target_type,
                             svga_handle surface);
/** @brief Clear the currently bound color/depth/stencil targets. */
int svga3d_clear(svga_handle context, uint32_t flags, uint32_t color,
                 float depth, uint32_t stencil);
/** @brief Draw three fixed-pipeline vertices from a managed vertex surface. */
int svga3d_draw_triangle(svga_handle context, svga_handle vertex_surface,
                         svga_handle vertex_buffer, uint32_t target_width,
                         uint32_t target_height);
/** @brief Present rectangles from a managed surface to the legacy framebuffer. */
int svga3d_present(svga_handle surface, const struct svga_present_rect *rects,
                   uint32_t count);
/** @brief Return a managed object's wire ID, or UINT32_MAX for a stale/invalid handle. */
uint32_t svga_resource_id(svga_handle resource);
/** @brief Submit a trusted legacy state/draw/shader/query packet (without its 8-byte header).
 * @param generation Epoch from svga_get_info; rejects packets prepared before a reset.
 * @param command VMware legacy command ID; resource creation/destruction and DMA use managed APIs.
 * @param payload Readable, immutable kernel storage for the call; contains protocol wire IDs.
 * @param bytes Exact payload size, divisible by four. Callers must obey shader/state semantics.
 * @return Zero when queued; negative SVGA error on invalid layout, unsupported commands or timeout.
 * GB devices accept the shared fixed-pipeline commands; shader/query commands
 * requiring GB object translation return ENOTSUP. Pending GB retirement returns EBUSY.
 */
int svga3d_submit(uint32_t generation, uint32_t command, const void *payload, uint32_t bytes);
/** @brief Queue a fence; the returned token includes the device generation. */
int svga_fence_insert(svga_handle *out);
/** @brief Queue a real fence without waiting or publishing a command body. */
int svga_fence_try_insert(svga_handle *out);
/** @brief Wait with a finite polling budget independent of the interrupt clock. */
int svga_fence_wait(svga_handle fence);
/**
 * @brief Initialize a trusted batch using one bound for input and output.
 * @param batch Writable caller-owned batch state.
 * @param generation Device generation captured before preparing the batch.
 * @param storage Writable DWORD storage with at least capacity bytes.
 * @param capacity Input and translated-output capacity in bytes; at most 64 KiB.
 * @return Zero on success, SVGA_EINVAL for invalid bounds, or SVGA_ENOMEM if
 *         heap-backed operation metadata could not be allocated.
 */
int svga_batch_init(struct svga_batch *batch, uint32_t generation,
                    uint32_t *storage, uint32_t capacity);
/**
 * @brief Initialize a batch with separate source and translated-output bounds.
 * @param batch Writable caller-owned batch state.
 * @param generation Device generation captured before preparing the batch.
 * @param storage Writable DWORD storage with at least output_capacity bytes.
 * @param output_capacity Translated command capacity in bytes, at most 256 KiB.
 * @param source_bytes Accepted source snapshot size in bytes, at most 64 KiB;
 *        it bounds heap-backed per-operation metadata.
 * @return Zero on success, SVGA_EINVAL for invalid bounds, or SVGA_ENOMEM if
 *         heap-backed operation metadata could not be allocated.
 */
int svga_batch_init_sized(struct svga_batch *batch, uint32_t generation,
                          uint32_t *storage, uint32_t output_capacity,
                          uint32_t source_bytes);
/**
 * @brief Abort an unsubmitted batch and release its staging reservations.
 * @param batch Caller-owned batch to discard; it must not be submitted.
 * @return Zero after releasing staged backing, or SVGA_EBUSY if svga_lock is
 *         contended before the abort begins.
 */
int svga_batch_abort(struct svga_batch *batch);
/**
 * @brief Copy one trusted packet into a batch without publishing FIFO work.
 * @param batch Open batch owning translated-output storage.
 * @param command FIFO command identifier.
 * @param three_d True for an SVGA3D packet with an 8-byte header.
 * @param payload Trusted readable DWORD-aligned payload, excluding the header.
 * @param bytes Payload length in bytes.
 * @return Zero on append, SVGA_ENOMEM for output exhaustion, or SVGA_EINVAL
 *         for malformed batch/payload state.
 */
int svga_batch_append(struct svga_batch *batch, uint32_t command, bool three_d,
                      const void *payload, uint32_t bytes);
/**
 * @brief Atomically publish a batch and trailing fence without waiting.
 * @param batch Open batch prepared for its captured device generation.
 * @param fence Writable output for the real issued fence token.
 * @return Zero after commit, SVGA_EBUSY for retryable FIFO pressure, or a
 *         negative SVGA error; failed submissions publish no FIFO words.
 */
int svga_batch_submit(struct svga_batch *batch, svga_handle *fence);
/**
 * @brief Check real FIFO fence completion without waiting.
 * @param fence Previously issued generation-tagged fence token.
 * @param signaled Writable output; false means the fence has not passed.
 * @return Zero for a valid issued token, SVGA_EINVAL for unissued, SVGA_EIO for
 *         a stale generation, or a negative SVGA error for FIFO state.
 */
int svga_fence_status(svga_handle fence, bool *signaled);
/**
 * @brief Snapshot the current generation and raw FIFO fence progress.
 * @param out Writable output for generation, issued sequence, and passed sequence.
 * @return Zero on success, SVGA_EINVAL for a null output, SVGA_EBUSY on lock
 *         contention, SVGA_ENODEV when the FIFO is unavailable, or
 *         SVGA_ENOTSUP when fence support is absent.
 */
int svga_fence_progress_snapshot(struct svga_fence_progress *out);
/** @brief Test explicit evidence that an issued FIFO fence has completed.
 *
 * A stale generation by itself is not evidence of completion. This returns
 * true only when a successful FIFO-wide sync recorded a passed sequence in
 * the token's generation that covers this token.
 */
bool svga_fence_is_quiesced(svga_handle fence);
/** @brief Retain a live context through this batch's returned fence.
 * @param batch Open batch owning reference metadata.
 * @param context Nonzero context handle to retain.
 * @return Zero when retained/deduplicated or a negative SVGA error.
 */
int svga_batch_retain_context(struct svga_batch *batch, svga_handle context);
/** @brief Retain a live surface through this batch's returned fence.
 * @param batch Open batch owning reference metadata.
 * @param surface Nonzero surface handle to retain.
 * @return Zero when retained/deduplicated or a negative SVGA error.
 */
int svga_batch_retain_surface(struct svga_batch *batch, svga_handle surface);
/** @brief Retain a live GMR through this batch's returned fence.
 * @param batch Open batch owning reference metadata.
 * @param buffer Nonzero GMR handle to retain.
 * @return Zero when retained/deduplicated or a negative SVGA error.
 */
int svga_batch_retain_gmr(struct svga_batch *batch, svga_handle buffer);
/** @brief Stage one render-target binding for atomic commit with the batch. */
int svga_batch_stage_render_target(struct svga_batch *batch, svga_handle context,
                                   uint32_t target, svga_handle surface,
                                   uint32_t face, uint32_t mipmap);
/** @brief Stage one texture-stage binding; surface zero clears the stage. */
int svga_batch_stage_texture(struct svga_batch *batch, svga_handle context,
                             uint32_t stage, svga_handle surface);
/** @brief Define/destroy VGPU9 shader compatibility keys without waiting. */
int svga3d_shader_define(svga_handle context, uint32_t local_id, uint32_t type,
                         const void *bytecode, uint32_t bytes);
int svga3d_shader_destroy(svga_handle context, uint32_t local_id, uint32_t type);
/**
 * @brief Stage a copied shader version for atomic publication with its batch.
 * @param batch Open batch whose source bound covers this operation.
 * @param context Live shader context; its shader state is reserved until submit
 *        or abort.
 * @param local_id VGPU9-local program identifier.
 * @param type Vertex or pixel shader type.
 * @param bytecode Trusted readable kernel bytes copied before returning.
 * @param bytes Bytecode length in bytes, DWORD-aligned and within host limits.
 * @return Zero when staged, SVGA_EBUSY on lock contention, SVGA_ENOMEM for
 *         exhausted heap/host/page resources or output capacity, or SVGA_EINVAL
 *         for invalid bytecode or an occupied batch-local key.
 */
int svga_batch_shader_define(struct svga_batch *batch, svga_handle context,
                             uint32_t local_id, uint32_t type,
                             const void *bytecode, uint32_t bytes);
/**
 * @brief Stage unbind and destruction of the batch-visible shader version.
 * @param batch Open batch whose source bound covers this operation.
 * @param context Live shader context reserved until submit or abort.
 * @param local_id VGPU9-local program identifier resolved in stream order.
 * @param type Vertex or pixel shader type.
 * @return Zero when staged, SVGA_EBUSY on lock contention, SVGA_ENOMEM for
 *         exhausted operation/output capacity, or SVGA_EINVAL if no matching
 *         batch-visible shader version exists.
 */
int svga_batch_shader_destroy(struct svga_batch *batch, svga_handle context,
                              uint32_t local_id, uint32_t type);
/**
 * @brief Translate a VGPU9 shader bind or unbind into ordered batch state.
 * @param batch Open batch owning output and operation metadata.
 * @param context Live context reserved through commit or abort.
 * @param type Vertex or pixel shader type.
 * @param local_id VGPU9-local shader key, or SVGA3D_INVALID_ID to unbind.
 * @return Zero when staged or a negative SVGA error for lock/state/capacity.
 */
int svga_batch_bind_shader(struct svga_batch *batch, svga_handle context,
                           uint32_t type, uint32_t local_id);
/**
 * @brief Translate one inline VGPU9 shader-constant update into the batch.
 * @param batch Open batch owning translated output.
 * @param context Live context handle retained to the submission fence.
 * @param reg First constant register index.
 * @param type Vertex or pixel shader type.
 * @param const_type Float, integer, or boolean constant type.
 * @param values Trusted readable DWORD values, four per register.
 * @param registers Number of consecutive 16-byte registers.
 * @return Zero on append or a negative SVGA error for invalid ranges, lock
 *         contention, or output-capacity exhaustion.
 */
int svga_batch_shader_constants(struct svga_batch *batch, svga_handle context,
                                uint32_t reg, uint32_t type, uint32_t const_type,
                                const uint32_t *values, uint32_t registers);
/** @brief Stage a GB query BEGIN operation in an open batch.
 * @param batch Open batch owning query operation metadata.
 * @param context Live SVGA context handle.
 * @param type Supported GB query type.
 * @return Zero when staged or a negative SVGA error for lock/state/capacity.
 */
int svga_batch_query_begin(struct svga_batch *batch, svga_handle context,
                           uint32_t type);
/** @brief Stage a GB query END and retain its borrowed result GMR.
 * @param batch Open batch owning query operation metadata.
 * @param context Live SVGA context handle.
 * @param type Supported GB query type.
 * @param result Fully mapped borrowed GMR that stores the result.
 * @param offset DWORD-aligned byte offset of the result structure.
 * @return Zero when staged or a negative SVGA error for invalid/unavailable state.
 */
int svga_batch_query_end(struct svga_batch *batch, svga_handle context,
                         uint32_t type, svga_handle result, uint32_t offset);
/** @brief Stage WAIT matched to a committed pending result or same-batch END.
 * @param batch Open batch owning query operation metadata.
 * @param context Live SVGA context handle.
 * @param type Supported GB query type.
 * @param result Fully mapped borrowed GMR that stores the result.
 * @param offset DWORD-aligned byte offset of the result structure.
 * @return Zero when staged or a negative SVGA error if no matching END exists.
 */
int svga_batch_query_wait(struct svga_batch *batch, svga_handle context,
                          uint32_t type, svga_handle result, uint32_t offset);
/**
 * @brief Reclaim a bounded number of shader backings after their real fence.
 * @param budget Maximum records to detach; internally capped to the host-slot
 *        count. Each record is freed outside svga_lock in task context.
 * @param reaped Writable count of records actually reclaimed.
 * @return Zero on success, SVGA_EINVAL for a null output, or SVGA_EBUSY if the
 *         lock is contended before any record is reclaimed.
 */
int svga_backend_reap_retired(uint32_t budget, uint32_t *reaped);
/** @brief Nonblocking context and surface lifecycle operations for DRM.
 * Each successfully created resource holds a mode-preservation lease until its
 * retirement fence completes, so framebuffer resize cannot reset live objects.
 */
int svga3d_context_try_create(svga_handle *out);
int svga3d_context_try_destroy(svga_handle context);
int svga3d_surface_try_create(const struct svga_surface_desc *desc, svga_handle *out);
int svga3d_surface_try_destroy(svga_handle surface);
/**
 * @brief Publish a bounded PRESENT and real fence without allocating or waiting.
 * @param surface Live surface retained by the caller until the fence completes.
 * @param rects One to 64 scanout/source rectangles.
 * @param count Number of rectangles; source and destination must fit the surface.
 * @param fence Receives the issued token, or zero on failure.
 * @return Zero after atomic FIFO publication, SVGA_EBUSY for contention or
 *         backpressure, or a negative SVGA error without publishing commands.
 * Successful submission updates the surface's last-use fence. Its existing
 * resource lease preserves the mode; callers retain ownership until completion.
 */
int svga3d_present_try(svga_handle surface, const struct svga_present_rect *rects,
                      uint32_t count, svga_handle *fence);
/** @brief Keep fbdev mode changes from resetting live DRM render resources. */
int svga_mode_lease_acquire(void);
/** @brief Release a mode lease after all leased resources have retired. */
int svga_mode_lease_release(void);
/** @brief Render, Present and read back the fixed-pipeline test, then release all resources. */
int svga3d_triangle_test(void);

/**
 * @brief Allocate bounded offscreen resources private to the owner process.
 * @param owner Nonzero process ID that receives ownership.
 * @param request Trusted kernel request; receives a handle only on success.
 * @return Zero, or a negative SVGA error; failed DMA retirement remains tracked.
 */
int svga_gpu_create(uint32_t owner, struct reliefos_gpu_context *request);
/**
 * @brief Retire an owned context after its DMA completes, retaining failures for retry.
 * @param owner Process ID whose ownership is checked.
 * @param handle Opaque handle returned by create.
 * @return Zero or a negative SVGA error, including EINVAL for foreign handles.
 */
int svga_gpu_destroy(uint32_t owner, uint64_t handle);
/**
 * @brief Queue a frame on an owned double-buffered offscreen context and return a fenced readback.
 * @param owner Process ID whose ownership is checked.
 * @param frame Trusted kernel metadata; its encoded user addresses are ignored.
 * @param vertices Trusted array with frame->vertex_count elements.
 * @param draws Trusted array with frame->draw_count elements.
 * @param pixels Validated writable output of width*height uint32_t elements;
 * the caller must keep its mappings stable throughout the call.
 * @return Zero with top-down 00RRGGBB output; negative errors leave output unchanged.
 */
int svga_gpu_render(uint32_t owner, const struct reliefos_gpu_frame *frame,
                    const struct reliefos_gpu_vertex *vertices,
                    const struct reliefos_gpu_draw *draws, uint32_t *pixels);
/**
 * @brief Retire all process contexts; stalled DMA stays tracked for later cleanup.
 * @param owner Process ID whose resources are released.
 */
void svga_gpu_release_owner(uint32_t owner);
/**
 * @brief Snapshot cumulative virtual GPU timing and managed resource totals.
 * @param out Non-NULL writable kernel structure; receives size and version too.
 */
void svga_gpu_get_info(struct reliefos_gpu_info *out);

/** @brief Copy the latest render failure to its owner without device I/O. */
void svga_gpu_get_diagnostics(uint32_t owner, struct reliefos_gpu_diagnostics *out);

#endif
