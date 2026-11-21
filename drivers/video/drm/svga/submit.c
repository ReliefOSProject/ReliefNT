/* SPDX-License-Identifier: Apache-2.0 */
#include "submit.h"
#include "dma.h"
#include <reliefnt/console.h>
#include <reliefnt/mm.h>
#include "timeout.h"

/* vmwgfx_cmdbuf.c retires command storage on COMPLETED. vmwgfx_cmd.c
 * emits a fence for retirement of the resources referenced by the commands. */
#define CB_SLOTS (SVGA_CB_MAX_QUEUED_PER_CONTEXT - 1)
#define CB_WAIT_US 1000000u
#define CB_WAIT_POLLS (1u << 27)
#define CB_FENCE_BYTES (2 * sizeof(uint32_t))

struct svga_cb {
    uint64_t physical;
    uint32_t pages;
};

enum svga_cb_control {
    CB_CONTROL_NONE,
    CB_CONTROL_START,
    CB_CONTROL_STOP,
};

static struct {
    struct svga_cb slots[CB_SLOTS];
    struct svga_cb control, staged;
    uint64_t id;
    uint32_t sent, completed, limit, staged_bytes;
    unsigned head, tail, count;
    enum svga_cb_control control_op;
    bool enabled, quarantined;
} cb;

_Static_assert(sizeof(SVGACBHeader) == 64, "SVGA command buffer address alignment");

static int fail_queue(int error)
{
    cb.quarantined = true;
    svga.fifo.fault = true;
    return error;
}

static volatile SVGACBHeader *header(const struct svga_cb *buffer)
{
    return mm_phys_to_virt(buffer->physical);
}

static void *payload(const struct svga_cb *buffer)
{
    return (void *)(header(buffer) + 1);
}

static int reserve_buffer(struct svga_cb *buffer, uint32_t length, uint32_t preserve)
{
    uint32_t pages = (sizeof(SVGACBHeader) + length + SVGA_PAGE_SIZE - 1) >> SVGA_PAGE_SHIFT;
    if (pages <= buffer->pages)
        return 0;
    uint64_t physical = mm_alloc_pages(pages);
    if (!physical)
        return -ENOMEM;
    struct svga_cb replacement = {.physical = physical, .pages = pages};
    if (preserve)
        __builtin_memcpy(payload(&replacement), payload(buffer), preserve);
    if (buffer->pages)
        mm_free_pages(buffer->physical, buffer->pages);
    *buffer = replacement;
    return 0;
}

static void encode_header(struct svga_cb *buffer, uint32_t bytes, uint32_t dx_context,
                           uint32_t sequence)
{
    volatile SVGACBHeader *wire = header(buffer);
    if (sequence) {
        const uint32_t fence[] = {SVGA_CMD_FENCE, sequence};
        __builtin_memcpy((uint8_t *)payload(buffer) + bytes, fence, sizeof(fence));
    }
    *wire = (SVGACBHeader){
        .id = ++cb.id, .flags = svga.irq ? 0 : SVGA_CB_FLAG_NO_IRQ,
        .length = bytes + (sequence ? CB_FENCE_BYTES : 0),
        .ptr = {.pa = buffer->physical + sizeof(*wire)},
    };
    if (dx_context != SVGA3D_INVALID_ID) {
        wire->flags |= SVGA_CB_FLAG_DX_CONTEXT;
        wire->dxContext = dx_context;
    }
}

static SVGACBStatus status(const struct svga_cb *buffer)
{
    /* The host has finished reading command bytes when it completes this header. */
    SVGACBStatus result = header(buffer)->status;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return result;
}

static void doorbell(const struct svga_cb *buffer, SVGACBContext context)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    svga_write(SVGA_REG_COMMAND_HIGH, buffer->physical >> 32);
    svga_write(SVGA_REG_COMMAND_LOW, (uint32_t)buffer->physical | context);
}

static void free_buffer(struct svga_cb *buffer)
{
    if (buffer->pages)
        mm_free_pages(buffer->physical, buffer->pages);
    *buffer = (struct svga_cb){0};
}

/* The device context has its own completion header. A timed-out command
 * retains that header and is reaped before it can be reused. */
static int poll_control(void)
{
    SVGACBStatus state = status(&cb.control);
    switch (state) {
    case SVGA_CB_STATUS_COMPLETED:
        cb.enabled = cb.control_op == CB_CONTROL_START;
        cb.control_op = CB_CONTROL_NONE;
        return 0;
    case SVGA_CB_STATUS_NONE:
        return -EBUSY;
    case SVGA_CB_STATUS_QUEUE_FULL:
    case SVGA_CB_STATUS_CB_HEADER_ERROR:
    case SVGA_CB_STATUS_SUBMISSION_ERROR:
        /* A rejected START can fall back to FIFO. A full device queue
         * did not accept STOP, so a later teardown may retry it. */
        if (cb.control_op == CB_CONTROL_START) {
            cb.control_op = CB_CONTROL_NONE;
            return -EOPNOTSUPP;
        }
        return state == SVGA_CB_STATUS_QUEUE_FULL ? -EAGAIN : -EIO;
    default:
        return -EIO;
    }
}

static int startstop(enum svga_cb_control op)
{
    struct {
        uint32_t id;
        SVGADCCmdStartStop body;
    } command = {SVGA_DC_CMD_START_STOP_CONTEXT, {op == CB_CONTROL_START, SVGA_CB_CONTEXT_0}};
    int ret = reserve_buffer(&cb.control, sizeof(command), 0);
    if (ret)
        return ret;
    __builtin_memcpy(payload(&cb.control), &command, sizeof(command));
    encode_header(&cb.control, sizeof(command), SVGA3D_INVALID_ID, 0);
    cb.control_op = op;
    doorbell(&cb.control, SVGA_CB_CONTEXT_DEVICE);
    uint64_t last = time_work_counter_us(), remaining = CB_WAIT_US;
    for (unsigned poll = 0; poll < CB_WAIT_POLLS; ++poll) {
        ret = poll_control();
        if (ret != -EBUSY || svga_poll_expired(&last, &remaining))
            break;
        __asm__ volatile("pause");
    }
    if (ret == -EBUSY)
        console_warn("[svga] command context %s did not complete", op == CB_CONTROL_START ? "start" : "stop");
    if (cb.control_op != CB_CONTROL_NONE)
        svga.fifo.fault = true;
    return ret == -EBUSY ? -ETIMEDOUT : ret;
}

static int collect(void)
{
    if (svga.fifo.fault)
        return -EIO;
    uint32_t completed;
    if (!svga_fifo_read_reg(&svga.fifo, SVGA_FIFO_FENCE, &completed))
        return fail_queue(-EIO);
    if ((uint32_t)(completed - cb.completed) > (uint32_t)(cb.sent - cb.completed)) {
        console_warn("[svga] invalid CB fence: observed=%u completed=%u sent=%u",
                     completed, cb.completed, cb.sent);
        return fail_queue(-EIO);
    }
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    cb.completed = completed;
    while (cb.count) {
        struct svga_cb *buffer = &cb.slots[cb.head];
        SVGACBStatus state = status(buffer);
        if (state == SVGA_CB_STATUS_NONE)
            break;
        if (state != SVGA_CB_STATUS_COMPLETED) {
            console_warn("[svga] cb id=%llu status=%u errorOffset=%u fence=%u/%u",
                         (unsigned long long)header(buffer)->id, state,
                         header(buffer)->errorOffset, cb.completed, cb.sent);
            return fail_queue(-EIO);
        }
        cb.head = (cb.head + 1) % CB_SLOTS;
        --cb.count;
    }
    return 0;
}

int svga_submit_init(void)
{
    if (cb.quarantined || cb.control_op != CB_CONTROL_NONE)
        return -EIO;
    if (!(svga.caps & SVGA_CAP_3D) || !(svga.caps & (SVGA_CAP_COMMAND_BUFFERS | SVGA_CAP_CMD_BUFFERS_2)))
        return 0;
    int ret = svga_fifo_wait_idle(&svga.fifo);
    if (ret) {
        console_warn("[svga] FIFO did not drain before command context start (%d)", ret);
        return ret;
    }
    uint32_t sequence;
    if (!svga_fifo_read_reg(&svga.fifo, SVGA_FIFO_FENCE, &sequence))
        return 0;
    cb.sent = cb.completed = sequence;
    svga_enable_dma();
    ret = startstop(CB_CONTROL_START);
    if (ret) {
        if (cb.control_op != CB_CONTROL_NONE)
            return ret;
        free_buffer(&cb.control);
        return 0;
    }
    cb.limit = (svga.caps2 & SVGA_CAP2_CB_MAX_SIZE_4MB) ?
        SVGA_CB_MAX_SIZE_4MB : SVGA_CB_MAX_SIZE_DEFAULT;
    return 0;
}

bool svga_submit_has_context(void)
{
    return cb.enabled;
}

uint32_t svga_submit_limit(void)
{
    if (cb.enabled)
        return cb.limit - CB_FENCE_BYTES;
    uint32_t capacity = svga.fifo.max - svga.fifo.min;
    uint32_t overhead = svga.fifo.caps & SVGA_FIFO_CAP_FENCE ? 12 : 4;
    return capacity >= overhead ? capacity - overhead : 0;
}

int svga_submit_stage(uint32_t bytes, uint32_t preserve, void **commands)
{
    if (!cb.enabled || svga.fifo.fault)
        return -EIO;
    if (!bytes || (bytes & 3) || preserve > bytes || preserve > cb.staged_bytes)
        return -EINVAL;
    if (bytes > svga_submit_limit())
        return -E2BIG;
    int ret = reserve_buffer(&cb.staged, bytes + CB_FENCE_BYTES, preserve);
    if (ret)
        return ret;
    cb.staged_bytes = bytes;
    *commands = payload(&cb.staged);
    return 0;
}

static int submit_commands(const void *commands, uint32_t bytes, uint32_t dx_context,
                           uint32_t *sequence, bool nowait, bool staged)
{
    bool fence_only = !bytes && !commands && !staged && sequence;
    if (!cb.enabled) {
        if (staged || dx_context != SVGA3D_INVALID_ID)
            return -EOPNOTSUPP;
        if (fence_only && !(svga.fifo.caps & SVGA_FIFO_CAP_FENCE)) {
            int ret = svga_fifo_fence_poll(&svga.fifo, 0);
            if (ret < 0)
                return ret;
            *sequence = 0;
            return 0;
        }
        if (nowait)
            return svga_fifo_try_fenced(&svga.fifo, commands, bytes, sequence);
        return svga_fifo_submit_fenced(&svga.fifo, commands, bytes, sequence);
    }
    if (!fence_only && (!bytes || (bytes & 3) ||
                        (staged ? bytes > cb.staged_bytes : !commands)))
        return -EINVAL;
    if (bytes > svga_submit_limit())
        return -E2BIG;
    int ret = collect();
    if (ret)
        return ret;
    if (cb.count == CB_SLOTS) {
        if (nowait)
            return -EBUSY;
        uint64_t last = time_work_counter_us(), remaining = CB_WAIT_US;
        for (unsigned poll = 0; cb.count == CB_SLOTS; ++poll) {
            if (poll == CB_WAIT_POLLS || svga_poll_expired(&last, &remaining))
                return fail_queue(-ETIMEDOUT);
            __asm__ volatile("pause");
            ret = collect();
            if (ret)
                return ret;
        }
    }
    struct svga_cb *buffer = &cb.slots[cb.tail];
    uint32_t next = cb.sent + 1;
    if (!next)
        next = 1;
    if (staged) {
        struct svga_cb available = *buffer;
        *buffer = cb.staged;
        cb.staged = available;
        cb.staged_bytes = 0;
    } else {
        ret = reserve_buffer(buffer, bytes + CB_FENCE_BYTES, 0);
        if (ret)
            return ret;
        if (bytes)
            __builtin_memcpy(payload(buffer), commands, bytes);
    }
    encode_header(buffer, bytes, dx_context, next);
    cb.sent = next;
    ++cb.count;
    cb.tail = (cb.tail + 1) % CB_SLOTS;
    doorbell(buffer, SVGA_CB_CONTEXT_0);
    if (sequence)
        *sequence = cb.sent;
    /* Ownership transferred at the doorbell. A device failure from here on
     * is observed on the timeline, after callers attach their DMA refs. */
    return 0;
}

int svga_submit_commands(const void *commands, uint32_t bytes, uint32_t dx_context,
                         uint32_t *sequence)
{
    return submit_commands(commands, bytes, dx_context, sequence, false, false);
}

int svga_submit_try_fenced(const void *commands, uint32_t bytes, uint32_t *sequence)
{
    return submit_commands(commands, bytes, SVGA3D_INVALID_ID, sequence, true, false);
}

int svga_submit_staged(uint32_t bytes, uint32_t dx_context, uint32_t *sequence)
{
    return submit_commands(NULL, bytes, dx_context, sequence, false, true);
}

int svga_submit_poll(uint32_t sequence)
{
    if (!cb.enabled)
        return svga_fifo_fence_poll(&svga.fifo, sequence);
    int ret = collect();
    if (sequence && (int32_t)(cb.completed - sequence) >= 0)
        return 1;
    if (ret)
        return ret;
    return !sequence && !cb.count && cb.completed == cb.sent;
}

uint32_t svga_submit_completed(void)
{
    if (!cb.enabled)
        return svga_fifo_completed(&svga.fifo);
    (void)collect();
    return cb.completed;
}

int svga_submit_wait(uint32_t sequence)
{
    if (!cb.enabled)
        return svga_fifo_wait_fence(&svga.fifo, sequence);
    uint64_t last = time_work_counter_us(), remaining = CB_WAIT_US;
    for (unsigned poll = 0; poll < CB_WAIT_POLLS; ++poll) {
        int ret = svga_submit_poll(sequence);
        if (ret)
            return ret == 1 ? 0 : ret;
        if (svga_poll_expired(&last, &remaining))
            break;
        __asm__ volatile("pause");
    }
    return fail_queue(-ETIMEDOUT);
}

int svga_submit_idle(void)
{
    return svga_submit_wait(0);
}

int svga_submit_checkpoint(uint32_t *sequence)
{
    if (cb.enabled) {
        int ret = collect();
        if (ret)
            return ret;
        *sequence = cb.sent;
        return 0;
    }
    if (!(svga.fifo.caps & SVGA_FIFO_CAP_FENCE)) {
        *sequence = 0;
        return 0;
    }
    return svga_fifo_submit_fenced(&svga.fifo, NULL, 0, sequence);
}

int svga_submit_fini(void)
{
    /* The staging buffer has never been submitted or has already retired. */
    free_buffer(&cb.staged);
    cb.staged_bytes = 0;
    if (cb.quarantined)
        return -EIO;
    if (cb.control_op != CB_CONTROL_NONE) {
        /* START was preceded by FIFO idle. STOP was preceded by a full
         * command buffer and fence drain. No data submission followed either
         * when the control completion was uncertain. */
        int ret = poll_control();
        if (ret && ret != -EOPNOTSUPP && ret != -EAGAIN)
            return ret;
    } else if (svga.fifo.started) {
        int ret = svga_submit_idle();
        if (ret)
            return ret;
    }
    if (cb.enabled) {
        int ret = startstop(CB_CONTROL_STOP);
        if (ret)
            return ret;
    }
    for (unsigned i = 0; i < CB_SLOTS; ++i)
        free_buffer(&cb.slots[i]);
    free_buffer(&cb.control);
    cb.head = cb.tail = cb.count = 0;
    cb.sent = cb.completed = cb.limit = 0;
    return 0;
}
