/* Linux v6.12 mm/process_vm_access.c, native x86-64 vector and copy ordering. */
#include <reliefnt/syscall_internal.h>
#include <reliefnt/syscall.h>
#include <reliefnt/usercopy.h>
#include <reliefnt/paging.h>
#include <reliefnt/mm.h>
#include <reliefnt/page_cache.h>
#include <reliefnt/heap.h>
#include <reliefnt/swap.h>
#include <linux/capability.h>
#include <linux/errno.h>
#include <linux/uio.h>

extern void page_cache_note_write(uint64_t) __attribute__((weak));

#define PVM_PAGE_SIZE 4096ULL
#define PVM_MAX_RW_COUNT 0x7ffff000ULL
#define PVM_MAX_PIN_PAGES 1024u

/** @brief Check native access_ok without faulting or checking page permissions. */
static bool process_vm_access_ok(uint64_t address, uint64_t size)
{
    const uint64_t limit = (1ULL << 47) - PVM_PAGE_SIZE;
    return address <= limit && size <= limit - address;
}

static bool process_vm_may_access(struct task *caller,struct task *target);

/** @brief Import lengths before addresses and keep Linux's input-error ordering. */
#ifndef CONFIG_SWAP
static int process_vm_import(uint64_t pointer, uint64_t count, bool local,
                              struct iovec **out, struct iovec fast[8], uint64_t *total)
{
    if (local) count = (uint32_t)count;
    if (count > 1024) return -LINUX_EINVAL;
    *out = fast;
    *total = 0;
    if (!count) return 0;
    if (count > 8) {
        *out = kernel_malloc(count * sizeof(**out));
        if (!*out) return -LINUX_ENOMEM;
    }
    if (!process_vm_access_ok(pointer, count * sizeof(**out))) return -LINUX_EFAULT;
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t item = pointer + i * sizeof(**out), length, address;
        if (!user_range_ok(item + 8, 8)) return -LINUX_EFAULT;
        __builtin_memcpy(&length, (const void *)(uintptr_t)(item + 8), 8);
        if (!user_range_ok(item, 8)) return -LINUX_EFAULT;
        __builtin_memcpy(&address, (const void *)(uintptr_t)item, 8);
        if ((int64_t)length < 0) return -LINUX_EINVAL;
        (*out)[i] = (struct iovec){(void *)(uintptr_t)address, length};
    }
    if (!local) return 0;
    if (count == 1 && (*out)[0].iov_len > PVM_MAX_RW_COUNT) (*out)[0].iov_len = PVM_MAX_RW_COUNT;
    for (uint64_t i = 0; i < count; ++i) {
        struct iovec *v = &(*out)[i];
        if (!process_vm_access_ok((uintptr_t)v->iov_base, v->iov_len)) return -LINUX_EFAULT;
        if (v->iov_len > PVM_MAX_RW_COUNT - *total) v->iov_len = PVM_MAX_RW_COUNT - *total;
        *total += v->iov_len;
    }
    return 0;
}
#endif

#ifdef CONFIG_SWAP
struct process_vm_vector_snapshot {
    struct iovec fast[8],*items;
    uint64_t pointer,count,total,pending_length;
    uint32_t index;
    bool initialized,length_loaded,complete;
};

struct process_vm_snapshot {
    int32_t pid;
    bool writing;
    uint64_t local_pointer,local_count,remote_pointer,remote_count,flags;
    struct process_vm_vector_snapshot local,remote;
    struct task_identity *target_identity;
    uint64_t target_root,target_generation;
};

/** @brief Release one scope-owned process_vm snapshot's subordinate ownership.
 * @param value Private snapshot allocated in a lookup scope. Its caller frees
 * the snapshot itself after this returns. No MM pointer or page pin is held.
 */
void task_process_vm_scope_release(void *value)
{
    struct process_vm_snapshot *state=value;if(!state)return;
    if(state->target_identity)sched_task_identity_put(state->target_identity);
    if(state->local.items && state->local.items!=state->local.fast)kernel_free(state->local.items);
    if(state->remote.items && state->remote.items!=state->remote.fast)kernel_free(state->remote.items);
    state->target_identity=NULL;state->local.items=state->remote.items=NULL;
}

/** @brief Snapshot one iovec array in the ABI's length-then-address order.
 * @param task Calling task. @param vector Owned resumable import. @param pointer
 * User metadata address. @param count Native iovec count. @param local True for
 * the locally capped vector. @return Zero, errno or private contention marker.
 * At most 1024 entries are held; already imported words are never reread after
 * a scheduling boundary. Payload pages are not prefaulted by this import.
 */
static int process_vm_snapshot_import(struct task *task,struct process_vm_vector_snapshot *vector,
                                      uint64_t pointer,uint64_t count,bool local)
{
    if(vector->complete)return 0;
    if(!vector->initialized) {
        if(local)count=(uint32_t)count;
        if(count>1024)return -LINUX_EINVAL;
        vector->pointer=pointer;vector->count=count;vector->items=vector->fast;
        if(count>8) {
            vector->items=kernel_malloc(count*sizeof(*vector->items));
            if(!vector->items)return -LINUX_ENOMEM;
        }
        if(count && !process_vm_access_ok(pointer,count*sizeof(*vector->items)))return -LINUX_EFAULT;
        vector->initialized=true;
    }
    while(vector->index<vector->count) {
        uint64_t item=pointer+(uint64_t)vector->index*sizeof(struct iovec),address;
        if(!vector->length_loaded) {
            int ret=swap_prepare_user_range(task,item+8,8,false);
            if(ret==SWAP_STORAGE_CONTENDED)return KERNEL_SYSCALL_SWAP_CONTENDED;
            if(ret<0 || !user_range_ok(item+8,8))return -LINUX_EFAULT;
            __builtin_memcpy(&vector->pending_length,(const void *)(uintptr_t)(item+8),8);
            vector->length_loaded=true;
        }
        int ret=swap_prepare_user_range(task,item,8,false);
        if(ret==SWAP_STORAGE_CONTENDED)return KERNEL_SYSCALL_SWAP_CONTENDED;
        if(ret<0 || !user_range_ok(item,8))return -LINUX_EFAULT;
        __builtin_memcpy(&address,(const void *)(uintptr_t)item,8);
        if((int64_t)vector->pending_length<0)return -LINUX_EINVAL;
        vector->items[vector->index++]=(struct iovec){(void *)(uintptr_t)address,vector->pending_length};
        vector->length_loaded=false;
    }
    if(local) {
        if(vector->count==1 && vector->items[0].iov_len>PVM_MAX_RW_COUNT)
            vector->items[0].iov_len=PVM_MAX_RW_COUNT;
        for(uint32_t i=0;i<vector->count;++i) {
            struct iovec *item=&vector->items[i];
            if(!process_vm_access_ok((uintptr_t)item->iov_base,item->iov_len))return -LINUX_EFAULT;
            if(item->iov_len>PVM_MAX_RW_COUNT-vector->total)item->iov_len=PVM_MAX_RW_COUNT-vector->total;
            vector->total+=item->iov_len;
        }
    }
    vector->complete=true;return 0;
}

/** @brief Resolve a current task page only at an outer, nonqueued I/O boundary.
 * @param task Live task whose root is protected by this execution transaction.
 * @param address User byte. @param write Write access requested. @param remote
 * Enforce remote VMA/device rules. @param physical Required page-base output.
 * @return Zero, EFAULT or private contention. No pointer escapes this call.
 */
static int process_vm_page_staged(struct task *task,uint64_t address,bool write,bool remote,uint64_t *physical)
{
    *physical=0;
    if(address<RELIEFNT_USER_BASE || address>=RELIEFNT_USER_TOP)return -LINUX_EFAULT;
    struct address_space *as=sched_task_as(task);
    if(remote) {
        if(address_space_user_page_is_device(as,address))return -LINUX_EFAULT;
        struct task_vma *vma=NULL;
        for(uint32_t i=0;i<sched_task_vma_capacity(task);++i) {
            struct task_vma *candidate=sched_task_vma_at(task,i);
            if(candidate && candidate->used && address>=candidate->start && address<candidate->end){vma=candidate;break;}
        }
        if(vma) {
            if((vma->flags&TASK_VMA_FLAG_DEVICE) ||
               !(vma->prot&(write?TASK_VMA_PROT_WRITE:TASK_VMA_PROT_READ)))return -LINUX_EFAULT;
        } else if(!address_space_user_page_phys(as,address)) {
            struct task_address_space_state *mm=sched_task_mm(task);
            uint64_t low=mm->initial_stack_low?mm->initial_stack_low:task->stack_low;
            uint64_t top=mm->initial_stack_top?mm->initial_stack_top:task->stack_top;
            if(!low || address<low || address>=top)return -LINUX_EFAULT;
        }
    }
    if(paging_user_entry_kind(as,address)==PAGING_USER_ENTRY_SWAP) {
        int ret=swap_prepare_user_range(task,address,1,write);
        if(ret==SWAP_STORAGE_CONTENDED)return KERNEL_SYSCALL_SWAP_CONTENDED;
        if(ret<0)return -LINUX_EFAULT;
    }
    bool accessible=write?address_space_user_page_writable(as,address):address_space_user_page_readable(as,address);
    if(!accessible) {
        bool cow=write && address_space_handle_cow_fault(as,address);
        if(!cow && !syscall_handle_task_page_fault(task,address,write?6:4))return -LINUX_EFAULT;
    }
    if(write?!address_space_user_page_writable(as,address):!address_space_user_page_readable(as,address))return -LINUX_EFAULT;
    *physical=address_space_user_page_phys(as,address);
    return *physical?0:-LINUX_EFAULT;
}

/** @brief Copy one held remote page into/from checked local vector segments.
 * @param caller Live caller. @param remote Remote physical page base.
 * @param offset Remote page offset. @param bytes Requested byte count.
 * @param vectors Owned local snapshot. @param index In/out vector index.
 * @param position In/out vector offset. @param remaining In/out remaining total.
 * @param write True to write remote memory. @return Zero, EFAULT or contention.
 */
static int process_vm_copy_staged(struct task *caller,uint64_t remote,uint64_t offset,uint64_t bytes,
                                  const struct process_vm_vector_snapshot *vectors,uint32_t *index,
                                  uint64_t *position,uint64_t *remaining,bool write)
{
    uint64_t copied=0;
    while(copied<bytes && *remaining) {
        while(*index<vectors->count && *position==vectors->items[*index].iov_len){++*index;*position=0;}
        if(*index==vectors->count)return -LINUX_EFAULT;
        uint64_t address=(uintptr_t)vectors->items[*index].iov_base+*position,physical;
        int ret=process_vm_page_staged(caller,address,!write,false,&physical);if(ret)return ret;
        uint64_t page_offset=address&(PVM_PAGE_SIZE-1),take=PVM_PAGE_SIZE-page_offset;
        if(take>bytes-copied)take=bytes-copied;
        if(take>vectors->items[*index].iov_len-*position)take=vectors->items[*index].iov_len-*position;
        void *here=mm_phys_to_virt(physical+page_offset),*there=mm_phys_to_virt(remote+offset+copied);
        if(!here || !there)return -LINUX_EFAULT;
        if(write)__builtin_memcpy(there,here,take);else __builtin_memcpy(here,there,take);
        if (take && page_cache_note_write) page_cache_note_write(write ? remote : physical);
        copied+=take;*position+=take;*remaining-=take;
    }
    return 0;
}

/** @brief Perform one process_vm call with bounded swap-aware retry ownership.
 * @param pid Target PID/TID. @param local_pointer Caller iovec address.
 * @param local_count Native local count. @param remote_pointer Remote-vector
 * metadata address in caller memory. @param remote_count Native remote count.
 * @param flags Must be zero. @param write True for process_vm_writev.
 * @return Transferred prefix, Linux errno, or private prerequisite contention.
 * A zero-byte wait retains only copied vectors, task identity and numeric
 * root/generation; resume resolves the live target afresh and rechecks access.
 * Any copied prefix returns immediately on contention after releasing its page.
 */
int64_t syscall_process_vm(int32_t pid,uint64_t local_pointer,uint64_t local_count,
                           uint64_t remote_pointer,uint64_t remote_count,uint64_t flags,bool write)
{
    if(flags)return -LINUX_EINVAL;
    struct task *caller=sched_current_task();
    struct process_vm_snapshot local_state={0},*state=&local_state;
    uint64_t stack_pages[16],*held_pages=stack_pages;
    struct task_lookup_path *slot=NULL;int rc=task_lookup_scope_next(caller,&slot);
    if(rc)return rc;
    bool scoped=slot!=NULL;
    if(slot && slot->name) {
        if(slot->flags!=TASK_LOOKUP_PROCESS_VM_KIND)return -LINUX_EINVAL;
        state=(struct process_vm_snapshot *)slot->name;
        if(state->pid!=pid || state->writing!=write || state->local_pointer!=local_pointer ||
           state->local_count!=local_count || state->remote_pointer!=remote_pointer ||
           state->remote_count!=remote_count || state->flags!=flags)return -LINUX_EINVAL;
    } else {
        if(slot) {
            state=kernel_malloc(sizeof(*state));if(!state)return -LINUX_ENOMEM;
            slot->name=(char *)state;slot->flags=TASK_LOOKUP_PROCESS_VM_KIND;
        }
        *state=(struct process_vm_snapshot){.pid=pid,.writing=write,.local_pointer=local_pointer,
            .local_count=local_count,.remote_pointer=remote_pointer,.remote_count=remote_count,.flags=flags};
    }
    int64_t result=process_vm_snapshot_import(caller,&state->local,local_pointer,local_count,true);
    if(result || !state->local.total)goto done;
    result=process_vm_snapshot_import(caller,&state->remote,remote_pointer,remote_count,false);
    if(result)goto done;
    bool any=false;
    uint64_t largest=0;
    for(uint32_t i=0;i<state->remote.count;++i)if(state->remote.items[i].iov_len) {
        any=true;
        uint64_t address=(uintptr_t)state->remote.items[i].iov_base,length=state->remote.items[i].iov_len;
        uint64_t pages=(address+length-1)/PVM_PAGE_SIZE-address/PVM_PAGE_SIZE+1;
        if(pages>largest)largest=pages;
    }
    if(!any){result=0;goto done;}
    if(largest>16) {
        uint64_t capacity=largest>PVM_MAX_PIN_PAGES?PVM_MAX_PIN_PAGES:largest;
        held_pages=kernel_malloc(capacity*sizeof(*held_pages));
        if(!held_pages){held_pages=stack_pages;result=-LINUX_ENOMEM;goto done;}
    }
    struct task *target=state->target_identity?sched_task_identity_task(state->target_identity):sched_find((uint32_t)pid);
    result=-LINUX_ESRCH;
    if(!caller || !target || target->kind!=TASK_KIND_USER || target->state==TASK_EXITED || !sched_task_as(target)->cr3)goto done;
    if(state->target_identity && (sched_task_as(target)->cr3!=state->target_root ||
       mm_context_generation_existing(sched_task_as(target))!=state->target_generation)) {
        result=-LINUX_EFAULT;goto done;
    }
    result=-LINUX_EPERM;if(!process_vm_may_access(caller,target))goto done;
    uint64_t remaining=state->local.total,position=0;uint32_t index=0;result=0;
    for(uint32_t i=0;i<state->remote.count && remaining && !result;++i) {
        uint64_t address=(uintptr_t)state->remote.items[i].iov_base,length=state->remote.items[i].iov_len;
        if(!length)continue;
        if(length>UINT64_MAX-address){result=-LINUX_EFAULT;break;}
        uint64_t offset=address&(PVM_PAGE_SIZE-1),page=address-offset;
        uint64_t nr_pages=(address+length-1)/PVM_PAGE_SIZE-address/PVM_PAGE_SIZE+1;
        while(nr_pages && remaining && !result) {
            uint32_t requested=nr_pages>PVM_MAX_PIN_PAGES?PVM_MAX_PIN_PAGES:(uint32_t)nr_pages;
            uint32_t pinned=0;int pin_error=0;
            for(;pinned<requested;++pinned) {
                uint64_t physical;
                pin_error=process_vm_page_staged(target,page+(uint64_t)pinned*PVM_PAGE_SIZE,write,true,&physical);
                if(pin_error)break;
                bool cached=page_cache_retain(physical)==0;
                if(!cached)mm_retain_page(physical);
                held_pages[pinned]=physical|(cached?1u:0u);
            }
            /* A prerequisite wait is before submission; discard this entire
             * pin batch before crossing a real scheduling boundary. */
            if(pin_error==KERNEL_SYSCALL_SWAP_CONTENDED || !pinned) {
                for(uint32_t p=0;p<pinned;++p) {
                    if(held_pages[p]&1u)page_cache_release(held_pages[p]&~1ULL);
                    else mm_free_page(held_pages[p]);
                }
                result=pin_error?pin_error:-LINUX_EFAULT;break;
            }
            uint64_t bytes=(uint64_t)pinned*PVM_PAGE_SIZE-offset;
            if(bytes>length)bytes=length;
            uint64_t left=bytes;
            for(uint32_t p=0;p<pinned;++p) {
                uint64_t take=PVM_PAGE_SIZE-offset;if(take>left)take=left;
                if(!result && remaining && take)
                    result=process_vm_copy_staged(caller,held_pages[p]&~1ULL,offset,take,
                                                  &state->local,&index,&position,&remaining,write);
                if(held_pages[p]&1u)page_cache_release(held_pages[p]&~1ULL);
                else mm_free_page(held_pages[p]);
                left-=take;offset=0;
            }
            length-=bytes;nr_pages-=pinned;page+=(uint64_t)pinned*PVM_PAGE_SIZE;
            if(!result && pin_error && remaining && length)result=pin_error;
        }
    }
    if(remaining!=state->local.total)result=(int64_t)(state->local.total-remaining);
    else if(result==KERNEL_SYSCALL_SWAP_CONTENDED && scoped && !state->target_identity) {
        rc=sched_task_identity_get(target,&state->target_identity);
        if(rc){result=rc;goto done;}
        state->target_root=sched_task_as(target)->cr3;
        state->target_generation=mm_context_generation_existing(sched_task_as(target));
        if(!state->target_generation) {
            rc=mm_context_ensure_generation(sched_task_as(target),&state->target_generation);
            if(rc){sched_task_identity_put(state->target_identity);state->target_identity=NULL;result=rc;}
        }
    }
done:
    if(held_pages!=stack_pages)kernel_free(held_pages);
    if(!scoped)task_process_vm_scope_release(state);
    return result;
}
#endif

/** @brief Apply mm_access(ATTACH_REALCREDS) and commoncap for the current user namespace. */
static bool process_vm_may_access(struct task *caller, struct task *target)
{
    if (sched_task_mm(caller) == sched_task_mm(target) ||
        sched_task_tgid(caller) == sched_task_tgid(target)) return true;
    bool privileged = (caller->cap_effective & (1ULL << CAP_SYS_PTRACE)) != 0;
    if (!privileged && (caller->uid != target->uid || caller->uid != target->euid ||
        caller->uid != target->suid || caller->gid != target->gid || caller->gid != target->egid ||
        caller->gid != target->sgid || sched_task_mm(target)->nondumpable)) return false;
    return privileged || !(target->cap_permitted & ~caller->cap_permitted);
}

/** @brief Resolve a page in either address space; remote GUP cannot access MMIO or grow stacks. */
#ifndef CONFIG_SWAP
static uint64_t process_vm_page(struct task *task, uint64_t address, bool write, bool remote)
{
    if (address < RELIEFNT_USER_BASE || address >= RELIEFNT_USER_TOP) return 0;
    struct address_space *as = sched_task_as(task);
    if (remote) {
        if (address_space_user_page_is_device(as, address)) return 0;
        struct task_vma *vma = NULL;
        for (uint32_t i = 0; i < sched_task_vma_capacity(task); ++i) {
            struct task_vma *candidate = sched_task_vma_at(task, i);
            if (candidate && candidate->used && address >= candidate->start && address < candidate->end) {
                vma = candidate;
                break;
            }
        }
        if (vma) {
            if ((vma->flags & TASK_VMA_FLAG_DEVICE) ||
                !(vma->prot & (write ? TASK_VMA_PROT_WRITE : TASK_VMA_PROT_READ))) return 0;
        } else if (!address_space_user_page_phys(as, address)) {
            struct task_address_space_state *mm = sched_task_mm(task);
            uint64_t low = mm->initial_stack_low ? mm->initial_stack_low : task->stack_low;
            uint64_t top = mm->initial_stack_top ? mm->initial_stack_top : task->stack_top;
            if (!low || address < low || address >= top) return 0;
        }
    }
    bool accessible = write ? address_space_user_page_writable(as, address)
                            : address_space_user_page_readable(as, address);
    if (!accessible) {
        bool cow = write && address_space_handle_cow_fault(as, address);
        if (!cow && !syscall_handle_task_page_fault(task, address, write ? 6 : 4)) return 0;
    }
    if (write ? !address_space_user_page_writable(as, address)
              : !address_space_user_page_readable(as, address)) return 0;
    return address_space_user_page_phys(as, address);
}

/** @brief Copy a pinned remote page across local vector and page boundaries.
 * @param caller Pinned task owning the local address space.
 * @param remote_phys Pinned remote physical page base.
 * @param offset Starting byte offset in the remote page.
 * @param size Requested bytes remaining in that page.
 * @param local Borrowed local iovec array with count entries.
 * @param count Number of local iovecs.
 * @param index In/out current iovec index.
 * @param position In/out byte offset within the current iovec.
 * @param remaining In/out total local bytes remaining.
 * @param write True when copying local bytes to the remote page.
 * @return Zero or -EFAULT; earlier chunks stay copied on a later failure.
 */
static int process_vm_copy_page(struct task *caller, uint64_t remote_phys, uint64_t offset,
                                uint64_t size, const struct iovec *local, uint32_t count,
                                uint32_t *index, uint64_t *position, uint64_t *remaining, bool write)
{
    uint64_t copied = 0;
    while (copied < size && *remaining) {
        while (*index < count && *position == local[*index].iov_len) {
            ++*index;
            *position = 0;
        }
        if (*index == count) return -LINUX_EFAULT;
        uint64_t address = (uintptr_t)local[*index].iov_base + *position;
        uint64_t local_phys = process_vm_page(caller, address, !write, false);
        if (!local_phys) return -LINUX_EFAULT;
        uint64_t page_offset = address & (PVM_PAGE_SIZE - 1);
        uint64_t take = PVM_PAGE_SIZE - page_offset;
        if (take > size - copied) take = size - copied;
        if (take > local[*index].iov_len - *position) take = local[*index].iov_len - *position;
        void *here = mm_phys_to_virt(local_phys + page_offset);
        void *there = mm_phys_to_virt(remote_phys + offset + copied);
        if (!here || !there) return -LINUX_EFAULT;
        if (write) __builtin_memcpy(there, here, take);
        else __builtin_memcpy(here, there, take);
        if (take && page_cache_note_write) page_cache_note_write(write ? remote_phys : local_phys);
        copied += take;
        *position += take;
        *remaining -= take;
    }
    return 0;
}
#endif

/**
 * @brief Transfer bytes between native iovecs using independently validated address spaces.
 * @param pid Target TID/PID, not a process-group selector.
 * @param local_pointer User address of the caller's vectors.
 * @param local_count Native unsigned long; import_iovec truncates to unsigned int.
 * @param remote_pointer User address of the target-address vectors, stored in the caller.
 * @param remote_count Native unsigned long, checked before truncation.
 * @param flags Must be zero.
 * @param write True for process_vm_writev, false for process_vm_readv.
 * @return Transferred bytes or Linux errno; prior transfer wins over a later copy fault.
 * The native dispatch execution lock pins task/MM lifetime and serializes mapping changes.
 */
#ifndef CONFIG_SWAP
int64_t syscall_process_vm(int32_t pid, uint64_t local_pointer, uint64_t local_count,
                           uint64_t remote_pointer, uint64_t remote_count, uint64_t flags, bool write)
{
    if (flags) return -LINUX_EINVAL;
    struct iovec fast_local[8], fast_remote[8], *local = fast_local, *remote = fast_remote;
    uint64_t total = 0, unused;
    int64_t result = process_vm_import(local_pointer, local_count, true, &local, fast_local, &total);
    if (result < 0 || !total) goto done;
    result = process_vm_import(remote_pointer, remote_count, false, &remote, fast_remote, &unused);
    if (result < 0) goto done;
    uint64_t nr_pages = 0;
    for (uint64_t i = 0; i < remote_count; ++i) if (remote[i].iov_len) {
        uint64_t address = (uintptr_t)remote[i].iov_base;
        uint64_t pages = (address + remote[i].iov_len - 1) / PVM_PAGE_SIZE - address / PVM_PAGE_SIZE + 1;
        if (pages > nr_pages) nr_pages = pages;
    }
    if (!nr_pages) goto done;
    uint64_t stack_pages[16], *pages = stack_pages;
    if (nr_pages > 16) {
        uint64_t capacity = nr_pages > PVM_MAX_PIN_PAGES ? PVM_MAX_PIN_PAGES : nr_pages;
        pages = kernel_malloc(capacity * sizeof(*pages));
        if (!pages) { result = -LINUX_ENOMEM; goto done; }
    }
    struct task *caller = sched_current_task(), *target = sched_find((uint32_t)pid);
    result = -LINUX_ESRCH;
    if (!caller || !target || target->kind != TASK_KIND_USER || target->state == TASK_EXITED ||
        !sched_task_as(target)->cr3) goto free_pages;
    result = -LINUX_EPERM;
    if (!process_vm_may_access(caller, target)) goto free_pages;
    uint64_t remaining = total, position = 0;
    uint32_t index = 0;
    result = 0;
    for (uint64_t i = 0; i < remote_count && remaining && !result; ++i) {
        uint64_t address = (uintptr_t)remote[i].iov_base, length = remote[i].iov_len;
        if (!length) continue;
        uint64_t offset = address & (PVM_PAGE_SIZE - 1), page = address - offset;
        nr_pages = (address + length - 1) / PVM_PAGE_SIZE - address / PVM_PAGE_SIZE + 1;
        while (!result && nr_pages && remaining) {
            uint32_t requested = nr_pages > PVM_MAX_PIN_PAGES ? PVM_MAX_PIN_PAGES : (uint32_t)nr_pages;
            uint32_t pinned = 0;
            for (; pinned < requested; ++pinned) {
                uint64_t phys = process_vm_page(target, page + pinned * PVM_PAGE_SIZE, write, true);
                if (!phys) break;
                bool cached = page_cache_retain(phys) == 0;
                if (!cached) mm_retain_page(phys);
                pages[pinned] = phys | (cached ? 1 : 0);
            }
            if (!pinned) { result = -LINUX_EFAULT; break; }
            uint64_t bytes = pinned * PVM_PAGE_SIZE - offset;
            if (bytes > length) bytes = length;
            uint64_t left = bytes;
            for (uint32_t p = 0; p < pinned; ++p) {
                uint64_t take = PVM_PAGE_SIZE - offset;
                if (take > left) take = left;
                if (!result && remaining && take)
                    result = process_vm_copy_page(caller, pages[p] & ~1ULL, offset, take,
                        local, (uint32_t)local_count, &index, &position, &remaining, write);
                if (pages[p] & 1) page_cache_release(pages[p] & ~1ULL);
                else mm_free_page(pages[p]);
                left -= take;
                offset = 0;
            }
            length -= bytes;
            nr_pages -= pinned;
            page += pinned * PVM_PAGE_SIZE;
        }
    }
    if (remaining != total) result = (int64_t)(total - remaining);
free_pages:
    if (pages != stack_pages) kernel_free(pages);
done:
    if (local && local != fast_local) kernel_free(local);
    if (remote && remote != fast_remote) kernel_free(remote);
    return result;
}
#endif
