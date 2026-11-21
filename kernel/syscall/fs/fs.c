/* File-system syscall category boundary.  The legacy backend remains shared
 * while individual handlers are moved here incrementally. */
#include "../internal.h"

int syscall_fs_owns(uint64_t number)
{
    switch (number) {
    case __NR_getxattr: case __NR_lgetxattr: case __NR_fgetxattr:
    case __NR_listxattr: case __NR_llistxattr: case __NR_flistxattr:
    case __NR_setxattr: case __NR_lsetxattr: case __NR_fsetxattr:
    case __NR_removexattr: case __NR_lremovexattr: case __NR_fremovexattr:
    case LINUX_SYS_READ:
    case LINUX_SYS_WRITE:
    case LINUX_SYS_OPEN:
    case LINUX_SYS_OPENAT:
    case LINUX_SYS_OPENAT2:
    case LINUX_SYS_CLOSE:
    case LINUX_SYS_STAT:
    case LINUX_SYS_STATFS:
    case LINUX_SYS_FSTATFS:
    case LINUX_SYS_FSTAT:
    case LINUX_SYS_LSTAT:
    case LINUX_SYS_NEWFSTATAT:
    case LINUX_SYS_STATX:
    case LINUX_SYS_SENDFILE:
    case LINUX_SYS_COPY_FILE_RANGE:
    case LINUX_SYS_EVENTFD:
    case LINUX_SYS_EVENTFD2:
    case LINUX_SYS_CHMOD:
    case LINUX_SYS_FCHMOD:
    case LINUX_SYS_CHOWN:
    case LINUX_SYS_FCHOWN:
    case LINUX_SYS_LCHOWN:
    case LINUX_SYS_FCHMODAT:
    case LINUX_SYS_FCHMODAT2:
    case LINUX_SYS_FCHOWNAT:
    case LINUX_SYS_UTIMENSAT:
    case LINUX_SYS_UTIME:
    case LINUX_SYS_UTIMES:
    case LINUX_SYS_FUTIMESAT:
    case LINUX_SYS_FACCESSAT:
    case LINUX_SYS_FACCESSAT2:
    case LINUX_SYS_LSEEK:
    case LINUX_SYS_FTRUNCATE:
    case LINUX_SYS_GETCWD:
    case LINUX_SYS_CHDIR:
    case LINUX_SYS_CHROOT:
    case LINUX_SYS_RENAME:
    case LINUX_SYS_RENAMEAT:
    case LINUX_SYS_RENAMEAT2:
    case LINUX_SYS_CREAT:
    case LINUX_SYS_GETDENTS:
    case LINUX_SYS_LINK:
    case LINUX_SYS_SYMLINK:
    case LINUX_SYS_READLINK:
    case LINUX_SYS_LINKAT:
    case LINUX_SYS_SYMLINKAT:
    case LINUX_SYS_READLINKAT:
    case LINUX_SYS_MKDIR:
    case LINUX_SYS_MKDIRAT:
    case LINUX_SYS_RMDIR:
    case LINUX_SYS_UNLINK:
    case LINUX_SYS_UNLINKAT:
    case LINUX_SYS_FCNTL:
    case LINUX_SYS_FLOCK:
    case LINUX_SYS_MOUNT:
    case LINUX_SYS_UMOUNT2:
        return 1;
    default:
        return 0;
    }
}

int64_t syscall_fs_dispatch(uint64_t number, uint64_t a0, uint64_t a1,
                            uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    if (number >= __NR_setxattr && number <= __NR_fremovexattr)
        return syscall_xattr(vfs_system_namespace(), sched_current_task(), number, a0, a1, a2, a3, a4);
    return syscall_dispatch_regs_legacy(number, a0, a1, a2, a3, a4, a5);
}
