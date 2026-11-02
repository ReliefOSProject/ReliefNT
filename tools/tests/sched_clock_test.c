/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <reliefnt/power.h>
#define RELIEFNT_TEST_SCHED_CLOCK
#include "modules/scheduler.h"

static uint64_t legacy = 1000000;
static uint32_t reference, reference_mask = 0x00ffffff;
static bool reference_enabled = true;
uint64_t time_uptime_us(void) { return legacy; }
bool power_acpi_pm_timer_read(uint32_t *counter, uint32_t *mask)
{
    *counter = reference & reference_mask; *mask = reference_mask;
    return reference_enabled;
}
void kernel_spin_init(struct kernel_spinlock *lock) { lock->state = 0; }
bool smp_cpu_online(uint32_t cpu) { return cpu < 4; }
void console_log(enum console_log_level level, const char *format, ...)
{ (void)level; (void)format; }

int main(void)
{
    sched_init();
    uint64_t first = fair_now();
    struct task running[2] = {0};
    for (unsigned cpu = 0; cpu < 2; ++cpu) {
        running[cpu].fair = (struct eevdf_entity){.weight = 1024,
            .initialized = true, .deadline = 10000};
        fair_running[cpu] = &running[cpu]; fair_started[cpu] = first;
    }
    reference += ACPI_PM_TIMER_HZ / 20u;
    uint64_t second = fair_now();
    assert(second >= first + 49999 && second <= first + 50000);
    for (unsigned cpu = 0; cpu < 2; ++cpu) {
        fair_account_cpu(cpu, second);
        assert(running[cpu].fair.runtime == second - first);
        assert(running[cpu].fair.vruntime == (int64_t)(second - first));
    }
    puts("PASS scheduler clock progresses while the PIT/legacy clock is capped");

    const uint32_t masks[] = {0x00ffffffu, UINT32_MAX};
    for (unsigned i = 0; i < 2; ++i) {
        sched_init(); reference_mask = masks[i]; reference = masks[i] - 10u;
        first = fair_now(); reference += ACPI_PM_TIMER_HZ / 2u;
        second = fair_now();
        assert(second >= first + 499999 && second <= first + 500000);
    }
    sched_init(); reference = 0;
    first = fair_now(); second = first;
    for (unsigned i = 0; i < 3579; ++i) {
        ++reference;
        uint64_t next = fair_now(); assert(next >= second); second = next;
    }
    assert(second - first == 999); /* Preserve sub-microsecond remainders. */
    reference_enabled = false; legacy += 5000;
    second = fair_now(); assert(second >= legacy);
    legacy -= 20000;
    assert(fair_now() == second);
    reference_enabled = true;
    assert(fair_now() == second); /* Reappearance reanchors without a jump. */
    reference += ACPI_PM_TIMER_HZ;
    assert(fair_now() == second + 1000000);
    second = fair_now(); reference_mask = 0x00ffffff;
    assert(fair_now() == second); /* A width change reanchors as well. */
    reference_mask = 0x1234;
    assert(fair_now() == second); /* Invalid firmware data uses the floor. */
    reference_enabled = false; legacy = second + 1000;
    assert(fair_now() == legacy);
    puts("PASS scheduler clock: 24/32-bit wrap, fractional carry, source loss/width changes and legacy floor");
    return 0;
}
