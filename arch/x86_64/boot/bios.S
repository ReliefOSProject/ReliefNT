/* SPDX-License-Identifier: Apache-2.0 */
.section .text.boot, "ax"
.code32
.global bios_start
.type bios_start, @function
/**
 * @brief Multiboot2 i386 entry: own BSP state and enter long mode.
 * EAX is magic and EBX a borrowed 32-bit info pointer. Runs with IRQs off,
 * no allocator or locks; all tables/stacks below belong to the kernel image.
 */
bios_start:
    cli
    cld
    mov %eax, %esi
    mov %ebx, %edi
    lgdt bios_gdt_pointer
    ljmp $0x08, $bios_protected
bios_protected:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov %ax, %fs
    mov %ax, %gs
    mov $bios_stack_top, %esp
    and $-16, %esp
    /* ESP and GDTR were unspecified at handoff; only now may we use a stack. */
    pushfl
    pop %eax
    mov %eax, %ecx
    xor $0x200000, %eax
    push %eax
    popfl
    pushfl
    pop %eax
    xor %ecx, %eax
    push %ecx
    popfl
    test $0x200000, %eax
    jz bios_cpu_error
    mov $1, %eax
    cpuid
    and $0x68, %edx /* PSE, MSR, PAE */
    cmp $0x68, %edx
    jne bios_cpu_error
    mov $0x80000000, %eax
    cpuid
    cmp $0x80000001, %eax
    jb bios_cpu_error
    mov $0x80000001, %eax
    cpuid
    test $0x20000000, %edx
    jz bios_cpu_error

    mov $bios_page_tables, %ebx
    mov $(6 * 4096 / 4), %ecx
1:  movl $0, (%ebx)
    add $4, %ebx
    loop 1b
    mov $(bios_pdpt + 3), %eax
    mov %eax, bios_page_tables
    mov $bios_pdpt, %edx
    mov $(bios_pd0 + 3), %eax
    mov $4, %ecx
2:  mov %eax, (%edx)
    add $4096, %eax
    add $8, %edx
    loop 2b
    mov $bios_pd0, %edx
    mov $0x83, %eax /* Present, writable, supervisor-only 2 MiB leaf. */
    mov $2048, %ecx
3:  mov %eax, (%edx)
    add $0x200000, %eax
    add $8, %edx
    loop 3b

    mov $0x20, %eax /* Known CR4: PAE, no PCID/LA57 or inherited policies. */
    mov %eax, %cr4
    mov $bios_page_tables, %eax
    mov %eax, %cr3
    mov $0xc0000080, %ecx
    rdmsr
    or $0x100, %eax
    wrmsr
    mov %cr0, %eax
    and $0x9ffffff3, %eax /* Enable caches; clear EM/TS. */
    or $0x80010033, %eax /* PG, WP, NE, ET, MP, PE. */
    mov %eax, %cr0
    ljmp $0x18, $bios_long_mode

/** @brief Emit a bounded early COM1/VGA diagnostic and stop an unsupported CPU. */
bios_cpu_error:
    mov $0x3fb, %dx
    mov $0x80, %al
    out %al, %dx
    mov $0x3f8, %dx
    mov $1, %al
    out %al, %dx
    inc %dx
    xor %al, %al
    out %al, %dx
    mov $0x3fb, %dx
    mov $3, %al
    out %al, %dx
    mov $bios_cpu_message, %esi
    mov $0xb8000, %edi
4:  lodsb
    test %al, %al
    jz 7f
    mov %al, %bl
    mov %al, (%edi)
    movb $0x4f, 1(%edi)
    add $2, %edi
    mov $0x3fd, %dx
    mov $100000, %ecx
5:  in %dx, %al
    test $0x20, %al
    jnz 6f
    loop 5b
6:  mov $0x3f8, %dx
    mov %bl, %al
    out %al, %dx
    jmp 4b
7:  cli
    hlt
    jmp 7b
.size bios_start, . - bios_start

.code64
bios_long_mode:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov %edi, %ebx /* Zero-extend the IA32 info pointer for the shared entry. */
    mov %esi, %eax
    jmp _start

.section .rodata.boot, "a"
.balign 8
bios_gdt:
    .quad 0
    .quad 0x00cf9a000000ffff
    .quad 0x00cf92000000ffff
    .quad 0x00af9a000000ffff
bios_gdt_end:
bios_gdt_pointer:
    .word bios_gdt_end - bios_gdt - 1
    .long bios_gdt
bios_cpu_message:
    .asciz "ReliefNT BIOS: x86-64 CPU with PAE/MSR required\r\n"

.section .bss.boot, "aw", @nobits
.balign 4096
bios_page_tables:
    .skip 4096
bios_pdpt:
    .skip 4096
bios_pd0:
    .skip 4 * 4096
bios_page_tables_end:
.balign 16
bios_stack_bottom:
    .skip 65536
bios_stack_top:
.section .note.GNU-stack, "", @progbits
