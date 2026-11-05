/* SPDX-License-Identifier: Apache-2.0 */
.text
.global arch_kthread_switch
.type arch_kthread_switch,@function
/** @brief Save a SysV callee continuation and resume another retained stack.
 * @param rdi Writable old RSP slot. @param rsi Stable new RSP slot.
 * IRQs disabled, no held locks; GPR-only kernel code needs no FPU switch.
 * @return To the saved caller when this context is next dispatched.
 */
arch_kthread_switch:
    push %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15
    mov %rsp,(%rdi)
    mov (%rsi),%rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp
    ret
.size arch_kthread_switch,.-arch_kthread_switch
.section .note.GNU-stack,"",@progbits
