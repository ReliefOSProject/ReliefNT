/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <reliefnt/console.h>
#include "../../drivers/video/drm/svga/dma.h"
#include "../../drivers/video/drm/svga/submit.h"
#include "../../drivers/video/drm/svga/protocol/svga3d_cmd.h"

struct svga_device svga;
static SVGACBHeader *submitted, *control;
static SVGACBStatus control_reply;
static unsigned control_requests, control_clock_samples;
static bool defer_control;
static uint32_t high;
static volatile uint32_t fifo[2048];
#define DMA_PAGES 5u
static _Alignas(4096) uint8_t dma[DMA_PAGES][SVGA_PAGE_SIZE];
static unsigned allocations;
static bool fail_alloc;

uint64_t mm_alloc_pages(uint32_t pages)
{
    assert(pages && pages <= DMA_PAGES);
    if (fail_alloc)
        return 0;
    unsigned mask = (1u << pages) - 1;
    for (unsigned i = 0; i + pages <= DMA_PAGES; ++i) {
        unsigned bits = mask << i;
        if (!(allocations & bits)) {
            allocations |= bits;
            return (uintptr_t)dma[i];
        }
    }
    return 0;
}

void mm_free_pages(uint64_t address, uint32_t pages)
{
    assert(pages && pages <= DMA_PAGES);
    for (unsigned i = 0; i + pages <= DMA_PAGES; ++i) {
        if (address == (uintptr_t)dma[i]) {
            unsigned bits = ((1u << pages) - 1) << i;
            assert((allocations & bits) == bits);
            allocations &= ~bits;
            return;
        }
    }
    assert(0);
}

void *mm_phys_to_virt(uint64_t address)
{
    return (void *)(uintptr_t)address;
}

void console_log(enum console_log_level level, const char *fmt, ...)
{
    (void)level;
    (void)fmt;
}

void svga_enable_dma(void) {}
void svga_fifo_kick(struct svga_fifo *ring, uint32_t reason) { (void)ring; (void)reason; }
bool svga_fifo_host_busy(struct svga_fifo *ring) { (void)ring; return false; }

uint64_t time_work_counter_us(void)
{
    static uint64_t now = 1000000;
    now += 10000;
    if (control_clock_samples) {
        if (--control_clock_samples == 1)
            now -= 20000;
        if (!control_clock_samples)
            control->status = SVGA_CB_STATUS_COMPLETED;
    }
    return now;
}

void svga_write(uint32_t reg, uint32_t value)
{
    if (reg == SVGA_REG_COMMAND_HIGH) {
        high = value;
        return;
    }
    assert(reg == SVGA_REG_COMMAND_LOW);
    SVGACBHeader *header = mm_phys_to_virt((uint64_t)high << 32 | (value & ~SVGA_CB_CONTEXT_MASK));
    if ((value & SVGA_CB_CONTEXT_MASK) == SVGA_CB_CONTEXT_DEVICE) {
        const uint32_t *command = mm_phys_to_virt(header->ptr.pa);
        assert(header->length == sizeof(uint32_t) + sizeof(SVGADCCmdStartStop));
        assert(command[0] == SVGA_DC_CMD_START_STOP_CONTEXT);
        assert(command[1] == (control_requests == 0));
        assert(command[2] == SVGA_CB_CONTEXT_0);
        ++control_requests;
        control = header;
        control->status = defer_control ? SVGA_CB_STATUS_NONE : control_reply;
        if (defer_control)
            control_clock_samples = 3;
        return;
    }
    submitted = header;
}

static void setup(void)
{
    assert(!allocations);
    fail_alloc = false;
    __builtin_memset((void *)fifo, 0, sizeof(fifo));
    svga = (struct svga_device){.caps = SVGA_CAP_3D | SVGA_CAP_COMMAND_BUFFERS};
    control_requests = 0;
    defer_control = false;
    control_clock_samples = 0;
    control_reply = SVGA_CB_STATUS_COMPLETED;
    assert(!svga_fifo_init(&svga.fifo, fifo, sizeof(fifo), 1024, true, false));
    fifo[SVGA_FIFO_FENCE] = UINT32_MAX - 1;
    assert(!svga_fifo_start(&svga.fifo));
}

static void test_control_clock_step(void)
{
    setup();
    defer_control = true;
    assert(!svga_submit_init() && svga_submit_has_context() && !svga.fifo.fault);
    assert(control_requests == 1 && !control_clock_samples);
    defer_control = false;
    assert(!svga_submit_fini() && !allocations);
}

static void test_late_control(void)
{
    setup();
    control_reply = SVGA_CB_STATUS_NONE;
    assert(svga_submit_init() == -ETIMEDOUT);
    SVGACBHeader *original = control;
    uint64_t request_id = control->id;
    assert(svga_submit_fini() == -EBUSY);
    assert(allocations && control_requests == 1 && control->id == request_id);

    /* START has now completed. An ambiguous STOP reply still owns the slot. */
    control->status = SVGA_CB_STATUS_COMPLETED;
    control_reply = SVGA_CB_STATUS_PARTIAL_COMPLETE;
    assert(svga_submit_fini() == -EIO);
    assert(allocations && control_requests == 2 && control == original);
    request_id = control->id;
    assert(svga_submit_fini() == -EIO);
    assert(allocations && control_requests == 2 && control->id == request_id);
    control->status = SVGA_CB_STATUS_COMPLETED;
    assert(!svga_submit_fini());
    assert(!allocations && control_requests == 2);

    setup();
    control_reply = SVGA_CB_STATUS_CB_HEADER_ERROR;
    assert(!svga_submit_init() && !svga_submit_has_context());
    assert(!allocations && control_requests == 1);
    assert(!svga_submit_fini());
}

static void test_stop_retry(void)
{
    setup();
    assert(!svga_submit_init() && svga_submit_has_context());
    uint32_t sequence;
    assert(!svga_submit_fenced(NULL, 0, &sequence));
    submitted->status = SVGA_CB_STATUS_COMPLETED;
    fifo[SVGA_FIFO_FENCE] = sequence;
    unsigned owned = allocations;

    control_reply = SVGA_CB_STATUS_QUEUE_FULL;
    assert(svga_submit_fini() == -EAGAIN);
    uint64_t previous_id = control->id;
    assert(svga.fifo.fault && allocations == owned && control_requests == 2);
    assert(svga_submit_fenced(NULL, 0, &sequence) == -EIO);
    assert(svga_submit_fini() == -EAGAIN);
    assert(allocations == owned && control_requests == 3 && control->id != previous_id);

    control_reply = SVGA_CB_STATUS_COMPLETED;
    assert(!svga_submit_fini());
    assert(!allocations && !svga_submit_has_context() && control_requests == 4);
    assert(svga.fifo.fault);
}

static void test_staged_commands(void)
{
    setup();
    assert(!svga_submit_init() && svga_submit_has_context());
    const uint32_t command[] = {SVGA_3D_CMD_NOP, 0};
    void *payload;
    uint32_t sequence;
    assert(!svga_submit_stage(sizeof(command), 0, &payload));
    __builtin_memcpy(payload, command, sizeof(command));

    /* Validation can submit a kernel packet before publishing these bytes. */
    assert(!svga_submit_fenced(command, sizeof(command), &sequence));
    assert(submitted->ptr.pa != (uintptr_t)payload);
    submitted->status = SVGA_CB_STATUS_COMPLETED;
    fifo[SVGA_FIFO_FENCE] = sequence;
    assert(svga_submit_poll(sequence) == 1);

    const uint32_t grown = SVGA_PAGE_SIZE;
    fail_alloc = true;
    assert(svga_submit_stage(grown, sizeof(command), &payload) == -ENOMEM);
    assert(!__builtin_memcmp(payload, command, sizeof(command)));
    fail_alloc = false;
    void *original = payload;
    assert(!svga_submit_stage(grown, sizeof(command), &payload));
    assert(payload != original && !__builtin_memcmp(payload, command, sizeof(command)));
    for (uint32_t offset = sizeof(command); offset < grown; offset += sizeof(command))
        __builtin_memcpy((uint8_t *)payload + offset, command, sizeof(command));
    assert(!svga_submit_staged(grown, 5, &sequence));
    assert(submitted->ptr.pa == (uintptr_t)payload);
    assert(submitted->flags & SVGA_CB_FLAG_DX_CONTEXT);
    assert(submitted->dxContext == 5);
    assert(submitted->length == grown + 2 * sizeof(uint32_t));
    const uint32_t *fence = (const void *)((uint8_t *)payload + grown);
    assert(fence[0] == SVGA_CMD_FENCE && fence[1] == sequence);
    assert(svga_submit_staged(sizeof(command), 5, NULL) == -EINVAL);
    submitted->status = SVGA_CB_STATUS_COMPLETED;
    fifo[SVGA_FIFO_FENCE] = sequence;
    assert(!svga_submit_fini());
    assert(!allocations);
}

int main(void)
{
    test_control_clock_step();
    test_late_control();
    test_stop_retry();
    test_staged_commands();
    setup();
    assert(!svga_submit_init() && svga_submit_has_context());
    const uint32_t command[] = {SVGA_3D_CMD_NOP, 0};
    uint32_t first, second;
    assert(!svga_submit_fenced(NULL, 0, &first));
    submitted->status = SVGA_CB_STATUS_COMPLETED;
    assert(!svga_submit_poll(first));
    assert(!svga_submit_poll(0));
    const uint32_t *wire = mm_phys_to_virt(submitted->ptr.pa);
    assert(submitted->length == 2 * sizeof(*wire));
    assert(wire[0] == SVGA_CMD_FENCE && wire[1] == first && first == UINT32_MAX);
    fifo[SVGA_FIFO_FENCE] = first;
    assert(svga_submit_poll(first) == 1 && svga_submit_poll(0) == 1);
    assert(!svga_submit_fenced(command, sizeof(command), &second) && second == 1);
    submitted->status = SVGA_CB_STATUS_COMMAND_ERROR;
    assert(svga_submit_poll(second) == -EIO && svga_submit_poll(first) == 1);
    assert(svga_submit_completed() == first);
    assert(svga_submit_fini() == -EIO);
    assert(allocations && control_requests == 1);
    puts("PASS SVGA command transport: fences, wrap, staged DMA, late completion and STOP retry");
}
