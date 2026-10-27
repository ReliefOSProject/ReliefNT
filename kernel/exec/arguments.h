/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_EXEC_ARGUMENTS_H
#define RELIEFNT_EXEC_ARGUMENTS_H
#include <reliefnt/types.h>
struct task;
struct vfs_path;
/* Owned staging, shared by ordinary exec and deferred kernel launches. Every
 * vector element points into data, never into a caller's user address space. */
struct exec_params_kernel {
    uint32_t argc,envc;
    char **argv,**envp,*data;
    uint32_t data_cap,argv_cap,envp_cap,limit,data_len;
};
/** @brief Retire all owned exec staging allocations and reset their metadata.
 * @param params Initialized owned staging, nullable; all fields reset to zero.
 * @return None. Caller owns execution and staging exclusively; no VFS lookup,
 * I/O, provider callbacks or ownership escapes.
 */
void free_exec_params(struct exec_params_kernel *params);
/** @brief Follow shebang interpreters using retained executable objects.
 * @param task Caller under execution ownership with an optional retry scope.
 * @param image Owned actual path, replaced after successful interpreter lookup.
 * @param execfn Original AT_EXECFN spelling used by script argv construction.
 * @param inaccessible Original execveat script FD would close on exec.
 * @param params Owned argv/environment staging within the stack-derived budget.
 * @return Zero for an ELF candidate or negative lookup/type/format/argument
 * errno. Provider paths stay pinned across callbacks, without graph locks;
 * diagnostic names never regain execution authority. Staging/path mutations
 * remain caller-owned even on error and must be retired by failed launch.
 */
int exec_resolve_scripts(struct task *task,struct vfs_path *image,const char *execfn,
                            bool inaccessible,struct exec_params_kernel *params);
#endif
