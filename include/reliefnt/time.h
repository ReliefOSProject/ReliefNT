/*
 * ReliefOS time interface: declares kernel tick and clock services.
 * Provides monotonic time and timer coordination for scheduling and devices.
 */
#ifndef RELIEFNT_TIME_H
#define RELIEFNT_TIME_H

#include <reliefnt/types.h>

#define RELIEFNT_TICK_HZ 100ULL

struct reliefos_time_info;
struct linux_timespec;
struct linux_timex;
/** @brief Apply/query the actual clock discipline with privilege checked by the caller. */
int time_adjust(struct linux_timex *value, bool privileged);
/** @brief Read a supported wall/monotonic clock atomically across CPUs and IRQs.
 * @param clock Linux clock ID. @param value Writable caller-owned timespec output.
 * @return Zero or EINVAL for an unsupported clock; no pointer ownership transfer.
 * IRQ/task safe short clock lock; no allocation, scheduling or hardware wait.
 */
int time_clock_get(int32_t clock, struct linux_timespec *value);
/**
 * @brief Validate a supported Linux clock and copy its current resolution.
 * @param clock Linux clock selector.
 * @param value Optional writable kernel result; NULL validates without copying.
 * @return Zero or EINVAL for an unsupported clock, leaving output unchanged on failure.
 * Task/IRQ-safe query under the clock lock, with no user access, allocation or ownership transfer.
 */
int time_clock_getres(int32_t clock, struct linux_timespec *value);

/**
 * @brief Initialize the tick counter and read the RTC wall clock.
 */
void time_init(void);
/**
 * @brief Advance the clock and service devices after releasing the clock lock.
 * @return None. IRQ context; audio service consumes at most 32 completions;
 * no codec response wait or sleeping callback is permitted.
 */
void time_on_tick(void);
/**
 * @brief Read the aligned monotonic boot tick counter.
 * @return Number of delivered timer ticks since initialization; no wall-clock adjustment.
 * @par Context
 * Boot/task/IRQ snapshot on x86_64, without allocation, wait, reference transfer or clock lock.
 */
uint64_t time_ticks(void);
/**
 * @brief Return milliseconds elapsed since boot.
 */
uint64_t time_uptime_ms(void);
/**
 * @brief Read boot uptime using bounded TSC interpolation with PIT/tick fallback.
 * @return Best-effort elapsed microseconds from boot, with no wall-clock adjustment.
 * Task/IRQ context after time setup; no allocation or retained pointer. Retries preserve paired tick anchors; failed interpolation falls back to the existing lower-resolution clock path.
 */
uint64_t time_uptime_us(void);
/**
 * @brief Publish the active PIT divisor for sub-tick uptime interpolation.
 */
void time_set_pit_divisor(uint16_t divisor);
/**
 * @brief Copy a consistent wall-clock calendar and uptime snapshot.
 * @param info Required writable kernel output, updated only when the clock is valid.
 * @return Zero after a valid snapshot, or internal -1 for NULL output or an invalid wall clock.
 * @par Context
 * Acquires the IRQ-safe clock lock; no user-pointer access, allocation or ownership transfer. Timezone conversion is not performed.
 */
int time_wall_clock(struct reliefos_time_info *info);
/**
 * @brief Set the wall clock to unix_seconds (Unix epoch); 0 on success.
 */
int time_set_wall_clock(uint64_t unix_seconds);
int time_set_wall_clock_ns(uint64_t unix_seconds, uint32_t nanoseconds);
/**
 * @brief Busy-sleep the current CPU for ms milliseconds.
 */
void time_sleep_ms(uint64_t ms);

#endif
