#include <reliefnt/syscall_internal.h>
#include <reliefnt/syscall.h>
#include <reliefnt/time.h>
#include <reliefnt/futex.h>
#include <reliefnt/heap.h>
#include <reliefnt/mm.h>
#ifdef CONFIG_SWAP
#include <reliefnt/lock.h>
#include <reliefnt/memory_pressure.h>
#endif
#include <reliefnt/page_cache.h>
#include <reliefnt/device_mapping.h>
#include <reliefnt/paging.h>
#include <reliefnt/swap.h>
#include <reliefnt/usercopy.h>
#include <reliefos/fs_abi.h>
#include <linux/errno.h>
#include <reliefnt/file_limits.h>
#include "vector_internal.h"

extern void page_cache_note_write(uint64_t) __attribute__((weak));

#define REGULAR_IO_CHUNK 32768u
#define REGULAR_IO_PAGES 9u

/** One retained original physical alias; cache and PMM references differ. */
struct regular_io_page {
    uint64_t physical;
    uint16_t offset, length;
    bool cache;
    bool device;
    struct device_user_page device_page;
    struct paging_writable_alias alias;
};

/** A task owns this stage from admission until provider completion or drain. */
struct regular_io_stage {
    struct regular_io_stage *next;
    struct task *task;
    uint64_t address;
    uint32_t length, consumed, pages;
    bool writing;
    struct regular_io_page page[REGULAR_IO_PAGES];
    uint8_t bytes[];
};

/* The execution writer protects this list; task teardown invokes the common
 * cancellation path before freeing a task or its stable OFD. */
static struct regular_io_stage *regular_io_stages;

/** @brief Find the current task's owned stage under execution ownership.
 * @param task Live task. @return Borrowed stage or NULL.
 */
static struct regular_io_stage *regular_stage_find(const struct task *task)
{
    for (struct regular_io_stage *s = regular_io_stages; s; s = s->next)
        if (s->task == task) return s;
    return NULL;
}

/** @brief Retire staged bytes and original frame references after I/O drains.
 * @param task Execution-owned owner, nullable. No provider callback is made.
 */
void task_regular_io_stage_cancel(struct task *task)
{
    struct regular_io_stage **link = &regular_io_stages;
    while (*link && (*link)->task != task) link = &(*link)->next;
    if (!*link) return;
    struct regular_io_stage *s = *link;
    *link = s->next;
    for (uint32_t i = 0; i < s->pages; ++i) {
        if(s->page[i].device)device_user_page_put(&s->page[i].device_page);
        else {
            paging_writable_alias_remove(&s->page[i].alias);
            if (s->page[i].cache) page_cache_release(s->page[i].physical);
            else mm_free_page(s->page[i].physical);
        }
    }
    kernel_free(s);
}

/** @brief Snapshot one write slice or retain one read slice's physical aliases.
 * @param task Live user task under execution ownership. @param address User VA.
 * @param length Bounded nonzero byte count. @param writing Direction.
 * @return Zero or negative EFAULT/ENOMEM. No provider request exists on entry.
 * User pages must already be prepared for this outer-writer boundary.
 */
static int regular_stage_begin(struct task *task, uint64_t address,
                               uint32_t length, bool writing)
{
    if (!length || length > REGULAR_IO_CHUNK || regular_stage_find(task)) return -LINUX_EFAULT;
    int ret = swap_prepare_user_range(task, address, length, !writing);
    if (ret < 0) return ret;
    if (!(writing ? user_range_ok(address, length) : user_range_writable(address, length))) {
#ifdef CONFIG_SWAP
        /* A valid lazy/COW output may have failed only because an ordinary
         * frame was unavailable. Retry the detailed faults before submitting
         * any provider request, retaining the distinction from bad pointers. */
        if (writing || task != sched_current_task() || !kernel_execution_is_outermost() ||
            !task->syscall_file || task->syscall_file_number != LINUX_SYS_READ)
            return -LINUX_EFAULT;
        for (uint64_t page = address & ~4095ULL; page < address + length; page += 4096) {
            if (address_space_user_page_writable(sched_task_as(task), page)) continue;
            uint64_t fault_flags = 6u;
            if (paging_user_entry_kind(sched_task_as(task), page) == PAGING_USER_ENTRY_RESIDENT)
                fault_flags |= 1u;
            int fault = syscall_handle_user_page_fault_detailed(page, fault_flags, false);
            if (fault == -ENOMEM) return -ENOMEM;
            if (fault <= 0) return -LINUX_EFAULT;
        }
        if (!user_range_writable(address, length)) return -LINUX_EFAULT;
#else
        return -LINUX_EFAULT;
#endif
    }
    struct regular_io_stage *s = kernel_malloc(sizeof(*s) + length);
    if (!s) return -LINUX_ENOMEM;
    *s = (struct regular_io_stage){.task = task, .address = address,
        .length = length, .writing = writing, .next = regular_io_stages};
    regular_io_stages = s;
    for (uint32_t copied = 0; copied < length;) {
        uint64_t va = address + copied;
        uint64_t phys = address_space_user_page_phys(sched_task_as(task), va);
        uint64_t flags = address_space_user_page_flags(sched_task_as(task), va);
        uint32_t offset = (uint32_t)(va & 4095u), take = 4096u - offset;
        if (take > length - copied) take = length - copied;
        struct device_user_page device_page={0};
        bool device=(flags&RELIEFNT_PAGE_DEVICE)!=0;
        if(device) {
            int error=device_user_page_get(task,va,phys,flags,&device_page);
            if(error){task_regular_io_stage_cancel(task);return error;}
        }
        uint8_t *alias=device ? device_page.alias : phys ? mm_phys_to_virt(phys) : NULL;
        if (!alias) { task_regular_io_stage_cancel(task); return -LINUX_EFAULT; }
        if (writing) {
            __builtin_memcpy(s->bytes + copied, alias + offset, take);
            if(device)device_user_page_put(&device_page);
        } else {
            if (s->pages == REGULAR_IO_PAGES) {
                if(device)device_user_page_put(&device_page);
                task_regular_io_stage_cancel(task); return -LINUX_EFAULT;
            }
            if(device) {
                struct regular_io_page *page=&s->page[s->pages++];
                *page=(struct regular_io_page){.physical=phys,.offset=offset,.length=take,
                                               .device=true,.device_page=device_page};
                copied+=take;continue;
            }
            bool cache = !page_cache_retain(phys);
            if (!cache) {
                if (!mm_page_refcount(phys)) {
                    task_regular_io_stage_cancel(task); return -LINUX_EFAULT;
                }
                mm_retain_page(phys);
            }
            struct regular_io_page *page=&s->page[s->pages++];
            *page=(struct regular_io_page){.physical=phys,.offset=offset,.length=take,.cache=cache};
            paging_writable_alias_add(&page->alias,phys);
        }
        copied += take;
    }
    return 0;
}

/** @brief Copy an accepted staged-read prefix into the originally pinned pages.
 * @param s Owned read stage. @param count New bytes accepted by provider.
 * No current user address is dereferenced, including after unmap/replacement.
 */
static void regular_stage_copy_read(struct regular_io_stage *s, uint32_t count)
{
    uint32_t begin = s->consumed, end = begin + count, position = 0;
    for (uint32_t i = 0; i < s->pages && position < end; ++i) {
        struct regular_io_page *p = &s->page[i];
        uint32_t limit = position + p->length;
        if (limit > begin && position < end) {
            uint32_t first = begin > position ? begin : position;
            uint32_t last = end < limit ? end : limit;
            uint8_t *alias = p->device ? p->device_page.alias : mm_phys_to_virt(p->physical);
            __builtin_memcpy(alias + p->offset + first - position, s->bytes + first, last - first);
            if (p->cache && page_cache_note_write) page_cache_note_write(p->physical);
        }
        position = limit;
    }
}

/**
 * @brief Transfer regular-file or genuine block-node data, retaining progress across retries.
 * @param task Current task, with dispatcher-owned retry state.
 * @param file Authorized canonical OFD retaining its actual provider path.
 * @param buffer User buffer already validated by the dispatcher for count bytes.
 * @param count Requested byte count; capped at Linux MAX_RW_COUNT.
 * @param position Byte offset for positional I/O; otherwise use the shared file offset.
 * @param writing Whether to write rather than read.
 * @param positional Whether to preserve the shared file offset.
 * @return Bytes transferred, negative errno, or EAGAIN/BLOCKED with saved
 * retry progress. An ordinary vector owns this element's imported metadata;
 * its parent state separately accounts for earlier completed elements.
 * A vector retains the OFD reservation between completed elements until its
 * common parent completion/cancellation, so another caller cannot interleave.
 * Execution ownership is required. Ordinary files use their retained provider
 * and optional OFD-local provider capture/cursor, never a diagnostic filename;
 * genuine block nodes use the block service. No graph/module lock spans
 * callbacks. O_APPEND stats the actual inode; sync flags flush the retained
 * provider, except for the immediately committed proc diagnostic control.
 */
int64_t syscall_regular_io(struct task *task, struct task_file *file, uint64_t buffer, uint64_t count,
                           uint64_t position, bool writing, bool positional)
{
    uint64_t done = 0;
    uint64_t started = time_work_counter_us();
    bool block = file->flags & TASK_FILE_FLAG_DEV_BLOCK;
    if (block && (!(file->flags & TASK_FILE_FLAG_DEV_NODE) ||
                  (file->node.flags & (STORAGE_NODE_FLAG_DEV_NODE | STORAGE_NODE_FLAG_DEV_BLOCK)) !=
                    (STORAGE_NODE_FLAG_DEV_NODE | STORAGE_NODE_FLAG_DEV_BLOCK) ||
                  file->node.first_cluster != STORAGE_DEV_KIND_DISK))
        return -LINUX_EBADF;
    bool resumable =
        task->syscall_file == file &&
        (task->syscall_file_number == LINUX_SYS_READ || task->syscall_file_number == LINUX_SYS_WRITE ||
         task->syscall_file_number == LINUX_SYS_PREAD64 || task->syscall_file_number == LINUX_SYS_PWRITE64 ||
         (task->vector_io && (task->syscall_file_number==LINUX_SYS_READV || task->syscall_file_number==LINUX_SYS_WRITEV ||
          task->syscall_file_number==LINUX_SYS_PREADV || task->syscall_file_number==LINUX_SYS_PWRITEV ||
          task->syscall_file_number==LINUX_SYS_PREADV2 || task->syscall_file_number==LINUX_SYS_PWRITEV2)));
    count = count > 0x7ffff000u ? 0x7ffff000u : count;
    if (file->io_owner && file->io_owner != task->pid)
        return -LINUX_EAGAIN;
    if (resumable && task->regular_io.active) {
        buffer = task->regular_io.buffer;
        count = task->regular_io.count;
        position = task->regular_io.position;
        done = task->regular_io.done;
    } else {
        if (!positional)
            position = file->offset;
        if (!block && writing && (file->flags & RELIEFOS_O_APPEND)) {
            struct linux_stat_abi status;
            int ret = vfs_path_stat(&file->path_ref,&status);
            if (ret < 0)
                return ret;
            if(status.st_size<0)return -LINUX_EIO;
            position = (uint64_t)status.st_size;
        }
        if (position > INT64_MAX || count > (uint64_t)INT64_MAX - position)
            return -LINUX_EINVAL;
        if (!block && writing && !(file->node.flags & (STORAGE_NODE_FLAG_PROC | STORAGE_NODE_FLAG_SYSFS))) {
            int ret = task_file_limit_write(task, position, &count,
                                             !task->vector_io || !task->vector_io->completed);
            if (ret) return ret;
        }
        if (resumable) {
            task->regular_io.buffer = buffer;
            task->regular_io.count = count;
            task->regular_io.position = position;
            task->regular_io.done = 0;
            task->regular_io.active = true;
        }
    }
    if (!positional)
        file->io_owner = task->pid;
    int error = 0;
    while (done < count) {
        uint32_t length = count - done > REGULAR_IO_CHUNK ? REGULAR_IO_CHUNK : (uint32_t)(count - done);
        uint32_t transferred = 0;
        struct regular_io_stage *stage = NULL;
        void *io_buffer = (void *)(uintptr_t)(buffer + done);
        if (task->kind == TASK_KIND_USER) {
            stage = regular_stage_find(task);
            if (!stage) {
                /* The previous provider callback completed before page-in;
                 * pending callbacks always retain their existing stage. */
                storage_release_task_io(task->pid);
                error = regular_stage_begin(task, buffer + done, length, writing);
                if (error == SWAP_STORAGE_CONTENDED && resumable)
                    return KERNEL_SYSCALL_SWAP_CONTENDED;
                if (error == SWAP_STORAGE_CONTENDED) error = -LINUX_EBUSY;
#ifdef CONFIG_SWAP
                if (error == -LINUX_ENOMEM && !writing && resumable &&
                    task->syscall_file_number == LINUX_SYS_READ &&
                    task == sched_current_task() && kernel_execution_is_outermost())
                    return KERNEL_SYSCALL_REGULAR_READ_RECLAIM;
#endif
                if (error) break;
                stage = regular_stage_find(task);
            }
            if (!stage || stage->writing != writing ||
                stage->address + stage->consumed != buffer + done || stage->consumed >= stage->length) {
                storage_drain_task_io(task->pid);
                task_regular_io_stage_cancel(task);
                error = -LINUX_EIO; break;
            }
            length = stage->length - stage->consumed;
            io_buffer = stage->bytes + stage->consumed;
        }
        int ret;
        if (block) {
            uint32_t disk = STORAGE_BLOCK_DISK_ID(file->node.volume_id);
            int32_t part = STORAGE_BLOCK_PARTITION(file->node.volume_id);
            ret = writing ? storage_disk_block_write(disk, part, position + done,
                                                     io_buffer, length,
                                                     &transferred)
                          : storage_disk_block_read(disk, part, position + done,
                                                    io_buffer, length, &transferred);
        } else if (writing) {
            file->read_cursor.valid = 0;
            ret = vfs_path_write(&file->path_ref,position+done,
                io_buffer,length,&transferred);
        } else if(file->provider_file) {
            file->read_cursor.valid=0;
            ret=vfs_file_read(file->provider_file,position+done,io_buffer,length,&transferred);
        } else {
            ret = vfs_path_read_cursor(&file->path_ref, position + done, io_buffer,
                                           length, &transferred, &file->read_cursor);
        }
        if (transferred > length) {
            if (stage) { storage_drain_task_io(task->pid); task_regular_io_stage_cancel(task); }
            error = -LINUX_EIO;
            break;
        }
        if (stage) {
            if (!writing && transferred) regular_stage_copy_read(stage, transferred);
            stage->consumed += transferred;
        }
        done += transferred;
        if (!positional)
            file->offset = position + done;
        if (!block && writing && transferred) {
            file->node=*vfs_path_node(&file->path_ref);
            if (position + done > file->node.size)
                file->node.size = position + done;
        }
        if (resumable)
            task->regular_io.done = done;
        if (ret == -LINUX_EAGAIN || ret == KERNEL_SYSCALL_BLOCKED) {
            if (resumable && (!stage || stage->consumed < stage->length)) return ret;
            if (stage) storage_drain_task_io(task->pid);
        } else if (ret < 0 && stage) storage_drain_task_io(task->pid);
        if (stage) task_regular_io_stage_cancel(task);
        if ((ret == -LINUX_EAGAIN || ret == KERNEL_SYSCALL_BLOCKED) && resumable && transferred == length)
            ret = 0;
        if (ret < 0) {
            error = ret;
            break;
        }
        if (transferred < length)
            break;
        /* Yield only after a completed storage operation, with no DMA pending.
         * The dispatcher resumes this blocking syscall with its saved progress. */
        if (resumable && done < count && time_work_counter_us() - started >= 2000) {
            storage_release_task_io(task->pid);
            return -LINUX_EAGAIN;
        }
    }
    if (!task->vector_io && file->io_owner == task->pid)
        file->io_owner = 0;
    if (resumable)
        task->regular_io.active = false;
    if (writing && done && (file->flags & LINUX_O_DSYNC)) {
        int sync = block ? storage_sync_disk(STORAGE_BLOCK_DISK_ID(file->node.volume_id))
                         : vfs_path_sync_after_write(&file->path_ref,
                             (file->flags&LINUX_O_SYNC)==LINUX_O_DSYNC);
        if (sync < 0) return sync;
    }
    return done ? (int64_t)done : error;
}

#ifdef CONFIG_SWAP
/** @brief Reclaim and resume a scalar read before its next provider callback.
 * @param task Current task retaining the original OFD and completed prefix.
 * @param frame Borrowed original syscall registers. @return Read result,
 * private retry marker, or completed prefix/ENOMEM on terminal shortage.
 * Reclaim runs only under the outer execution writer with no staged pages,
 * provider request, graph lock or PMM lock retained. Re-entry revalidates the
 * current slice and preserves the OFD offset and any already copied prefix.
 */
int64_t syscall_regular_read_reclaim(struct task *task, const struct trap_frame *frame)
{
    if (!task || task != sched_current_task() || !frame ||
        frame->rax != LINUX_SYS_READ || frame->rip < 2 ||
        !kernel_execution_is_outermost() || !task->syscall_file ||
        task->syscall_file_number != LINUX_SYS_READ ||
        task->syscall_fd != (int32_t)frame->rdi || !task->regular_io.active ||
        task->regular_io.buffer != frame->rsi ||
        task->regular_io.count != (frame->rdx > 0x7ffff000u ? 0x7ffff000u : frame->rdx) ||
        task->regular_io.done >= task->regular_io.count ||
        task->regular_io.done > UINT64_MAX - task->regular_io.buffer ||
        regular_stage_find(task))
        return -LINUX_EFAULT;
    uint64_t address = task->regular_io.buffer + task->regular_io.done;
    memory_reclaim_fault_scope_begin(task, frame->rip - 2, address);
    int rc = memory_fault_oom(task, address);
    memory_reclaim_fault_scope_end();
    if (rc == -EAGAIN) return KERNEL_SYSCALL_BLOCKED;
    if (rc) return task->regular_io.done ? (int64_t)task->regular_io.done : -LINUX_ENOMEM;
    return syscall_regular_io(task, task->syscall_file, frame->rsi, frame->rdx, 0, false, false);
}
#endif
