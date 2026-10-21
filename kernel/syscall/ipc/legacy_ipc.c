#include "../internal.h"





/**
 * @brief Dispatch an already-normalised syscall within this ABI category.
 * @param number Linux or ReliefOS syscall number selected by the common dispatcher.
 * @param a0 First native ABI argument; pointers remain subject to the original checks.
 * @param a1 Second native ABI argument.
 * @param a2 Third native ABI argument.
 * @param a3 Fourth native ABI argument.
 * @param a4 Fifth native ABI argument.
 * @param a5 Sixth native ABI argument.
 * @return Original result, negative errno, or internal blocking/retry status.
 * @note The caller retains the kernel execution transaction and descriptor pins.
 */
int64_t syscall_legacy_ipc(uint64_t number, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;

    if (number == LINUX_SYS_SOCKET || number == LINUX_SYS_CONNECT ||
        number == LINUX_SYS_ACCEPT || number == LINUX_SYS_ACCEPT4 ||
        number == LINUX_SYS_SOCKETPAIR || number == LINUX_SYS_BIND ||
        number == LINUX_SYS_LISTEN || number == LINUX_SYS_GETSOCKNAME || number == LINUX_SYS_GETPEERNAME ||
        number == LINUX_SYS_GETSOCKOPT || number == LINUX_SYS_SETSOCKOPT ||
        number == LINUX_SYS_SHUTDOWN || number == LINUX_SYS_SEND ||
        number == LINUX_SYS_RECV || number == LINUX_SYS_SENDTO ||
        number == LINUX_SYS_RECVFROM || number == LINUX_SYS_SENDMSG ||
        number == LINUX_SYS_RECVMSG || number == LINUX_SYS_SENDMMSG || number == LINUX_SYS_RECVMMSG) {
        return syscall_socket_dispatch(number, a0, a1, a2, a3, a4, a5);
    }

    if (number >= LINUX_SYS_MSGGET && number <= LINUX_SYS_MSGCTL)
        return syscall_sysv_msg(number, a0, a1, a2, a3, a4);

    if ((number >= LINUX_SYS_SEMGET && number <= LINUX_SYS_SEMCTL) || number == LINUX_SYS_SEMTIMEDOP)
        return syscall_sysv_sem(number, a0, a1, a2, a3);

    if (number == LINUX_SYS_FUTEX) {
        return syscall_futex(a0, a1, a2, a3, a4, a5);
    }

    if (number == LINUX_SYS_FUTEX_WAITV) return syscall_futex_waitv(a0, a1, a2, a3, a4);

    if (number == LINUX_SYS_FUTEX_WAKE) return syscall_futex_wake2(a0, a1, a2, a3);

    if (number == LINUX_SYS_FUTEX_WAIT) return syscall_futex_wait2(a0, a1, a2, a3, a4, a5);

    if (number == LINUX_SYS_FUTEX_REQUEUE) return syscall_futex_requeue2(a0, a1, a2, a3);

    return -RELIEFOS_ENOSYS;
}
