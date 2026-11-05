/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#ifdef STORAGE_CONTEXT_MISSING
int main(void){puts("FAIL: background context inherits the user's asynchronous storage state");return 1;}
#else
#include <reliefnt/storage_context.h>
#include "../../block/core/storage_internal.h"
#include <reliefnt/sched.h>
bool storage_io_async_context,storage_io_write_started;
uint32_t storage_task_io_owner;
static uint32_t pid=9;
uint32_t sched_current_pid(void){return pid;}
uint32_t smp_cpu_count(void){return 1;}
int main(void)
{
    storage_io_async_context=true;storage_io_write_started=false;storage_task_io_owner=8;
    struct storage_io_context saved;
    storage_kernel_context_enter(&saved);
    assert(!storage_io_async_context && storage_io_write_started);
    assert(storage_acquire_task_io()==-11 && storage_task_io_owner==8);
    storage_task_io_owner=0;assert(!storage_acquire_task_io());
    storage_kernel_context_leave(&saved);
    assert(storage_io_async_context && !storage_io_write_started);
    assert(!storage_acquire_task_io() && storage_task_io_owner==9);
    puts("PASS background storage scope preserves user flags and rejects a competing pending owner");return 0;
}
#endif
