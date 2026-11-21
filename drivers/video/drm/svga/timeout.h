/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_SVGA_TIMEOUT_H
#define RELIEFNT_SVGA_TIMEOUT_H

#include <reliefnt/time.h>

/* Local, non-sleeping polls only. The caller also caps the number of polls
 * in case the clock stalls. A backward step contributes no elapsed time. */
static inline bool svga_poll_expired(uint64_t *last, uint64_t *remaining)
{
    uint64_t now = time_work_counter_us();
    uint64_t elapsed = now >= *last ? now - *last : 0;
    *last = now;
    if (elapsed >= *remaining)
        return true;
    *remaining -= elapsed;
    return false;
}

#endif
