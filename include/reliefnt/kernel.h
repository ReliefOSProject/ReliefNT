/*
 * ReliefOS kernel lifecycle interface: declares top-level kernel initialization.
 * Defines the entry point used after architecture and boot setup complete.
 */
#ifndef RELIEFNT_KERNEL_H
#define RELIEFNT_KERNEL_H

#include <reliefnt/types.h>

/**
 * @brief Top-level kernel entry point, called once by the Multiboot2 entry stub.
 * @param multiboot_info Borrowed full-width bootloader block, retained until reserved.
 * BSP with IRQs disabled; owns initial stack and acquires no outer transaction.
 */
void kernel_main(uint32_t magic, uintptr_t multiboot_info);
void kernel_idle_loop(void) __attribute__((noreturn));

#endif
