/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_KERNEL_SECCOMP_INTERNAL_H
#define RELIEFNT_KERNEL_SECCOMP_INTERNAL_H

#include <reliefnt/seccomp.h>
#include <reliefnt/seccomp_bpf.h>

/* Apart from refs, published nodes are immutable. Each task's head and
 * each node's link own references. A task is not allowed to discard a node
 * except by exiting or moving to a chain which includes it as an ancestor. */
struct seccomp_filter {
    uint64_t refs;
    struct seccomp_filter *older;
    uint32_t length, path_insns, count;
    bool log;
    struct sock_filter code[];
};

#endif
