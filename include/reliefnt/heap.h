/*
 * ReliefOS kernel heap interface.
 * Allocates page-backed kernel memory with explicit ownership and release.
 */
#ifndef RELIEFNT_HEAP_H
#define RELIEFNT_HEAP_H

#include <reliefnt/types.h>

void kernel_heap_init(void);
void *kernel_malloc(size_t size);
void kernel_free(void *memory);

#endif
