/* SPDX-License-Identifier: Apache-2.0 */
#include "../../drivers/video/drm/svga/mksstat.h"
#include <reliefnt/heap.h>
#include <reliefnt/mm.h>
#include <reliefnt/paging.h>
#include <reliefnt/sched.h>
#include <reliefnt/usercopy.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t descriptor[SVGA_HOST_PAGE_SIZE / sizeof(uint64_t)];
static bool available, allocated, registered, fail_pin;
static unsigned pins, adds, removes, resets;

void *kernel_malloc(size_t bytes) { return malloc(bytes); }
void kernel_free(void *p) { free(p); }

uint64_t mm_alloc_pages(uint32_t pages)
{
    assert(pages == 1 && !allocated);
    allocated = true;
    memset(descriptor, 0, sizeof(descriptor));
    return SVGA_HOST_PAGE_SIZE;
}

void *mm_phys_to_virt(uint64_t physical)
{
    assert(allocated && physical == SVGA_HOST_PAGE_SIZE);
    return descriptor;
}

void mm_free_pages(uint64_t physical, uint32_t pages)
{
    assert(allocated && !registered && !pins && pages == 1 && physical == SVGA_HOST_PAGE_SIZE);
    allocated = false;
}

int user_pin_read_page(struct task *task, uint64_t address, struct paging_writable_alias *pin)
{
    assert(task && allocated && !registered && !pin->physical);
    if (fail_pin && pins == 1)
        return -EFAULT;
    pin->physical = address;
    ++pins;
    return 0;
}

void user_unpin_read_page(struct paging_writable_alias *pin)
{
    assert(pin->physical && pins && allocated && !registered);
    pin->physical = 0;
    --pins;
}

bool svga_host_available(void) { return available; }

int svga_host_copy_string(struct task *task, uint64_t address, char *str, uint32_t capacity)
{
    assert(task && address != 0 && capacity == 1023);
    memset(str, 0xa5, capacity);
    memcpy(str, "probe", 6);
    return 5;
}

void svga_host_stats(enum svga_host_stats_op op, uint64_t ppn)
{
    if (op == SVGA_HOST_STATS_ADD) {
        assert(ppn == 1 && allocated && !registered && pins == 4);
        assert(descriptor[0] == 0 && descriptor[1] == RELIEFNT_USER_BASE);
        assert(descriptor[2] == RELIEFNT_USER_BASE + 3 * SVGA_HOST_PAGE_SIZE);
        assert(descriptor[3] == SVGA_HOST_PAGE_SIZE + 1 && descriptor[4] == 32 && descriptor[5] == 6);
        assert(descriptor[6] == RELIEFNT_USER_BASE / SVGA_HOST_PAGE_SIZE);
        assert(descriptor[7] == descriptor[6] + 1 && descriptor[8] == INVALID_PPN64);
        assert(descriptor[30] == descriptor[6] + 2 && descriptor[31] == INVALID_PPN64);
        assert(descriptor[62] == descriptor[6] + 3 && descriptor[63] == INVALID_PPN64);
        const char *description = (const char *)&descriptor[102];
        assert(!strcmp(description, "probe"));
        assert(description[6] == 0 && description[1023] == 0);
        registered = true;
        ++adds;
    } else {
        assert(allocated && registered && pins == 4);
        if (op == SVGA_HOST_STATS_REMOVE) {
            assert(ppn == 1);
            ++removes;
        } else {
            assert(op == SVGA_HOST_STATS_RESET && ppn == 0);
            ++resets;
        }
        registered = false;
    }
}

int main(void)
{
    static struct task owner, other;
    static struct drm_file file, other_file;
    owner.process_group = 5;
    other.process_group = 6;
    owner.process_session = other.process_session = 5;
    struct drm_vmw_mksstat_add_arg add = {
        .stat = RELIEFNT_USER_BASE, .stat_len = SVGA_HOST_PAGE_SIZE + 1,
        .info = RELIEFNT_USER_BASE + 2 * SVGA_HOST_PAGE_SIZE, .info_len = 32,
        .strs = RELIEFNT_USER_BASE + 3 * SVGA_HOST_PAGE_SIZE, .strs_len = 6, .description = 1,
    };
    assert(svga_mksstat_ioctl(&owner, &file, VMW_MKSSTAT_ADD, &add) == -ENODEV);
    available = true;
    --add.info;
    assert(svga_mksstat_ioctl(&owner, &file, VMW_MKSSTAT_ADD, &add) == -EINVAL);
    ++add.info;
    fail_pin = true;
    assert(svga_mksstat_ioctl(&owner, &file, VMW_MKSSTAT_ADD, &add) == -EFAULT);
    assert(add.id == UINT64_MAX && !allocated && !pins && !adds);
    fail_pin = false;

    assert(!svga_mksstat_ioctl(&owner, &file, VMW_MKSSTAT_ADD, &add) && add.id == 0);
    struct drm_vmw_mksstat_remove_arg remove = {.id = add.id};
    assert(svga_mksstat_ioctl(&other, &other_file, VMW_MKSSTAT_REMOVE, &remove) == -EAGAIN);
    svga_mksstat_file_release(&other_file);
    assert(!svga_mksstat_ioctl(&owner, &file, VMW_MKSSTAT_REMOVE, &remove));
    assert(!svga_mksstat_ioctl(&owner, &file, VMW_MKSSTAT_REMOVE, &remove));
    assert(!svga_mksstat_ioctl(&owner, &file, VMW_MKSSTAT_ADD, &add));
    svga_mksstat_rollback(&file, add.id);
    assert(!svga_mksstat_ioctl(&owner, &file, VMW_MKSSTAT_ADD, &add));
    svga_mksstat_file_release(&file);
    assert(!svga_mksstat_ioctl(&owner, &file, VMW_MKSSTAT_ADD, &add));
    assert(!svga_mksstat_ioctl(&other, &other_file, VMW_MKSSTAT_RESET, NULL));
    assert(!svga_mksstat_ioctl(&owner, &file, VMW_MKSSTAT_ADD, &add));
    svga_mksstat_fini();
    assert(!allocated && !registered && !pins && adds == 5 && removes == 4 && resets == 1);
    puts("SVGA MKSSTAT: wire descriptor, partial pin rollback, owner, reset and stop passed");
}
