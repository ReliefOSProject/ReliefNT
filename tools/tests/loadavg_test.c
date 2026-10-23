#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <reliefnt/loadavg.h>
int main(void)
{
    uint64_t loads[3] = {0};
    for (int i = 0; i < 12; ++i) sched_load_update(loads, 1);
    /* One minute at one runnable task: 1-exp(-1) ~= 0.632. */
    assert(loads[0] > 41000 && loads[0] < 42000);
    assert(loads[0] > loads[1] && loads[1] > loads[2]);
    uint64_t before = loads[0];
    for (int i = 0; i < 12; ++i) sched_load_update(loads, 0);
    assert(loads[0] < before / 2);
    for (int i = 0; i < 2000; ++i) sched_load_update(loads, 4);
    assert(loads[0] > 4*65536-100 && loads[0] <= 4*65536);
    puts("scheduler load-average regression passed");
    return 0;
}
