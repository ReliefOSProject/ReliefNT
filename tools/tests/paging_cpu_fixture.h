/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_PAGING_CPU_FIXTURE_H
#define RELIEFNT_PAGING_CPU_FIXTURE_H
#include <stdint.h>

/** @brief Select the sole logical CPU of a hosted page-table fixture.
 * @return CPU zero; production CPUID caching needs no host APIC/GS setup.
 */
uint32_t smp_current_cpu(void) { return 0; }

#endif
