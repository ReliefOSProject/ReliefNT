/* Regression: a controller reply consumed with IRQs masked leaves a stale
 * IRQ1 pending; reading an empty data port must not publish a phantom key. */
#include <assert.h>
#include <stdio.h>
#include "../../arch/x86_64/interrupt/irq.c"

static uint8_t controller_status, controller_byte;
static unsigned data_reads, input_events, eois;
static unsigned apic_eois, timer_entries, timer_pic_eois, timer_apic_eois;
static unsigned frame_entries, local_ticks, global_ticks;
static uint32_t fixture_cpu;
static uint8_t last_key, last_pressed;

uint8_t x86_64_inb(uint16_t port)
{
    if (port == 0x64) return controller_status;
    assert(port == 0x60);
    ++data_reads;
    return controller_byte;
}
void x86_64_outb(uint8_t value, uint16_t port)
{
    assert(value == 0x20 && port == 0x20);
    ++eois;
}
void input_handle_scancode(uint8_t key, uint8_t pressed)
{
    ++input_events; last_key = key; last_pressed = pressed;
}
void pci_irq_dispatch(uint32_t vector) { (void)vector; }
void apic_eoi(void) { ++apic_eois; }
void time_on_tick(void) { ++global_ticks; }
uint64_t time_uptime_us(void) { return 0; }
uint8_t input_caps_lock_active(void) { return 0; }
struct task *userland_schedule_from_frame(struct trap_frame *frame)
{ (void)frame; ++frame_entries; return NULL; }
struct task *userland_schedule_from_timer(struct trap_frame *frame)
{ ++timer_entries; timer_pic_eois = eois; timer_apic_eois = apic_eois;
  return userland_schedule_from_frame(frame); }
void smp_release_aps(void) {}
void driver_manager_mouse_poll(void) {}
uint32_t smp_current_cpu(void) { return fixture_cpu; }
void sched_on_cpu_tick(void) { ++local_ticks; }
void smp_membarrier_poll(void) {}
/* The retained-stack engine is covered separately; this boundary verifies
 * only acknowledged kernel timer/reschedule returns reach its hook. */
bool kthread_preempt(struct trap_frame *frame)
{
    assert(frame && !(frame->cs&3));
    assert(frame->vector==0x20 || frame->vector==0x40 || frame->vector==0x42);
    if(frame->vector==0x20)assert(eois);else assert(apic_eois);
    return false;
}

int main(void)
{
    struct trap_frame frame = {.vector = 0x21, .cs = 0x8};
    controller_status = 0x1c; controller_byte = 0x47;
    assert(irq_dispatch(&frame) == NULL);
    assert(data_reads == 0 && input_events == 0 && eois == 1);
    controller_status = 0x21; controller_byte = 0x08;
    assert(irq_dispatch(&frame) == NULL);
    assert(data_reads == 0 && input_events == 0 && eois == 2);
    controller_status = 0x01; controller_byte = 0x1e;
    assert(irq_dispatch(&frame) == NULL);
    assert(data_reads == 1 && input_events == 1 && last_key == 30 && last_pressed == 1);
    controller_byte = 0x9e;
    assert(irq_dispatch(&frame) == NULL);
    assert(data_reads == 2 && input_events == 2 && last_key == 30 && last_pressed == 0);
    controller_byte = 0xfa;
    assert(irq_dispatch(&frame) == NULL);
    assert(data_reads == 3 && input_events == 2 && eois == 5);
    /* A reschedule IPI acknowledges only its LAPIC and enters the normal
     * saved-frame scheduler only when it interrupted user mode. */
    unsigned saved_pic = eois, saved_apic = apic_eois;
    unsigned saved_frames = frame_entries, saved_timers = timer_entries;
    frame.vector = 0x42;
    assert(irq_dispatch(&frame) == NULL);
    assert(eois == saved_pic && apic_eois == saved_apic + 1);
    assert(frame_entries == saved_frames && timer_entries == saved_timers);
    frame.cs = 0x23;
    assert(irq_dispatch(&frame) == NULL);
    assert(eois == saved_pic && apic_eois == saved_apic + 2);
    assert(frame_entries == saved_frames + 1 && timer_entries == saved_timers);
    frame.vector = 0x40; frame.cs = 0x8; fixture_cpu = 0;
    saved_timers = timer_entries;
    assert(irq_dispatch(&frame) == NULL && local_ticks == 1);
    assert(!global_ticks && timer_entries == saved_timers);
    frame.cs = 0x23;
    assert(irq_dispatch(&frame) == NULL && local_ticks == 2);
    fixture_cpu = 1;
    assert(irq_dispatch(&frame) == NULL && local_ticks == 3);
    assert(!global_ticks && timer_entries == saved_timers + 2);
    fixture_cpu = 0; frame.vector = 0x20; keyboard_led.applied = 0;
    assert(irq_dispatch(&frame) == NULL && global_ticks == 1);
    puts("PASS local preemption on BSP/AP: no extra public clock ticks");
    puts("PASS IRQ1: no empty/AUX read, real key make/break and ACK handling preserved");
    puts("PASS reschedule IRQ: LAPIC EOI, deferred kernel return, user scheduling without timer accounting");
    return 0;
}
