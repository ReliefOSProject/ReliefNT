/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { WORKERS = 8, ITERATIONS = 1800 };

/** @brief Check changing SSE patterns across actual blocking Linux syscalls.
 * @param id Distinct worker identity embedded in every pattern.
 * @return Zero if preserved, one for corruption, or two for a syscall error.
 */
static int worker(unsigned id)
{
    for (unsigned n = 0; n < ITERATIONS; ++n) {
        const uint64_t expected[2] = {
            0x1234567800000000ULL | ((uint64_t)id << 20) | n,
            0xfedcba9800000000ULL | ((uint64_t)id << 20) | n,
        };
        uint64_t actual[6];
        struct timespec delay = {0, 1000000};
        long result = SYS_nanosleep;

        /* No C call intervenes: caller-saved XMM registers must survive the
         * syscall itself, including its in-kernel sleep and CPU migration. */
        __asm__ volatile(
            "movdqu %5, %%xmm0\n\t"
            "movdqu %5, %%xmm7\n\t"
            "movdqu %5, %%xmm15\n\t"
            "syscall\n\t"
            "movdqu %%xmm0, %0\n\t"
            "movdqu %%xmm7, %1\n\t"
            "movdqu %%xmm15, %2"
            : "=m"(*(uint64_t (*)[2])&actual[0]),
              "=m"(*(uint64_t (*)[2])&actual[2]),
              "=m"(*(uint64_t (*)[2])&actual[4]), "+a"(result)
            : "D"(&delay), "m"(expected), "S"(0L)
            : "rcx", "r11", "memory", "xmm0", "xmm7", "xmm15");
        for (unsigned reg = 0; reg < 3; ++reg) {
            if (actual[reg * 2] != expected[0] || actual[reg * 2 + 1] != expected[1]) {
                dprintf(STDOUT_FILENO,
                        "FPU_FAIL id=%u iteration=%u reg=%u expected=%016llx:%016llx "
                        "actual=%016llx:%016llx sleep=%ld\n", id, n, reg,
                        (unsigned long long)expected[0], (unsigned long long)expected[1],
                        (unsigned long long)actual[reg * 2],
                        (unsigned long long)actual[reg * 2 + 1], result);
                return 1;
            }
        }
        if (result) {
            dprintf(STDOUT_FILENO, "FPU_SLEEP_FAIL id=%u result=%ld\n", id, result);
            return 2;
        }
    }
    dprintf(STDOUT_FILENO, "FPU_PASS id=%u iterations=%u\n", id, ITERATIONS);
    return 0;
}

/** @brief Run as an ordinary process, or as a self-shutting-down ReliefNT init.
 * @return Nonzero on any worker failure; guest PID 1 powers off after reporting.
 */
int main(void)
{
    int guest_init = getpid() == 1;
    if (guest_init) {
        struct utsname identity;
        if (uname(&identity) || strcmp(identity.sysname, "ReliefNT")) return 77;
        int serial = open("/dev/ttyS0", O_WRONLY);
        if (serial < 0 || dup2(serial, STDOUT_FILENO) < 0 ||
            dup2(serial, STDERR_FILENO) < 0) {
            for (;;) pause();
        }
        if (serial > STDERR_FILENO) close(serial);
    }
    dprintf(STDOUT_FILENO, "FPU_START pid=%ld workers=%u\n", (long)getpid(), WORKERS);
    unsigned children = 0, failed = 0;
    for (unsigned id = 0; id < WORKERS; ++id) {
        pid_t child = fork();
        if (!child) _exit(worker(id));
        if (child < 0) { ++failed; break; }
        ++children;
    }
    for (unsigned n = 0; n < children; ++n) {
        int status;
        pid_t child;
        do child = wait(&status); while (child < 0 && errno == EINTR);
        if (child < 0 || !WIFEXITED(status) || WEXITSTATUS(status)) ++failed;
    }
    dprintf(STDOUT_FILENO, "FPU_DONE workers=%u failed=%u\n", children, failed);
    if (guest_init) {
        syscall(SYS_reboot, 0xfee1dead, 672274793, 0x4321fedc, 0);
        for (;;) pause();
    }
    return failed != 0;
}
