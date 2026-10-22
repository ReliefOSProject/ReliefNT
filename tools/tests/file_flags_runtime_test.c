#include <assert.h>
#include <stdio.h>
#include <reliefnt/task/types.h>
#include <linux/fcntl.h>

_Static_assert(!(TASK_FILE_FLAG_TIMERFD & LINUX_O_SYNC),
               "O_SYNC must not turn a regular/block descriptor into timerfd");
_Static_assert(!(TASK_FILE_FLAG_EVENTFD & LINUX_O_TMPFILE),
               "O_TMPFILE must not synthesize an eventfd descriptor");

/** @brief Validate the open flag namespace consumed by the real descriptor code. */
int main(void)
{
    const unsigned kinds = TASK_FILE_FLAG_PIPE | TASK_FILE_FLAG_PIPE_WRITE |
        TASK_FILE_FLAG_DEV_NULL | TASK_FILE_FLAG_DEV_NODE | TASK_FILE_FLAG_DEV_BLOCK |
        TASK_FILE_FLAG_SOCKET | TASK_FILE_FLAG_SOCKET_UNIX | TASK_FILE_FLAG_DEV_SHM |
        TASK_FILE_FLAG_SOCKET_INET | TASK_FILE_FLAG_EVENTFD | TASK_FILE_FLAG_TIMERFD;
    const unsigned flags = LINUX_O_ACCMODE | LINUX_O_CREAT | LINUX_O_EXCL |
        LINUX_O_NOCTTY | LINUX_O_TRUNC | LINUX_O_APPEND | LINUX_O_NONBLOCK |
        LINUX_O_DSYNC | LINUX_O_ASYNC | LINUX_O_DIRECT | LINUX_O_LARGEFILE |
        LINUX_O_DIRECTORY | LINUX_O_NOFOLLOW | LINUX_O_NOATIME |
        LINUX_O_CLOEXEC | LINUX_O_SYNC | LINUX_O_TMPFILE;
    assert(!(kinds & flags));
    /* O_PATH is explicitly translated by open/openat, before kind dispatch. */
    assert(!(TASK_FILE_FLAG_PATH & (flags | TASK_FILE_FLAG_EPOLL | kinds)));
    puts("PASS descriptor flags: Linux synchronous/tmpfile flags cannot alias private descriptor kinds");
    return 0;
}
