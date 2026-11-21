/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include "fifo.h"

static uint32_t memory[2048];
static uint64_t clock_us;
static unsigned kicks, delayed_idle_polls;
static bool stalled;

uint64_t time_work_counter_us(void)
{
    return clock_us += 10000;
}

void svga_fifo_kick(struct svga_fifo *fifo, uint32_t reason)
{
    (void)fifo;
    assert(reason != 0);
    ++kicks;
    if (!stalled)
        memory[SVGA_FIFO_STOP] = memory[SVGA_FIFO_NEXT_CMD];
}

bool svga_fifo_host_busy(struct svga_fifo *fifo)
{
    (void)fifo;
    if (delayed_idle_polls == 2) {
        --delayed_idle_polls;
        clock_us -= 20000;
        return true;
    }
    if (delayed_idle_polls == 1) {
        --delayed_idle_polls;
        return false;
    }
    return stalled;
}

int main(void)
{
    struct svga_fifo fifo;
    uint32_t words[] = {SVGA_CMD_UPDATE, 1, 2, 3, 4};
    const uint32_t min = 4096, max = sizeof(memory);

    assert(svga_fifo_init(&fifo, memory, 4096, 1024, true, false) == -EINVAL);
    assert(svga_fifo_init(&fifo, memory, max, 1024, true, false) == 0);
    memory[SVGA_FIFO_CAPABILITIES] = SVGA_FIFO_CAP_RESERVE;
    memory[SVGA_FIFO_NEXT_CMD] = memory[SVGA_FIFO_STOP] = max - 8;
    assert(svga_fifo_init(&fifo, memory, max, 1024, true, true) == 0);
    assert(svga_fifo_start(&fifo) == 0);
    assert(svga_fifo_submit(&fifo, words, sizeof(words)) == 0);
    assert(memory[max/4 - 2] == SVGA_CMD_UPDATE && memory[max/4 - 1] == 1);
    assert(memory[min/4] == 2 && memory[min/4 + 2] == 4);
    assert(memory[SVGA_FIFO_NEXT_CMD] == min + 12);
    assert(memory[SVGA_FIFO_RESERVED] == 0 && kicks == 1);
    assert(svga_fifo_wait_idle(&fifo) == 0);
    clock_us = 1000000;
    delayed_idle_polls = 2;
    assert(svga_fifo_wait_idle(&fifo) == 0 && !fifo.fault && !delayed_idle_polls);

    assert(svga_fifo_init(&fifo, memory, max, 4, false, false) == 0);
    assert(svga_fifo_start(&fifo) == 0);
    stalled = true;
    memory[SVGA_FIFO_STOP] = min + 4; /* Only the separating word remains. */
    assert(svga_fifo_submit(&fifo, words, sizeof(words)) == -ETIMEDOUT);
    assert(memory[SVGA_FIFO_NEXT_CMD] == min);
    assert(svga_fifo_submit(&fifo, words, sizeof(words)) == -EIO);

    assert(svga_fifo_init(&fifo, memory, max, 1024, true, false) == 0);
    assert(svga_fifo_start(&fifo) == 0);
    memory[SVGA_FIFO_STOP] = min + 1;
    assert(svga_fifo_submit(&fifo, words, sizeof(words)) == -EIO);
    assert(memory[SVGA_FIFO_NEXT_CMD] == min);

    stalled = false;
    assert(svga_fifo_init(&fifo, memory, max, 4, false, false) == 0);
    assert(svga_fifo_start(&fifo) == 0);
    assert(svga_fifo_submit(&fifo, words, 3) == -EINVAL);
    assert(svga_fifo_submit(&fifo, words, max - min) == -E2BIG);
    assert(svga_fifo_submit(&fifo, words, sizeof(words)) == 0);
    assert(memory[min/4] == SVGA_CMD_UPDATE);
    assert(memory[SVGA_FIFO_NEXT_CMD] == min + sizeof(words));

    /* Workstation advertises this reserved capability and returns 2. */
    assert(!svga_fifo_init(&fifo, memory, max, 291, true, false));
    memory[SVGA_FIFO_CAPABILITIES] = SVGA_FIFO_CAP_RESERVE | SVGA_FIFO_CAP_DEAD;
    memory[SVGA_FIFO_DEAD] = 2;
    assert(!svga_fifo_start(&fifo));
    assert(!svga_fifo_submit(&fifo, words, sizeof(words)));
    assert(memory[SVGA_FIFO_NEXT_CMD] == min + sizeof(words));

    assert(!svga_fifo_init(&fifo, memory, max, 1024, true, false));
    memory[SVGA_FIFO_CAPABILITIES] = SVGA_FIFO_CAP_FENCE;
    memory[SVGA_FIFO_FENCE] = UINT32_MAX - 1;
    assert(!svga_fifo_start(&fifo));
    uint32_t passed, pending;
    assert(!svga_fifo_submit_fenced(&fifo, words, sizeof(words), &passed));
    assert(!svga_fifo_submit_fenced(&fifo, words, sizeof(words), &pending));
    assert(passed == UINT32_MAX && pending == 1);
    memory[SVGA_FIFO_FENCE] = passed;
    assert(svga_fifo_fence_poll(&fifo, passed) == 1);
    memory[SVGA_FIFO_STOP] = min + 1;
    assert(svga_fifo_fence_poll(&fifo, pending) == -EIO);
    assert(svga_fifo_fence_poll(&fifo, passed) == 1);
    puts("SVGA FIFO: reserve wrap, legacy publish, full timeout, malformed cursors, reserved host word passed");
    return 0;
}
