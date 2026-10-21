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
int64_t syscall_legacy_memory(uint64_t number, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;

    if (number == LINUX_SYS_MEMFD_CREATE) return syscall_memfd_create(a0, (uint32_t)a1);

    if (number == LINUX_SYS_MSYNC) {
        return syscall_mm_msync(a0, a1, a2);
    }

    if (number == LINUX_SYS_MINCORE) {
        return syscall_mm_mincore(a0, a1, a2);
    }

    if (number == LINUX_SYS_MADVISE) {
        return syscall_mm_madvise(a0, a1, a2);
    }

    if (number == LINUX_SYS_MMAP) {
        return syscall_mm_mmap(a0, a1, a2, a3, a4, a5);
    }

    if (number == LINUX_SYS_MREMAP) {
        return syscall_mm_mremap(a0, a1, a2, a3, a4);
    }

    if (number == LINUX_SYS_MLOCK || number == LINUX_SYS_MLOCK2) {
        return syscall_mm_mlock(a0, a1, number == LINUX_SYS_MLOCK2 ? a2 : 0);
    }

    if (number == LINUX_SYS_MUNLOCK) return syscall_mm_munlock(a0, a1);

    if (number == LINUX_SYS_MLOCKALL) return syscall_mm_mlockall(a0);

    if (number == LINUX_SYS_MUNLOCKALL) return syscall_mm_munlockall();

    if (number == LINUX_SYS_BRK) {
        return syscall_mm_brk(a0);
    }

    if (number == LINUX_SYS_MUNMAP) {
        return syscall_mm_munmap(a0, a1);
    }

    if (number == LINUX_SYS_MPROTECT) {
        return syscall_mm_mprotect(a0, a1, a2);
    }

    return -RELIEFOS_ENOSYS;
}
