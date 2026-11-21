/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_SVGA_HOST_H
#define RELIEFNT_SVGA_HOST_H

#include "internal.h"
#include <drm/vmwgfx_drm.h>

#define SVGA_HOST_PAGE_SIZE 4096u

/* Device and ioctl entry points hold the execution writer. */
void svga_host_init(void);
void svga_host_fini(void);
bool svga_host_available(void);
/* Copy through the first NUL or capacity, returning the byte count before
 * NUL (or capacity for a truncated string), or a negative errno. */
int svga_host_copy_string(struct task *task, uint64_t address, char *str, uint32_t capacity);
int svga_host_message(struct task *task, struct drm_vmw_msg_arg *arg);

enum svga_host_stats_op {
    SVGA_HOST_STATS_RESET, SVGA_HOST_STATS_ADD, SVGA_HOST_STATS_REMOVE,
};
/* The stats transport has no reply. ADD/REMOVE take a descriptor page number. */
void svga_host_stats(enum svga_host_stats_op op, uint64_t ppn);

#endif
