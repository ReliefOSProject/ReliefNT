/* SPDX-License-Identifier: MIT */
/**********************************************************
 * Copyright 2015-2021 VMware, Inc.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 **********************************************************/

#include "mksstat.h"
#include <reliefnt/heap.h>
#include <reliefnt/mm.h>
#include <reliefnt/paging.h>
#include <reliefnt/sched.h>
#include <reliefnt/usercopy.h>

#define MKSSTAT_SLOTS 32u
/* 4096 records with a 24-byte counter, 32-byte info, and average 40-byte
 * string allowance. These page counts and the descriptor are host ABI. */
#define MKSSTAT_STAT_PAGES 24u
#define MKSSTAT_INFO_PAGES 32u
#define MKSSTAT_STRS_PAGES 40u
#define MKSSTAT_DESCRIPTION 1024u

struct mksstat_descriptor {
    uint64_t reserved;
    uint64_t stat_va, strs_va;
    uint64_t stat_len, info_len, strs_len;
    uint64_t stat_ppns[MKSSTAT_STAT_PAGES];
    uint64_t info_ppns[MKSSTAT_INFO_PAGES];
    uint64_t strs_ppns[MKSSTAT_STRS_PAGES];
    char description[MKSSTAT_DESCRIPTION];
};

_Static_assert(offsetof(struct mksstat_descriptor, stat_ppns) == 48, "stats descriptor header");
_Static_assert(offsetof(struct mksstat_descriptor, description) == 816, "stats descriptor PPN arrays");
_Static_assert(sizeof(struct mksstat_descriptor) == 1840, "stats descriptor length");

struct mksstat_entry {
    struct drm_file *owner;
    uint64_t descriptor;
    uint32_t group, session;
    unsigned pinned;
    struct paging_writable_alias pages[];
};

/* All slots and user mappings are serialized by the DRM execution writer.
 * The host stats transport does not take part in FIFO fault recovery. */
static struct mksstat_entry *slots[MKSSTAT_SLOTS];

static void release_entry(struct mksstat_entry *entry)
{
    while (entry->pinned)
        user_unpin_read_page(&entry->pages[--entry->pinned]);
    if (entry->descriptor)
        mm_free_pages(entry->descriptor, 1);
    kernel_free(entry);
}

static void remove_slot(unsigned slot)
{
    struct mksstat_entry *entry = slots[slot];
    svga_host_stats(SVGA_HOST_STATS_REMOVE, entry->descriptor / SVGA_HOST_PAGE_SIZE);
    slots[slot] = NULL;
    release_entry(entry);
}

static int pin_area(struct task *task, struct mksstat_entry *entry, uint64_t address,
                    unsigned count, uint64_t *ppns, unsigned capacity)
{
    for (unsigned i = 0; i < count; ++i) {
        struct paging_writable_alias *pin = &entry->pages[entry->pinned];
        int ret = user_pin_read_page(task, address, pin);
        if (ret)
            return ret;
        ++entry->pinned;
        ppns[i] = pin->physical / SVGA_HOST_PAGE_SIZE;
        address += SVGA_HOST_PAGE_SIZE;
    }
    for (unsigned i = count; i < capacity; ++i)
        ppns[i] = INVALID_PPN64;
    return 0;
}

static int area_pages(uint64_t address, uint64_t bytes, unsigned limit)
{
    if (!bytes || bytes > (uint64_t)limit * SVGA_HOST_PAGE_SIZE || (address & (SVGA_HOST_PAGE_SIZE - 1)) ||
        address < RELIEFNT_USER_BASE || address >= RELIEFNT_USER_TOP ||
        bytes > RELIEFNT_USER_TOP - address)
        return -EINVAL;
    return (bytes + SVGA_HOST_PAGE_SIZE - 1) / SVGA_HOST_PAGE_SIZE;
}

static int add_stats(struct task *task, struct drm_file *file, struct drm_vmw_mksstat_add_arg *arg)
{
    arg->id = UINT64_MAX;
    int stats = area_pages(arg->stat, arg->stat_len, MKSSTAT_STAT_PAGES);
    int info = area_pages(arg->info, arg->info_len, MKSSTAT_INFO_PAGES);
    int strs = area_pages(arg->strs, arg->strs_len, MKSSTAT_STRS_PAGES);
    if (stats < 0 || info < 0 || strs < 0 || !arg->description)
        return -EINVAL;

    unsigned slot = 0;
    while (slot < MKSSTAT_SLOTS && slots[slot])
        ++slot;
    if (slot == MKSSTAT_SLOTS)
        return -ENOSPC;

    size_t bytes = sizeof(struct mksstat_entry) + (stats + info + strs) * sizeof(struct paging_writable_alias);
    struct mksstat_entry *entry = kernel_malloc(bytes);
    if (!entry)
        return -ENOMEM;
    __builtin_memset(entry, 0, bytes);
    entry->descriptor = mm_alloc_pages(1);
    struct mksstat_descriptor *desc = entry->descriptor ? mm_phys_to_virt(entry->descriptor) : NULL;
    int ret = -ENOMEM;
    if (!desc)
        goto fail;
    __builtin_memset(desc, 0, sizeof(*desc));
    ret = svga_host_copy_string(task, arg->description, desc->description, MKSSTAT_DESCRIPTION - 1);
    if (ret < 0)
        goto fail;
    /* The import works by page. Clear any bytes copied after the NUL. */
    __builtin_memset(desc->description + ret, 0, MKSSTAT_DESCRIPTION - ret);

#ifdef CONFIG_SWAP
    ret = drm_user_prepare(task, arg->stat, arg->stat_len, false);
    if (!ret)
        ret = drm_user_prepare(task, arg->info, arg->info_len, false);
    if (!ret)
        ret = drm_user_prepare(task, arg->strs, arg->strs_len, false);
    if (ret)
        goto fail;
#endif
    ret = pin_area(task, entry, arg->stat, stats, desc->stat_ppns, MKSSTAT_STAT_PAGES);
    if (!ret)
        ret = pin_area(task, entry, arg->info, info, desc->info_ppns, MKSSTAT_INFO_PAGES);
    if (!ret)
        ret = pin_area(task, entry, arg->strs, strs, desc->strs_ppns, MKSSTAT_STRS_PAGES);
    if (ret)
        goto fail;

    desc->stat_va = arg->stat;
    desc->strs_va = arg->strs;
    desc->stat_len = arg->stat_len;
    desc->info_len = arg->info_len;
    desc->strs_len = arg->strs_len;
    entry->owner = file;
    entry->group = task->process_group;
    entry->session = task->process_session;
    svga_host_stats(SVGA_HOST_STATS_ADD, entry->descriptor / SVGA_HOST_PAGE_SIZE);
    slots[slot] = entry;
    arg->id = slot;
    return 0;

fail:
    release_entry(entry);
    return ret;
}

int svga_mksstat_ioctl(struct task *task, struct drm_file *file, uint32_t command, void *data)
{
    if (!svga_host_available())
        return -ENODEV;
    switch (command) {
    case VMW_MKSSTAT_ADD:
        return add_stats(task, file, data);
    case VMW_MKSSTAT_REMOVE: {
        uint64_t id = ((const struct drm_vmw_mksstat_remove_arg *)data)->id;
        if (id >= MKSSTAT_SLOTS)
            return -EINVAL;
        struct mksstat_entry *entry = slots[id];
        if (!entry)
            return 0;
        if (entry->group != task->process_group || entry->session != task->process_session)
            return -EAGAIN;
        remove_slot(id);
        return 0;
    }
    case VMW_MKSSTAT_RESET:
        svga_host_stats(SVGA_HOST_STATS_RESET, 0);
        for (unsigned i = 0; i < MKSSTAT_SLOTS; ++i) {
            if (!slots[i])
                continue;
            release_entry(slots[i]);
            slots[i] = NULL;
        }
        return 0;
    default:
        return -EINVAL;
    }
}

void svga_mksstat_rollback(struct drm_file *file, uint64_t id)
{
    if (id < MKSSTAT_SLOTS && slots[id] && slots[id]->owner == file)
        remove_slot(id);
}

void svga_mksstat_file_release(struct drm_file *file)
{
    for (unsigned i = 0; i < MKSSTAT_SLOTS; ++i)
        svga_mksstat_rollback(file, i);
}

void svga_mksstat_fini(void)
{
    for (unsigned i = 0; i < MKSSTAT_SLOTS; ++i)
        if (slots[i])
            remove_slot(i);
}
