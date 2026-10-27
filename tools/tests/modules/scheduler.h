/* Test-only white-box composition of the separately built production units. */
#ifndef RELIEFNT_TEST_MODULE_SCHEDULER_H
#define RELIEFNT_TEST_MODULE_SCHEDULER_H
#ifdef RELIEFNT_TEST_EXEC_EXIT_BOUNDARY
static void fixture_exec_other_exit(uint32_t pid, uint64_t code);
#define sched_exit fixture_exec_other_exit
#endif
#include "../../../kernel/sched/exec.c"
#ifdef RELIEFNT_TEST_EXEC_EXIT_BOUNDARY
#undef sched_exit
#endif
#include "../../../kernel/sched/task.c"
#include "../../../kernel/sched/core.c"
#include "../../../kernel/sched/fair.c"
#include "../../../kernel/sched/fs.c"
#include "../../../kernel/sched/identity.c"
#include "../../../kernel/sched/tables.c"
#include "../../../kernel/sched/lookup.c"
#include "../../../kernel/sched/session.c"
#include "../../../kernel/sched/accounting.c"
#include "../../../kernel/sched/clone.c"
#include "../../../kernel/sched/signal.c"
#include "../../../kernel/sched/timers.c"
#include "../../../kernel/sched/wait.c"
#include "../../../fs/vfs/mount_tree.c"
#endif
