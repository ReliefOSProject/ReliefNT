/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

/* A failed HALT or bus-master disable leaves all DMA and code owned. Even on
 * HALT timeout, attempt master disable before reporting the uncertainty. */
int xhci_quiesce(struct xhci_stop_state *state, const struct xhci_stop_ops *ops,
                 void *context)
{
    if (!state || !ops || !ops->unregister || !ops->halt_request ||
        !ops->wait_halt || !ops->disable_master) return -EINVAL;
    if (state->stopped) return 0;
    __atomic_store_n(&state->failed,true,__ATOMIC_RELEASE);
    if (state->registered) {
        int rc=ops->unregister(context);if(rc)return rc;
        state->registered=false;
    }
    ops->halt_request(context);
    int halted=ops->wait_halt(context);
    int disabled=ops->disable_master(context);
    if (disabled) return disabled;
    if (halted) return halted;
    state->stopped=true;
    return 0;
}
