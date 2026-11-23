/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/export.h>
#include <reliefnt/serial.h>

#define CONSOLE_LOG_MESSAGE_BYTES 768u
#define CONSOLE_LOG_RECORD_BYTES (CONSOLE_LOG_MESSAGE_BYTES + 96u)

static bool console_log_color_enabled = true;
static bool log_screen = true;
static bool log_serial = true;
/* Kernel priorities are Linux loglevel values. Only priorities below the
 * console threshold are emitted. FATAL always reaches the emergency sink. */
static unsigned loglevel = 7;
static uint32_t console_log_owner = UINT32_MAX;
static uint64_t last_timestamp;
static bool log_head_fragment;
static const unsigned priorities[] = {7, 6, 4, 3, 0};
static const char *const prefixes[] = {
    "[DEBUG] ", "[INFO] ", "[WARN] ", "[ERROR] ", "[FATAL] ",
};
static const char *const colors[] = {
    "\033[38;2;128;144;160m", "\033[38;2;144;168;160m", "\033[38;2;192;168;112m",
    "\033[38;2;192;128;128m", "\033[38;2;176;128;160m",
};

struct log_format {
    char *data;
    size_t length;
    size_t capacity;
    bool truncated;
};

static void format_char(struct log_format *output, char ch)
{
    if (output->length == output->capacity) output->truncated = true;
    else output->data[output->length++] = ch ? ch : '?';
}

static void format_string(struct log_format *output, const char *text)
{
    for (; text && *text && !output->truncated; ++text) format_char(output, *text);
}

static void format_number(struct log_format *output, uint64_t value,
                          unsigned base, bool upper, unsigned width,
                          bool zero_pad, bool negative)
{
    char buffer[32];
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    unsigned count = 0;
    do {
        buffer[count++] = digits[value % base];
        value /= base;
    } while (value);
    unsigned length = count + (negative ? 1u : 0u);
    if (negative && zero_pad) format_char(output, '-');
    while (width > length) {
        format_char(output, zero_pad ? '0' : ' ');
        --width;
    }
    if (negative && !zero_pad) format_char(output, '-');
    while (count) format_char(output, buffer[--count]);
}

static void format_message(struct log_format *output, const char *fmt, va_list arguments)
{
    for (const char *p = fmt; *p && !output->truncated; ++p) {
        if (*p != '%') {
            format_char(output, *p);
            continue;
        }

        ++p;
        if (!*p) {
            format_char(output, '%');
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
            format_string(output, text ? text : "(null)");
            break;
        }
        case 'c':
            format_char(output, (char)va_arg(arguments, int));
            break;
        case 'd':
        case 'i': {
            int64_t value = long_long_flag ? va_arg(arguments, long long)
                         : long_flag ? va_arg(arguments, long)
                         : size_flag ? va_arg(arguments, ptrdiff_t)
                                     : va_arg(arguments, int);
            uint64_t magnitude = value < 0 ? 0ULL - (uint64_t)value : (uint64_t)value;
            format_number(output, magnitude, 10, false, width, zero_pad, value < 0);
            break;
        }
        case 'u': {
            uint64_t value = long_long_flag ? va_arg(arguments, unsigned long long)
                          : long_flag ? va_arg(arguments, unsigned long)
                          : size_flag ? va_arg(arguments, size_t)
                                      : va_arg(arguments, unsigned int);
            format_number(output, value, 10, false, width, zero_pad, false);
            break;
        }
        case 'x':
        case 'X': {
            uint64_t value = long_long_flag ? va_arg(arguments, unsigned long long)
                          : long_flag ? va_arg(arguments, unsigned long)
                          : size_flag ? va_arg(arguments, size_t)
                                      : va_arg(arguments, unsigned int);
            format_number(output, value, 16, *p == 'X', width, zero_pad, false);
            break;
        }
        case 'p': {
            uintptr_t value = (uintptr_t)va_arg(arguments, void *);
            format_string(output, "0x");
            format_number(output, value, 16, false, width, zero_pad, false);
            break;
        }
        case '%':
            format_char(output, '%');
            break;
        default:
            format_char(output, '%');
            format_char(output, *p);
            break;
        }
    }

    if (output->truncated) {
        static const char marker[] = " ... [truncated]";
        size_t length = sizeof(marker) - 1u;
        __builtin_memcpy(output->data + output->length - length, marker, length);
    }
}

/* Text history is shared with boot-screen replay and the lock-free panic owner.
 * Regular producers append complete encoded records while owning the logger. */
static void log_store_bytes(const char *text, size_t length)
{
    size_t added = log_len + length;
    if (added > CONSOLE_LOG_CAP) {
        size_t keep = (log_start + added - CONSOLE_LOG_CAP) % CONSOLE_LOG_CAP;
        log_head_fragment = log_buffer[(keep + CONSOLE_LOG_CAP - 1u) % CONSOLE_LOG_CAP] != '\n';
    }
    size_t end = (log_start + log_len) % CONSOLE_LOG_CAP;
    size_t first = CONSOLE_LOG_CAP - end;
    if (first > length) first = length;
    __builtin_memcpy(log_buffer + end, text, first);
    __builtin_memcpy(log_buffer, text + first, length - first);
    if (added > CONSOLE_LOG_CAP) {
        log_start = (log_start + added - CONSOLE_LOG_CAP) % CONSOLE_LOG_CAP;
        added = CONSOLE_LOG_CAP;
    }
    log_len = added;
}

void log_store(char ch)
{
    log_store_bytes(&ch, 1);
}

size_t console_log_replay_start(void)
{
    size_t first = 0;
    if (!log_screen) return log_len;
    if (log_len == CONSOLE_LOG_CAP && log_head_fragment) {
        while (first < log_len && log_buffer[(log_start + first) % CONSOLE_LOG_CAP] != '\n') ++first;
        if (first < log_len) ++first;
    }
    return first;
}

static bool token_is(const char *token, size_t length, const char *value)
{
    return length == __builtin_strlen(value) && !__builtin_memcmp(token, value, length);
}

void console_log_configure(const char *cmdline)
{
    loglevel = 7;
    console_log_color_enabled = true;
    log_screen = log_serial = true;
    while (cmdline && *cmdline) {
        while (*cmdline && (unsigned char)*cmdline <= ' ') ++cmdline;
        const char *start = cmdline;
        while ((unsigned char)*cmdline > ' ') ++cmdline;
        size_t length = (size_t)(cmdline - start);
        if (token_is(start, length, "debug")) loglevel = 8;
        else if (token_is(start, length, "quiet")) loglevel = 4;
        else if (length == 10 && !__builtin_memcmp(start, "loglevel=", 9) &&
                 start[9] >= '0' && start[9] <= '8') loglevel = (unsigned)(start[9] - '0');
        else if (token_is(start, length, "logcolor=0")) console_log_color_enabled = false;
        else if (token_is(start, length, "logcolor=1")) console_log_color_enabled = true;
        else if (token_is(start, length, "log=screen")) { log_screen = true; log_serial = false; }
        else if (token_is(start, length, "log=serial")) { log_screen = false; log_serial = true; }
        else if (token_is(start, length, "log=none")) { log_screen = false; log_serial = false; }
        else if (token_is(start, length, "log=serial,screen") || token_is(start, length, "log=screen,serial"))
            log_screen = log_serial = true;
    }
}

static enum console_log_level normalize_level(enum console_log_level level)
{
    return (unsigned)level <= CONSOLE_LOG_FATAL ? level : CONSOLE_LOG_INFO;
}

bool console_log_enabled(enum console_log_level level)
{
    level = normalize_level(level);
    return level == CONSOLE_LOG_FATAL || (priorities[level] < loglevel && (log_screen || log_serial));
}

static bool has_component(const char *text, size_t length)
{
    if (length < 4 || text[0] != '[') return false;
    for (size_t i = 1; i < length && i <= 32; ++i) {
        unsigned char ch = (unsigned char)text[i];
        if (ch == ']') return i > 1 && i + 1 < length && text[i + 1] == ' ';
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.')) return false;
    }
    return false;
}

static void publish_line(enum console_log_level level, uint64_t uptime,
                         const char *text, size_t length, bool owned)
{
    char record[CONSOLE_LOG_RECORD_BYTES];
    struct log_format line = {.data = record, .capacity = sizeof(record) - 1u};
    if (owned) {
        if (uptime < last_timestamp) uptime = last_timestamp;
        last_timestamp = uptime;
    }
    format_char(&line, '[');
    format_number(&line, uptime / 1000000u, 10, false, 5, false, false);
    format_char(&line, '.');
    format_number(&line, uptime % 1000000u, 10, false, 6, true, false);
    format_string(&line, "] ");
    if (console_log_color_enabled) format_string(&line, colors[level]);
    format_string(&line, prefixes[level]);
    if (console_log_color_enabled) format_string(&line, "\033[0m");
    if (!has_component(text, length)) format_string(&line, "[reliefnt] ");
    for (size_t i = 0; i < length; ++i) format_char(&line, text[i]);
    format_char(&line, '\n');
    record[line.length] = 0;
    if (owned) {
        log_store_bytes(record, line.length);
        if (log_screen || level == CONSOLE_LOG_FATAL) console_emit_display(record, line.length);
    }
    if (level == CONSOLE_LOG_FATAL) serial_write_emergency(record, line.length);
    else if (log_serial) {
        if (console_service_logs_only) serial_tx_record(record, line.length, level >= CONSOLE_LOG_WARN);
        else serial_write(record);
    }
}

static void console_publish(enum console_log_level level, const char *text, size_t length)
{
    uint64_t uptime = time_uptime_us();
    uint64_t flags = kernel_irq_save();
    uint32_t cpu = smp_current_cpu();
    bool acquired = false;
    bool owned = __atomic_load_n(&console_log_owner, __ATOMIC_RELAXED) == cpu;
    if (!owned) {
        do {
            if (__atomic_load_n(&console_log_owner, __ATOMIC_RELAXED) == UINT32_MAX) {
                uint32_t expected = UINT32_MAX;
                acquired = __atomic_compare_exchange_n(&console_log_owner, &expected, cpu,
                    false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
            }
            if (acquired || level == CONSOLE_LOG_FATAL) break;
            smp_membarrier_poll();
            __asm__ volatile("pause" ::: "memory");
        } while (!acquired);
        owned = acquired;
    }
    for (size_t from = 0; from < length;) {
        size_t end = from;
        while (end < length && text[end] != '\n') ++end;
        size_t line_length = end - from;
        if (line_length && text[end - 1u] == '\r') --line_length;
        publish_line(level, uptime, text + from, line_length, owned);
        from = end + 1u;
    }
    if (acquired) __atomic_store_n(&console_log_owner, UINT32_MAX, __ATOMIC_RELEASE);
    kernel_irq_restore(flags);
}

void console_vlog(enum console_log_level level, const char *fmt, va_list arguments)
{
    if (!console_log_enabled(level) || !fmt || !*fmt) return;
    char message[CONSOLE_LOG_MESSAGE_BYTES];
    struct log_format output = {.data = message, .capacity = sizeof(message)};
    format_message(&output, fmt, arguments);
    console_publish(normalize_level(level), message, output.length);
}

void console_log(enum console_log_level level, const char *fmt, ...)
{
    if (!console_log_enabled(level)) return;
    va_list arguments;
    va_start(arguments, fmt);
    console_vlog(level, fmt, arguments);
    va_end(arguments);
}
EXPORT_SYMBOL(console_log);

void console_printf(const char *fmt, ...)
{
    va_list arguments;
    va_start(arguments, fmt);
    console_vlog(CONSOLE_LOG_INFO, fmt, arguments);
    va_end(arguments);
}

void console_log_write(const char *text)
{
    if (!text) return;
    enum console_log_level level = CONSOLE_LOG_INFO;
    for (unsigned candidate = CONSOLE_LOG_DEBUG; candidate <= CONSOLE_LOG_FATAL; ++candidate) {
        const char *prefix = prefixes[candidate];
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

void console_write_len(const char *text, size_t length)
{
    if (!text || !console_log_enabled(CONSOLE_LOG_INFO)) return;
    /* User buffers may fault. Stage them before acquiring logger ownership. */
    char data[CONSOLE_LOG_MESSAGE_BYTES];
    while (length) {
        size_t chunk = length < sizeof(data) ? length : sizeof(data);
        __builtin_memcpy(data, text, chunk);
        console_publish(CONSOLE_LOG_INFO, data, chunk);
        text += chunk;
        length -= chunk;
    }
}
