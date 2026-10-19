/*
 * ReliefOS x86_64 IDT interface: declares interrupt-table initialization.
 * Shared by architecture startup and the interrupt implementation.
 */
#ifndef RELIEFNT_X86_64_IDT_H
#define RELIEFNT_X86_64_IDT_H

struct task;
struct trap_frame;

/**
 * Idt init.
 */
void idt_init(void);

/**
 * Load the already initialized shared IDT on another CPU.
 *
 * The IDT entries are process-wide kernel text pointers and are immutable
 * after boot.  APs must only load the table; rebuilding it concurrently can
 * expose partially written gate descriptors to another CPU.
 */
void idt_load(void);

/**
 * @brief Dispatch a general-protection fault using its complete saved frame.
 * @param frame Interrupted register state, including the CPU error code and user iret frame.
 * @return The task selected by the existing user-return scheduler, or NULL if it cannot select one.
 */
struct task *general_protection_dispatch(struct trap_frame *frame);

#endif
