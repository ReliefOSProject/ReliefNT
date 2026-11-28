/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_TEST_SIGNAL_ARCH_FIXTURE_H
#define RELIEFNT_TEST_SIGNAL_ARCH_FIXTURE_H
#include <reliefnt/arch.h>
#include <reliefnt/sched.h>
static uint32_t fixture_fpu_size = RELIEFNT_FPU_LEGACY_STATE_SIZE;
static uint64_t fixture_xstate_mask;
uint32_t arch_fpu_state_size(void) { return fixture_fpu_size; }
uint64_t arch_fpu_xstate_mask(void) { return fixture_xstate_mask; }
uint32_t arch_fpu_mxcsr_mask(void) { return 0xffbfU; }
void arch_fpu_task_init(void *state)
{
    __builtin_memset(state, 0, RELIEFNT_FPU_STATE_MAX_SIZE);
    uint16_t control = 0x037fU;
    uint32_t mxcsr = 0x1f80U;
    __builtin_memcpy(state, &control, sizeof(control));
    __builtin_memcpy((uint8_t *)state + 24, &mxcsr, sizeof(mxcsr));
}
#ifndef RELIEFNT_TEST_REAL_RSEQ
int sched_rseq_notify(struct task *task, struct trap_frame *frame, uint32_t cpu, bool fixup)
{ (void)task; (void)frame; (void)cpu; (void)fixup; return 0; }
#endif
#endif
