/* Private shared-data ABI between ReliefNT and its embedded x86-64 vDSO. */
#ifndef RELIEFNT_VDSO_DATA_H
#define RELIEFNT_VDSO_DATA_H

#include <stdint.h>

#define RELIEFNT_VDSO_CLOCKS 12
#define RELIEFNT_VDSO_MODE_NONE 0u
#define RELIEFNT_VDSO_MODE_TSC 1u
#define RELIEFNT_VDSO_SHIFT 32u

struct reliefnt_vdso_timestamp {
    int64_t sec;
    uint64_t nsec;
};

struct reliefnt_vdso_data {
    uint32_t seq;
    uint32_t clock_mode;
    uint32_t clock_mask;
    uint32_t hres_mask;
    uint64_t cycle_last;
    uint64_t mult;
    uint64_t max_cycles;
    uint64_t resolution_ns;
    struct reliefnt_vdso_timestamp basetime[RELIEFNT_VDSO_CLOCKS];
};

void time_vdso_bind(struct reliefnt_vdso_data *data);

#endif
