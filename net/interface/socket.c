/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/net_interface.h>
#include <reliefnt/sched.h>
#include <reliefnt/socket_usercopy.h>
#include <reliefnt/usercopy.h>
#include <linux/capability.h>
#include <linux/errno.h>

/** @brief Apply Linux SO_BINDTODEVICE validation for one socket's bound index.
 * @param set Update when true, query otherwise. @param address User name buffer.
 * @param length Input byte count for updates, user socklen_t pointer for queries.
 * @param index Current bound device; changed only after a successful update.
 * @return Zero or negative errno. Execution-owned, user memory copied with the
 * socket helpers, CAP_NET_RAW required when changing an existing binding. No provider pointer escapes. */
int net_interface_device_option(bool set, uint64_t address, uint64_t length, uint32_t *index)
{
    if (!index) return -LINUX_EINVAL;
    struct task *task = sched_current_task();
    char name[NET_IF_NAME_LEN] = {0};
    if (set) {
        uint32_t size = (uint32_t)length;
        if ((int32_t)size < 0) return -LINUX_EINVAL;
        if (!task) return -LINUX_EPERM;
        if (size >= sizeof(name)) size = sizeof(name) - 1;
        if (size) {
            int result = socket_user_import(task, address, name, size);
            if (result) return result;
        }
        uint32_t selected = name[0] ? net_interface_index(name) : 0;
        if (name[0] && !selected) return -LINUX_ENODEV;
        if (*index && !(task->cap_effective & (1ULL << CAP_NET_RAW))) return -LINUX_EPERM;
        *index = selected;
        return 0;
    }
    uint32_t capacity;
    int result = socket_user_length(task, length, &capacity);
    if (result) return result;
    if ((int32_t)capacity < 0) return -LINUX_EINVAL;
    uint32_t size = 0;
    if (*index) {
        if (capacity < sizeof(name)) return -LINUX_EINVAL;
        if (!net_interface_name(*index, name)) return -LINUX_ENODEV;
        while (size < sizeof(name) && name[size]) ++size;
        ++size;
    }
    if (size) {
        result = socket_user_prepare(task, address, size, true);
        if (result) return result;
        if (!user_range_writable(address, size)) return -LINUX_EFAULT;
        __builtin_memcpy((void *)(uintptr_t)address, name, size);
    }
    *(uint32_t *)(uintptr_t)length = size;
    return 0;
}
