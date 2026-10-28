/* SPDX-License-Identifier: Apache-2.0 */
/* Exercise the production VMA invalidation dispatch with retained inode aliases. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <reliefnt/sched.h>
#include <reliefnt/syscall.h>

static struct task_vma mappings[7];
static unsigned calls;
static uint64_t starts[3], ends[3];

uint32_t sched_task_vma_capacity(const struct task *task)
{ (void)task; return 7; }
struct task_vma *sched_task_vma_at(struct task *task, uint32_t index)
{ (void)task; assert(index < 7); return &mappings[index]; }
void task_unmap_pages(struct task *task, uint64_t start, uint64_t end)
{
    assert(task && calls < 3);
    starts[calls] = start;
    ends[calls++] = end;
}

int main(void)
{
    struct task *task = calloc(1, sizeof(*task));
    assert(task);
    struct storage_node inode = {.volume_id = 19, .first_cluster = 37,
        .flags = STORAGE_NODE_FLAG_TMPFS, .size = 16384};
    for (unsigned i = 0; i < 7; ++i) {
        mappings[i] = (struct task_vma){.used = true,
            .start = 0x100000 + i * 0x10000, .end = 0x102000 + i * 0x10000,
            .flags = TASK_VMA_FLAG_FILE, .file_node = inode};
    }
    mappings[0].flags |= TASK_VMA_FLAG_SHARED_FILE;
    mappings[0].file_node.flags |= STORAGE_NODE_FLAG_MEMFD;
    mappings[0].file_node.entry_id = 0x10017;
    mappings[1].file_node.flags |= STORAGE_NODE_FLAG_MEMFD;
    mappings[1].file_node.size = 8192; /* A stale size does not change identity. */
    mappings[2].file_offset = 4096;
    mappings[3].file_node.volume_id++;
    mappings[4].file_node.first_cluster++;
    mappings[5].file_node.flags = STORAGE_NODE_FLAG_EXT2;
    mappings[6].flags = TASK_VMA_FLAG_ANON;
    syscall_mm_truncate_file(task, &inode, 0);
    assert(calls == 3);
    for (unsigned i = 0; i < 3; ++i)
        assert(starts[i] == mappings[i].start && ends[i] == mappings[i].end);
    calls = 0;
    inode.flags |= STORAGE_NODE_FLAG_MEMFD;
    syscall_mm_truncate_file(task, &inode, 4097);
    assert(calls == 1 && starts[0] == mappings[2].start + 4096 &&
           ends[0] == mappings[2].end);
    free(task);
    puts("memfd truncation revokes shared/private aliases by backing inode, preserving unrelated VMAs");
    return 0;
}
