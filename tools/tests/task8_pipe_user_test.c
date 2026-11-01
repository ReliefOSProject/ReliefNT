/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: pipe user.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
#include <assert.h>
#define main inherited_pipe_wake_main
#include "pipe_wake_test.c"
#undef main
#include "../../kernel/syscall/io/io.c"
#include <reliefnt/swap.h>
static uint64_t swapped;
static bool contended;
static unsigned preparations;
struct task_file *task_file_for_io(struct task *task,int fd)
{ assert(task==current_task);return fd==3 ? &write_end : fd==4 ? &read_end : NULL; }
struct task_pty_fd *task_pty_for_io(struct task *task,int fd)
{ (void)task;(void)fd;return NULL; }
int swap_prepare_user_range(struct task *task,uint64_t address,uint64_t length,bool writing)
{
    (void)writing;assert(task==current_task && length<=32768);++preparations;
    if(address==swapped){if(contended)return SWAP_STORAGE_CONTENDED;swapped=0;}
    return 0;
}
bool user_range_ok(uint64_t address,uint64_t length)
{ return address>=4096 && length<=32768 && address!=swapped; }
bool user_range_writable(uint64_t address,uint64_t length)
{ return user_range_ok(address,length); }
int main(void)
{
    reset_pipe();current_task=&test_tasks[0];current_task->syscall_file=&write_end;
    assert(syscall_io_write(3,(uintptr_t)source_data,TASK_PIPE_CAP)==32768);
    current_task=&test_tasks[READER_INDEX];current_task->syscall_file=&read_end;
    assert(syscall_io_read(4,(uintptr_t)read_data,TASK_PIPE_CAP)==32768);
    assert(!memcmp(source_data,read_data,32768));
    reset_pipe();assert(!fill_pipe(TASK_PIPE_CAP-2048));
    current_task=&test_tasks[1];current_task->syscall_file=&write_end;
    assert(syscall_io_write(3,(uintptr_t)writer_a_data,TEST_PIPE_BUF)==-RELIEFOS_EAGAIN);
    assert(current_task->state==TASK_BLOCKED && current_task->waiting_queue==&pipe_fixture.wait_write);
    /* Model a real readiness wake followed by a competing storage owner. */
    current_task->state=TASK_RUNNING;swapped=(uintptr_t)writer_a_data;contended=true;
    assert(syscall_io_write(3,(uintptr_t)writer_a_data,TEST_PIPE_BUF)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(!current_task->waiting_queue && pipe_fixture.wait_write.head==NULL);
    current_task=&test_tasks[READER_INDEX];assert(task_pipe_read(&read_end,read_data,2048)==2048);
    current_task=&test_tasks[1];current_task->state=TASK_RUNNING;contended=false;
    assert(syscall_io_write(3,(uintptr_t)writer_a_data,TEST_PIPE_BUF)==TEST_PIPE_BUF);
    assert(!swapped && preparations);
    current_task=&test_tasks[READER_INDEX];
    assert(task_pipe_read(&read_end,read_data,TASK_PIPE_CAP)==TASK_PIPE_CAP);
    assert(!memcmp(read_data+TASK_PIPE_CAP-TEST_PIPE_BUF,writer_a_data,TEST_PIPE_BUF));
    puts("PASS scalar and real pipe: 32KiB slice, PIPE_BUF atomic retry and wait removal");
}
