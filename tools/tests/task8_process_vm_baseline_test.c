/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: process vm baseline.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
#define main inherited_process_vm_main
#include "process_vm_test.c"
#undef main
#include <reliefnt/swap.h>
int task_lookup_scope_next(struct task *task,struct task_lookup_path **out)
{ (void)task;*out=NULL;return 0; }
int swap_prepare_user_range(struct task *task,uint64_t address,uint64_t length,bool writing)
{ (void)task;(void)address;(void)length;(void)writing;return 0; }
int sched_task_identity_get(struct task *task,struct task_identity **out)
{ (void)task;(void)out;abort(); }
struct task *sched_task_identity_task(const struct task_identity *identity)
{ (void)identity;abort(); }
void sched_task_identity_put(struct task_identity *identity)
{ (void)identity;abort(); }
uint64_t mm_context_generation_existing(const struct address_space *as)
{ (void)as;abort(); }
int mm_context_ensure_generation(struct address_space *as,uint64_t *generation)
{ (void)as;(void)generation;abort(); }
int main(void) { return inherited_process_vm_main(); }
