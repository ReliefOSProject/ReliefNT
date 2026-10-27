/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_EXEC_PENDING_H
#define RELIEFNT_EXEC_PENDING_H
struct task;
/** @brief Apply the ordinary script handler to a prebound unstarted kernel launch.
 * @param task Pending task with final credentials, owned root/cwd and exec arrays.
 * @param execfn Original executable spelling, required through this call.
 * @return Zero or input/path/permission/format/argument/allocation errno. Task
 * retains every argument/path allocation on both success and failure; failed
 * launch cleanup consumes them. Must run under execution and loader exclusion,
 * before executable inode/MM publication. No graph/module lock spans file I/O.
 */
int userland_resolve_pending_scripts(struct task *task,const char *execfn);
#endif
