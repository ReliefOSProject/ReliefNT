/* GPU device-UAPI syscall boundary. */
#include <reliefnt/gpu.h>
#include <reliefnt/sched.h>
#include <reliefnt/storage.h>
#include <reliefnt/syscall.h>
#include <reliefnt/syscall_internal.h>

/** @brief Return whether this ioctl belongs to the GPU device boundary.
 * @param number Linux syscall number.
 * @param a1 Ioctl command.
 * @return Nonzero for a supported GPU ioctl. */
int syscall_gpu_device_owns(uint64_t number, uint64_t a1)
{
    return number == LINUX_SYS_IOCTL && syscall_gpu_owns(a1);
}

/** @brief Dispatch an ioctl only for a descriptor opened on the GPU device.
 * @param number Linux syscall number.
 * @param a0 Device descriptor.
 * @param a1 Ioctl command.
 * @param a2 User request address, validated by the GPU dispatcher.
 * @param a3 Unused syscall argument.
 * @param a4 Unused syscall argument.
 * @param a5 Unused syscall argument.
 * @return Device result or negative ENOSYS/ENOTTY. */
int64_t syscall_gpu_device_dispatch(uint64_t number, uint64_t a0, uint64_t a1,
                             uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a2; (void)a3; (void)a4; (void)a5;
    if (number != LINUX_SYS_IOCTL || !syscall_gpu_owns(a1)) {
        return -RELIEFOS_ENOSYS;
    }
    {
        struct task *task = sched_current_task();
        struct task_file *file = task_file_for_fd(task, (int)a0);
        if (!(file && (file->flags & TASK_FILE_FLAG_DEV_NODE) &&
              file->node.first_cluster == STORAGE_DEV_KIND_GPU)) {
            return -RELIEFOS_ENOTTY;
        }
        return syscall_gpu_dispatch(a1, a2);
    }
}
