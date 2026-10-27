#include <reliefnt/export.h>
/*
 * ReliefOS panic wrapper: provides the short fatal-error API.
 * Converts a panic message into the full kernel bugcheck path.
 */
#include <reliefnt/panic.h>

/**
 * @brief Enter the full kernel panic/bugcheck path without returning.
 * @param message Borrowed diagnostic text passed to bugcheck_panic.
 * Fatal kernel context; stops normal execution and uses the existing panic reporting/display policy. The caller must not expect cleanup or return.
 */
__attribute__((noreturn)) void panic(const char *message)
{
    bugcheck_panic(message);
}

EXPORT_SYMBOL(panic);
