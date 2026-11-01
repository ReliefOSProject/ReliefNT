/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: poll user.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
/* Production poll implementation with a controlled prerequisite owner. */
#define main inherited_select_main
#include "select_bitmap_test.c"
#undef main
#include <reliefnt/swap.h>

static bool blocked;
static bool block_write;
static uint64_t block_address;
static unsigned preparations;
static unsigned block_after;

void storage_drain_task_io(uint32_t pid) { assert(pid == current.pid); }
void kernel_wait_queue_remove(struct kernel_wait_queue *queue, struct task *task)
{ (void)queue; (void)task; assert(0); }
void vfs_walk_cancel(struct vfs_walk **walk) { assert(!*walk); }
void vfs_path_put(struct vfs_path *path) { assert(!path->mount); }
void storage_inode_allow_write(struct storage_inode_ref *inode) { (void)inode; assert(0); }
int storage_inode_put(struct storage_inode_ref *inode) { (void)inode; assert(0); return 0; }
void task_process_vm_scope_release(void *state) { (void)state; assert(0); }

int swap_prepare_user_range(struct task *task, uint64_t address,
                            uint64_t length, bool writing)
{
    assert(task == &current && length && length <= 32768);
    ++preparations;
    if (blocked && preparations >= block_after && writing == block_write && address == block_address)
        return SWAP_STORAGE_CONTENDED;
    return 0;
}

static void clear_scope(void)
{
    task_lookup_scope_cancel(&current);
}

static int64_t call_poll(struct pollfd *fds, uint64_t count)
{
    struct trap_frame frame = {.rax = LINUX_SYS_POLL, .rdi = (uintptr_t)fds, .rsi = count};
    task_lookup_scope_begin(&current, &frame);
    return syscall_poll((uintptr_t)fds, count, 0);
}

static int64_t call_select(uint64_t nfds, uint64_t *read_set, uint64_t *write_set)
{
    struct trap_frame frame = {.rax = LINUX_SYS_SELECT, .rdi = nfds,
                               .rsi = (uintptr_t)read_set, .rdx = (uintptr_t)write_set};
    task_lookup_scope_begin(&current, &frame);
    return syscall_select_common(nfds, (uintptr_t)read_set, (uintptr_t)write_set, 0, 0, false);
}

#ifndef TASK8_POLL_NO_MAIN
int main(void)
{
    enum { N = 4097 };
    struct pollfd *fds = calloc(N, sizeof(*fds));
    assert(fds);
    current.pid = 1;
    current.kind = TASK_KIND_USER;
    current.limits.nofile.rlim_cur = N;
    set_readable_file(5);
    for (unsigned i = 0; i < N; ++i) fds[i].fd = -1;
    fds[0] = (struct pollfd){.fd = 5, .events = POLLIN};
    block_address = (uint64_t)(uintptr_t)&fds[4096];
    block_write = false;
    block_after = 1;
    blocked = true;
    assert(call_poll(fds, N) == KERNEL_SYSCALL_SWAP_CONTENDED);
    fds[0].fd = -1; /* The already imported prefix is immutable. */
    blocked = false;
    assert(call_poll(fds, N) == 1);
    assert(fds[0].fd == -1 && (fds[0].revents & POLLIN));
    clear_scope();

    fds[0] = (struct pollfd){.fd = 5, .events = POLLIN};
    fds[4096] = (struct pollfd){.fd = 5, .events = POLLIN};
    block_write = true;
    block_after = preparations + 6;
    blocked = true;
    assert(call_poll(fds, N) == KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(fds[0].revents & POLLIN);
    files[5].used = 0; /* Do not repeat the completed readiness scan. */
    blocked = false;
    assert(call_poll(fds, N) == 2);
    assert(fds[4096].revents & POLLIN);
    clear_scope();
    /* A bitmap larger than one slice also survives an import or output wait. */
    enum { WORDS = 4097, SELECT_NFDS = WORDS * 64 };
    uint64_t *mask = calloc(WORDS, sizeof(*mask));
    assert(mask);
    current.limits.nofile.rlim_cur = SELECT_NFDS;
    set_readable_file(5);
    mask[0] = 1ULL << 5;
    block_address = (uintptr_t)&mask[4096];
    block_write = false;
    block_after = preparations + 1;
    blocked = true;
    assert(call_select(SELECT_NFDS, mask, NULL) ==
           KERNEL_SYSCALL_SWAP_CONTENDED);
    mask[0] = 0;
    blocked = false;
    assert(call_select(SELECT_NFDS, mask, NULL) == 1);
    assert(mask[0] == (1ULL << 5));
    clear_scope();

    mask[0] = 1ULL << 5;
    block_write = true;
    block_after = preparations + 6;
    blocked = true;
    assert(call_select(SELECT_NFDS, mask, NULL) ==
           KERNEL_SYSCALL_SWAP_CONTENDED);
    files[5].used = 0;
    blocked = false;
    assert(call_select(SELECT_NFDS, mask, NULL) == 1);
    assert(mask[0] == (1ULL << 5));
    clear_scope();
    set_readable_file(5);
    mask[0] = 1ULL << 5;
    assert(call_select(6, mask, mask) == 1);
    assert(mask[0] == (1ULL << 5));
    clear_scope();
    free(mask);
    assert(preparations);
    free(fds);
    puts("PASS poll/select: bounded snapshots, immutable prefixes and completed-result retries");
    return 0;
}
#endif
