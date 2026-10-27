#include <reliefnt/sound_service.h>
/*
 * ReliefOS kernel timekeeping: provides ticks, clocks, and timer services.
 * Coordinates scheduler wakeups, elapsed time, and periodic device polling.
 */
#include <reliefnt/export.h>
#include <reliefos/system_abi.h>
#include <reliefnt/console.h>
#include <reliefnt/sched.h>
#include <reliefnt/time.h>
#include <reliefnt/usb.h>
#include <linux/time.h>
#include <linux/timex.h>
#include <reliefnt/time_discipline.h>
#include <reliefnt/smp.h>
#include <reliefnt/vdso_data.h>
#include <reliefnt/lock.h>
#include <linux/errno.h>

#include "../../arch/x86_64/port.h"

#define CMOS_ADDRESS 0x70u
#define CMOS_DATA 0x71u
#define CMOS_NMI_DISABLE 0x80u
#define CMOS_SECONDS 0x00u
#define CMOS_MINUTES 0x02u
#define CMOS_HOURS 0x04u
#define CMOS_DAY 0x07u
#define CMOS_MONTH 0x08u
#define CMOS_YEAR 0x09u
#define CMOS_STATUS_A 0x0au
#define CMOS_STATUS_B 0x0bu
#define CMOS_CENTURY 0x32u
#define PIT_CHANNEL0 0x40u
#define PIT_COMMAND 0x43u
#define PIT_LATCH_CHANNEL0 0x00u

static volatile uint64_t ticks;
static uint64_t wall_unix_seconds;
static uint64_t monotonic_seconds, monotonic_ns;
static struct kernel_spinlock clock_lock = KERNEL_SPINLOCK_INIT;
static uint64_t wall_fraction_ns;
static uint8_t wall_clock_valid;
static volatile uint16_t pit_divisor;
/* TSC interpolation replaces the PIT counter port reads on the steady-state
 * path: each port access traps under KVM, so a lock-free reader instead
 * samples a per-tick TSC anchor against a boot-time rate. The sequence
 * counter lets readers detect a tick update landing inside their window
 * rather than pairing one tick's anchor with another's count. */
static volatile uint32_t tsc_anchor_seq;
static volatile uint64_t tsc_anchor;
static volatile uint64_t uptime_tsc_anchor;
static uint64_t tsc_per_us;
static uint64_t tsc_calib_first;
static uint64_t tsc_calib_previous;
static uint64_t tsc_calib_min_interval;
static uint64_t tsc_calib_max_interval;
static uint64_t tsc_calib_confirm_rate;
static uint64_t tsc_calib_span;
static volatile uint8_t tsc_ready;
/* A separate continuous, discipline-aware anchor is used only when RDTSC has
 * been proven safe for user mode. The legacy syscall clock remains intact on
 * systems that do not meet that condition. */
static uint8_t tsc_trusted;
static uint64_t hres_cycle_last, hres_mult, hres_max_cycles, hres_resolution_ns;
static struct reliefnt_vdso_timestamp hres_realtime, hres_monotonic;
static struct reliefnt_vdso_timestamp monotonic_floor;
static uint8_t monotonic_floor_valid;
static struct reliefnt_vdso_data *vdso_data;

#define TIME_NOMINAL_TICK_NS (1000000000ULL / RELIEFNT_TICK_HZ)
#define TIME_NOMINAL_TICK_US (1000000ULL / RELIEFNT_TICK_HZ)
#define TIME_TSC_MIN_CYCLES_PER_US 100ULL
#define TIME_TSC_MAX_CYCLES_PER_US 10000ULL

#if defined(RELIEFNT_TIME_HOST_TEST)
extern uint64_t time_test_rdtsc(void);
extern bool time_test_tsc_safe(void);
#endif

/**
 * @brief Short I/O delay between CMOS register select and read.
 */
static void cmos_wait(void)
{
    x86_64_outb(0, 0x80);
}

/**
 * @brief Read one CMOS register with NMI disabled.
 */
static uint8_t cmos_read(uint8_t reg)
{
    x86_64_outb((uint8_t)(CMOS_NMI_DISABLE | reg), CMOS_ADDRESS);
    cmos_wait();
    return x86_64_inb(CMOS_DATA);
}

/**
 * @brief Return 1 while the RTC is mid-update (status A update flag set).
 */
static int rtc_update_in_progress(void)
{
    return (cmos_read(CMOS_STATUS_A) & 0x80u) != 0;
}

/**
 * @brief Convert a packed BCD byte to its binary value.
 */
static uint32_t bcd_to_binary(uint8_t value)
{
    return (uint32_t)((value & 0x0fu) + ((value >> 4) * 10u));
}

/**
 * @brief Return 1 if year is a leap year.
 */
static int is_leap_year(uint32_t year)
{
    return (year % 4u == 0 && year % 100u != 0) || (year % 400u == 0);
}

/**
 * @brief Return the number of days in month of year (Feb 29 on leap years; 31 for out-of-range months).
 */
static uint32_t days_in_month(uint32_t year, uint32_t month)
{
    static const uint8_t days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && is_leap_year(year)) {
        return 29;
    }
    if (month < 1 || month > 12) {
        return 31;
    }
    return days[month - 1];
}

/**
 * @brief Return 1 if the date/time fields form a plausible calendar value.
 */
static int rtc_datetime_valid(uint32_t year, uint32_t month, uint32_t day,
                              uint32_t hour, uint32_t minute, uint32_t second)
{
    return year >= 1970 && year <= 9999 &&
           month >= 1 && month <= 12 &&
           day >= 1 && day <= days_in_month(year, month) &&
           hour < 24 && minute < 60 && second < 60;
}

/**
 * @brief Convert a Gregorian date/time to Unix seconds since 1970.
 */
static uint64_t datetime_to_unix(uint32_t year, uint32_t month, uint32_t day,
                                 uint32_t hour, uint32_t minute, uint32_t second)
{
    uint64_t days = 0;
    for (uint32_t y = 1970; y < year; ++y) {
        days += is_leap_year(y) ? 366ULL : 365ULL;
    }
    for (uint32_t m = 1; m < month; ++m) {
        days += days_in_month(year, m);
    }
    days += day - 1;
    return days * 86400ULL + (uint64_t)hour * 3600ULL +
           (uint64_t)minute * 60ULL + second;
}

/**
 * @brief Split unix_seconds into year/month/day/hour/minute/second in info.
 */
static void unix_to_datetime(uint64_t unix_seconds, struct reliefos_time_info *info)
{
    uint64_t days = unix_seconds / 86400ULL;
    uint32_t rem = (uint32_t)(unix_seconds % 86400ULL);
    uint32_t year = 1970;
    while (1) {
        uint32_t year_days = is_leap_year(year) ? 366u : 365u;
        if (days < year_days) {
            break;
        }
        days -= year_days;
        ++year;
    }
    uint32_t month = 1;
    while (1) {
        uint32_t month_days = days_in_month(year, month);
        if (days < month_days) {
            break;
        }
        days -= month_days;
        ++month;
    }
    info->year = year;
    info->month = month;
    info->day = (uint32_t)days + 1;
    info->hour = rem / 3600u;
    rem %= 3600u;
    info->minute = rem / 60u;
    info->second = rem % 60u;
}

/**
 * @brief Read the RTC (BCD or binary, 12/24h) into Unix seconds; returns 0, or -1 if invalid.
 */
static int rtc_read_unix_seconds(uint64_t *out)
{
    uint8_t second;
    uint8_t minute;
    uint8_t hour_raw;
    uint8_t day;
    uint8_t month;
    uint8_t year;
    uint8_t century;
    uint8_t status_b;
    for (uint32_t attempt = 0; attempt < 100000; ++attempt) {
        if (!rtc_update_in_progress()) {
            break;
        }
    }
    second = cmos_read(CMOS_SECONDS);
    minute = cmos_read(CMOS_MINUTES);
    hour_raw = cmos_read(CMOS_HOURS);
    day = cmos_read(CMOS_DAY);
    month = cmos_read(CMOS_MONTH);
    year = cmos_read(CMOS_YEAR);
    century = cmos_read(CMOS_CENTURY);
    status_b = cmos_read(CMOS_STATUS_B);

    uint8_t pm = hour_raw & 0x80u;
    uint32_t hour = hour_raw & 0x7fu;
    uint32_t full_year;
    if ((status_b & 0x04u) == 0) {
        second = (uint8_t)bcd_to_binary(second);
        minute = (uint8_t)bcd_to_binary(minute);
        hour = bcd_to_binary((uint8_t)hour);
        day = (uint8_t)bcd_to_binary(day);
        month = (uint8_t)bcd_to_binary(month);
        year = (uint8_t)bcd_to_binary(year);
        century = (uint8_t)bcd_to_binary(century);
    }
    if ((status_b & 0x02u) == 0) {
        if (pm && hour < 12) {
            hour += 12;
        } else if (!pm && hour == 12) {
            hour = 0;
        }
    }
    if (century) {
        full_year = (uint32_t)century * 100u + year;
    } else {
        full_year = year >= 70 ? 1900u + year : 2000u + year;
    }
    if (!rtc_datetime_valid(full_year, month, day, hour, minute, second)) {
        return -1;
    }
    *out = datetime_to_unix(full_year, month, day, hour, minute, second);
    return 0;
}

/**
 * @brief Zero tick counters and seed the wall clock from the RTC if readable.
 */
void time_init(void)
{
    ticks = 0;
    wall_unix_seconds = 0;
    monotonic_seconds = monotonic_ns = 0;
    wall_fraction_ns = 0;
    wall_clock_valid = 0;
    pit_divisor = 0;
    tsc_anchor_seq = 0;
    tsc_anchor = 0;
    uptime_tsc_anchor = 0;
    tsc_per_us = 0;
    tsc_calib_first = 0;
    tsc_calib_previous = 0;
    tsc_calib_min_interval = 0;
    tsc_calib_max_interval = 0;
    tsc_calib_confirm_rate = 0;
    tsc_calib_span = 0;
    tsc_ready = 0;
    tsc_trusted = 0;
    hres_cycle_last = hres_mult = hres_max_cycles = hres_resolution_ns = 0;
    hres_realtime = (struct reliefnt_vdso_timestamp){0, 0};
    hres_monotonic = (struct reliefnt_vdso_timestamp){0, 0};
    monotonic_floor = (struct reliefnt_vdso_timestamp){0, 0};
    monotonic_floor_valid = 0;
    vdso_data = 0;
    if (rtc_read_unix_seconds(&wall_unix_seconds) == 0) {
        struct reliefos_time_info info;
        wall_clock_valid = 1;
        unix_to_datetime(wall_unix_seconds, &info);
        console_info("[reliefnt] rtc wall clock %u-%u-%u %u:%u:%u\n",
                       info.year, info.month, info.day,
                       info.hour, info.minute, info.second);
    } else {
        console_warn("[reliefnt] rtc wall clock unavailable\n");
    }
}

static uint64_t time_rdtsc(void)
{
#if defined(RELIEFNT_TIME_HOST_TEST)
    return time_test_rdtsc();
#else
    uint32_t low;
    uint32_t high;
    __asm__ volatile("lfence; rdtsc; lfence" : "=a"(low), "=d"(high) :: "memory");
    return ((uint64_t)high << 32) | low;
#endif
}

static bool time_tsc_hardware_safe(void)
{
#if defined(RELIEFNT_TIME_HOST_TEST)
    return time_test_tsc_safe();
#else
    uint32_t eax, ebx, ecx, edx;
    uint32_t max_basic, max_extended;
    __asm__ volatile("cpuid" : "=a"(max_basic), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(0), "c"(0));
    if (max_basic < 1) return false;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1), "c"(0));
    if (!(edx & (1u << 4))) return false; /* CPUID.1:EDX.TSC */
    __asm__ volatile("cpuid" : "=a"(max_extended), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(0x80000000u), "c"(0));
    if (max_extended < 0x80000007u) return false;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(0x80000007u), "c"(0));
    if (!(edx & (1u << 8))) return false; /* invariant TSC */
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    if (cr4 & (1ULL << 2)) return false; /* CR4.TSD prevents user RDTSC */
    return true;
#endif
}

static bool time_tsc_calibration_sane(void)
{
    return tsc_ready && tsc_per_us >= TIME_TSC_MIN_CYCLES_PER_US &&
           tsc_per_us <= TIME_TSC_MAX_CYCLES_PER_US;
}

static bool time_tsc_candidate_safe(void)
{
    uint32_t online = 0;
    uint32_t count = smp_cpu_count();
    uint64_t interval_ns = time_discipline_peek_tick_ns();
    /* smp_cpu_count() reports discovered CPUs, including APs not yet released
     * to userspace. Keep the vDSO on the syscall path until the machine is
     * known to be single-CPU; another CPU could otherwise race this check. */
    if (count != 1) return false;
    for (uint32_t cpu = 0; cpu < count; ++cpu) {
        if (smp_cpu_online(cpu)) ++online;
    }
    return online == 1 && time_tsc_calibration_sane() &&
           interval_ns >= TIME_NOMINAL_TICK_NS / 2 &&
           interval_ns <= TIME_NOMINAL_TICK_NS * 2 && time_tsc_hardware_safe();
}

static uint64_t time_cycles_to_ns(uint64_t cycles, uint64_t mult)
{
    __uint128_t result = ((__uint128_t)cycles * mult) >> RELIEFNT_VDSO_SHIFT;
    return result > UINT64_MAX ? UINT64_MAX : (uint64_t)result;
}

static void time_timestamp_add_ns(struct reliefnt_vdso_timestamp *timestamp,
                                  uint64_t nanoseconds)
{
    uint64_t seconds = nanoseconds / 1000000000ULL;
    uint64_t subsecond = nanoseconds % 1000000000ULL;
    uint64_t nsec = timestamp->nsec + subsecond;
    if (nsec >= 1000000000ULL) {
        nsec -= 1000000000ULL;
        ++seconds;
    }
    if (timestamp->sec >= 0 && seconds > (uint64_t)(INT64_MAX - timestamp->sec)) {
        timestamp->sec = INT64_MAX;
        timestamp->nsec = 999999999ULL;
        return;
    }
    timestamp->sec += (int64_t)seconds;
    timestamp->nsec = nsec;
}

static void time_timestamp_add_signed_seconds(struct reliefnt_vdso_timestamp *timestamp,
                                               int32_t seconds)
{
    if (seconds > 0 && timestamp->sec > INT64_MAX - seconds) {
        timestamp->sec = INT64_MAX;
        timestamp->nsec = 999999999ULL;
    } else if (seconds < 0 && timestamp->sec < INT64_MIN - seconds) {
        timestamp->sec = INT64_MIN;
        timestamp->nsec = 0;
    } else {
        timestamp->sec += seconds;
    }
}

static void time_timestamp_to_timespec(const struct reliefnt_vdso_timestamp *base,
                                      uint64_t nanoseconds,
                                      struct linux_timespec *value)
{
    struct reliefnt_vdso_timestamp result = *base;
    time_timestamp_add_ns(&result, nanoseconds);
    value->tv_sec = result.sec;
    value->tv_nsec = (int64_t)result.nsec;
}

static bool time_timestamp_less(const struct reliefnt_vdso_timestamp *left,
                                const struct reliefnt_vdso_timestamp *right)
{
    return left->sec < right->sec ||
           (left->sec == right->sec && left->nsec < right->nsec);
}

static void time_monotonic_floor_advance_locked(struct reliefnt_vdso_timestamp *value)
{
    if (monotonic_floor_valid && time_timestamp_less(value, &monotonic_floor)) {
        *value = monotonic_floor;
    } else {
        monotonic_floor = *value;
        monotonic_floor_valid = 1;
    }
}

static uint64_t time_legacy_fraction_at(uint64_t sample)
{
    if (!tsc_ready || !tsc_per_us || sample < tsc_anchor) return 0;
    uint64_t fraction = ((sample - tsc_anchor) / tsc_per_us) * 1000ULL;
    return fraction > TIME_NOMINAL_TICK_NS ? TIME_NOMINAL_TICK_NS : fraction;
}

static void time_hres_configure_locked(uint64_t anchor, uint64_t interval_ns)
{
    /* With sane calibration and bounded discipline input, interval_ns is far
     * below 2^32 and tsc_per_us * nominal_tick_us is below 2^27. This keeps
     * the Q32 scale in native 64-bit arithmetic on the freestanding kernel.
     * Round the multiplier upward when its rational value is not representable:
     * rounding down and discarding the low Q32 product bits at every tick
     * otherwise loses a nanosecond repeatedly at common TSC rates. */
    uint64_t denominator = tsc_per_us * TIME_NOMINAL_TICK_US;
    if (denominator && interval_ns <= (UINT64_MAX >> RELIEFNT_VDSO_SHIFT)) {
        uint64_t numerator = interval_ns << RELIEFNT_VDSO_SHIFT;
        uint64_t quotient = numerator / denominator;
        uint64_t remainder = numerator % denominator;
        hres_mult = quotient + (remainder != 0);
    } else {
        hres_mult = 0;
    }
    /* Q32 remains a finite fixed-point scale; this prevents the demonstrated
     * per-full-tick negative bias but does not certify absolute UTC accuracy. */
    hres_cycle_last = anchor;
    hres_max_cycles = tsc_per_us * TIME_NOMINAL_TICK_US;
    hres_resolution_ns = (hres_mult >> RELIEFNT_VDSO_SHIFT) +
                         ((hres_mult & 0xffffffffULL) != 0);
    if (!hres_resolution_ns) hres_resolution_ns = 1;
}

static void time_hres_settle_locked(uint64_t sample)
{
    if (!tsc_trusted) return;
    uint64_t elapsed = sample >= hres_cycle_last ? sample - hres_cycle_last : 0;
    uint64_t nanoseconds = time_cycles_to_ns(elapsed, hres_mult);
    time_timestamp_add_ns(&hres_monotonic, nanoseconds);
    if (wall_clock_valid) {
        time_timestamp_add_ns(&hres_realtime, nanoseconds);
    } else {
        hres_realtime = hres_monotonic;
    }
    hres_cycle_last = sample;
}

static void time_hres_from_legacy_locked(uint64_t sample)
{
    uint64_t fraction = time_legacy_fraction_at(sample);
    hres_monotonic.sec = (int64_t)monotonic_seconds;
    hres_monotonic.nsec = monotonic_ns;
    time_timestamp_add_ns(&hres_monotonic, fraction);
    if (wall_clock_valid) {
        hres_realtime.sec = (int64_t)wall_unix_seconds;
        hres_realtime.nsec = wall_fraction_ns;
        time_timestamp_add_ns(&hres_realtime, fraction);
    } else {
        hres_realtime = hres_monotonic;
    }
}

static void time_hres_enable_locked(uint64_t sample)
{
    time_hres_from_legacy_locked(sample);
    time_monotonic_floor_advance_locked(&hres_monotonic);
    tsc_trusted = 1;
    time_hres_configure_locked(sample, time_discipline_peek_tick_ns());
}

static void time_hres_disable_locked(uint64_t sample)
{
    if (!tsc_trusted) return;
    time_hres_settle_locked(sample);
    time_monotonic_floor_advance_locked(&hres_monotonic);
    /* Pivot the old syscall clock forward before returning to its legacy
     * interpolation, so losing a hardware guarantee cannot move monotonic
     * time backwards. Keep the uptime interpolation anchor independent. */
    struct reliefnt_vdso_timestamp coarse_mono = {
        (int64_t)monotonic_seconds, monotonic_ns
    };
    if (time_timestamp_less(&coarse_mono, &hres_monotonic)) {
        monotonic_seconds = (uint64_t)hres_monotonic.sec;
        monotonic_ns = hres_monotonic.nsec;
    }
    if (wall_clock_valid) {
        struct reliefnt_vdso_timestamp coarse_real = {
            (int64_t)wall_unix_seconds, wall_fraction_ns
        };
        if (time_timestamp_less(&coarse_real, &hres_realtime)) {
            wall_unix_seconds = (uint64_t)hres_realtime.sec;
            wall_fraction_ns = hres_realtime.nsec;
        }
    }
    tsc_anchor = sample;
    tsc_trusted = 0;
}

static void time_hres_refresh_safety_locked(uint64_t sample)
{
    bool safe = time_tsc_candidate_safe();
    if (tsc_trusted && !safe) {
        time_hres_disable_locked(sample);
    } else if (!tsc_trusted && safe) {
        time_hres_enable_locked(sample);
    }
}

static void time_vdso_publish_locked(void)
{
    struct reliefnt_vdso_data *data = vdso_data;
    if (!data) return;

    uint32_t seq = __atomic_load_n(&data->seq, __ATOMIC_RELAXED);
    if (seq & 1u) ++seq;
    __atomic_store_n(&data->seq, seq + 1u, __ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    uint32_t clock_mask = (1u << LINUX_CLOCK_MONOTONIC_COARSE);
    uint32_t hres_mask = 0;
    uint32_t mode = RELIEFNT_VDSO_MODE_NONE;
    if (wall_clock_valid) clock_mask |= 1u << LINUX_CLOCK_REALTIME_COARSE;
    if (tsc_trusted) {
        clock_mask |= (1u << LINUX_CLOCK_MONOTONIC);
        if (wall_clock_valid) clock_mask |= 1u << LINUX_CLOCK_REALTIME;
        hres_mask = (1u << LINUX_CLOCK_MONOTONIC);
        if (wall_clock_valid) hres_mask |= 1u << LINUX_CLOCK_REALTIME;
        mode = RELIEFNT_VDSO_MODE_TSC;
    }

    __atomic_store_n(&data->clock_mode, mode, __ATOMIC_RELAXED);
    __atomic_store_n(&data->clock_mask, clock_mask, __ATOMIC_RELAXED);
    __atomic_store_n(&data->hres_mask, hres_mask, __ATOMIC_RELAXED);
    __atomic_store_n(&data->cycle_last, tsc_trusted ? hres_cycle_last : 0,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&data->mult, tsc_trusted ? hres_mult : 0, __ATOMIC_RELAXED);
    __atomic_store_n(&data->max_cycles, tsc_trusted ? hres_max_cycles : 0,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&data->resolution_ns, tsc_trusted ? hres_resolution_ns : 0,
                     __ATOMIC_RELAXED);
    for (uint32_t i = 0; i < RELIEFNT_VDSO_CLOCKS; ++i) {
        struct reliefnt_vdso_timestamp value = {0, 0};
        if (tsc_trusted && i == LINUX_CLOCK_REALTIME) value = hres_realtime;
        else if (tsc_trusted && i == LINUX_CLOCK_MONOTONIC) value = hres_monotonic;
        else if (i == LINUX_CLOCK_REALTIME_COARSE && wall_clock_valid) {
            value.sec = (int64_t)wall_unix_seconds;
            value.nsec = wall_fraction_ns;
        } else if (i == LINUX_CLOCK_MONOTONIC_COARSE) {
            value.sec = (int64_t)monotonic_seconds;
            value.nsec = monotonic_ns;
        }
        __atomic_store_n(&data->basetime[i].sec, value.sec, __ATOMIC_RELAXED);
        __atomic_store_n(&data->basetime[i].nsec, value.nsec, __ATOMIC_RELAXED);
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&data->seq, seq + 2u, __ATOMIC_RELEASE);
}

/* Start a new candidate window at the most recent sample. A disturbed PIT
 * callback can bunch one interval even while the average of the whole window
 * looks plausible, so calibration uses individually checked intervals and
 * requires a stable seven-interval window before enabling TSC timekeeping. */
static void time_tsc_calibration_restart(uint64_t sample)
{
    tsc_calib_first = sample;
    tsc_calib_previous = sample;
    tsc_calib_min_interval = UINT64_MAX;
    tsc_calib_max_interval = 0;
    tsc_calib_span = 1;
}

static void time_tsc_calibration_reject(uint64_t sample)
{
    tsc_calib_confirm_rate = 0;
    time_tsc_calibration_restart(sample);
}

static void time_tsc_calibration_sample(uint64_t sample)
{
    const uint64_t minimum = TIME_TSC_MIN_CYCLES_PER_US * TIME_NOMINAL_TICK_US;
    const uint64_t maximum = TIME_TSC_MAX_CYCLES_PER_US * TIME_NOMINAL_TICK_US;

    if (!tsc_calib_span) {
        time_tsc_calibration_restart(sample);
        return;
    }
    if (tsc_calib_span >= 8u) return;

    if (sample <= tsc_calib_previous) {
        time_tsc_calibration_reject(sample);
        return;
    }
    uint64_t interval = sample - tsc_calib_previous;
    if (interval < minimum || interval > maximum) {
        time_tsc_calibration_reject(sample);
        return;
    }

    tsc_calib_previous = sample;
    if (interval < tsc_calib_min_interval) tsc_calib_min_interval = interval;
    if (interval > tsc_calib_max_interval) tsc_calib_max_interval = interval;
    ++tsc_calib_span;
    if (tsc_calib_span == 8u) {
        uint64_t interval_spread = tsc_calib_max_interval - tsc_calib_min_interval;
        uint64_t per_us = (sample - tsc_calib_first) /
                          (7ULL * TIME_NOMINAL_TICK_US);
        if (interval_spread > tsc_calib_min_interval / 100u ||
            per_us < TIME_TSC_MIN_CYCLES_PER_US ||
            per_us > TIME_TSC_MAX_CYCLES_PER_US) {
            time_tsc_calibration_reject(sample);
            return;
        }

        if (!tsc_calib_confirm_rate) {
            tsc_calib_confirm_rate = per_us;
            time_tsc_calibration_restart(sample);
            return;
        }

        uint64_t lower_rate = per_us < tsc_calib_confirm_rate ?
                              per_us : tsc_calib_confirm_rate;
        uint64_t rate_difference = per_us < tsc_calib_confirm_rate ?
                                   tsc_calib_confirm_rate - per_us :
                                   per_us - tsc_calib_confirm_rate;
        if (rate_difference <= lower_rate / 100u) {
            tsc_per_us = per_us;
            tsc_ready = 1;
        } else {
            time_tsc_calibration_reject(sample);
        }
    }
}

/**
 * @brief Advance clock and service devices with no clock lock held.
 * @return None. IRQ context; audio consumes at most 32 completions, no waiting.
 */
void time_on_tick(void)
{
    uint64_t flags;
    kernel_spin_lock_irqsave(&clock_lock, &flags);
    uint64_t anchor = time_rdtsc();
    if (tsc_trusted) time_hres_settle_locked(anchor);
    /* Publish the tick and its TSC anchor as one sequence so lock-free
     * readers can retry instead of pairing one tick's anchor with another's
     * count. Anchor precedes the increment so a reader that still sees the
     * old count cannot observe a half-published boundary. */
    ++tsc_anchor_seq;
    tsc_anchor = anchor;
    uptime_tsc_anchor = anchor;
    ++ticks;
    ++tsc_anchor_seq;
    int32_t leap_correction = 0;
    if (!tsc_ready) {
        time_tsc_calibration_sample(tsc_anchor);
    }
    uint64_t elapsed = time_discipline_tick_ns();
    monotonic_ns += elapsed;
    if (monotonic_ns >= 1000000000) { monotonic_ns -= 1000000000; ++monotonic_seconds; }
    if (wall_clock_valid) {
        wall_fraction_ns += elapsed;
        if (wall_fraction_ns >= 1000000000) {
            wall_fraction_ns -= 1000000000;
            ++wall_unix_seconds;
            leap_correction = time_discipline_second(wall_unix_seconds);
            wall_unix_seconds += leap_correction;
        }
    }
    if (tsc_trusted && leap_correction) {
        time_timestamp_add_signed_seconds(&hres_realtime, leap_correction);
    }
    time_hres_refresh_safety_locked(anchor);
    if (tsc_trusted) {
        time_hres_configure_locked(anchor, time_discipline_peek_tick_ns());
    }
    time_vdso_publish_locked();
    kernel_spin_unlock_irqrestore(&clock_lock, flags);
    sound_service_tick();
    usb_poll();
    sched_on_tick();
    /* Fallback scanout refresh cadence for hosts without framebuffer traces.
     * This schedules 50 Hz updates at a 100 Hz PIT tick; it is not a hardware
     * presentation-rate measurement. Trace-capable hosts skip the work in the
     * framebuffer backend. */
    if ((ticks % (RELIEFNT_TICK_HZ / 50ULL)) == 0) {
        console_display_tick();
    }
}

/**
 * @brief Read the aligned monotonic boot tick counter.
 * @return Number of delivered timer ticks since initialization; no wall-clock adjustment.
 * @par Context
 * Boot/task/IRQ snapshot on x86_64, without allocation, wait, reference transfer or clock lock.
 */
uint64_t time_ticks(void)
{
    return ticks;
}

/** @brief Nanoseconds elapsed since the last tick anchor, sub-tick TSC-resolved.
 * @return Interpolated fraction in [0, one nominal tick]; 0 when TSC not ready.
 * Caller holds the clock lock, so the anchor/rate cannot change mid-read.
 */
static uint64_t time_subtick_fraction_ns(void)
{
    uint64_t sample = time_rdtsc();
    if (tsc_trusted) {
        uint64_t elapsed = sample >= hres_cycle_last ? sample - hres_cycle_last : 0;
        return time_cycles_to_ns(elapsed, hres_mult);
    }
    return time_legacy_fraction_at(sample);
}

/** @brief Read a consistent clock snapshot with the timekeeper lock held. */
static int time_clock_get_locked(int32_t clock, struct linux_timespec *value)
{
    uint64_t fraction;
    switch (clock) {
    case LINUX_CLOCK_REALTIME:
    case 11: /* CLOCK_TAI */
        fraction = time_subtick_fraction_ns();
        if (tsc_trusted) {
            time_timestamp_to_timespec(&hres_realtime, fraction, value);
        } else {
            value->tv_sec = wall_clock_valid ? (int64_t)wall_unix_seconds : (int64_t)monotonic_seconds;
            value->tv_nsec = (wall_clock_valid ? wall_fraction_ns : monotonic_ns) + fraction;
        }
        if (!tsc_trusted && value->tv_nsec >= 1000000000LL) {
            value->tv_nsec -= 1000000000LL;
            ++value->tv_sec;
        }
        if (clock == 11) {
            struct reliefnt_vdso_timestamp tai = {value->tv_sec,
                                                   (uint64_t)value->tv_nsec};
            time_timestamp_add_signed_seconds(&tai, time_discipline_tai());
            value->tv_sec = tai.sec;
            value->tv_nsec = (int64_t)tai.nsec;
        }
        return 0;
    case LINUX_CLOCK_REALTIME_COARSE:
        value->tv_sec = wall_clock_valid ? (int64_t)wall_unix_seconds : (int64_t)monotonic_seconds;
        value->tv_nsec = wall_clock_valid ? wall_fraction_ns : monotonic_ns;
        return 0;
    case LINUX_CLOCK_MONOTONIC_RAW:
        value->tv_sec = ticks / RELIEFNT_TICK_HZ;
        value->tv_nsec = ticks % RELIEFNT_TICK_HZ * (1000000000ULL / RELIEFNT_TICK_HZ);
        return 0;
    case LINUX_CLOCK_MONOTONIC:
    case LINUX_CLOCK_BOOTTIME:
        fraction = time_subtick_fraction_ns();
        if (tsc_trusted) {
            time_timestamp_to_timespec(&hres_monotonic, fraction, value);
        } else {
            value->tv_sec = monotonic_seconds;
            value->tv_nsec = monotonic_ns + fraction;
        }
        if (!tsc_trusted && value->tv_nsec >= 1000000000LL) {
            value->tv_nsec -= 1000000000LL;
            ++value->tv_sec;
        }
        {
            struct reliefnt_vdso_timestamp timestamp = {
                value->tv_sec, (uint64_t)value->tv_nsec
            };
            time_monotonic_floor_advance_locked(&timestamp);
            value->tv_sec = timestamp.sec;
            value->tv_nsec = (int64_t)timestamp.nsec;
        }
        return 0;
    case LINUX_CLOCK_MONOTONIC_COARSE:
        value->tv_sec = monotonic_seconds;
        value->tv_nsec = monotonic_ns;
        return 0;
    default: return -LINUX_EINVAL;
    }
}

/** @brief Read a supported wall/monotonic clock atomically across CPUs and IRQs.
 * @param clock Linux clock ID. @param value Writable caller-owned timespec output.
 * @return Zero or EINVAL for an unsupported clock; no pointer ownership transfer.
 * IRQ/task safe short clock lock; no allocation, scheduling or hardware wait.
 */
int time_clock_get(int32_t clock, struct linux_timespec *value)
{
    uint64_t flags;
    kernel_spin_lock_irqsave(&clock_lock, &flags);
    int result = time_clock_get_locked(clock, value);
    kernel_spin_unlock_irqrestore(&clock_lock, flags);
    return result;
}

/**
 * @brief Validate a supported Linux clock and copy its current resolution.
 * @param clock Linux clock selector.
 * @param value Optional writable kernel result; NULL validates without copying.
 * @return Zero or EINVAL for an unsupported clock, leaving output unchanged on failure.
 * Task/IRQ-safe query under the clock lock, with no user access, allocation or ownership transfer.
 */
int time_clock_getres(int32_t clock, struct linux_timespec *value)
{
    uint64_t flags;
    kernel_spin_lock_irqsave(&clock_lock, &flags);
    struct linux_timespec now;
    int result = time_clock_get_locked(clock, &now);
    if (result == 0) {
        uint64_t resolution = TIME_NOMINAL_TICK_NS;
        if (clock == LINUX_CLOCK_REALTIME_COARSE ||
            clock == LINUX_CLOCK_MONOTONIC_COARSE ||
            clock == LINUX_CLOCK_MONOTONIC_RAW) {
            resolution = TIME_NOMINAL_TICK_NS;
        } else if (tsc_trusted) {
            resolution = hres_resolution_ns;
        } else if (tsc_ready && tsc_per_us) {
            /* The legacy syscall interpolation divides whole microseconds. */
            resolution = 1000;
        }
        if (value) {
            value->tv_sec = 0;
            value->tv_nsec = (int64_t)resolution;
        }
    }
    kernel_spin_unlock_irqrestore(&clock_lock, flags);
    return result;
}

void time_vdso_bind(struct reliefnt_vdso_data *data)
{
    uint64_t flags;
    kernel_spin_lock_irqsave(&clock_lock, &flags);
    vdso_data = data;
    if (data) {
        uint64_t sample = time_rdtsc();
        time_hres_refresh_safety_locked(sample);
        time_vdso_publish_locked();
    }
    kernel_spin_unlock_irqrestore(&clock_lock, flags);
}

/** @brief Apply real oscillator/phase discipline and return its current state. */
int time_adjust(struct linux_timex *value, bool privileged)
{
    uint64_t flags;
    kernel_spin_lock_irqsave(&clock_lock, &flags);
    uint64_t sample = time_rdtsc();
    time_hres_refresh_safety_locked(sample);
    struct linux_timespec now;
    time_clock_get_locked(LINUX_CLOCK_REALTIME, &now);
    int result = time_discipline_adjust(value, &now, privileged);
    if (result >= 0) {
        sample = time_rdtsc();
        if (tsc_trusted) {
            time_hres_settle_locked(sample);
            time_hres_configure_locked(sample, time_discipline_peek_tick_ns());
        }
        time_hres_refresh_safety_locked(sample);
        time_vdso_publish_locked();
    }
    kernel_spin_unlock_irqrestore(&clock_lock, flags);
    return result;
}

/**
 * @brief Convert the tick count to milliseconds.
 */
uint64_t time_uptime_ms(void)
{
    return (ticks * 1000ULL) / RELIEFNT_TICK_HZ;
}

void time_set_pit_divisor(uint16_t divisor)
{
    pit_divisor = divisor;
}

static uint16_t pit_current_count(void)
{
    uint8_t low;
    uint8_t high;

    x86_64_outb(PIT_LATCH_CHANNEL0, PIT_COMMAND);
    low = x86_64_inb(PIT_CHANNEL0);
    high = x86_64_inb(PIT_CHANNEL0);
    return (uint16_t)((uint16_t)low | ((uint16_t)high << 8));
}

/**
 * @brief Read boot uptime using bounded TSC interpolation with PIT/tick fallback.
 * @return Best-effort elapsed microseconds from boot, with no wall-clock adjustment.
 * Task/IRQ context after time setup; no allocation or retained pointer. Retries preserve paired tick anchors; failed interpolation falls back to the existing lower-resolution clock path.
 */
uint64_t time_uptime_us(void)
{
    uint64_t tick_count;
    uint64_t elapsed_counts;
    uint16_t divisor = pit_divisor;

    /* Steady-state path: interpolate between tick anchors with the TSC rate
     * so syscall-bound readers avoid the PIT port I/O that traps under KVM.
     * The sequence check keeps one tick's anchor paired with one tick's
     * count; anything that slips through falls back to the PIT or, failing
     * that, tick-granularity uptime. */
    if (tsc_ready) {
        for (uint32_t attempt = 0; attempt < 3U; ++attempt) {
            uint32_t seq_before = tsc_anchor_seq;
            if (seq_before & 1U) {
                continue;
            }
            uint64_t before = ticks;
            uint64_t anchor = uptime_tsc_anchor;
            uint64_t sample = time_rdtsc();
            uint32_t seq_after = tsc_anchor_seq;
            if (seq_before != seq_after) {
                continue;
            }
            uint64_t rate = tsc_per_us;
            if (!rate) {
                break;
            }
            uint64_t fraction_us = sample >= anchor ? (sample - anchor) / rate : 0ULL;
            uint64_t tick_us = 1000000ULL / RELIEFNT_TICK_HZ;
            if (fraction_us > tick_us) {
                fraction_us = tick_us;
            }
            return (before * 1000000ULL) / RELIEFNT_TICK_HZ + fraction_us;
        }
    }

    if (!divisor) {
        return (ticks * 1000000ULL) / RELIEFNT_TICK_HZ;
    }

    /* A tick may interrupt the PIT latch sequence.  Retry if that happens so
     * the counter fraction and its whole-tick base always come from one tick. */
    for (uint32_t attempt = 0; attempt < 3U; ++attempt) {
        uint64_t before = ticks;
        uint16_t counter = pit_current_count();
        uint64_t after = ticks;
        if (before != after) {
            continue;
        }
        tick_count = before;
        elapsed_counts = counter <= divisor ? (uint64_t)(divisor - counter) : 0ULL;
        return (tick_count * 1000000ULL) / RELIEFNT_TICK_HZ +
               (elapsed_counts * 1000000ULL) / ((uint64_t)divisor * RELIEFNT_TICK_HZ);
    }
    return (ticks * 1000000ULL) / RELIEFNT_TICK_HZ;
}

/**
 * @brief Copy a consistent wall-clock calendar and uptime snapshot.
 * @param info Required writable kernel output, updated only when the clock is valid.
 * @return Zero after a valid snapshot, or internal -1 for NULL output or an invalid wall clock.
 * @par Context
 * Acquires the IRQ-safe clock lock; no user-pointer access, allocation or ownership transfer. Timezone conversion is not performed.
 */
int time_wall_clock(struct reliefos_time_info *info)
{
    if (!info || !wall_clock_valid) {
        return -1;
    }
    uint64_t flags;
    kernel_spin_lock_irqsave(&clock_lock, &flags);
    info->unix_seconds = wall_unix_seconds;
    info->uptime_ms = time_uptime_ms();
    info->valid = 1;
    info->reserved = 0;
    unix_to_datetime(wall_unix_seconds, info);
    kernel_spin_unlock_irqrestore(&clock_lock, flags);
    return 0;
}

/**
 * @brief Set the wall clock to unix_seconds if it maps to a valid date; returns 0 or -1.
 */
int time_set_wall_clock(uint64_t unix_seconds)
{
    return time_set_wall_clock_ns(unix_seconds, 0);
}

/** @brief Atomically step realtime and reset phase discipline without moving monotonic time. */
int time_set_wall_clock_ns(uint64_t unix_seconds, uint32_t nanoseconds)
{
    struct reliefos_time_info info;
    if (nanoseconds >= 1000000000u || unix_seconds > 253402300799ULL) {
        return -1;
    }
    unix_to_datetime(unix_seconds, &info);
    if (!rtc_datetime_valid(info.year, info.month, info.day, info.hour,
                            info.minute, info.second)) {
        return -1;
    }
    uint64_t flags;
    kernel_spin_lock_irqsave(&clock_lock, &flags);
    uint64_t sample = time_rdtsc();
    time_hres_refresh_safety_locked(sample);
    if (tsc_trusted) time_hres_settle_locked(sample);
    wall_unix_seconds = unix_seconds;
    wall_fraction_ns = nanoseconds;
    wall_clock_valid = 1;
    hres_realtime.sec = (int64_t)unix_seconds;
    hres_realtime.nsec = nanoseconds;
    time_discipline_clear();
    if (tsc_trusted) time_hres_configure_locked(sample, time_discipline_peek_tick_ns());
    time_hres_refresh_safety_locked(sample);
    time_vdso_publish_locked();
    kernel_spin_unlock_irqrestore(&clock_lock, flags);
    console_info("[reliefnt] wall clock set %u-%u-%u %u:%u:%u\n",
                   info.year, info.month, info.day, info.hour,
                   info.minute, info.second);
    return 0;
}

/**
 * @brief Busy-halt until at least ms milliseconds have elapsed.
 */
void time_sleep_ms(uint64_t ms)
{
    uint64_t delta = (ms * RELIEFNT_TICK_HZ + 999ULL) / 1000ULL;
    if (delta == 0) {
        delta = 1;
    }
    uint64_t end = ticks + delta;
    while (ticks < end) {
        __asm__ volatile("sti; hlt; cli");
    }
}

/* Native filesystem modules share this core-owned interface; CRC/release
 * checks bind it to the exact kernel, without a stable cross-release API. */
EXPORT_SYMBOL(time_wall_clock);

EXPORT_SYMBOL(time_ticks);

EXPORT_SYMBOL(time_clock_get);

EXPORT_SYMBOL(time_clock_getres);
EXPORT_SYMBOL(time_uptime_us);
