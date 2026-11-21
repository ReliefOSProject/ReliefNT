#include <reliefnt/export.h>
/*
 * ReliefOS user-copy implementation: performs checked Ring-3 buffer access.
 * Validates pointers and transfers strings or bytes without kernel overreach.
 */
#include <reliefnt/usercopy.h>
#include <reliefnt/paging.h>
#include <reliefnt/sched.h>
#include <reliefnt/syscall.h>
#include <linux/errno.h>

#define PAGE_SIZE 4096ULL

/**
 * @brief Round value down to the previous 4096-byte page boundary.
 */
static uint64_t align_down_page(uint64_t value)
{
    return value & ~(PAGE_SIZE - 1ULL);
}

/* Keep the resident-page check shared by legacy and retryable copyin. */
#ifndef CONFIG_SWAP
static
#endif
int user_range_readable(uint64_t ptr, uint64_t len)
{
    if (!len) return 0;
    if (ptr < RELIEFNT_USER_BASE || ptr >= RELIEFNT_USER_TOP ||
        len > RELIEFNT_USER_TOP - ptr) return -EFAULT;

    struct task *task = sched_current_task();
    if (!task || task->kind != TASK_KIND_USER) return 0;

    struct address_space *as = sched_task_as(task);
    uint64_t end = ptr + len;
    for (uint64_t page = align_down_page(ptr); page < end; page += PAGE_SIZE) {
        if (address_space_user_page_readable(as, page)) continue;
#ifdef CONFIG_SWAP
        int fault = syscall_handle_user_page_fault_detailed(page, 4, false);
        if (fault == -EAGAIN) return fault;
#else
        int fault = syscall_handle_user_page_fault(page, 4);
#endif
        if (fault <= 0 || !address_space_user_page_readable(as, page)) return -EFAULT;
    }
    return 0;
}

bool user_range_ok(uint64_t ptr, uint64_t len)
{
    return user_range_readable(ptr, len) == 0;
}

/** @brief Prepare current user pages for output, resolving lazy/COW backing.
 * @param ptr User virtual destination. @param len Byte count, zero allowed.
 * @return True for writable pages, false without a user task or on bounds/fault/
 * allocation failure. Execution-owned task context; may allocate/fault backing
 * without a graph/module lock. No buffer/OFD ownership transfers; copyout is
 * performed separately only after the complete range has been validated.
 */
bool user_range_writable(uint64_t ptr, uint64_t len)
{
    struct task *task = sched_current_task();
    if (!task || task->kind != TASK_KIND_USER) return false;
    if (!len) return true;

    /* Match user_range_ok's bounds checks before computing the end. A writable
     * PTE is present and user-accessible, which also implies readable on x86.
     * Keep this shortcut to one page: multi-page ranges must validate every
     * readable page before any COW page can be changed below. */
    if (ptr < RELIEFNT_USER_BASE || ptr > RELIEFNT_USER_TOP ||
        len > RELIEFNT_USER_TOP - ptr) return false;
    uint64_t end = ptr + len;
    uint64_t first_page = align_down_page(ptr);
    if (first_page == align_down_page(end - 1) &&
        address_space_user_page_writable(sched_task_as(task), first_page)) {
        return true;
    }

    if (!user_range_ok(ptr, len)) return false;
    for (uint64_t page = first_page; page < end; page += PAGE_SIZE) {
        if (!address_space_user_page_writable(sched_task_as(task), page)) {
            if (!address_space_handle_cow_fault(sched_task_as(task), page) ||
                !address_space_user_page_writable(sched_task_as(task), page)) return false;
        }
    }
    return true;
}

/** @brief Return a bounded length, checking each user byte before reading. */
size_t user_strlen(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && user_range_ok((uint64_t)(uintptr_t)(s + n), 1) && s[n]) {
        ++n;
    }
    return n;
}

EXPORT_SYMBOL(user_range_ok);
EXPORT_SYMBOL(user_range_writable);
