/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <reliefnt/paging.h>
#undef RELIEFNT_KERNEL_DIRECT_MAP_BASE
#define RELIEFNT_KERNEL_DIRECT_MAP_BASE 0
#ifndef USERCOPY_IMPL
#define USERCOPY_IMPL "../../kernel/exec/usercopy_task.c"
#endif
#include USERCOPY_IMPL

static unsigned char page[4096] __attribute__((aligned(4096)));
static bool blocked;

/** @brief Translate one fixture physical address, optionally simulating a hole.
 * @param phys Physical byte address represented by its host pointer.
 * @return The borrowed host pointer or NULL when the map is unavailable.
 */
void *mm_phys_to_virt(uint64_t phys) { return blocked ? NULL : (void *)(uintptr_t)phys; }
/** @brief Return the fixture's single user page.
 * @param as Borrowed address space. @param va Fixture user address.
 * @return Host-backed fake physical page base.
 */
uint64_t address_space_user_page_phys(const struct address_space *as, uint64_t va)
{ (void)as; (void)va; return (uintptr_t)page; }
/** @brief Mark the fixture page readable.
 * @param as Borrowed address space. @param va Fixture user address.
 * @return True.
 */
bool address_space_user_page_readable(const struct address_space *as, uint64_t va)
{ (void)as; (void)va; return true; }
/** @brief Mark the fixture page writable.
 * @param as Borrowed address space. @param va Fixture user address.
 * @return True.
 */
bool address_space_user_page_writable(const struct address_space *as, uint64_t va)
{ (void)as; (void)va; return true; }
/** @brief Reject unnecessary COW faults.
 * @param as Borrowed address space. @param va Fixture user address.
 * @return False.
 */
bool address_space_handle_cow_fault(struct address_space *as, uint64_t va)
{ (void)as; (void)va; return false; }
/** @brief Reject unnecessary demand faults.
 * @param task Borrowed task. @param va Fixture user address. @param error Fault flags.
 * @return Zero.
 */
int syscall_handle_task_page_fault(struct task *task, uint64_t va, uint64_t error)
{ (void)task; (void)va; (void)error; return 0; }

/** @brief Verify copies reject missing physical aliases and preserve data.
 * @return Zero after all fixture assertions succeed.
 */
int main(void)
{
    struct task task = {0};
    unsigned char source = 0x5a, result = 0;
    uint64_t va = RELIEFNT_USER_BASE + 4096;
    blocked = true;
    assert(user_copy_to_task(&task, va, &source, 1) == -LINUX_EFAULT && !page[0]);
    assert(user_copy_from_task(&task, &result, va, 1) == -LINUX_EFAULT && !result);
    blocked = false;
    assert(!user_copy_to_task(&task, va, &source, 1));
    assert(!user_copy_from_task(&task, &result, va, 1) && result == source);
    puts("PASS task copies reject direct-map holes and preserve valid copies");
    return 0;
}
