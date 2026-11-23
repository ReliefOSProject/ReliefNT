/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <reliefnt/console.h>
#include <reliefnt/lock.h>
#include <reliefnt/serial.h>
#include <linux/errno.h>
#include "../../arch/x86_64/port.h"

#define UART_COM1 0x3f8u
#define UART_IER (UART_COM1 + 1u)
#define UART_IIR (UART_COM1 + 2u)
#define UART_LCR (UART_COM1 + 3u)
#define UART_LSR (UART_COM1 + 5u)
#define UART_THRE 0x20u
#define UART_TEMT 0x40u
#define UART_TX_IRQ 0x02u
#define UART_FIFO_FLAGS 0xc0u
#define UART_FIFO_BYTES 16u
#define UART_TX_CAP 32768u
#define UART_LOG_RESERVE 2048u

/* Emulate the port boundary only. A load occupies the FIFO until the test
 * advances the wire and is limited to what the UART acknowledged at init. */
static unsigned char wire[UART_TX_CAP + 1024];
static size_t wire_length;
static unsigned fifo_load, fifo_capacity = UART_FIFO_BYTES, iir_reads;
static uint8_t uart_ier, uart_lcr;
static unsigned storage_wakes, poll_wakes, overrun_drops;
static bool fifo_ready = true, uart_absent, panic_phase;
static unsigned lock_depth;

void x86_64_outb(uint8_t byte, uint16_t port)
{
    if (port == UART_LCR) uart_lcr = byte;
    if (uart_lcr & 0x80u) {
        if (port == UART_COM1) assert(byte == 1); /* 115200 baud */
        if (port == UART_IER) assert(byte == 0);
        return;
    }
    if (port == UART_IER) uart_ier = byte;
    if (port != UART_COM1) return;
    assert(fifo_ready && fifo_load < fifo_capacity);
    assert(wire_length < sizeof(wire));
    wire[wire_length++] = byte;
    if (++fifo_load == fifo_capacity) fifo_ready = false;
}

uint8_t x86_64_inb(uint16_t port)
{
    if (uart_absent) return 0xffu;
    if (port == UART_IIR) {
        ++iir_reads;
        return fifo_capacity == 1 ? 1 : UART_FIFO_FLAGS | 1u;
    }
    assert(port == UART_LSR);
    return fifo_ready ? UART_THRE | UART_TEMT : 0;
}

void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{ assert(!panic_phase && !lock->state); lock->state = ++lock_depth; *flags = 0; }
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{ assert(lock->state == 1 && !flags); lock->state = 0; --lock_depth; }
void storage_completion_notify(void) { ++storage_wakes; }
void kernel_poll_readiness_notify(void) { ++poll_wakes; }
void console_log(enum console_log_level level, const char *format, ...)
{
    assert(!lock_depth && level == CONSOLE_LOG_WARN);
    assert(!strncmp(format, "[serial] transmit queue full", 28));
    va_list args;
    va_start(args, format);
    overrun_drops += va_arg(args, unsigned);
    va_end(args);
}

static void wire_ready(void)
{
    fifo_load = 0;
    fifo_ready = true;
}

int main(void)
{
    uart_absent = true;
    serial_init();
    assert(!serial_com1_available());
    assert(serial_tx_user("X", 1) == -ENODEV);
    uart_absent = false;
    serial_init();
    assert(serial_com1_available());
    serial_tx_record("early\r", 6, false);
    serial_tx_set_irq(true);
    serial_tx_start();
    assert(iir_reads == 1 && wire_length == 6 && uart_ier == 0);

    assert(serial_tx_user("\nA\0B\n", 5) == 5);
    assert(wire_length == 6 && uart_ier == UART_TX_IRQ);
    wire_ready();
    serial_tx_irq();
    assert(iir_reads == 2 && !uart_ier);
    assert(wire_length == 12 && !memcmp(wire, "early\r\nA\0B\r\n", 12));
    wire_length = 0;

    static char bulk[UART_TX_CAP];
    memset(bulk, 'U', sizeof(bulk));
    size_t user_capacity = UART_TX_CAP - UART_LOG_RESERVE;
    assert(serial_tx_user(bulk, user_capacity - 1u) == (int)user_capacity - 1);
    assert(!serial_tx_writable());
    assert(serial_tx_user("\n", 1) == -EAGAIN); /* CR/LF must both fit */
    serial_tx_record("bad", 3, false);
    serial_tx_record("kept\n", 5, true);
    wire_ready();
    serial_tx_tick(); /* An IRQ may have been missed. */
    assert(wire_length == UART_FIFO_BYTES);
    assert(serial_tx_writable() && storage_wakes == 1 && poll_wakes == 1);
    assert(serial_tx_user("\n", 1) == 1);
    for (size_t i = 0; uart_ier && i < UART_TX_CAP / UART_FIFO_BYTES; ++i) {
        wire_ready();
        serial_tx_irq();
    }
    assert(!uart_ier && overrun_drops == 1);
    assert(wire_length == user_capacity + 7u);
    assert(!memcmp(wire, bulk, user_capacity - 1u));
    assert(!memcmp(wire + user_capacity - 1u, "kept\r\n\r\n", 8));

    wire_length = 0;
    wire_ready();
    assert(serial_tx_user("shutdown\n", 9) == 9);
    serial_tx_flush();
    assert(wire_length == 10 && !memcmp(wire, "shutdown\r\n", 10));

    wire_length = 0;
    wire_ready();
    serial_tx_record("queued\n", 7, false);
    /* Panic output must work even if another CPU died holding TX ownership. */
    panic_phase = true;
    serial_panic_quiesce();
    serial_write_emergency("panic\n", 6);
    assert(wire_length == 7 && !memcmp(wire, "panic\r\n", 7) && !uart_ier);
    puts("PASS serial TX ordering, backpressure, IRQ/PIT drain, shutdown and panic bypass");
    return 0;
}
