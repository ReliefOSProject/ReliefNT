/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <reliefnt/module.h>
static unsigned filesystem_calls,block_calls,device_calls,subsystem_calls;
static int fs_start(void) { ++filesystem_calls;assert(!device_calls);return 0; }
static int block_start(void) { ++block_calls;assert(filesystem_calls==1 && !device_calls);return -19; }
static int device_start(void) { ++device_calls;assert(filesystem_calls==1 && block_calls==1 && subsystem_calls==1);return -5; }
static int subsystem_start(void){++subsystem_calls;assert(filesystem_calls==1 && block_calls==1 && !device_calls);return -12;}
fs_initcall(fs_start);
block_initcall(block_start);
module_init(device_start);
subsys_initcall(subsystem_start);
void console_log(unsigned level,const char *format,...) { (void)level;(void)format; }
int main(void)
{
    assert(!filesystem_calls && !device_calls);
    assert(!module_run_fs_initcalls() && filesystem_calls==1 && !device_calls);
    assert(!module_run_fs_initcalls() && filesystem_calls==1 && !device_calls);
    assert(module_run_block_initcalls()==-19 && block_calls==1 && !device_calls);
    assert(module_run_block_initcalls()==-19 && block_calls==1 && !device_calls);
    assert(module_run_builtin_initcalls()==-12 && device_calls==1);
    assert(module_run_builtin_initcalls()==-12 && device_calls==1);
    assert(!module_run_fs_initcalls() && filesystem_calls==1);
    puts("PASS initcall phases: filesystem and block providers before root, ordinary devices later, exactly once and retained failure");
}
