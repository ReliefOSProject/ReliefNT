/* Map one immutable Linux vDSO image and a separately shared vvar time page.
 * Physical pages retain a kernel reference for the lifetime of the kernel;
 * each mm acquires its own references through the ordinary paging lifecycle.
 */
#include <reliefnt/mm.h>
#include <reliefnt/paging.h>
#include <reliefnt/random.h>
#include <reliefnt/sched.h>
#include <reliefnt/vdso.h>
#include <reliefnt/vdso_data.h>
#include <linux/errno.h>

#define VDSO_PAGE_SIZE 4096ULL
#define VDSO_MAX_IMAGE_PAGES 16u

extern const unsigned char reliefnt_vdso_image_start[];
extern const unsigned char reliefnt_vdso_image_end[];

/* Serialized by userland_loader_lock; the timer only sees the bound data. */
static uint64_t vdso_image_pages[VDSO_MAX_IMAGE_PAGES];
static uint64_t vdso_data_page;
static uint32_t vdso_image_page_count;

/** @brief Allocate and initialize the shared vDSO image and data pages.
 * @return Zero on success or a negative Linux errno after full rollback.
 */
static int vdso_prepare_image(void)
{
    uint64_t pages[VDSO_MAX_IMAGE_PAGES] = {0};
    uint64_t length = (uintptr_t)reliefnt_vdso_image_end -
                      (uintptr_t)reliefnt_vdso_image_start;
    if (vdso_image_page_count) return 0;
    if (!length || length > VDSO_MAX_IMAGE_PAGES * VDSO_PAGE_SIZE ||
        sizeof(struct reliefnt_vdso_data) > VDSO_PAGE_SIZE) return -LINUX_ENOEXEC;
    uint32_t count = (uint32_t)((length + VDSO_PAGE_SIZE - 1) / VDSO_PAGE_SIZE);
    uint64_t data = mm_alloc_page();
    if (!data) return -LINUX_ENOMEM;
    void *data_map = mm_phys_to_virt(data);
    if (!data_map) { mm_free_page(data); return -LINUX_ENOMEM; }
    __builtin_memset(data_map, 0, VDSO_PAGE_SIZE);
    for (uint32_t i = 0; i < count; ++i) {
        void *page_map;
        pages[i] = mm_alloc_page();
        if (!pages[i]) {
            for (uint32_t j = 0; j < i; ++j) mm_free_page(pages[j]);
            mm_free_page(data);
            return -LINUX_ENOMEM;
        }
        page_map = mm_phys_to_virt(pages[i]);
        if (!page_map) {
            for (uint32_t j = 0; j <= i; ++j) if (pages[j]) mm_free_page(pages[j]);
            mm_free_page(data);
            return -LINUX_ENOMEM;
        }
        __builtin_memset(page_map, 0, VDSO_PAGE_SIZE);
        uint64_t offset = (uint64_t)i * VDSO_PAGE_SIZE;
        uint64_t bytes = length - offset;
        if (bytes > VDSO_PAGE_SIZE) bytes = VDSO_PAGE_SIZE;
        __builtin_memcpy(page_map, reliefnt_vdso_image_start + offset, bytes);
    }
    time_vdso_bind((struct reliefnt_vdso_data *)data_map);
    vdso_data_page = data;
    for (uint32_t i = 0; i < count; ++i) vdso_image_pages[i] = pages[i];
    vdso_image_page_count = count;
    return 0;
}

/* Return the first page after a collision, or start when the range is free. */
static uint64_t vdso_skip_collision(struct task *task, uint64_t start, uint64_t length)
{
    uint64_t end = start + length;
    uint64_t next = start;
    for (uint32_t i = 0; i < sched_task_vma_capacity(task); ++i) {
        const struct task_vma *vma = sched_task_vma_at(task, i);
        if (vma && vma->used && start < vma->end && end > vma->start && vma->end > next)
            next = (vma->end + VDSO_PAGE_SIZE - 1) & ~(VDSO_PAGE_SIZE - 1);
    }
    if (next != start) return next;
    for (uint64_t page = start; page < end; page += VDSO_PAGE_SIZE) {
        if (address_space_user_page_phys(sched_task_as(task), page))
            return page + VDSO_PAGE_SIZE;
    }
    return start;
}

static uint64_t vdso_find_range(struct task *task, uint64_t length)
{
    const uint64_t low = RELIEFNT_KERNEL_HOLE_END;
    uint64_t stack_top = sched_task_mm(task)->initial_stack_top;
    if (!stack_top) stack_top = task->stack_top;
    uint64_t stack_window = (uint64_t)RELIEFNT_USER_STACK_MAX_PAGES * VDSO_PAGE_SIZE;
    if (stack_top > RELIEFNT_USER_TOP || stack_top < stack_window + VDSO_PAGE_SIZE)
        return 0;
    uint64_t high = (stack_top - stack_window - VDSO_PAGE_SIZE) & ~(VDSO_PAGE_SIZE - 1);
    if (high <= low || length > high - low) return 0;
    uint64_t random;
    if (kernel_random_fill(&random, sizeof(random)) < 0) return 0;
    uint64_t slots = (high - low - length) / VDSO_PAGE_SIZE + 1;
    uint64_t first = low + random % slots * VDSO_PAGE_SIZE;
    for (uint32_t pass = 0; pass < 2; ++pass) {
        uint64_t cursor = pass ? low : first;
        uint64_t last = pass ? first : high - length + VDSO_PAGE_SIZE;
        while (cursor < last && cursor <= high - length) {
            uint64_t next = vdso_skip_collision(task, cursor, length);
            if (next == cursor) return cursor;
            if (next < cursor) break;
            cursor = next;
        }
    }
    return 0;
}

static int vdso_reserve_vma(struct task *task, uint32_t *index)
{
    uint32_t capacity = sched_task_vma_capacity(task);
    for (uint32_t i = 0; i <= capacity; ++i) {
        struct task_vma *vma = sched_task_vma_at(task, i);
        if (!vma) return -LINUX_ENOMEM;
        if (!vma->used) {
            *vma = (struct task_vma){.used = UINT32_MAX};
            *index = i;
            return 0;
        }
    }
    return -LINUX_ENOMEM;
}

int vdso_map_task(struct task *task, uint64_t *ehdr)
{
    if (!ehdr) return -LINUX_EINVAL;
    *ehdr = 0;
    if (!task) return -LINUX_EINVAL;
    int result = vdso_prepare_image();
    if (result < 0) return result;
    uint64_t length = (uint64_t)(vdso_image_page_count + 1) * VDSO_PAGE_SIZE;
    uint64_t start = vdso_find_range(task, length);
    if (!start) return -LINUX_ENOMEM;
    uint32_t data_index, image_index;
    result = vdso_reserve_vma(task, &data_index);
    if (result < 0) return result;
    result = vdso_reserve_vma(task, &image_index);
    if (result < 0) {
        *sched_task_vma_at(task, data_index) = (struct task_vma){0};
        return result;
    }
    uint32_t mapped = 0;
    if (!address_space_prepare_user_range(sched_task_as(task), start, start + length))
        goto failed;
    for (uint32_t i = 0; i <= vdso_image_page_count; ++i) {
        uint64_t physical = i ? vdso_image_pages[i - 1] : vdso_data_page;
        uint64_t flags = RELIEFNT_PAGE_SHARED | (i ? 0 : RELIEFNT_PAGE_NOEXEC);
        mm_retain_page(physical);
        if (!address_space_map_user_page(sched_task_as(task), start + i * VDSO_PAGE_SIZE,
                                         physical, flags)) {
            mm_free_page(physical);
            goto failed;
        }
        ++mapped;
    }
    *sched_task_vma_at(task, data_index) = (struct task_vma){
        .used = 1, .prot = TASK_VMA_PROT_READ, .max_prot = TASK_VMA_PROT_READ,
        .flags = TASK_VMA_FLAG_SHARED | TASK_VMA_FLAG_VVAR,
        .start = start, .end = start + VDSO_PAGE_SIZE,
    };
    *sched_task_vma_at(task, image_index) = (struct task_vma){
        .used = 1, .prot = TASK_VMA_PROT_READ | TASK_VMA_PROT_EXEC,
        .max_prot = TASK_VMA_PROT_READ | TASK_VMA_PROT_EXEC,
        .flags = TASK_VMA_FLAG_PRIVATE | TASK_VMA_FLAG_VDSO,
        .start = start + VDSO_PAGE_SIZE, .end = start + length,
    };
    *ehdr = start + VDSO_PAGE_SIZE;
    return 0;

failed:
    for (uint32_t i = 0; i < mapped; ++i) {
        uint64_t physical = address_space_unmap_user_page(sched_task_as(task), start + i * VDSO_PAGE_SIZE);
        if (physical) mm_free_page(physical);
    }
    *sched_task_vma_at(task, data_index) = (struct task_vma){0};
    *sched_task_vma_at(task, image_index) = (struct task_vma){0};
    return -LINUX_ENOMEM;
}
