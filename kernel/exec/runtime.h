/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_EXEC_RUNTIME_H
#define RELIEFNT_EXEC_RUNTIME_H
#include <reliefnt/mount.h>

/** @brief Prepare boot-scoped directories through the actual shared mount tree.
 * @param ns Borrowed initialized namespace, required; no userspace exists yet.
 * @return Zero or negative lookup/type/readonly/I/O/allocation errno. Clears
 * /run and /dev/shm without following final symlinks or crossing attachments,
 * then sets root ownership and standard directory modes. Traversal uses held
 * objects without a formatted-path depth limit or recursive C stack. Exclusive
 * boot execution is required, with no graph/module lock spanning callbacks.
 */
int userland_prepare_runtime(struct vfs_namespace *ns);
#endif
