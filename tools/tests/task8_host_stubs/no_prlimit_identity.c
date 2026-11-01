/* SPDX-License-Identifier: Apache-2.0 */
/* Fixtures without prlimit identity ownership must never release one. A real
 * scheduler identity implementation, when linked, overrides this test stub. */
#include <reliefnt/sched.h>
#include <stdlib.h>
__attribute__((weak)) void sched_task_identity_put(struct task_identity *identity)
{ (void)identity;abort(); }
