/* Call a kernel function on a dedicated scheduler stack and return to the
 * suspended caller stack. SysV x86-64 stack alignment is preserved. */
.global arch_call_on_stack
.type arch_call_on_stack, @function
arch_call_on_stack:
    push %r12
    mov %rsp, %r12
    mov %rdi, %rax
    mov %rsi, %rcx
    mov %rdx, %rdi
    mov %rax, %rsp
    and $-16, %rsp
    call *%rcx
    mov %r12, %rsp
    pop %r12
    ret
.size arch_call_on_stack, .-arch_call_on_stack
