/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_SVGA_FIFO_H
#define RELIEFNT_SVGA_FIFO_H

#include <linux/errno.h>
#include "protocol/svga_reg.h"

/* Single producer under ReliefNT execution ownership. The mapping is borrowed
 * from the PCI device. The host owns STOP, capability words and fence progress.
 * A protocol fault poisons the queue until the device lifetime ends. */
struct svga_fifo {
    volatile uint32_t *mem;
    uint32_t min, max, caps, next;
    uint32_t fence, completed;
    bool extended, started, fault;
};

/* Initialize ring controls while CONFIG_DONE is clear, or adopt a live
 * firmware ring after the caller has drained it. start() follows CONFIG_DONE. */
int svga_fifo_init(struct svga_fifo *fifo, volatile uint32_t *mem,
                   uint32_t size, uint32_t registers, bool extended, bool adopt);
int svga_fifo_start(struct svga_fifo *fifo);
/* Byte count must be nonzero and dword-aligned. Optional fence is appended
 * in the same commit. A returned fence belongs to this FIFO lifetime. */
int svga_fifo_submit(struct svga_fifo *fifo, const void *bytes, uint32_t length);
int svga_fifo_submit_fenced(struct svga_fifo *fifo, const void *bytes,
                            uint32_t length, uint32_t *fence);
int svga_fifo_try_fenced(struct svga_fifo *fifo, const void *bytes,
                         uint32_t length, uint32_t *fence);
int svga_fifo_wait_fence(struct svga_fifo *fifo, uint32_t fence);
int svga_fifo_wait_idle(struct svga_fifo *fifo);
/* 1 when complete, 0 when pending, negative errno on a dead FIFO. A zero
 * fence uses an idle check on hosts without a fence register. */
int svga_fifo_fence_poll(struct svga_fifo *fifo, uint32_t fence);
/* Last validated fence marker, including progress confirmed before a fault. */
uint32_t svga_fifo_completed(struct svga_fifo *fifo);
bool svga_fifo_read_reg(const struct svga_fifo *fifo, unsigned reg, uint32_t *value);

/* Device hooks for the port transport. SYNC kicks once per submitted batch.
 * Reading BUSY may also advance a legacy host's synchronous FIFO parser. */
void svga_fifo_kick(struct svga_fifo *fifo, uint32_t reason);
bool svga_fifo_host_busy(struct svga_fifo *fifo);

#endif
