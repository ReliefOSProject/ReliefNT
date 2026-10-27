/* SPDX-License-Identifier: Apache-2.0 */
#include "pending.h"
#include "arguments.h"
#include <reliefnt/sched.h>
#include <reliefnt/mount.h>
#include <linux/errno.h>

/** @brief Resolve a pending launch through the same shebang path as execve.
 * @param task Unstarted pending task with final credentials and owned arrays.
 * @param execfn Original AT_EXECFN spelling, required.
 * @return Zero or input/permission/path/format/argument/allocation errno. Moves
 * arguments into owned staging before the handler may replace/free them and
 * restores ownership on every outcome. Caller holds execution/loader exclusion;
 * no graph/module lock spans script/interpreter I/O. Only failed-launch cleanup
 * may dispose of state after an error, before the task becomes started.
 */
int userland_resolve_pending_scripts(struct task *task,const char *execfn)
{
    if(!task || !execfn || !(task->flags&TASK_FLAG_PENDING_LOAD) ||
       (task->flags&TASK_FLAG_STARTED) || sched_task_mm(task)->executable_inode)return -EINVAL;
    struct exec_params_kernel params={.argc=task->exec_argc,.envc=task->exec_envc,
        .argv=task->exec_argv,.envp=task->exec_envp,.data=task->exec_data,
        .data_len=task->exec_data_len,.data_cap=task->exec_data_len,
        .argv_cap=task->exec_argc,.envp_cap=task->exec_envc,
        .limit=sched_exec_argument_limit(sched_task_limits(task)->stack.rlim_cur)};
    task->exec_argv=NULL;task->exec_envp=NULL;task->exec_data=NULL;
    task->exec_argc=task->exec_envc=task->exec_data_len=0;
    int rc=exec_resolve_scripts(task,&sched_task_mm(task)->executable_path,execfn,false,&params);
    task->exec_argv=params.argv;task->exec_envp=params.envp;task->exec_data=params.data;
    task->exec_argc=params.argc;task->exec_envc=params.envc;task->exec_data_len=params.data_len;
    if(!rc)task->image_node=*vfs_path_node(&sched_task_mm(task)->executable_path);
    return rc;
}
