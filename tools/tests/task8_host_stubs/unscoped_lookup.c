/* SPDX-License-Identifier: Apache-2.0 */
/* Fixtures linking no lookup scope must never invoke the scoped import. */
#include <reliefnt/syscall_internal.h>
#include <reliefnt/sched.h>
#include <stdlib.h>
int task_lookup_user_bytes(struct task *task,uint64_t address,uint32_t length,struct task_user_text *out)
{ (void)task;(void)address;(void)length;(void)out;abort(); }
int task_lookup_user_text(struct task *task,uint64_t address,uint32_t length,bool allow_null,struct task_user_text *out)
{ (void)task;(void)address;(void)length;(void)allow_null;(void)out;abort(); }
int task_lookup_scope_next(struct task *task,struct task_lookup_path **out)
{ (void)task;(void)out;abort(); }
int sched_task_identity_get(struct task *task,struct task_identity **out)
{ (void)task;(void)out;abort(); }
struct task *sched_task_identity_task(const struct task_identity *identity)
{ (void)identity;abort(); }
