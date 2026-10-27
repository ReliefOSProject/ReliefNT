/* SPDX-License-Identifier: Apache-2.0 */
#include "../internal.h"

/** @brief Decode a retry-stable Linux timestamp vector for the actual inode.
 * @param task Borrowed execution-owned caller with optional operand scope.
 * @param number UTIME/UTIMES/FUTIMESAT/UTIMENSAT selects the public layout.
 * @param a0 Native first argument (pathname or signed directory descriptor).
 * @param a1 Native second argument (timestamp vector or pathname).
 * @param a2 Native third argument (timestamp vector for at-family calls).
 * @param a3 Native fourth argument (UTIMENSAT lookup flags).
 * @return Zero or negative errno/internal retry. Scoped binary bytes and the
 * actual lookup target survive user unmap/mutation and descriptor/name reuse.
 * Unscoped calls release their temporary snapshot on every outcome. Input is
 * decoded with memcpy, including unaligned vectors. No graph/module lock
 * spans copying, validation or retained provider updates.
 */
int64_t syscall_timestamp_object(struct task *task,uint64_t number,uint64_t a0,uint64_t a1,uint64_t a2,uint64_t a3)
{
    if(number!=LINUX_SYS_UTIME && number!=LINUX_SYS_UTIMES &&
       number!=LINUX_SYS_FUTIMESAT && number!=LINUX_SYS_UTIMENSAT)return -RELIEFOS_ENOSYS;
    bool at=number==LINUX_SYS_FUTIMESAT || number==LINUX_SYS_UTIMENSAT;
    uint64_t input=at ? a2 : a1,path=at ? a1 : a0;
    int32_t dirfd=at ? (int32_t)a0 : LINUX_AT_FDCWD;
    uint32_t flags=number==LINUX_SYS_UTIMENSAT ? (uint32_t)a3 : 0;
    struct task_user_text copy={0};
    int rc=0;
    if(input) {
        rc=task_lookup_user_bytes(task,input,number==LINUX_SYS_UTIME ? 16 : 32,&copy);
        if(rc)return rc;
    }
    const int64_t now=1073741823,omit=1073741822;
    struct linux_timespec times[2]={{0,now},{0,now}};
    if(input && number==LINUX_SYS_UTIMENSAT)__builtin_memcpy(times,copy.text,sizeof(times));
    else if(input && number==LINUX_SYS_UTIME) {
        int64_t seconds[2];__builtin_memcpy(seconds,copy.text,sizeof(seconds));
        times[0]=(struct linux_timespec){seconds[0],0};times[1]=(struct linux_timespec){seconds[1],0};
    } else if(input) {
        struct linux_timeval values[2];__builtin_memcpy(values,copy.text,sizeof(values));
        for(unsigned i=0;i<2;++i) {
            if(values[i].tv_usec<0 || values[i].tv_usec>999999){rc=-RELIEFOS_EINVAL;goto out;}
            times[i]=(struct linux_timespec){values[i].tv_sec,values[i].tv_usec*1000};
        }
    }
    for(unsigned i=0;i<2;++i)
        if(times[i].tv_nsec<0 || (times[i].tv_nsec>999999999 && times[i].tv_nsec!=now &&
           !(number==LINUX_SYS_UTIMENSAT && times[i].tv_nsec==omit))){rc=-RELIEFOS_EINVAL;goto out;}
    if(number==LINUX_SYS_UTIMENSAT && input && times[0].tv_nsec==omit && times[1].tv_nsec==omit)goto out;
    rc=syscall_update_path_times(task,dirfd,path,times,flags,input!=0);
out:
    kernel_free(copy.allocation);return rc;
}
