/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <reliefnt/console.h>
#include <reliefnt/module.h>
static unsigned starts[2], exits, warnings,subsystems;
static int subsystem(void){++subsystems;assert(!starts[0] && !starts[1]);return 0;}
static int first(void) { assert(subsystems==1); ++starts[0]; return -5; }
static int second(void) { ++starts[1]; return 0; }
static void stop(void) { ++exits; }
module_init(first);
module_init(second);
subsys_initcall(subsystem);
module_exit(stop);
void console_log(enum console_log_level level,const char *format,...)
{ (void)format; assert(level==CONSOLE_LOG_WARN); ++warnings; }
int main(void)
{
    assert(module_run_builtin_initcalls()==-5);
    assert(starts[0]==1 && starts[1]==1 && warnings==1 && !exits);
    assert(module_run_builtin_initcalls()==-5);
    assert(starts[0]==1 && starts[1]==1 && warnings==1 && !exits);
    puts("PASS builtin initcalls run once, continue after errors, retain first error, never call exits");
}
