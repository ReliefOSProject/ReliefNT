#include <stdint.h>

#include "../include/reliefnt/vdso_data.h"

#define VDSO_CLOCK_REALTIME 0
#define VDSO_CLOCK_MONOTONIC 1
#define VDSO_CLOCK_REALTIME_COARSE 5
#define VDSO_CLOCK_MONOTONIC_COARSE 6
#define VDSO_CLOCKS 12
#define VDSO_RETRIES 8
#define NSEC_PER_SEC UINT64_C(1000000000)

struct vdso_timespec {
	int64_t tv_sec;
	int64_t tv_nsec;
};

struct vdso_snapshot {
	uint32_t clock_mode;
	uint32_t clock_mask;
	uint32_t hres_mask;
	uint64_t cycle_last;
	uint64_t cycle_now;
	uint64_t mult;
	uint64_t max_cycles;
	uint64_t resolution_ns;
	struct reliefnt_vdso_timestamp basetime;
};

extern struct reliefnt_vdso_data __vvar_data;

static uint64_t vdso_read_tsc(void);

static const volatile struct reliefnt_vdso_data *vdso_data_address(void)
{
	const volatile struct reliefnt_vdso_data *data;

	__asm__ volatile("leaq __vvar_data(%%rip), %0" : "=r"(data));
	return data;
}

static long vdso_syscall2(long number, long clock_id, long output)
{
	long result;

	__asm__ volatile("syscall"
			 : "=a"(result)
			 : "0"(number), "D"(clock_id), "S"(output)
			 : "rcx", "r11", "cc", "memory");
	return result;
}

static int vdso_fast_clock(int clock_id)
{
	return clock_id == VDSO_CLOCK_REALTIME ||
	       clock_id == VDSO_CLOCK_MONOTONIC ||
	       clock_id == VDSO_CLOCK_REALTIME_COARSE ||
	       clock_id == VDSO_CLOCK_MONOTONIC_COARSE;
}

static int vdso_read_snapshot(int clock_id, struct vdso_snapshot *snapshot,
			     int sample_tsc)
{
	const volatile struct reliefnt_vdso_data *data = vdso_data_address();
	uint32_t bit;
	unsigned int attempt;

	if (clock_id < 0 || clock_id >= VDSO_CLOCKS)
		return 0;
	bit = UINT32_C(1) << (unsigned int)clock_id;

	for (attempt = 0; attempt < VDSO_RETRIES; ++attempt) {
		uint32_t seq = __atomic_load_n(&data->seq, __ATOMIC_ACQUIRE);
		if (seq & 1U)
			continue;

		snapshot->clock_mode = data->clock_mode;
		snapshot->clock_mask = data->clock_mask;
		snapshot->hres_mask = data->hres_mask;
		snapshot->cycle_last = data->cycle_last;
		snapshot->mult = data->mult;
		snapshot->max_cycles = data->max_cycles;
		snapshot->resolution_ns = data->resolution_ns;
		snapshot->basetime.sec = data->basetime[clock_id].sec;
		snapshot->basetime.nsec = data->basetime[clock_id].nsec;
		if (sample_tsc &&
		    (clock_id == VDSO_CLOCK_REALTIME || clock_id == VDSO_CLOCK_MONOTONIC) &&
		    snapshot->clock_mode == RELIEFNT_VDSO_MODE_TSC &&
		    (snapshot->hres_mask & bit))
			snapshot->cycle_now = vdso_read_tsc();

		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		if (seq == __atomic_load_n(&data->seq, __ATOMIC_RELAXED) &&
		    (snapshot->clock_mask & bit))
			return 1;
	}
	return 0;
}

static uint64_t vdso_read_tsc(void)
{
	uint32_t low;
	uint32_t high;

	__asm__ volatile("lfence\n\trdtsc\n\tlfence"
			 : "=a"(low), "=d"(high)
			 :
			 : "memory");
	return ((uint64_t)high << 32) | low;
}

static int vdso_timespec_from_snapshot(int clock_id,
				       const struct vdso_snapshot *snapshot,
				       struct vdso_timespec *result)
{
	uint64_t nsec = snapshot->basetime.nsec;
	uint64_t increment = 0;
	uint32_t bit = UINT32_C(1) << (unsigned int)clock_id;
	int64_t sec = snapshot->basetime.sec;

	if (clock_id == VDSO_CLOCK_REALTIME || clock_id == VDSO_CLOCK_MONOTONIC) {
		uint64_t cycles;
		uint64_t now;
		__uint128_t scaled;

		if (!(snapshot->hres_mask & bit) ||
		    snapshot->clock_mode != RELIEFNT_VDSO_MODE_TSC ||
		    !snapshot->mult || !snapshot->max_cycles)
			return 0;

		now = snapshot->cycle_now;
		if (now < snapshot->cycle_last)
			return 0;
		cycles = now - snapshot->cycle_last;
		if (cycles > snapshot->max_cycles)
			return 0;
		scaled = (__uint128_t)cycles * snapshot->mult;
		scaled >>= RELIEFNT_VDSO_SHIFT;
		if (scaled > UINT64_MAX)
			return 0;
		increment = (uint64_t)scaled;
	} else if (snapshot->hres_mask & bit) {
		return 0;
	}

	if (UINT64_MAX - nsec < increment)
		return 0;
	nsec += increment;
	if (nsec >= NSEC_PER_SEC) {
		uint64_t seconds = nsec / NSEC_PER_SEC;
		if (seconds > INT64_MAX || sec > INT64_MAX - (int64_t)seconds)
			return 0;
		sec += (int64_t)seconds;
		nsec %= NSEC_PER_SEC;
	}
	result->tv_sec = sec;
	result->tv_nsec = (int64_t)nsec;
	return 1;
}

int __vdso_clock_gettime(int clock_id, struct vdso_timespec *tp)
	__attribute__((visibility("default")));
int __vdso_clock_getres(int clock_id, struct vdso_timespec *tp)
	__attribute__((visibility("default")));

int __vdso_clock_gettime(int clock_id, struct vdso_timespec *tp)
{
	struct vdso_snapshot snapshot;
	struct vdso_timespec result;

	if (!tp || !vdso_fast_clock(clock_id) ||
	    !vdso_read_snapshot(clock_id, &snapshot, 1) ||
	    !vdso_timespec_from_snapshot(clock_id, &snapshot, &result))
		return (int)vdso_syscall2(228, clock_id, (long)tp);

	*tp = result;
	return 0;
}

int __vdso_clock_getres(int clock_id, struct vdso_timespec *tp)
{
	struct vdso_snapshot snapshot;
	struct vdso_timespec result;
	uint32_t bit;

	if (!tp || !vdso_fast_clock(clock_id) ||
	    clock_id == VDSO_CLOCK_REALTIME_COARSE ||
	    clock_id == VDSO_CLOCK_MONOTONIC_COARSE ||
	    !vdso_read_snapshot(clock_id, &snapshot, 0))
		return (int)vdso_syscall2(229, clock_id, (long)tp);

	bit = UINT32_C(1) << (unsigned int)clock_id;
	if (!(snapshot.hres_mask & bit) ||
	    snapshot.clock_mode != RELIEFNT_VDSO_MODE_TSC ||
	    !snapshot.resolution_ns)
		return (int)vdso_syscall2(229, clock_id, (long)tp);

	result.tv_sec = (int64_t)(snapshot.resolution_ns / NSEC_PER_SEC);
	result.tv_nsec = (int64_t)(snapshot.resolution_ns % NSEC_PER_SEC);
	*tp = result;
	return 0;
}

int clock_gettime(int clock_id, struct vdso_timespec *tp)
	__attribute__((alias("__vdso_clock_gettime"), visibility("default")));
int clock_getres(int clock_id, struct vdso_timespec *tp)
	__attribute__((alias("__vdso_clock_getres"), visibility("default")));
