/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/console.h>
#include <reliefnt/export.h>
#include <reliefnt/lock.h>
#include <reliefnt/serial.h>
#include <reliefnt/storage_wait.h>
#include <reliefnt/wait.h>
#include <linux/errno.h>
#include "../../arch/x86_64/port.h"

#define UART_COM1 0x3f8u
#define UART_IER (UART_COM1 + 1u)
#define UART_IIR (UART_COM1 + 2u)
#define UART_FCR (UART_COM1 + 2u)
#define UART_LCR (UART_COM1 + 3u)
#define UART_MCR (UART_COM1 + 4u)
#define UART_LSR (UART_COM1 + 5u)
#define UART_THRE 0x20u
#define UART_TEMT 0x40u
#define UART_TX_IRQ 0x02u
#define UART_FIFO_FLAGS 0xc0u
#define UART_FIFO_BYTES 16u
#define UART_POLL_LIMIT 100000u
#define UART_TX_CAP 32768u
#define UART_LOG_RESERVE 2048u

static struct kernel_spinlock tx_lock = KERNEL_SPINLOCK_INIT;
static unsigned char tx_bytes[UART_TX_CAP];
static size_t tx_read, tx_used;
static unsigned fifo_bytes = 1;
static bool uart_present, irq_routed, tx_running, tx_irq_enabled;
static bool last_was_cr, polled_last_was_cr;
static bool tx_pending;
static unsigned dropped_records;

/** @brief Check the COM1 hardware independently of the driver inventory.
 * @return True if early initialization found a UART.
 */
bool serial_com1_available(void)
{
    return uart_present;
}
EXPORT_SYMBOL(serial_com1_available);

/** @brief Probe and initialize COM1 for early output at 115200 baud, 8N1. */
void serial_init(void)
{
    if (x86_64_inb(UART_LSR) == 0xffu) return;
    x86_64_outb(0, UART_IER);
    x86_64_outb(0x80u, UART_LCR);
    /* 115200 8N1, matching the GRUB serial terminal. */
    x86_64_outb(1, UART_COM1);
    x86_64_outb(0, UART_IER);
    x86_64_outb(3, UART_LCR);
    x86_64_outb(7, UART_FCR);
    if ((x86_64_inb(UART_IIR) & UART_FIFO_FLAGS) == UART_FIFO_FLAGS)
        fifo_bytes = UART_FIFO_BYTES;
    else
        x86_64_outb(0, UART_FCR);
    x86_64_outb(0x0bu, UART_MCR);
    uart_present = true;
}

/** @brief Record the IRQ4 routing result before starting queued transmission.
 * @param routed True when the installed interrupt route can deliver IRQ4.
 */
void serial_tx_set_irq(bool routed)
{
    irq_routed = routed;
}

/** @brief Transfer COM1 from boot polling to the queue before entering userspace. */
void serial_tx_start(void)
{
    last_was_cr = polled_last_was_cr;
    __atomic_store_n(&tx_running, uart_present, __ATOMIC_RELEASE);
}

/** @brief Poll for a transmitter state without sleeping.
 * @param mask Status bits sought in the line status register.
 * @return True when any requested bit is set before the cap.
 */
static bool uart_wait(unsigned mask)
{
    for (unsigned attempt = 0; attempt < UART_POLL_LIMIT; ++attempt) {
        uint8_t status = x86_64_inb(UART_LSR);
        if (status == 0xffu) return false;
        if (status & mask) return true;
        __asm__ volatile("pause" ::: "memory");
    }
    return false;
}

/** @brief Fill the UART in batches during early boot and emergency output.
 * @param data Borrowed kernel buffer under boot or emergency ownership.
 * @param length Available source bytes, including embedded NULs.
 * @return Number of input bytes sent before a timeout. A single-byte UART may
 * send the two bytes of a translated newline in separate FIFO refills.
 */
static size_t uart_polled_write(const char *data, size_t length)
{
    size_t consumed = 0;
    while (consumed != length && uart_wait(UART_THRE)) {
        for (unsigned slot = 0; slot < fifo_bytes && consumed != length; ++slot) {
            char byte = data[consumed];
            if (byte == '\n' && !polled_last_was_cr) {
                x86_64_outb('\r', UART_COM1);
                polled_last_was_cr = true;
                continue;
            }
            x86_64_outb((uint8_t)byte, UART_COM1);
            polled_last_was_cr = byte == '\r';
            ++consumed;
        }
    }
    return consumed;
}

/** @brief Append one byte to the ring with TX locked and space available.
 * @param byte Encoded byte to enqueue.
 */
static void queue_byte(unsigned char byte)
{
    tx_bytes[(tx_read + tx_used) & (UART_TX_CAP - 1u)] = byte;
    ++tx_used;
}

/** @brief Publish pending work and enable THRE interrupts only with queued bytes. */
static void uart_arm_locked(void)
{
    bool pending = tx_used != 0;
    bool want_irq = irq_routed && pending;
    if (want_irq != tx_irq_enabled) {
        x86_64_outb(want_irq ? UART_TX_IRQ : 0, UART_IER);
        tx_irq_enabled = want_irq;
    }
    __atomic_store_n(&tx_pending, pending, __ATOMIC_RELEASE);
}

/** @brief Check the user write allowance while the TX lock is held.
 * @return True if any byte, including a translated LF, can fit outside the reserve.
 */
static bool uart_writable_locked(void)
{
    return UART_TX_CAP - tx_used >= UART_LOG_RESERVE + 2u;
}

/** @brief Check whether a terminal write can enqueue any single byte.
 * @return True if COM1 is present and room remains for one byte or CR/LF.
 */
bool serial_tx_writable(void)
{
    if (!uart_present) return false;
    uint64_t flags;
    kernel_spin_lock_irqsave(&tx_lock, &flags);
    bool writable = uart_writable_locked();
    kernel_spin_unlock_irqrestore(&tx_lock, flags);
    return writable;
}

/** @brief Transfer at most one batch and notify waiters after unlocking.
 * @param from_irq True to acknowledge THRE via IIR even if the queue emptied.
 */
static void uart_pump(bool from_irq)
{
    if (!from_irq && !__atomic_load_n(&tx_pending, __ATOMIC_ACQUIRE)) return;
    uint64_t flags;
    kernel_spin_lock_irqsave(&tx_lock, &flags);
    if (from_irq) (void)x86_64_inb(UART_IIR);
    bool notify = false;
    unsigned dropped = 0;
    if (tx_used) {
        uint8_t status = x86_64_inb(UART_LSR);
        if (status != 0xffu && (status & UART_THRE)) {
            bool was_writable = uart_writable_locked();
            size_t count = tx_used < fifo_bytes ? tx_used : fifo_bytes;
            for (size_t i = 0; i < count; ++i) {
                x86_64_outb(tx_bytes[tx_read], UART_COM1);
                tx_read = (tx_read + 1u) & (UART_TX_CAP - 1u);
            }
            tx_used -= count;
            notify = !was_writable && uart_writable_locked();
            if (!tx_used) {
                dropped = dropped_records;
                dropped_records = 0;
            }
        }
    }
    uart_arm_locked();
    kernel_spin_unlock_irqrestore(&tx_lock, flags);
    if (notify) {
        storage_completion_notify();
        kernel_poll_readiness_notify();
    }
    /* The logger can enqueue to this UART. Report lost output only after
     * releasing the TX lock, once the preceding backlog has drained. */
    if (dropped)
        console_warn("[serial] transmit queue full, dropped %u records or console chunks\n", dropped);
}

/** @brief Acknowledge the UART interrupt and fill at most one transmit FIFO. */
void serial_tx_irq(void)
{
    if (uart_present) uart_pump(true);
}

/** @brief Attempt a FIFO refill on a PIT tick when queued bytes remain. */
void serial_tx_tick(void)
{
    uart_pump(false);
}

/** @brief Copy and translate bytes into the bounded ring with local IRQs masked.
 * @param data Kernel-owned buffer valid for length bytes.
 * @param length Available input bytes.
 * @param record Whole-record mode, with drop accounting when it cannot fit.
 * @param important Permission to consume the diagnostic reserve.
 * @return Number of input bytes enqueued. CR/LF expansion does not count.
 */
static size_t uart_enqueue(const char *data, size_t length, bool record, bool important)
{
    uint64_t flags;
    kernel_spin_lock_irqsave(&tx_lock, &flags);
    size_t available = UART_TX_CAP - tx_used;
    if (!important)
        available = available > UART_LOG_RESERVE ? available - UART_LOG_RESERVE : 0;
    if (record) {
        size_t needed = length;
        bool previous_cr = last_was_cr;
        for (size_t i = 0; i < length && needed <= available; ++i) {
            if (data[i] == '\n' && !previous_cr) ++needed;
            previous_cr = data[i] == '\r';
        }
        if (needed > available) {
            if (dropped_records != UINT32_MAX) ++dropped_records;
            kernel_spin_unlock_irqrestore(&tx_lock, flags);
            return 0;
        }
    }
    size_t consumed = 0;
    while (consumed < length && available) {
        char byte = data[consumed];
        if (byte == '\n' && !last_was_cr) {
            if (available < 2u) break;
            queue_byte('\r');
            --available;
        }
        queue_byte((unsigned char)byte);
        last_was_cr = byte == '\r';
        --available;
        ++consumed;
    }
    uart_arm_locked();
    kernel_spin_unlock_irqrestore(&tx_lock, flags);
    return consumed;
}

/** @brief Submit a complete best-effort diagnostic record or console chunk.
 * @param text Kernel-owned bytes copied before return, or NULL to skip output.
 * @param length Number of bytes in the buffer, including any line break.
 * @param important Permit using the reserve for warnings and errors.
 * Runtime submission is safe with interrupts disabled and never polls the UART.
 */
void serial_tx_record(const char *text, size_t length, bool important)
{
    if (!uart_present || !text || !length) return;
    if (__atomic_load_n(&tx_running, __ATOMIC_ACQUIRE)) {
        (void)uart_enqueue(text, length, true, important);
        return;
    }
    /* Early writers are already serialized by the boot console. */
    (void)uart_polled_write(text, length);
}
EXPORT_SYMBOL(serial_tx_record);

/** @brief Enqueue user terminal bytes from a staged kernel buffer.
 * @param data Kernel-owned source bytes, preserving embedded NULs.
 * @param length Maximum input bytes to consume.
 * @return Positive input bytes accepted, zero for an empty write, or
 * negative ENODEV/EAGAIN. The syscall layer handles blocking retries.
 */
int serial_tx_user(const void *data, size_t length)
{
    if (!length) return 0;
    if (!uart_present) return -ENODEV;
    size_t written = uart_enqueue(data, length, false, false);
    /* The syscall retry path sleeps interruptibly for a blocking descriptor.
     * A nonblocking descriptor returns EAGAIN directly to the caller. */
    return written ? (int)written : -EAGAIN;
}

/** @brief Drain pending bytes with bounded polling before reset or poweroff. */
void serial_tx_flush(void)
{
    if (!uart_present) return;
    /* Shutdown can complete with interrupts disabled. Bound the number of
     * successful refills as well as each hardware wait. */
    for (size_t i = 0; i < UART_TX_CAP + UART_LOG_RESERVE &&
                       __atomic_load_n(&tx_pending, __ATOMIC_ACQUIRE); ++i) {
        if (!uart_wait(UART_THRE)) break;
        uart_pump(false);
    }
    (void)uart_wait(UART_TEMT);
}

/** @brief Disable UART interrupts after the panic owner stops its peer CPUs. */
void serial_panic_quiesce(void)
{
    if (uart_present) x86_64_outb(0, UART_IER);
}

/** @brief Send emergency bytes directly to COM1 using bounded FIFO polling.
 * @param text Kernel-owned bytes to transmit or NULL to skip output.
 * @param length Number of bytes available in the buffer.
 */
void serial_write_emergency(const char *text, size_t length)
{
    if (uart_present && text) (void)uart_polled_write(text, length);
}
