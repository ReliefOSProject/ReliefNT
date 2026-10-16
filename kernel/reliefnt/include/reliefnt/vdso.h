/* Linux x86-64 vDSO mapping; private kernel interface, not exported UAPI. */
#ifndef RELIEFNT_VDSO_H
#define RELIEFNT_VDSO_H

#include <reliefnt/types.h>

struct task;

/** Map the embedded vDSO and shared read-only time page into a new user mm.
 * Caller holds userland_loader_lock and execution ownership for task. On
 * success, ehdr receives the ELF header's user address. On failure, the task
 * gains no VMA or physical-page reference and ehdr remains zero.
 */
int vdso_map_task(struct task *task, uint64_t *ehdr);

#endif
