#ifndef RELIEFOS_UAPI_LINUX_TIME_H
#define RELIEFOS_UAPI_LINUX_TIME_H
#include <linux/types.h>

/* Native x86-64 time64 shapes. Respect both glibc and musl guards, including
 * when this UAPI is included before libc <time.h>. Freestanding kernel builds
 * must not import host libc headers. */
#if !defined(__time_t_defined) && !defined(__DEFINED_time_t)
typedef __s64 time_t;
#define __time_t_defined 1
#define __DEFINED_time_t 1
#endif

#if !defined(_STRUCT_TIMESPEC) && !defined(__DEFINED_struct_timespec)
#define _STRUCT_TIMESPEC 1
#define __DEFINED_struct_timespec 1
struct timespec {
    __s64 tv_sec;
    __s64 tv_nsec;
};
#endif

struct __kernel_timespec {
    __s64 tv_sec;
    __s64 tv_nsec;
};

#define LINUX_CLOCK_REALTIME 0
#define LINUX_CLOCK_MONOTONIC 1
#define LINUX_CLOCK_PROCESS_CPUTIME_ID 2
#define LINUX_CLOCK_THREAD_CPUTIME_ID 3
#define LINUX_CLOCK_MONOTONIC_RAW 4
#define LINUX_CLOCK_REALTIME_COARSE 5
#define LINUX_CLOCK_MONOTONIC_COARSE 6
#define LINUX_CLOCK_BOOTTIME 7
#define LINUX_TIMER_ABSTIME 1
#define LINUX_ITIMER_REAL 0
#define LINUX_ITIMER_VIRTUAL 1
#define LINUX_ITIMER_PROF 2

struct linux_timespec { int64_t tv_sec, tv_nsec; };
struct linux_timeval { int64_t tv_sec, tv_usec; };
struct linux_itimerval { struct linux_timeval it_interval, it_value; };
struct linux_itimerspec { struct linux_timespec it_interval, it_value; };
#endif
