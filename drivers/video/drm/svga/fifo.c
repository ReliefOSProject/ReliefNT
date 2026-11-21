/* SPDX-License-Identifier: Apache-2.0 */
#include "fifo.h"
#include "timeout.h"

/* SYNC reason codes from vmwgfx_reg.h. */
#define SVGA_SYNC_GENERIC 1u
#define SVGA_SYNC_FIFOFULL 2u
#define SVGA_FIFO_PAGE 4096u
#define SVGA_WAIT_US 100000u
#define SVGA_WAIT_POLLS 262144u

static bool valid_cursor(const struct svga_fifo *fifo, uint32_t pos)
{
    return !(pos & 3) && pos >= fifo->min && pos < fifo->max;
}

static bool has_register(const struct svga_fifo *fifo, unsigned reg)
{
    return fifo->extended && reg < fifo->min / 4;
}

bool svga_fifo_read_reg(const struct svga_fifo *fifo, unsigned reg, uint32_t *value)
{
    if (!fifo->started || fifo->fault || !has_register(fifo, reg))
        return false;
    *value = fifo->mem[reg];
    return true;
}

static int poison(struct svga_fifo *fifo, int error)
{
    fifo->fault = true;
    return error;
}

static int read_stop(struct svga_fifo *fifo, uint32_t *stop)
{
    if (fifo->fault)
        return -EIO;
    if (!fifo->started)
        return -ENODEV;
    /* SVGA_FIFO_DEAD is a reserved host word, not an error indicator. */
    if (fifo->mem[SVGA_FIFO_MIN] != fifo->min ||
        fifo->mem[SVGA_FIFO_MAX] != fifo->max ||
        fifo->mem[SVGA_FIFO_NEXT_CMD] != fifo->next)
        return poison(fifo, -EIO);
    *stop = fifo->mem[SVGA_FIFO_STOP];
    return valid_cursor(fifo, *stop) ? 0 : poison(fifo, -EIO);
}

int svga_fifo_init(struct svga_fifo *fifo, volatile uint32_t *mem,
                   uint32_t size, uint32_t registers, bool extended, bool adopt)
{
    uint32_t min, max;

    if (size < SVGA_FIFO_PAGE + 8 || (size & 3) || registers > size / 4)
        return -EINVAL;

    if (adopt) {
        min = mem[SVGA_FIFO_MIN];
        max = mem[SVGA_FIFO_MAX];
    } else {
        min = registers * 4;
        if (min < SVGA_FIFO_PAGE)
            min = SVGA_FIFO_PAGE;
        max = size;
    }
    if (min < 16 || min < registers * 4 || max > size ||
        ((min | max) & 3) || max <= min || max - min < 8)
        return -EINVAL;

    *fifo = (struct svga_fifo){
        .mem = mem, .min = min, .max = max, .next = min, .extended = extended,
    };
    if (adopt) {
        fifo->next = mem[SVGA_FIFO_NEXT_CMD];
        if (!valid_cursor(fifo, fifo->next) || mem[SVGA_FIFO_STOP] != fifo->next)
            return -EIO;
        return 0;
    }

    mem[SVGA_FIFO_MIN] = min;
    mem[SVGA_FIFO_MAX] = max;
    mem[SVGA_FIFO_NEXT_CMD] = min;
    mem[SVGA_FIFO_STOP] = min;
    if (has_register(fifo, SVGA_FIFO_BUSY))
        mem[SVGA_FIFO_BUSY] = 0;
    if (has_register(fifo, SVGA_FIFO_RESERVED))
        mem[SVGA_FIFO_RESERVED] = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return 0;
}

int svga_fifo_start(struct svga_fifo *fifo)
{
    uint32_t stop;

    if (has_register(fifo, SVGA_FIFO_CAPABILITIES))
        fifo->caps = fifo->mem[SVGA_FIFO_CAPABILITIES];
    if ((!has_register(fifo, SVGA_FIFO_RESERVED) && (fifo->caps & SVGA_FIFO_CAP_RESERVE)) ||
        (!has_register(fifo, SVGA_FIFO_FENCE) && (fifo->caps & SVGA_FIFO_CAP_FENCE)))
        return poison(fifo, -EIO);
    if (fifo->caps & SVGA_FIFO_CAP_FENCE)
        fifo->completed = fifo->fence = fifo->mem[SVGA_FIFO_FENCE];
    fifo->started = true;
    int ret = read_stop(fifo, &stop);
    if (ret)
        fifo->started = false;
    return ret;
}

static int reserve(struct svga_fifo *fifo, uint32_t bytes, bool nowait)
{
    uint32_t stop;
    int ret = read_stop(fifo, &stop);
    if (ret)
        return ret;
    if (bytes >= fifo->max - fifo->min)
        return -E2BIG;
    uint64_t last = 0, remaining = SVGA_WAIT_US;

    for (unsigned poll = 0;; ++poll) {
        uint32_t room = stop > fifo->next ? stop - fifo->next :
            (fifo->max - fifo->next) + (stop - fifo->min);
        if (room > bytes)
            return 0;
        if (nowait)
            return -EBUSY;
        if (!poll) {
            last = time_work_counter_us();
            svga_fifo_kick(fifo, SVGA_SYNC_FIFOFULL);
        } else if (poll == SVGA_WAIT_POLLS || svga_poll_expired(&last, &remaining)) {
            return poison(fifo, -ETIMEDOUT);
        }
        (void)svga_fifo_host_busy(fifo);
        __asm__ volatile("pause");
        ret = read_stop(fifo, &stop);
        if (ret)
            return ret;
    }
}

static uint32_t write_words(struct svga_fifo *fifo, uint32_t next,
                            const uint8_t *source, uint32_t bytes, bool batch)
{
    for (uint32_t remaining = bytes; remaining; remaining -= 4, source += 4) {
        uint32_t word;
        __builtin_memcpy(&word, source, sizeof(word));
        fifo->mem[next / 4] = word;
        next += 4;
        if (next == fifo->max)
            next = fifo->min;
        /* The legacy parser may already be running. Publish exactly one
         * initialized dword at a time when RESERVED is not available. */
        if (!batch)
            fifo->mem[SVGA_FIFO_NEXT_CMD] = next;
    }
    return next;
}

static int submit(struct svga_fifo *fifo, const void *source,
                  uint32_t bytes, uint32_t *fence_out, bool nowait)
{
    uint32_t fence[] = {SVGA_CMD_FENCE, fifo->fence + 1};
    bool emit = fence_out && (fifo->caps & SVGA_FIFO_CAP_FENCE);
    uint32_t tail = emit ? sizeof(fence) : 0;

    if ((!bytes && !emit) || (bytes && !source) || (bytes & 3) || bytes > UINT32_MAX - tail)
        return -EINVAL;
    if (!fence[1])
        fence[1] = 1;
    int ret = reserve(fifo, bytes + tail, nowait);
    if (ret)
        return ret;

    /* x86 UC stores are ordered. The explicit barrier before the doorbell
     * also orders normal cached BO/page-table writes with the submission. */
    bool batch = fifo->caps & SVGA_FIFO_CAP_RESERVE;
    if (batch)
        fifo->mem[SVGA_FIFO_RESERVED] = bytes + tail;
    uint32_t next = write_words(fifo, fifo->next, source, bytes, batch);
    if (emit)
        next = write_words(fifo, next, (const uint8_t *)fence, tail, batch);
    if (batch) {
        fifo->mem[SVGA_FIFO_NEXT_CMD] = next;
        fifo->mem[SVGA_FIFO_RESERVED] = 0;
    }
    fifo->next = next;
    if (emit)
        fifo->fence = fence[1];
    if (fence_out)
        *fence_out = emit ? fence[1] : 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    svga_fifo_kick(fifo, SVGA_SYNC_GENERIC);
    return 0;
}

int svga_fifo_submit(struct svga_fifo *fifo, const void *bytes, uint32_t length)
{
    return submit(fifo, bytes, length, NULL, false);
}

int svga_fifo_submit_fenced(struct svga_fifo *fifo, const void *bytes,
                            uint32_t length, uint32_t *fence)
{
    return submit(fifo, bytes, length, fence, false);
}

int svga_fifo_try_fenced(struct svga_fifo *fifo, const void *bytes,
                         uint32_t length, uint32_t *fence)
{
    return submit(fifo, bytes, length, fence, true);
}

static int refresh_fence(struct svga_fifo *fifo)
{
    uint32_t stop;
    int ret = read_stop(fifo, &stop);
    if (ret)
        return ret;
    uint32_t completed = fifo->mem[SVGA_FIFO_FENCE];
    if ((uint32_t)(completed - fifo->completed) > (uint32_t)(fifo->fence - fifo->completed))
        return poison(fifo, -EIO);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    fifo->completed = completed;
    return 0;
}

uint32_t svga_fifo_completed(struct svga_fifo *fifo)
{
    if (fifo->caps & SVGA_FIFO_CAP_FENCE)
        (void)refresh_fence(fifo);
    return fifo->completed;
}

int svga_fifo_fence_poll(struct svga_fifo *fifo, uint32_t fence)
{
    if (fence) {
        if (!(fifo->caps & SVGA_FIFO_CAP_FENCE))
            return -EINVAL;
        int ret = refresh_fence(fifo);
        if ((int32_t)(fifo->completed - fence) >= 0)
            return 1;
        return ret;
    }
    uint32_t stop;
    int ret = read_stop(fifo, &stop);
    if (ret)
        return ret;
    if (svga_fifo_host_busy(fifo))
        return 0;
    ret = read_stop(fifo, &stop);
    if (ret)
        return ret;
    if (stop != fifo->next)
        return 0;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return 1;
}

int svga_fifo_wait_fence(struct svga_fifo *fifo, uint32_t fence)
{
    if (!fence)
        return svga_fifo_wait_idle(fifo);
    uint64_t last = time_work_counter_us(), remaining = SVGA_WAIT_US;
    for (unsigned poll = 0; poll < SVGA_WAIT_POLLS; ++poll) {
        int status = svga_fifo_fence_poll(fifo, fence);
        if (status)
            return status == 1 ? 0 : status;
        if (!poll)
            svga_fifo_kick(fifo, SVGA_SYNC_GENERIC);
        if (svga_poll_expired(&last, &remaining))
            break;
        __asm__ volatile("pause");
    }
    return poison(fifo, -ETIMEDOUT);
}

int svga_fifo_wait_idle(struct svga_fifo *fifo)
{
    uint64_t last = time_work_counter_us(), remaining = SVGA_WAIT_US;
    uint32_t stop;
    int ret = read_stop(fifo, &stop);
    if (ret)
        return ret;
    svga_fifo_kick(fifo, SVGA_SYNC_GENERIC);
    for (unsigned poll = 0; poll < SVGA_WAIT_POLLS; ++poll) {
        bool busy = svga_fifo_host_busy(fifo);
        ret = read_stop(fifo, &stop);
        if (ret)
            return ret;
        if (!busy && stop == fifo->next)
            return 0;
        if (svga_poll_expired(&last, &remaining))
            break;
        __asm__ volatile("pause");
    }
    return poison(fifo, -ETIMEDOUT);
}
