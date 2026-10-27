/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_VECTOR_INTERNAL_H
#define RELIEFNT_VECTOR_INTERNAL_H
#include <reliefnt/types.h>
#include <linux/uio.h>

/* Task-owned imported metadata, independent of user iovec lifetime. The
 * syscall admission owns the OFD; regular_io owns only the current element. */
struct task_vector_state {
    uint64_t number,args[6],position,completed,requested;
    uint32_t index,count;
    bool writing,positional;
    struct iovec vectors[];
};
#endif
