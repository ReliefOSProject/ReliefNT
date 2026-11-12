/* SPDX-License-Identifier: Apache-2.0 */
/* Exercise the production register and delay functions at their hardware boundary. */
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "../../drivers/net/r8169/internal.h"

#define TEST_IF (1ULL << 9)
#define TICK_NS 10000000ULL

enum clock_mode { CLOCK_RUNNING, CLOCK_STALLED, CLOCK_BACKWARD };

static enum clock_mode mode;
static uint64_t wall_ns;
static unsigned clock_reads, sleeps;
static bool irq_enabled;
static uint16_t word_port, word_value;
static uint16_t byte_port, dword_port;
static uint8_t byte_value;
static uint32_t dword_value;

/** @brief Save simulated local IF while confirming that no real lock is taken. */
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{
    assert(!lock && flags);
    *flags = irq_enabled ? TEST_IF : 0;
    irq_enabled = false;
}

/** @brief Restore the IF saved by the real driver's IRQ guard. */
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{
    assert(!lock);
    irq_enabled = (flags & TEST_IF) != 0;
}

/** @brief Sample a quantized clock with selectable progress and fault behavior. */
uint64_t time_uptime_us(void)
{
    assert(!irq_enabled);
    ++clock_reads;
    assert(clock_reads < 100000);
    wall_ns += 250;
    if (mode == CLOCK_STALLED)
        return 1000;
    if (mode == CLOCK_BACKWARD)
        return clock_reads == 2 ? 1001 : 1000;
    return wall_ns / 1000;
}

/** @brief Model the production 100 Hz sleep and its IF-cleared return state. */
static void sleep_ms(uint64_t milliseconds)
{
    uint64_t delta = (milliseconds * 100 + 999) / 1000;
    assert(delta != 0);
    ++sleeps;
    wall_ns = (wall_ns / TICK_NS + delta) * TICK_NS;
    irq_enabled = false;
}

/** @brief Read a modeled legacy byte port. */
static uint8_t inb(uint16_t port)
{
    byte_port = port;
    return byte_value;
}

/** @brief Write a modeled legacy byte port. */
static void outb(uint16_t port, uint8_t value)
{
    byte_port = port;
    byte_value = value;
}

/** @brief Read a modeled legacy dword port. */
static uint32_t inl(uint16_t port)
{
    dword_port = port;
    return dword_value;
}

/** @brief Write a modeled legacy dword port. */
static void outl(uint16_t port, uint32_t value)
{
    dword_port = port;
    dword_value = value;
}

/** @brief Read a modeled legacy word port. */
uint16_t x86_64_inw(uint16_t port)
{
    word_port = port;
    return word_value;
}

/** @brief Write a modeled legacy word port. */
void x86_64_outw(uint16_t value, uint16_t port)
{
    word_port = port;
    word_value = value;
}

static const struct reliefos_driver_kernel_api api = {
    .inb = inb, .outb = outb, .inl = inl, .outl = outl, .sleep_ms = sleep_ms,
};
static struct rtl_device device = { .api = &api };

/** @brief Reset the simulated clocks and caller IRQ state for one scenario. */
static void reset_clock(enum clock_mode next_mode, uint64_t start_ns, bool entry_if)
{
    mode = next_mode;
    wall_ns = start_ns;
    clock_reads = sleeps = 0;
    irq_enabled = entry_if;
}

/** @brief Require submillisecond delays to progress without a tick-level sleep. */
static void test_short_delays(void)
{
    static const unsigned waits[] = { 1, 10, 20, 40, 100, 200, 999 };
    static const uint64_t starts[] = { 50000, 50250, 50700, 50999 };
    for (unsigned state = 0; state < 2; ++state) {
        for (unsigned i = 0; i < sizeof(waits) / sizeof(waits[0]); ++i) {
            for (unsigned j = 0; j < sizeof(starts) / sizeof(starts[0]); ++j) {
                reset_clock(CLOCK_RUNNING, starts[j], state != 0);
                uint64_t start = wall_ns;
                rtl_delay_us(&device, waits[i]);
                assert(sleeps == 0);
                assert(wall_ns - start >= (uint64_t)waits[i] * 1000);
                assert(irq_enabled == (state != 0));
            }
        }
        reset_clock(CLOCK_RUNNING, 50999, state != 0);
        rtl_delay_us(&device, 0);
        assert(clock_reads == 0 && sleeps == 0 && irq_enabled == (state != 0));
    }
}

/** @brief Test partial-first-tick sleep phases including the maximum argument. */
static void test_tick_sleep_minimum_and_irq_state(void)
{
    static const unsigned waits[] = { 1000, 3000, 9999, 10000, 10001, UINT_MAX };
    static const uint64_t phases[] = { 1, 5000000, 9999999 };
    for (unsigned state = 0; state < 2; ++state) {
        for (unsigned i = 0; i < sizeof(waits) / sizeof(waits[0]); ++i) {
            for (unsigned j = 0; j < sizeof(phases) / sizeof(phases[0]); ++j) {
                reset_clock(CLOCK_RUNNING, TICK_NS * 2 + phases[j], state != 0);
                uint64_t start = wall_ns;
                rtl_delay_us(&device, waits[i]);
                assert(sleeps == 1);
                assert(wall_ns - start >= (uint64_t)waits[i] * 1000);
                assert(wall_ns - start <= (uint64_t)waits[i] * 1000 + 30000000ULL);
                assert(irq_enabled == (state != 0));
            }
        }
    }
}

/** @brief The sleep callback's cleared IF must not leak to its caller. */
static void test_sleep_restores_if(void)
{
    reset_clock(CLOCK_RUNNING, TICK_NS + 1, true);
    rtl_delay_us(&device, 1000);
    assert(sleeps == 1 && irq_enabled);
}

/** @brief Clock stalls or reversal must terminate polling and use a full fallback wait. */
static void test_clock_faults(void)
{
    for (unsigned state = 0; state < 2; ++state) {
        for (enum clock_mode fault = CLOCK_STALLED; fault <= CLOCK_BACKWARD; ++fault) {
            reset_clock(fault, 3 * TICK_NS - 1, state != 0);
            uint64_t start = wall_ns;
            rtl_delay_us(&device, 20);
            assert(clock_reads > 0 && clock_reads < 100000);
            if (fault == CLOCK_BACKWARD)
                assert(clock_reads <= 16);
            assert(sleeps == 1);
            assert(wall_ns - start >= 20000);
            assert(irq_enabled == (state != 0));
        }
    }
}

/** @brief Check that MMIO and port reads/writes use the requested width and offset. */
static void test_register_accesses(void)
{
    uint32_t registers[8] = {0};
    device.io_base = 0;
    device.registers = (volatile uint8_t *)registers;
    rtl_write8(&device, 1, 0xab);
    rtl_write16(&device, 2, 0xcdef);
    rtl_write32(&device, 4, 0x12345678);
    assert(rtl_read8(&device, 1) == 0xab);
    assert(rtl_read16(&device, 2) == 0xcdef);
    assert(rtl_read32(&device, 4) == 0x12345678);

    device.io_base = 0xe000;
    rtl_write8(&device, 1, 0x56);
    rtl_write16(&device, 2, 0x789a);
    rtl_write32(&device, 4, 0xbcdef012);
    assert(byte_port == 0xe001 && word_port == 0xe002 && dword_port == 0xe004);
    assert(rtl_read8(&device, 1) == 0x56 && byte_port == 0xe001);
    assert(rtl_read16(&device, 2) == 0x789a && word_port == 0xe002);
    assert(rtl_read32(&device, 4) == 0xbcdef012 && dword_port == 0xe004);
}

/** @brief Run all scenarios, or one named scenario to diagnose a regression.
 * @param argc Argument count. @param argv Optional test name.
 * @return Zero after the selected assertions pass.
 */
int main(int argc, char **argv)
{
    assert(argc <= 2);
    if (argc == 1 || !strcmp(argv[1], "short")) test_short_delays();
    if (argc == 1 || !strcmp(argv[1], "long")) test_tick_sleep_minimum_and_irq_state();
    if (argc == 1 || !strcmp(argv[1], "irq")) test_sleep_restores_if();
    if (argc == 1 || !strcmp(argv[1], "faults")) test_clock_faults();
    if (argc == 1 || !strcmp(argv[1], "registers")) test_register_accesses();
    puts("r8169 I/O and timing tests passed");
    return 0;
}
