/* Shared UTS state for uname, hostname syscalls, and procfs. */
#include <reliefnt/uts.h>
#include <reliefnt/lock.h>

static struct kernel_spinlock uts_lock = KERNEL_SPINLOCK_INIT;
static char uts_hostname[65] = "reliefos";
static char uts_domainname[65] = "(none)";

/** @brief Snapshot both names under a short lock.
 * @param hostname Writable 65-byte kernel buffer.
 * @param domainname Writable 65-byte kernel buffer.
 */
void linux_uts_names(char hostname[65], char domainname[65])
{
    uint64_t flags;
    kernel_spin_lock_irqsave(&uts_lock, &flags);
    __builtin_memcpy(hostname, uts_hostname, 65);
    __builtin_memcpy(domainname, uts_domainname, 65);
    kernel_spin_unlock_irqrestore(&uts_lock, flags);
}

/** @brief Atomically replace a captured kernel name without touching user memory under the lock.
 * @param name Kernel input bytes, nullable when length is zero.
 * @param length Length in bytes, zero through 64.
 * @param domain Nonzero selects NIS domain, otherwise hostname.
 * @return Zero or negative EINVAL; rejected input leaves the previous name intact.
 */
int linux_uts_set(const char *name, uint32_t length, int domain)
{
    char value[65] = {0};
    uint64_t flags;
    if (length > 64 || (!name && length)) return -22;
    if (length) __builtin_memcpy(value, name, length);
    kernel_spin_lock_irqsave(&uts_lock, &flags);
    __builtin_memcpy(domain ? uts_domainname : uts_hostname, value, sizeof(value));
    kernel_spin_unlock_irqrestore(&uts_lock, flags);
    return 0;
}
