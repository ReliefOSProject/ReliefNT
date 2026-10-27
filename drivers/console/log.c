#include "internal.h"
#include <reliefnt/export.h>

static void print_unsigned_raw(uint64_t value, unsigned base, bool upper);

static void console_log_control(const char *sequence);

static void console_log_putc(enum console_log_level level, char character);

static void console_log_string(enum console_log_level level, const char *text);

static void console_log_number(enum console_log_level level, uint64_t value,
                               unsigned base, bool upper, unsigned width,
                               bool zero_pad, bool negative);

static bool console_log_color_enabled;
static uint32_t console_log_owner = UINT32_MAX;
static const char *const console_log_prefixes[] = {
    "[DEBUG] ", "[INFO] ", "[WARN] ", "[ERROR] ", "[FATAL] ",
};
static const char *const console_log_colors[] = {
    "\033[38;2;128;144;160m",
    "\033[38;2;144;168;160m",
    "\033[38;2;192;168;112m",
    "\033[38;2;192;128;128m",
    "\033[38;2;176;128;160m",
};
void log_store(char ch)
{
    if (log_len < CONSOLE_LOG_CAP) {
        log_buffer[(log_start + log_len++) % CONSOLE_LOG_CAP] = ch;
    } else {
        log_buffer[log_start] = ch;
        log_start = (log_start + 1u) % CONSOLE_LOG_CAP;
    }
}

size_t console_log_replay_start(void)
{
    size_t first = 0;
    if (log_len == CONSOLE_LOG_CAP) {
        while (first < log_len && log_buffer[(log_start + first) % CONSOLE_LOG_CAP] != '\n') ++first;
        if (first < log_len) ++first;
    }
    return first;
}

static void print_unsigned_raw(uint64_t value, unsigned base, bool upper)
{
    char buf[32];
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    size_t i = 0;

    if (value == 0) {
        console_emit_raw('0');
        return;
    }

    while (value && i < sizeof(buf)) {
        buf[i++] = digits[value % base];
        value /= base;
    }
    while (i) {
        console_emit_raw(buf[--i]);
    }
}

void console_emit_timestamp(void)
{
    uint64_t uptime_us = time_uptime_us();
    uint64_t seconds = uptime_us / 1000000ULL;
    uint64_t micros = uptime_us % 1000000ULL;
    uint64_t divisor = 1ULL;
    uint32_t digits = 1U;

    while (seconds >= divisor * 10ULL && digits < 20U) {
        divisor *= 10ULL;
        ++digits;
    }
    console_emit_raw('[');
    for (uint32_t i = digits; i < 5U; ++i) {
        console_emit_raw(' ');
    }
    print_unsigned_raw(seconds, 10, false);
    console_emit_raw('.');
    console_emit_raw((char)('0' + (micros / 100000ULL) % 10ULL));
    console_emit_raw((char)('0' + (micros / 10000ULL) % 10ULL));
    console_emit_raw((char)('0' + (micros / 1000ULL) % 10ULL));
    console_emit_raw((char)('0' + (micros / 100ULL) % 10ULL));
    console_emit_raw((char)('0' + (micros / 10ULL) % 10ULL));
    console_emit_raw((char)('0' + micros % 10ULL));
    console_emit_raw(']');
    console_emit_raw(' ');
}

void console_log_configure(const char *cmdline)
{
    console_log_color_enabled = true;
    const char *cursor = cmdline;
    while (cursor && *cursor) {
        while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n') ++cursor;
        const char *end = cursor;
        while (*end && *end != ' ' && *end != '\t' && *end != '\r' && *end != '\n') ++end;
        size_t length = (size_t)(end - cursor);
        if (length == sizeof("logcolor=0") - 1u) {
            if (__builtin_memcmp(cursor, "logcolor=0", length) == 0)
                console_log_color_enabled = false;
            else if (__builtin_memcmp(cursor, "logcolor=1", length) == 0)
                console_log_color_enabled = true;
        }
        cursor = end;
    }
}

static void console_log_control(const char *sequence)
{
    serial_write(sequence);
    for (const char *cursor = sequence; *cursor; ++cursor) {
        log_store(*cursor);
        if (!console_service_logs_only) {
            fb_console_ansi_feed(*cursor);
        }
    }
}

static void console_log_putc(enum console_log_level level, char character)
{
    if (console_line_start && character != '\n' && character != '\r') {
        console_emit_timestamp();
        if (console_log_color_enabled) console_log_control(console_log_colors[level]);
        const char *prefix = console_log_prefixes[level];
        while (*prefix) console_emit_raw(*prefix++);
        if (console_log_color_enabled) console_log_control("\033[0m");
    }
    bool line_start = console_line_start;
    console_emit_raw(character);
    if (character == '\r' && line_start) console_line_start = true;
}

static void console_log_string(enum console_log_level level, const char *text)
{
    while (text && *text) console_log_putc(level, *text++);
}

static void console_log_number(enum console_log_level level, uint64_t value,
                               unsigned base, bool upper, unsigned width,
                               bool zero_pad, bool negative)
{
    char digits_buffer[32];
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    unsigned count = 0;
    do {
        digits_buffer[count++] = digits[value % base];
        value /= base;
    } while (value && count < sizeof(digits_buffer));
    unsigned length = count + (negative ? 1u : 0u);
    if (negative && zero_pad) console_log_putc(level, '-');
    while (width > length) {
        console_log_putc(level, zero_pad ? '0' : ' ');
        --width;
    }
    if (negative && !zero_pad) console_log_putc(level, '-');
    while (count) console_log_putc(level, digits_buffer[--count]);
}

void console_vlog(enum console_log_level level, const char *fmt, va_list arguments)
{
    if (!fmt || !*fmt) return;
    if ((unsigned)level > CONSOLE_LOG_FATAL) level = CONSOLE_LOG_INFO;
    uint64_t flags = kernel_irq_save();
    uint32_t cpu = smp_current_cpu();
    bool acquired = false;
    if (__atomic_load_n(&console_log_owner, __ATOMIC_RELAXED) != cpu) {
        do {
            uint32_t expected = UINT32_MAX;
            acquired = __atomic_compare_exchange_n(&console_log_owner, &expected,
                cpu, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
            if (acquired || level == CONSOLE_LOG_FATAL) break;
            smp_membarrier_poll();
            __asm__ volatile("pause" ::: "memory");
        } while (!acquired);
    }

    if (!console_line_start) console_emit_raw('\n');
    for (const char *p = fmt; *p; ++p) {
        if (*p != '%') {
            console_log_putc(level, *p);
            continue;
        }

        ++p;
        if (!*p) {
            console_log_putc(level, '%');
            break;
        }
        bool zero_pad = *p == '0';
        unsigned width = 0;
        while (*p >= '0' && *p <= '9') {
            width = width < 64u ? width * 10u + (unsigned)(*p - '0') : 64u;
            if (width > 64u) width = 64u;
            ++p;
        }
        bool long_flag = false;
        bool long_long_flag = false;
        bool size_flag = false;
        if (*p == 'l') {
            long_flag = true;
            ++p;
            if (*p == 'l') {
                long_long_flag = true;
                ++p;
            }
        } else if (*p == 'z') {
            size_flag = true;
            ++p;
        }
        if (!*p) break;

        switch (*p) {
        case 's': {
            const char *text = va_arg(arguments, const char *);
            console_log_string(level, text ? text : "(null)");
            break;
        }
        case 'c':
            console_log_putc(level, (char)va_arg(arguments, int));
            break;
        case 'd':
        case 'i': {
            int64_t value = long_long_flag ? va_arg(arguments, long long)
                         : long_flag ? va_arg(arguments, long)
                         : size_flag ? va_arg(arguments, ptrdiff_t)
                                     : va_arg(arguments, int);
            uint64_t magnitude = value < 0 ? 0ULL - (uint64_t)value : (uint64_t)value;
            console_log_number(level, magnitude, 10, false, width, zero_pad, value < 0);
            break;
        }
        case 'u': {
            uint64_t value = long_long_flag ? va_arg(arguments, unsigned long long)
                          : long_flag ? va_arg(arguments, unsigned long)
                          : size_flag ? va_arg(arguments, size_t)
                                      : va_arg(arguments, unsigned int);
            console_log_number(level, value, 10, false, width, zero_pad, false);
            break;
        }
        case 'x':
        case 'X': {
            uint64_t value = long_long_flag ? va_arg(arguments, unsigned long long)
                          : long_flag ? va_arg(arguments, unsigned long)
                          : size_flag ? va_arg(arguments, size_t)
                                      : va_arg(arguments, unsigned int);
            console_log_number(level, value, 16, *p == 'X', width, zero_pad, false);
            break;
        }
        case 'p': {
            uintptr_t value = (uintptr_t)va_arg(arguments, void *);
            console_log_string(level, "0x");
            console_log_number(level, value, 16, false, width, zero_pad, false);
            break;
        }
        case '%':
            console_log_putc(level, '%');
            break;
        default:
            console_log_putc(level, '%');
            console_log_putc(level, *p);
            break;
        }
    }

    if (!console_line_start) console_emit_raw('\n');
    console_present();
    if (acquired) __atomic_store_n(&console_log_owner, UINT32_MAX, __ATOMIC_RELEASE);
    kernel_irq_restore(flags);
}

/** @brief Append one formatted diagnostic through the permanent console/log core.
 * @param level Severity selecting the configured log policy.
 * @param fmt Borrowed terminated format string. @param ... Values matching fmt.
 * IRQ-safe and bounded by the underlying console policy. No caller storage is
 * retained; the core restores incoming IRQ state and serial callback pins.
 */
void console_log(enum console_log_level level, const char *fmt, ...)
{
    va_list arguments;
    va_start(arguments, fmt);
    console_vlog(level, fmt, arguments);
    va_end(arguments);
}
EXPORT_SYMBOL(console_log);

void console_log_write(const char *text)
{
    if (!text) return;
    enum console_log_level level = CONSOLE_LOG_INFO;
    for (unsigned candidate = CONSOLE_LOG_DEBUG; candidate <= CONSOLE_LOG_FATAL; ++candidate) {
        const char *prefix = console_log_prefixes[candidate];
        size_t matched = 0;
        while (prefix[matched] && text[matched] == prefix[matched]) ++matched;
        if (!prefix[matched]) {
            level = (enum console_log_level)candidate;
            text += matched;
            break;
        }
    }
    console_log(level, "%s", text);
}
