/* Kernel half of the evdev integration fixture; no userspace sources. */
#include <assert.h>
#include <stdio.h>
#include "../../drivers/tty/input/input.c"
#include "../../arch/x86_64/keyboard_led.h"
static unsigned console_key_calls;
uint32_t pty_vt_active(void) { return 1; }
void pty_console_key_event(uint8_t keycode, uint8_t pressed)
{ (void)keycode; (void)pressed; ++console_key_calls; }

uint64_t time_uptime_us(void) { return 0; }
const struct framebuffer *framebuffer_get(void)
{
    static const struct framebuffer fb = {.available = 1, .width = 1280, .height = 800};
    return &fb;
}
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{
    (void)lock;
    *flags = 0;
}
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{
    (void)lock;
    (void)flags;
}
/* This fixture covers event routing, not poll waiter wakeups. */
void kernel_poll_readiness_notify(void) {}

static void test_evdev_damaged_records(void)
{
    struct input_event events[3];
    uint64_t cursor = 0;
    input_init();
    /* Reproduce the two damaged sequence numbers from the VMware core. */
    while (evdev_next_sequence < 5842) {
        evdev_publish(STORAGE_DEV_KIND_MOUSE, EV_SYN, SYN_REPORT, 0);
    }
    evdev_queue[5103 % INPUT_EVDEV_QUEUE_CAP].sequence = 0xffffffa1u;
    evdev_queue[5104 % INPUT_EVDEV_QUEUE_CAP].sequence = 0;
    assert(!input_evdev_available(STORAGE_DEV_KIND_KEYBOARD, cursor, 0));
    assert(input_evdev_read(STORAGE_DEV_KIND_KEYBOARD, &cursor,
                            events, sizeof(events), 0) == 0);
    assert(cursor == 5842);

    input_push_key(30, 1);
    assert(input_evdev_available(STORAGE_DEV_KIND_KEYBOARD, cursor, 0));
    assert(input_evdev_read(STORAGE_DEV_KIND_KEYBOARD, &cursor,
                            events, sizeof(events), 0) == sizeof(events));
    assert(events[0].type == EV_LED && events[0].code == LED_CAPSL && events[0].value == 0);
    assert(events[1].type == EV_KEY && events[1].code == 30 && events[1].value == 1);
    assert(events[2].type == EV_SYN && events[2].code == SYN_REPORT);
    assert(cursor == input_evdev_cursor_now());

    /* The scan must also terminate when every retained record is damaged. */
    for (unsigned i = 0; i < INPUT_EVDEV_QUEUE_CAP; ++i) {
        evdev_queue[i].sequence = 0;
    }
    cursor = 0;
    assert(input_evdev_read(STORAGE_DEV_KIND_MOUSE, &cursor,
                            events, sizeof(events), 0) == 0);
    assert(cursor == input_evdev_cursor_now());
}


static void test_keyboard_led_protocol(void)
{
    struct keyboard_led_command state = {.applied = 0xff};
    uint8_t byte;
    assert(!keyboard_led_next(&state, 0, 0, 1, &byte));
    assert(keyboard_led_next(&state, 0, 0, 0, &byte) && byte == 0xed);
    assert(!keyboard_led_reply(&state, 30));
    assert(!keyboard_led_next(&state, 4, 1, 0, &byte));
    assert(keyboard_led_reply(&state, 0xfa));
    assert(keyboard_led_next(&state, 4, 2, 0, &byte) && byte == 0);
    assert(keyboard_led_reply(&state, 0xfa));
    assert(state.applied == 0);
    assert(keyboard_led_next(&state, 4, 3, 0, &byte) && byte == 0xed);
    assert(keyboard_led_reply(&state, 0xfe));
    assert(keyboard_led_next(&state, 4, 4, 0, &byte) && byte == 0xed);
    assert(keyboard_led_reply(&state, 0xfa));
    assert(keyboard_led_next(&state, 4, 5, 0, &byte) && byte == 4);
    assert(keyboard_led_reply(&state, 0xfa));
    assert(!keyboard_led_next(&state, 4, 6, 0, &byte));
    assert(state.applied == 4);
    assert(keyboard_led_next(&state, 0, 7, 0, &byte));
    for (unsigned i = 1; i <= 3; ++i)
        assert(keyboard_led_next(&state, 0, 7 + i * 100000, 0, &byte));
    assert(!keyboard_led_next(&state, 0, 400007, 0, &byte));
    assert(!keyboard_led_next(&state, 0, 400008, 0, &byte));
    puts("PS/2 LEDs passed: ACK, RESEND, busy controller and bounded timeout recovery");
}


static void test_absolute_frames_resynchronize(void)
{
    struct input_event ev;
    uint64_t cursor;
    int saw_x = 0, saw_y = 0, saw_syn = 0;
    input_init();
    input_push_mouse(100, 200, 100, 200, 0);
    /* A reader that misses this frame (a wrapped ring under a VMware-rate
     * burst) must still resynchronize on the next one: the surviving frame
     * carries the authoritative absolute position, which is the contract
     * xorg.conf's IgnoreAbsoluteAxes "false" relies on. A delta-only view of
     * this frame is (800,500) and can never recover the lost (100,200)
     * frame. */
    cursor = input_evdev_cursor_now();
    input_push_mouse(900, 700, 800, 500, 0);
    while (input_evdev_read(STORAGE_DEV_KIND_MOUSE, &cursor, &ev, sizeof(ev), 0) ==
           (int)sizeof(ev)) {
        if (ev.type == EV_ABS && ev.code == ABS_X) {
            assert(ev.value == 900);
            saw_x = 1;
        }
        if (ev.type == EV_ABS && ev.code == ABS_Y) {
            assert(ev.value == 700);
            saw_y = 1;
        }
        if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
            saw_syn = 1;
        }
    }
    assert(saw_x && saw_y && saw_syn);
    puts("Absolute resync passed: a lost motion frame heals from the next EV_ABS");
}

void input_routing_fixture_checks(void)
{
    input_init();
    input_handle_scancode(30, 1);
    assert(console_key_calls == 0); /* Physical IRQ must not render a VT. */
    input_process_pending();
    assert(console_key_calls == 1);
    test_keyboard_led_protocol();
    test_evdev_damaged_records();
    test_absolute_frames_resynchronize();
}
