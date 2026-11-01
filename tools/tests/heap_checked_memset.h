/* Test boundary for observing production slab clearing. */
#ifndef RELIEFNT_TEST_HEAP_CHECKED_MEMSET_H
#define RELIEFNT_TEST_HEAP_CHECKED_MEMSET_H
#include <stddef.h>
/** @brief Fill bytes while verifying that the heap lock is not held.
 * @param dest Writable payload. @param value Fill byte. @param bytes Number of bytes.
 * @return The original destination.
 */
void *fixture_checked_memset(void *dest, int value, size_t bytes);
#endif
