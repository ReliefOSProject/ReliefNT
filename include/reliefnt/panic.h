/*
 * ReliefOS panic interface: declares the concise fatal-error entry point.
 * Forwards unrecoverable conditions to the kernel bugcheck subsystem.
 */
#ifndef RELIEFNT_PANIC_H
#define RELIEFNT_PANIC_H

#include <reliefnt/bugcheck.h>

/**
 * @brief Print message and halt the kernel; never returns.
 */
__attribute__((noreturn)) void panic(const char *message);

#endif
