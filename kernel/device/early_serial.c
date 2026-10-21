#include "manager_internal.h"

static void early_serial_putc(char ch);


static void early_serial_putc(char ch)
{
    uint32_t attempts = 100000u;
    if (!early_serial_ready) {
        return;
    }
    while (!(x86_64_inb((uint16_t)EARLY_SERIAL_LSR) & 0x20u) && attempts--) {
        __asm__ volatile("pause");
    }
    x86_64_outb((uint8_t)ch, (uint16_t)EARLY_SERIAL_COM1);
}

void early_serial_write(const char *text)
{
    while (text && *text) {
        if (*text == '\n') {
            early_serial_putc('\r');
        }
        early_serial_putc(*text++);
    }
}

/**
 * @brief Bypass driver registration and locks when reporting a fatal failure.
 * @param text Bytes to send through the already initialized COM1 UART.
 * @param length Number of bytes, independent of NUL termination.
 */
void serial_write_emergency(const char *text, size_t length)
{
    if (!text) return;
    for (size_t index = 0; index < length; ++index) {
        if (text[index] == '\n') early_serial_putc('\r');
        early_serial_putc(text[index]);
    }
}
