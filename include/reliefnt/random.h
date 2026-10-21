#ifndef RELIEFNT_RANDOM_H
#define RELIEFNT_RANDOM_H

#include <reliefnt/types.h>

/* Hardware DRBG output only. Failure never substitutes time/PID/counter data. */
int kernel_random_fill(void *buffer, size_t length);

#endif
