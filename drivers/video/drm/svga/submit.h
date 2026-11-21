/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_SVGA_SUBMIT_H
#define RELIEFNT_SVGA_SUBMIT_H

#include "internal.h"
#include "protocol/svga3d_types.h"

/* One submission domain for both device-global commands and DX contexts.
 * Once command-buffer context 0 starts, no new work enters the FIFO. The
 * sequence belongs to this device lifetime. Zero denotes the idle fallback. */
int svga_submit_init(void);
/* Caller ends data submission and retires DMA mappings before this call.
 * A failed control operation retains command storage for a later retry. */
int svga_submit_fini(void);
bool svga_submit_has_context(void);
uint32_t svga_submit_limit(void);
/* NULL commands, zero bytes and a sequence output request a standalone
 * fence. The FIFO without hardware fences uses its idle fallback. */
int svga_submit_commands(const void *commands, uint32_t bytes, uint32_t dx_context,
                         uint32_t *sequence);
/* One unpublished DMA payload per DRM execution owner. A reserve with
 * preserve=0 begins a batch. Growing a batch preserves the requested prefix.
 * The address remains private until svga_submit_staged succeeds. A failed
 * batch can be discarded by beginning a new one. Kernel submits do not
 * consume the staged payload. */
int svga_submit_stage(uint32_t bytes, uint32_t preserve, void **commands);
int svga_submit_staged(uint32_t bytes, uint32_t dx_context, uint32_t *sequence);
/* Returns -EBUSY without publishing commands if the transport is full. */
int svga_submit_try_fenced(const void *commands, uint32_t bytes, uint32_t *sequence);
int svga_submit_poll(uint32_t sequence);
int svga_submit_wait(uint32_t sequence);
int svga_submit_idle(void);
int svga_submit_checkpoint(uint32_t *sequence);
uint32_t svga_submit_completed(void);

static inline int svga_submit(const void *commands, uint32_t bytes)
{
    return svga_submit_commands(commands, bytes, SVGA3D_INVALID_ID, NULL);
}

static inline int svga_submit_fenced(const void *commands, uint32_t bytes, uint32_t *sequence)
{
    return svga_submit_commands(commands, bytes, SVGA3D_INVALID_ID, sequence);
}

#endif
