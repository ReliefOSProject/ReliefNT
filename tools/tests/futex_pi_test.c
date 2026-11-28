/* SPDX-License-Identifier: Apache-2.0 */
#define main ordinary_futex_fixture_main
#include "futex_queue_test.c"
#undef main

static void reset_fixture(void)
{
    assert(!retained_pages);
    memset(tasks_test, 0, sizeof(tasks_test));
    memset(words, 0, sizeof(words));
    futex_lock.state = 0;
    futex_waiters = NULL;
    ticks = 100;
    for (unsigned i = 0; i < 4; ++i) {
        tasks_test[i].pid = i + 1;
        tasks_test[i].kind = TASK_KIND_USER;
        tasks_test[i].state = TASK_READY;
        tasks_test[i].as.cr3 = i == 3 ? 8192 : 4096;
    }
    current = &tasks_test[0];
}

static long pi(unsigned thread, unsigned word, unsigned operation, uint64_t timeout)
{
    return call(thread, word, operation | FUTEX_PRIVATE_FLAG, 0, timeout, 0, 0);
}

static void lock_and_handoff(void)
{
    reset_fixture();
    assert(pi(0, 0, FUTEX_LOCK_PI, 0) == 0);
    assert(words[0] == 1);
    assert(pi(0, 0, FUTEX_LOCK_PI, 0) == -EDEADLK);
    assert(pi(1, 0, FUTEX_TRYLOCK_PI, 0) == -EAGAIN);
    assert(pi(1, 0, FUTEX_UNLOCK_PI, 0) == -EPERM);
    assert(pi(1, 0, FUTEX_LOCK_PI, 0) == KERNEL_SYSCALL_BLOCKED);
    assert(pi(2, 0, FUTEX_LOCK_PI, 0) == KERNEL_SYSCALL_BLOCKED);
    assert(words[0] == (1 | FUTEX_WAITERS));
    assert(pi(0, 0, FUTEX_UNLOCK_PI, 0) == 0);
    assert((words[0] & FUTEX_TID_MASK) == 2);
    assert(pi(1, 0, FUTEX_LOCK_PI, 0) == 0);
    assert(pi(1, 0, FUTEX_UNLOCK_PI, 0) == 0);
    assert((words[0] & FUTEX_TID_MASK) == 3);
    assert(pi(2, 0, FUTEX_LOCK_PI, 0) == 0);
    assert(pi(2, 0, FUTEX_UNLOCK_PI, 0) == 0);
    assert(!words[0] && !futex_waiters);
    words[0] = 999;
    assert(pi(0, 0, FUTEX_LOCK_PI, 0) == -ESRCH);
    words[0] = 0;
    assert(pi(0, 0, FUTEX_TRYLOCK_PI, 1) == 0);
    assert(pi(0, 0, FUTEX_UNLOCK_PI, 1) == 0);
}

static void deadlines_and_cancel(void)
{
    reset_fixture();
    struct linux_timespec future = {1000, 710000000};
    struct linux_timespec expired = {0, 0};
    assert(pi(0, 0, FUTEX_LOCK_PI, (uintptr_t)&expired) == 0);
    assert(pi(1, 0, FUTEX_LOCK_PI, (uintptr_t)&expired) == -ETIMEDOUT);
    assert(pi(1, 0, FUTEX_LOCK_PI, (uintptr_t)&future) == KERNEL_SYSCALL_BLOCKED);
    ++ticks;
    assert(pi(1, 0, FUTEX_LOCK_PI, (uintptr_t)&future) == -ETIMEDOUT);
    assert(!futex_waiters);
    assert(pi(1, 0, FUTEX_LOCK_PI, 0) == KERNEL_SYSCALL_BLOCKED);
    tasks_test[1].pending_signals = 1ULL << 9;
    assert(pi(1, 0, FUTEX_LOCK_PI, 0) == KERNEL_SYSCALL_BLOCKED);
    assert(tasks_test[1].futex_pi && tasks_test[1].futex_state == 1);
    assert(tasks_test[1].state == TASK_READY);
    futex_cancel_wait(&tasks_test[1]);
    tasks_test[1].pending_signals = 0;
    assert(!futex_waiters);
    assert(pi(0, 0, FUTEX_UNLOCK_PI, 0) == 0);
    assert(pi(0, 0, FUTEX_LOCK_PI | FUTEX_CLOCK_REALTIME, 0) == -ENOSYS);
    assert(pi(0, 0, FUTEX_LOCK_PI2, 0) == 0);
    future = (struct linux_timespec){1, 20000000};
    assert(pi(1, 0, FUTEX_LOCK_PI2, (uintptr_t)&future) == KERNEL_SYSCALL_BLOCKED);
    ++ticks;
    assert(pi(1, 0, FUTEX_LOCK_PI2, (uintptr_t)&future) == -ETIMEDOUT);
    assert(pi(0, 0, FUTEX_UNLOCK_PI, 0) == 0);
}

static void cycles_and_queue_types(void)
{
    reset_fixture();
    assert(pi(0, 0, FUTEX_LOCK_PI, 0) == 0);
    assert(pi(1, 1, FUTEX_LOCK_PI, 0) == 0);
    assert(pi(0, 1, FUTEX_LOCK_PI, 0) == KERNEL_SYSCALL_BLOCKED);
    assert(pi(1, 0, FUTEX_LOCK_PI, 0) == -EDEADLK);
    assert(call(2, 1, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 1, 0, 0, 0) == -EINVAL);
    assert(call(2, 1, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, words[1], 0, 0, 0) == -EINVAL);
    assert(call(2, 1, FUTEX_REQUEUE | FUTEX_PRIVATE_FLAG, 0, 1, 2, 0) == -EINVAL);
    futex_cancel_wait(&tasks_test[0]);
    assert(pi(0, 0, FUTEX_UNLOCK_PI, 0) == 0);
    assert(pi(1, 1, FUTEX_UNLOCK_PI, 0) == 0);
    assert(call(2, 0, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, 0, 0, 0, 0) == KERNEL_SYSCALL_BLOCKED);
    assert(pi(0, 0, FUTEX_LOCK_PI, 0) == -EINVAL);
    futex_cancel_wait(&tasks_test[2]);
}

static void shared_and_owner_exit(void)
{
    reset_fixture();
    assert(call(0, 0, FUTEX_LOCK_PI, 0, 0, 0, 0) == 0);
    assert(call(3, 0, FUTEX_LOCK_PI, 0, 0, 0, 0) == KERNEL_SYSCALL_BLOCKED);
    assert(retained_pages == 1);
    assert(call(0, 0, FUTEX_UNLOCK_PI, 0, 0, 0, 0) == 0);
    assert(!retained_pages && words[0] == 4);
    assert(call(3, 0, FUTEX_LOCK_PI, 0, 0, 0, 0) == 0);
    assert(call(3, 0, FUTEX_UNLOCK_PI, 0, 0, 0, 0) == 0);

    robust = (struct linux_robust_list_head){(uintptr_t)&robust_node | 1, 8, 0};
    robust_node.next = (uintptr_t)&robust;
    robust_node.word = tasks_test[1].pid;
    tasks_test[1].robust_list = (uintptr_t)&robust;
    current = &tasks_test[0];
    assert(syscall_futex((uintptr_t)&robust_node.word, FUTEX_LOCK_PI | FUTEX_PRIVATE_FLAG,
                         0, 0, 0, 0) == KERNEL_SYSCALL_BLOCKED);
    futex_task_exit(&tasks_test[1]);
    assert((robust_node.word & (FUTEX_TID_MASK | FUTEX_OWNER_DIED)) == (1 | FUTEX_OWNER_DIED));
    assert(syscall_futex((uintptr_t)&robust_node.word, FUTEX_LOCK_PI | FUTEX_PRIVATE_FLAG,
                         0, 0, 0, 0) == 0);
    assert(syscall_futex((uintptr_t)&robust_node.word, FUTEX_UNLOCK_PI | FUTEX_PRIVATE_FLAG,
                         0, 0, 0, 0) == 0);
    assert(!futex_waiters && !retained_pages);
}

int main(void)
{
    lock_and_handoff();
    deadlines_and_cancel();
    cycles_and_queue_types();
    shared_and_owner_exit();
    puts("PASS PI futex: ownership, FIFO handoff, deadlocks, clocks, cancellation, queue types and robust exit");
    return 0;
}
