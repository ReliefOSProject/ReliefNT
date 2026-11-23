/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_SERIAL_H
#define RELIEFNT_SERIAL_H

#include <reliefnt/types.h>

/** @brief Check the COM1 hardware independently of the driver inventory.
 * @return True if early initialization found a UART.
 */
bool serial_com1_available(void);
/** @brief Probe and initialize COM1 for early output at 115200 baud, 8N1. */
void serial_init(void);
/** @brief Record the IRQ4 routing result before starting queued transmission.
 * @param routed True when the installed interrupt route can deliver IRQ4.
 */
void serial_tx_set_irq(bool routed);
/** @brief Transfer COM1 from boot polling to the queue before entering userspace. */
void serial_tx_start(void);
/** @brief Acknowledge the UART interrupt and fill at most one transmit FIFO. */
void serial_tx_irq(void);
/** @brief Attempt a FIFO refill on a PIT tick when queued bytes remain. */
void serial_tx_tick(void);
/** @brief Drain pending bytes with bounded polling before reset or poweroff. */
void serial_tx_flush(void);
/** @brief Check whether a terminal write can enqueue any single byte.
 * @return True if COM1 is present and room remains for one byte or CR/LF.
 */
bool serial_tx_writable(void);

/** @brief Submit a complete best-effort diagnostic record or console chunk.
 * @param text Kernel-owned bytes copied before return, or NULL to skip output.
 * @param length Number of bytes in the buffer, including any line break.
 * @param important Permit using the reserve for warnings and errors.
 * Runtime submission is safe with interrupts disabled and never polls the UART.
 */
void serial_tx_record(const char *text, size_t length, bool important);

/** @brief Enqueue user terminal bytes from a staged kernel buffer.
 * @param data Kernel-owned source bytes, preserving embedded NULs.
 * @param length Maximum input bytes to consume.
 * @return Positive input bytes accepted, zero for an empty write, or
 * negative ENODEV/EAGAIN. The syscall layer handles blocking retries.
 */
int serial_tx_user(const void *data, size_t length);

/** @brief Disable UART interrupts after the panic owner stops its peer CPUs. */
void serial_panic_quiesce(void);
/** @brief Send emergency bytes directly to COM1 using bounded FIFO polling.
 * @param text Kernel-owned bytes to transmit or NULL to skip output.
 * @param length Number of bytes available in the buffer.
 */
void serial_write_emergency(const char *text, size_t length);

#endif
