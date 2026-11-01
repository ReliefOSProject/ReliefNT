/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: pselect user.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
/* Real event dispatch, poll/select and lookup scope; readiness and swap modeled. */
#define TASK8_POLL_NO_MAIN
#include "task8_poll_user_test.c"
#include "../../kernel/syscall/event/legacy_events.c"

int64_t syscall_epoll_create(uint64_t a, uint32_t b) { (void)a;(void)b;assert(0);return 0; }
int64_t syscall_epoll_ctl(uint64_t a,uint64_t b,uint64_t c,uint64_t d)
{(void)a;(void)b;(void)c;(void)d;assert(0);return 0;}
int64_t syscall_epoll_wait(uint64_t a,uint64_t b,uint64_t c,int64_t d)
{(void)a;(void)b;(void)c;(void)d;assert(0);return 0;}
int64_t syscall_epoll_pwait2(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
{(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;assert(0);return 0;}
int64_t syscall_eventfd_create(uint64_t a,uint32_t b) {(void)a;(void)b;assert(0);return 0;}
int64_t syscall_signalfd(int32_t a,uint64_t b,uint64_t c,uint32_t d)
{(void)a;(void)b;(void)c;(void)d;assert(0);return 0;}
int64_t syscall_timerfd_create(uint64_t a,uint32_t b) {(void)a;(void)b;assert(0);return 0;}
int64_t syscall_timerfd_settime(uint64_t a,uint32_t b,uint64_t c,uint64_t d)
{(void)a;(void)b;(void)c;(void)d;assert(0);return 0;}
int64_t syscall_timerfd_gettime(uint64_t a,uint64_t b) {(void)a;(void)b;assert(0);return 0;}

static int64_t pselect_call(uint64_t *fds, uint64_t *mask_arg, bool frame_scope)
{
    struct trap_frame frame = {.rax = LINUX_SYS_PSELECT6, .rdi = 6,
        .rsi = (uintptr_t)fds, .r9 = (uintptr_t)mask_arg};
    if (frame_scope) task_lookup_scope_begin(&current, &frame);
    return syscall_legacy_events(LINUX_SYS_PSELECT6, 6, (uintptr_t)fds, 0, 0, 0,
                                 (uintptr_t)mask_arg);
}

static int64_t ppoll_call(struct pollfd *fds, struct linux_timespec *timeout)
{
    struct trap_frame frame = {.rax = LINUX_SYS_PPOLL, .rdi = (uintptr_t)fds,
                               .rsi = 1, .rdx = (uintptr_t)timeout};
    task_lookup_scope_begin(&current, &frame);
    return syscall_ppoll((uintptr_t)fds, 1, (uintptr_t)timeout, 0, 0);
}

int main(void)
{
    uint64_t sigmask = 0, mask_arg[] = {(uintptr_t)&sigmask, sizeof(sigmask)};
    uint64_t fds = 1ULL << 5, original = 1ULL << 9;
    current.pid = 1; current.kind = TASK_KIND_USER;
    current.limits.nofile.rlim_cur = 16; current.blocked_signals = original;
    set_readable_file(5);
    block_address = (uintptr_t)&fds; block_write = true; block_after = 1; blocked = true;
    assert(pselect_call(&fds, mask_arg, true) == KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(current.sigsuspend_active && current.blocked_signals == 0 &&
           current.sigsuspend_saved_mask == original);
    blocked = false;
    assert(pselect_call(&fds, mask_arg, true) == 1);
    assert(!current.sigsuspend_active && current.blocked_signals == original);
    clear_scope();

    fds = 1ULL << 5; blocked = true; block_after = 1;
    assert(pselect_call(&fds, mask_arg, true) == KERNEL_SYSCALL_SWAP_CONTENDED);
    clear_scope();
    assert(!current.sigsuspend_active && current.blocked_signals == original);

    fds = 1ULL << 5; blocked = true; block_after = preparations + 5;
    assert(pselect_call(&fds, mask_arg, true) == KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(current.sigsuspend_active && current.blocked_signals == 0);
    /* Both descriptor/mask and the finished readiness result are owned. */
    mask_arg[0] = 1; sigmask = UINT64_MAX; fds = 0; files[5].used = 0;
    blocked = false;
    assert(pselect_call(&fds, mask_arg, true) == 1);
    assert(fds == (1ULL << 5) && !current.sigsuspend_active && current.blocked_signals == original);
    clear_scope();
    mask_arg[0] = (uintptr_t)&sigmask; sigmask = 0; set_readable_file(5);

    fds = 1ULL << 5; blocked = true;
    assert(pselect_call(&fds, mask_arg, false) == KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(!current.sigsuspend_active && current.blocked_signals == original);

    struct pollfd request = {.fd = 5, .events = POLLIN};
    struct linux_timespec timeout = {.tv_sec = 2};
    block_address = (uintptr_t)&timeout; block_write = false; block_after = 1; blocked = true;
    assert(ppoll_call(&request, &timeout) == KERNEL_SYSCALL_SWAP_CONTENDED);
    blocked = false;
    assert(ppoll_call(&request, &timeout) == 1 && (request.revents & POLLIN));
    clear_scope();
    request = (struct pollfd){.fd = 5, .events = POLLIN}; timeout.tv_sec = 2;
    block_address = (uintptr_t)&request; block_write = true;
    block_after = preparations + 4; blocked = true;
    assert(ppoll_call(&request, &timeout) == KERNEL_SYSCALL_SWAP_CONTENDED);
    timeout.tv_sec = -1; request.fd = -1; files[5].used = 0;
    blocked = false;
    assert(ppoll_call(&request, &timeout) == 1 && request.fd == -1 && (request.revents & POLLIN));
    clear_scope();
    block_address = (uintptr_t)&sigmask; block_write = false; block_after = 1; blocked = true;
    assert(syscall_ppoll(0, 0, 0, (uintptr_t)&sigmask, 4) == -RELIEFOS_EINVAL);
    assert(syscall_ppoll(0, 0, 0, (uintptr_t)&sigmask, 8) == KERNEL_SYSCALL_SWAP_CONTENDED);
    blocked = false;
    assert(syscall_ppoll(0, 0, 0, (uintptr_t)&sigmask, 8) == -RELIEFOS_EOPNOTSUPP);
    puts("PASS pselect/ppoll: masks, bounded inputs, owned retry and cancel/no-frame restore");
    return 0;
}
