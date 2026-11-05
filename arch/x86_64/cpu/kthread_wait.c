/* SPDX-License-Identifier: Apache-2.0 */
/** @brief Wait for one IRQ with an atomic STI/HLT admission window.
 * @return With IRQs disabled. Caller has no execution/spin lock or live user
 * operands; used only by explicit synchronous kernel-thread drain waits.
 */
void arch_kthread_idle(void)
{ __asm__ volatile("sti; hlt; cli" ::: "memory"); }
