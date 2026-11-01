/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/types.h>
#include <reliefnt/heap.h>
#include <reliefnt/sched.h>
#include <reliefnt/page_cache.h>
#include <reliefnt/syscall.h>
#include "internal.h"
#include <linux/errno.h>

#define SWAP_HEADER_SIZE 4096u
#define SWAP_HEADER_BAD_OFFSET 1536u
#define SWAP_HEADER_MAGIC_OFFSET 4086u
#define SWAP_HEADER_MAX_BAD ((SWAP_HEADER_MAGIC_OFFSET - SWAP_HEADER_BAD_OFFSET) / 4u)

/** @brief Decode an unaligned little-endian 32-bit disk field.
 * @param p Readable four-byte field. @return Decoded value.
 */
static uint32_t swap_header_word(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}

/** @brief Validate a complete Linux swap v1 header against actual capacity.
 * @param header Borrowed readable header. @param length Available bytes.
 * @param capacity Complete backing pages. @param last Output last page index.
 * @param bad_count Output number of unusable pages. Outputs change on success.
 * @return Zero or -EINVAL. Pure bounded validation; allocates nothing and
 * cannot publish a partial area. Page zero and all bad entries are reserved.
 */
int swap_header_validate(const void *header, uint64_t length, uint64_t capacity,
                         uint32_t *last, uint32_t *bad_count)
{
    static const char magic[] = "SWAPSPACE2";
    const uint8_t *p = header;
    if (!p || !last || !bad_count || length < SWAP_HEADER_SIZE) return -EINVAL;
    for (uint32_t i=0; i<10; ++i)
        if (p[SWAP_HEADER_MAGIC_OFFSET+i] != (uint8_t)magic[i]) return -EINVAL;
    uint32_t version=swap_header_word(p+1024), end=swap_header_word(p+1028);
    uint32_t n=swap_header_word(p+1032);
    if (version != 1 || !end || (uint64_t)end >= capacity ||
        n > SWAP_HEADER_MAX_BAD || n >= end) return -EINVAL;
    for (uint32_t i=0; i<n; ++i) {
        uint32_t page=swap_header_word(p+SWAP_HEADER_BAD_OFFSET+i*4u);
        if (!page || page > end) return -EINVAL;
        for (uint32_t j=0; j<i; ++j)
            if (swap_header_word(p+SWAP_HEADER_BAD_OFFSET+j*4u) == page) return -EINVAL;
    }
    *last=end; *bad_count=n; return 0;
}

/** @brief Destroy all prepared slot blocks belonging to an unpublished area.
 * @param area Owned unpublished area, nullable. Published areas are untouched.
 */
void swap_area_destroy_checkpoint(struct swap_area *area,int (*checkpoint)(void *),void *context)
{
    if (!area || area->published) return;
    for (uint32_t i=0; i<area->initialized_blocks; ++i) {
        kernel_free(area->blocks[i]);
        if (checkpoint && (i&31u)==31u) (void)checkpoint(context);
    }
    kernel_free(area->blocks); kernel_free(area);
}

/** @brief Release an unpublished area without a yield callback.
 * @param area Owned unpublished area, nullable.
 */
void swap_area_destroy(struct swap_area *area)
{ swap_area_destroy_checkpoint(area,NULL,NULL); }

/** @brief Prepare bounded slot blocks and reserve the header and bad pages.
 * @param header Complete on-disk v1 page. @param length Available bytes.
 * @param capacity Backing capacity in full pages. @param priority Area priority.
 * @param out Required empty result. @return Zero or validation/allocation errno.
 */
int swap_area_create_checkpoint(const void *header,uint64_t length,uint64_t capacity,
                     int32_t priority,struct swap_area **out,
                     int (*checkpoint)(void *),void *context)
{
    if (!out) return -EINVAL;
    *out=NULL; uint32_t last,bad;
    int rc=swap_header_validate(header,length,capacity,&last,&bad); if (rc) return rc;
    struct swap_area *area=kernel_malloc(sizeof(*area)); if (!area) return -ENOMEM;
    *area=(struct swap_area){.last_page=last,.priority=priority};
    uint64_t blocks=(uint64_t)last/SWAP_SLOTS_PER_BLOCK+1;
    if (blocks>UINT32_MAX || blocks>SIZE_MAX/sizeof(*area->blocks)) { kernel_free(area);return -EOVERFLOW; }
    area->block_count=(uint32_t)blocks;
    area->blocks=kernel_malloc((size_t)blocks*sizeof(*area->blocks));
    if (!area->blocks) { kernel_free(area);return -ENOMEM; }
    for (uint32_t i=0; i<area->block_count; ++i) {
        struct swap_slot_block *block=kernel_malloc(sizeof(*block));
        if (!block) { swap_area_destroy_checkpoint(area,checkpoint,context);return -ENOMEM; }
        *block=(struct swap_slot_block){0};area->blocks[i]=block;++area->initialized_blocks;
        if (checkpoint && (i&31u)==31u && (rc=checkpoint(context))) {
            swap_area_destroy_checkpoint(area,checkpoint,context);return rc;
        }
    }
    area->blocks[0]->slots[0].state=SWAP_SLOT_BAD;
    const uint8_t *p=header;
    for (uint32_t i=0; i<bad; ++i) {
        uint32_t page=swap_header_word(p+SWAP_HEADER_BAD_OFFSET+i*4u);
        area->blocks[page/SWAP_SLOTS_PER_BLOCK]->slots[page%SWAP_SLOTS_PER_BLOCK].state=SWAP_SLOT_BAD;
    }
    for (uint64_t i=(uint64_t)last; i; --i) {
        struct swap_slot *slot=&area->blocks[i/SWAP_SLOTS_PER_BLOCK]->slots[i%SWAP_SLOTS_PER_BLOCK];
        if (slot->state==SWAP_SLOT_BAD) continue;
        slot->next=area->free_head;area->free_head=(uint32_t)i;
        ++area->free;++area->total;
        if (checkpoint && (i&1023u)==0 && (rc=checkpoint(context))) {
            swap_area_destroy_checkpoint(area,checkpoint,context);return rc;
        }
    }
    *out=area;return 0;
}

/** @brief Prepare an area without a checkpoint for bounded fixture callers.
 * @param header Complete v1 header. @param length Bytes available.
 * @param capacity Backing pages. @param priority Area priority.
 * @param out Owned result. @return Zero or negative errno.
 */
int swap_area_create(const void *header,uint64_t length,uint64_t capacity,
                     int32_t priority,struct swap_area **out)
{ return swap_area_create_checkpoint(header,length,capacity,priority,out,NULL,NULL); }

/** @brief Identify a syscall that can still submit a retained file write.
 * @param number Linux syscall number. @return True for ordinary write variants.
 */
static bool swap_write_syscall(uint32_t number)
{
    return number==LINUX_SYS_WRITE || number==LINUX_SYS_PWRITE64 ||
        number==LINUX_SYS_WRITEV || number==LINUX_SYS_PWRITEV || number==LINUX_SYS_PWRITEV2;
}

/** @brief Reject cached/shared-mapped data and an already admitted writer.
 * @param inode Borrowed held inode. @param node Borrowed identity for cache.
 * @return Zero or -EINVAL/-EBUSY. Caller holds execution ownership throughout
 * this check and the following claim; no provider sync is used as a substitute
 * for unimplemented dirty shared-file writeback.
 */
int swap_file_check_in_use(struct storage_inode_ref *inode,const struct storage_node *node)
{
    if (!inode || !node) return -EINVAL;
    if (page_cache_has_inode(node)) return -EBUSY;
    for (uint32_t i=0;;++i) {
        struct task *task=sched_task_at(i);if (!task) break;
        if (task->syscall_file && task->syscall_file->inode==inode &&
            swap_write_syscall(task->syscall_file_number)) return -EBUSY;
        for (uint32_t j=0; j<sched_task_vma_capacity(task); ++j) {
            struct task_vma *vma=sched_task_vma_at(task,j);
            if (vma && vma->used && vma->inode==inode &&
                (vma->flags&(TASK_VMA_FLAG_FILE|TASK_VMA_FLAG_SHARED))==
                    (TASK_VMA_FLAG_FILE|TASK_VMA_FLAG_SHARED)) return -EBUSY;
        }
    }
    return 0;
}
