/*
 * ReliefOS panic interface: declares the concise fatal-error entry point.
 * Forwards unrecoverable conditions to the kernel bugcheck subsystem.
 */
#ifndef RELIEFNT_PANIC_H
#define RELIEFNT_PANIC_H

#include <reliefnt/bugcheck.h>

/**
 * @brief Enter the full kernel panic/bugcheck path without returning.
 * @param message Borrowed diagnostic text passed to bugcheck_panic.
 * Fatal kernel context; stops normal execution and uses the existing panic reporting/display policy. The caller must not expect cleanup or return.
 */
__attribute__((noreturn)) void panic(const char *message);

#endif
