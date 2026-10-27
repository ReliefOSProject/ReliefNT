/* Trusted Ring-0 legacy SVGA3D API. No pointers in this API are user pointers. */
#ifndef RELIEFNT_SVGA_H
#define RELIEFNT_SVGA_H
#include <reliefnt/types.h>
#include <reliefos/gpu_abi.h>
struct module;

/**
 * @brief Decode supported SVGA storage geometry and its capability selector.
 * @param format Native format code.
 * @param bw Required writable block width.
 * @param bh Required writable block height.
 * @param bytes Required writable bytes per block.
 * @param cap Required writable capability selector.
 * @return Zero, EINVAL for missing output, or ENOTSUP for unsupported format; non-NULL outputs receive defaults before format decoding.
 * Pure kernel query, no lock, hardware access, allocation or ownership transfer.
 */
int svga_format(uint32_t format, uint32_t *bw, uint32_t *bh, uint32_t *bytes, uint32_t *cap);

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

/**
 * @brief Copy current SVGA capabilities, generation and live resource counts.
 * @param out Writable kernel snapshot, nullable; NULL is ignored.
 * Task context for module callers; acquires the SVGA lock internally and copies core-owned state without transferring resource ownership.
 */
void svga_get_info(struct svga_info *out);
/**
 * @brief Copy one cached host capability after applying supported local bounds.
 * @param index SVGA3D capability selector.
 * @param out Required writable kernel value, unchanged for an unavailable capability.
 * @return Zero or SVGA_ENOTSUP for an unavailable selector/NULL output.
 * Task context for module callers; acquires the SVGA lock internally. No probing, allocation, backing reference or pointer escapes.
 */
int svga3d_capability(uint32_t index, uint32_t *out);
/**
 * @brief Snapshot supported host operations for one SVGA surface format.
 * @param format Native SVGA format selector.
 * @param out Required writable operations mask, cleared before lock acquisition.
 * @return Zero, EBUSY for contention, EINVAL for NULL output, or the format capability error.
 * Task context for module callers; takes the SVGA lock internally. No allocation, retained resource or ownership transfer.
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
/** @brief Construct or progress a borrowed GMR with independent callback-code ownership.
 * @param phys Contiguous page-aligned backing base. @param bytes Backing length.
 * @param owner Required backing identity, transferred only on handle publication.
 * @param owner_put Final backing callback outside the device lock.
 * @param code_owner LIVE callback module or NULL for permanent code.
 * @param out Required in/out native handle, zero on initial creation.
 * @return Zero when ready, EBUSY while progressing, or SVGA error. Task/execution
 * ownership required. Unpublished errors balance the tentative code pin; a
 * published GMR keeps it through final hardware fence and callback return.
 * Task/execution context for module callers; never enter with the SVGA lock held.
 * Caller retains resources/backing and its independent module call pin through
 * return, and through the real fence or successful teardown when asynchronous.
 */
int svga_gmr_borrowed_attach(uint64_t phys, uint32_t bytes, void *owner,
                             void (*owner_put)(void *owner), struct module *code_owner,
                             svga_handle *out);
/**
 * @brief Resolve a fully mapped borrowed attachment to its backend MOB index.
 * @param buffer Original borrowed GMR handle.
 * @param mobid Required writable index, initialized to UINT32_MAX.
 * @return Zero, EBUSY for contention/mapping/retirement progress, or EINVAL for invalid input/handle.
 * Execution-owned task context; try-lock only and no reference transfer. Caller keeps the borrowed backing and callback owner alive until final detach.
 */
int svga_gmr_borrowed_mob_id(svga_handle buffer, uint32_t *mobid);
/** @brief Retire hardware backing before invoking and releasing its original callback.
 * @param buffer Exact borrowed GMR handle, including partially mapped objects.
 * @return Zero after final callback return, EBUSY while hardware/fences progress,
 * or SVGA error preserving ownership. Task/execution-owned context; callback and
 * final module put run outside the device lock. Code stays pinned through the
 * complete callback, even when that callback drops its own last object reference.
 * Task/execution context for module callers; never enter with the SVGA lock held.
 * Caller retains resources/backing and its independent module call pin through
 * return, and through the real fence or successful teardown when asynchronous.
 */
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
/**
 * @brief Resolve a current native resource handle to its backend index.
 * @param resource Generation-tagged context, surface or GMR handle.
 * @return Current resource index, or UINT32_MAX for invalid/stale handles.
 * Task context; acquires the SVGA lock internally. Returns only a snapshot index, not a retained resource reference.
 */
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
/** @brief Queue a real fence-only FIFO submission without blocking.
 * @param out Writable generation-tagged token for the newly issued fence.
 * @return Zero on commit, SVGA_EBUSY for transient lock/FIFO pressure, or a
 *         negative SVGA error. A failed call publishes no FIFO words.
 * Task/execution context for module callers; never enter with the SVGA lock held.
 * Caller retains resources/backing and its independent module call pin through
 * return, and through the real fence or successful teardown when asynchronous.
 */
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
 * @brief Initialize bounded batch storage and source-sized heap metadata.
 * @param batch Writable caller-owned batch state.
 * @param generation Device generation captured before preparation.
 * @param storage Caller-owned writable DWORD storage.
 * @param output_capacity Storage capacity in bytes, at most 256 KiB.
 * @param source_bytes Accepted source size in bytes, at most 64 KiB.
 * @return Zero on success, SVGA_EINVAL for invalid bounds, or SVGA_ENOMEM for
 *         metadata reservation/allocation failure.
 * Task/execution context for module callers; never enter with the SVGA lock held.
 * Caller retains resources/backing and its independent module call pin through
 * return, and through the real fence or successful teardown when asynchronous.
 */
int svga_batch_init_sized(struct svga_batch *batch, uint32_t generation,
                          uint32_t *storage, uint32_t output_capacity,
                          uint32_t source_bytes);
/**
 * @brief Abort staged work and reclaim all unpublished shader backing.
 * @param batch Open unsubmitted batch to discard.
 * @return Zero after release or SVGA_EBUSY if the first lock attempt fails.
 * Task/execution context for module callers; never enter with the SVGA lock held.
 * Caller retains resources/backing and its independent module call pin through
 * return, and through the real fence or successful teardown when asynchronous.
 */
int svga_batch_abort(struct svga_batch *batch);
/**
 * @brief Copy one trusted packet into a batch without publishing FIFO work.
 * @param batch Open batch owning the destination storage.
 * @param command FIFO command identifier.
 * @param three_d True when the packet uses the 8-byte SVGA3D header.
 * @param payload Trusted readable DWORD-aligned command payload.
 * @param bytes Payload length in bytes.
 * @return Zero on append, SVGA_ENOMEM for output exhaustion, or SVGA_EINVAL
 *         for malformed batch/payload state.
 * Task/execution context for module callers; never enter with the SVGA lock held.
 * Caller retains resources/backing and its independent module call pin through
 * return, and through the real fence or successful teardown when asynchronous.
 */
int svga_batch_append(struct svga_batch *batch, uint32_t command, bool three_d,
                      const void *payload, uint32_t bytes);
/**
 * @brief Submit an open batch atomically and release metadata after commit.
 * @param batch Caller-owned batch prepared for the current generation.
 * @param fence Writable output for the issued fence token.
 * @return Zero after commit, SVGA_EBUSY for retryable pressure, or a negative
 *         SVGA error; an unsubmitted batch remains available for retry/abort.
 * Task/execution context for module callers; never enter with the SVGA lock held.
 * Caller retains resources/backing and its independent module call pin through
 * return, and through the real fence or successful teardown when asynchronous.
 */
int svga_batch_submit(struct svga_batch *batch, svga_handle *fence);
/**
 * @brief Query real FIFO completion for a generation-tagged fence token.
 * @param fence Previously issued fence token.
 * @param signaled Writable completion output, false for an unfinished token.
 * @return Zero on a valid issued token, SVGA_EINVAL for an unissued token,
 *         SVGA_EIO for stale generation, or a negative SVGA error.
 * Task/execution context for module callers; never enter with the SVGA lock held.
 * Caller retains resources/backing and its independent module call pin through
 * return, and through the real fence or successful teardown when asynchronous.
 */
int svga_fence_status(svga_handle fence, bool *signaled);
/**
 * @brief Read generation and raw issued/passed fence progress under svga_lock.
 * @param out Writable snapshot output.
 * @return Zero on success or a negative SVGA error for lock/FIFO state.
 * Task/execution context for module callers; never enter with the SVGA lock held.
 * Caller retains resources/backing and its independent module call pin through
 * return, and through the real fence or successful teardown when asynchronous.
 */
int svga_fence_progress_snapshot(struct svga_fence_progress *out);
/**
 * @brief Test recorded reset-era hardware quiescence for a generation-tagged fence.
 * @param fence Original nonzero generation/sequence token.
 * @return True only for a covered quiescence record; false for invalid tokens, missing proof or lock contention.
 * Task context for module callers; try-lock only, no wait or allocation. False never authorizes releasing DMA backing.
 */
bool svga_fence_is_quiesced(svga_handle fence);
/**
 * @brief Record a deduplicated context handle for submit-time validation.
 * @param batch Exclusively owned open batch with allocated metadata.
 * @param context Borrowed nonzero handle, externally kept alive through submission/completion.
 * @return Zero, EINVAL for malformed batch/input, or ENOMEM for metadata capacity exhaustion.
 * Execution-owned task context, exclusive batch mutation. This records a handle; the caller separately retains backing/code ownership through the real GPU fence.
 */
int svga_batch_retain_context(struct svga_batch *batch, svga_handle context);
/**
 * @brief Record a deduplicated surface handle for submit-time validation.
 * @param batch Exclusively owned open batch with allocated metadata.
 * @param surface Borrowed nonzero surface, externally retained through completion.
 * @return Zero, EINVAL for malformed batch/input, or ENOMEM for metadata capacity exhaustion.
 * Execution-owned task context, exclusive batch mutation. Caller retains the native surface and backing through the actual submission fence.
 */
int svga_batch_retain_surface(struct svga_batch *batch, svga_handle surface);
/**
 * @brief Record a deduplicated GMR handle for submit-time validation.
 * @param batch Exclusively owned open batch with allocated metadata.
 * @param buffer Borrowed nonzero GMR, externally retained through completion.
 * @return Zero, EINVAL for malformed batch/input, or ENOMEM for metadata capacity exhaustion.
 * Execution-owned task context, exclusive batch mutation. No independent GMR/backing/code reference is acquired by this metadata operation.
 */
int svga_batch_retain_gmr(struct svga_batch *batch, svga_handle buffer);
/**
 * @brief Stage a transaction-owned render-target binding without FIFO publication.
 * @param batch Exclusively owned open batch.
 * @param context Externally retained native context.
 * @param target Render-target slot.
 * @param surface Externally retained surface or supported unbind selector.
 * @param face Surface face selector.
 * @param mipmap Mipmap level selector.
 * @return Zero when staged or SVGA validation/contention/capacity error.
 * Execution-owned task context, no caller SVGA lock. Caller retains resources until commit/abort and the actual fence; failed staging publishes no hardware packet.
 */
int svga_batch_stage_render_target(struct svga_batch *batch, svga_handle context,
                                   uint32_t target, svga_handle surface,
                                   uint32_t face, uint32_t mipmap);
/**
 * @brief Stage a transaction-owned texture binding without FIFO publication.
 * @param batch Exclusively owned open batch.
 * @param context Externally retained native context.
 * @param stage Texture stage selector.
 * @param surface Externally retained surface or supported unbind selector.
 * @return Zero when staged or SVGA validation/contention/capacity error.
 * Execution-owned task context, no caller SVGA lock. Resource ownership remains with the caller through submission/fence or abort.
 */
int svga_batch_stage_texture(struct svga_batch *batch, svga_handle context,
                             uint32_t stage, svga_handle surface);
/** @brief Define/destroy VGPU9 shader compatibility keys without waiting. */
int svga3d_shader_define(svga_handle context, uint32_t local_id, uint32_t type,
                         const void *bytecode, uint32_t bytes);
int svga3d_shader_destroy(svga_handle context, uint32_t local_id, uint32_t type);
/**
 * @brief Stage an ordered shader define with copied bytecode and reserved backing.
 * @param batch Open batch owning the sidecar and translated packets.
 * @param context Live context reserved until commit or abort.
 * @param local_id VGPU9-local shader identifier.
 * @param type Vertex or pixel shader type.
 * @param bytecode Trusted readable kernel bytecode copied outside svga_lock.
 * @param bytes DWORD-aligned bytecode length within host and page limits.
 * @return Zero when staged, SVGA_EBUSY on lock contention, SVGA_ENOMEM for
 *         resource/capacity exhaustion, or SVGA_EINVAL for invalid input/key.
 * Task/execution context for module callers; never enter with the SVGA lock held.
 * Caller retains resources/backing and its independent module call pin through
 * return, and through the real fence or successful teardown when asynchronous.
 */
int svga_batch_shader_define(struct svga_batch *batch, svga_handle context,
                             uint32_t local_id, uint32_t type,
                             const void *bytecode, uint32_t bytes);
/**
 * @brief Stage unbind and destruction of the current transaction-visible version.
 * @param batch Open batch owning ordered shader events.
 * @param context Live context reserved until commit or abort.
 * @param local_id VGPU9-local shader identifier resolved in stream order.
 * @param type Vertex or pixel shader type.
 * @return Zero when staged, SVGA_EBUSY on lock contention, SVGA_ENOMEM for
 *         capacity exhaustion, or SVGA_EINVAL when no visible version exists.
 * Task/execution context for module callers; never enter with the SVGA lock held.
 * Caller retains resources/backing and its independent module call pin through
 * return, and through the real fence or successful teardown when asynchronous.
 */
int svga_batch_shader_destroy(struct svga_batch *batch, svga_handle context,
                              uint32_t local_id, uint32_t type);
/**
 * @brief Stage an ordered VGPU9 shader bind or unbind in a trusted batch.
 * @param batch Open batch that owns translated packets and operation metadata.
 * @param context Live context reserved until the batch commits or aborts.
 * @param type Vertex or pixel shader type.
 * @param local_id VGPU9-local key, or SVGA3D_INVALID_ID to unbind.
 * @return Zero when staged, SVGA_EBUSY for lock contention, SVGA_ENOMEM for
 *         metadata/output exhaustion, or SVGA_EINVAL for an unresolved key.
 * Task/execution context for module callers; never enter with the SVGA lock held.
 * Caller retains resources/backing and its independent module call pin through
 * return, and through the real fence or successful teardown when asynchronous.
 */
int svga_batch_bind_shader(struct svga_batch *batch, svga_handle context,
                           uint32_t type, uint32_t local_id);
/**
 * @brief Translate and append one inline VGPU9 shader-constant update.
 * @param batch Open batch that owns translated output.
 * @param context Live shader context retained through the submission fence.
 * @param reg First register index.
 * @param type Vertex or pixel shader type.
 * @param const_type Float, integer, or boolean constant type.
 * @param values Trusted readable DWORD values, four per register.
 * @param registers Number of consecutive 16-byte registers.
 * @return Zero on append, SVGA_ENOMEM on output exhaustion, SVGA_EBUSY on lock
 *         contention, or SVGA_EINVAL for invalid register ranges or input.
 * Task/execution context for module callers; never enter with the SVGA lock held.
 * Caller retains resources/backing and its independent module call pin through
 * return, and through the real fence or successful teardown when asynchronous.
 */
int svga_batch_shader_constants(struct svga_batch *batch, svga_handle context,
                                uint32_t reg, uint32_t type, uint32_t const_type,
                                const uint32_t *values, uint32_t registers);
/**
 * @brief Stage a GB query BEGIN into an open trusted batch.
 * @param batch Open batch whose source bound covers this operation.
 * @param context Live SVGA context handle.
 * @param type Supported GB query type.
 * @return Zero when staged or a negative SVGA error for lock/state/capacity.
 * Task/execution context for module callers; never enter with the SVGA lock held.
 * Caller retains resources/backing and its independent module call pin through
 * return, and through the real fence or successful teardown when asynchronous.
 */
int svga_batch_query_begin(struct svga_batch *batch, svga_handle context,
                           uint32_t type);
/**
 * @brief Stage a GB query END and retain its result attachment through WAIT.
 * @param batch Open batch whose source bound covers this operation.
 * @param context Live SVGA context handle.
 * @param type Supported GB query type.
 * @param result Fully mapped borrowed GMR holding the result.
 * @param offset DWORD-aligned result byte offset.
 * @return Zero when staged or a negative SVGA error for lock/state/capacity.
 * Task/execution context for module callers; never enter with the SVGA lock held.
 * Caller retains resources/backing and its independent module call pin through
 * return, and through the real fence or successful teardown when asynchronous.
 */
int svga_batch_query_end(struct svga_batch *batch, svga_handle context,
                         uint32_t type, svga_handle result, uint32_t offset);
/**
 * @brief Stage a GB query WAIT matched to one pending or same-batch END.
 * @param batch Open batch whose source bound covers this operation.
 * @param context Live SVGA context handle.
 * @param type Supported GB query type.
 * @param result Fully mapped borrowed GMR holding the result.
 * @param offset DWORD-aligned result byte offset.
 * @return Zero when staged or a negative SVGA error for an unmatched result.
 * Task/execution context for module callers; never enter with the SVGA lock held.
 * Caller retains resources/backing and its independent module call pin through
 * return, and through the real fence or successful teardown when asynchronous.
 */
int svga_batch_query_wait(struct svga_batch *batch, svga_handle context,
                          uint32_t type, svga_handle result, uint32_t offset);
/**
 * @brief Free shader backing records only after their real FIFO fence passes.
 * @param budget Maximum number of records to reclaim; capped to host slots.
 * @param reaped Writable count of records detached and freed.
 * @return Zero on success, SVGA_EINVAL for null output, or SVGA_EBUSY if the
 *         lock is contended before any record is detached.
 * Task/execution context for module callers; never enter with the SVGA lock held.
 * Caller retains resources/backing and its independent module call pin through
 * return, and through the real fence or successful teardown when asynchronous.
 */
int svga_backend_reap_retired(uint32_t budget, uint32_t *reaped);
/**
 * @brief Publish a new native context with a real readiness fence and mode lease.
 * @param out Required owned-handle output, cleared before validation.
 * @return Zero with an owned handle, EBUSY for lock/FIFO pressure, or a validation/device/allocation error.
 * Execution-owned task context; no caller SVGA lock. Successful initialization may still await its readiness fence. Caller retains the handle and retries try_destroy until zero before releasing ownership.
 */
int svga3d_context_try_create(svga_handle *out);
/**
 * @brief Retire a context only after its queries, shaders and FIFO work quiesce.
 * @param context Owned native context, including retirement already in progress.
 * @return Zero after backing/lease release, EBUSY while active or fenced work remains, or a device/validation error.
 * Execution-owned task context; no caller SVGA lock. Busy/errors retain backing and ownership, requiring retry; no callback into optional module code is stored.
 */
int svga3d_context_try_destroy(svga_handle context);
/**
 * @brief Publish a validated native surface and its readiness fence/mode lease.
 * @param desc Borrowed validated kernel descriptor copied on success.
 * @param out Required owned-handle output, cleared before validation.
 * @return Zero with an owned handle, EBUSY for lock/FIFO pressure, or validation/device/allocation error.
 * Execution-owned task context; no caller SVGA lock. Retain the handle/backing through readiness and until try_destroy confirms final hardware retirement.
 */
int svga3d_surface_try_create(const struct svga_surface_desc *desc, svga_handle *out);
/**
 * @brief Retire an unbound surface after actual FIFO completion.
 * @param surface Owned surface handle, including pending retirement.
 * @return Zero after backing/lease release, EBUSY while referenced or fenced, or a device/validation error.
 * Execution-owned task context; no caller SVGA lock. Busy/errors preserve ownership; successful final destruction is the only permission to drop external backing/code pins.
 */
int svga3d_surface_try_destroy(svga_handle surface);
/**
 * @brief Queue only a PRESENT packet and fence, with no generic batch sidecar.
 * @param surface Caller-pinned, live source surface.
 * @param rects Trusted kernel rectangles, each fitting the source and scanout.
 * @param count Number of rectangles, between one and 64.
 * @param fence Output token, cleared before validation.
 * @return Zero after publication, SVGA_EBUSY for transient contention, or an
 *         unchanged negative SVGA status without publishing any packet.
 * Task/execution context for module callers; never enter with the SVGA lock held.
 * Caller retains resources/backing and its independent module call pin through
 * return, and through the real fence or successful teardown when asynchronous.
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
