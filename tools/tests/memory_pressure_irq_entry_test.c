/* Actual IRQ dispatcher and PIC/APIC acknowledgment with modeled CPU hooks. */
#define main old_keyboard_fixture_main
#include "keyboard_irq_ready_test.c"
#undef main

int main(void)
{
    struct trap_frame frame = {.vector=0x20,.cs=3};
    keyboard_led.applied = 0;
    assert(!irq_dispatch(&frame));
    assert(timer_entries == 1 && timer_pic_eois == 1 && eois == 1 && timer_apic_eois == 1);
    frame.cs = 0; assert(!irq_dispatch(&frame));
    assert(timer_entries == 1 && eois == 2);
    fixture_cpu = 1; frame.vector = 0x40; frame.cs = 3;
    assert(!irq_dispatch(&frame));
    assert(timer_entries == 2 && timer_apic_eois == 3 && apic_eois == 3);
    frame.cs = 0; assert(!irq_dispatch(&frame));
    assert(timer_entries == 2 && apic_eois == 4);
    puts("PASS only acknowledged Ring 3 PIT/AP timer enters timer scheduler");
    return 0;
}
