/* SPDX-License-Identifier: Apache-2.0 */
/* Explicitly invoked, bounded diagnostics for an owned ReliefNT guest only. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#define INPUT_CAPACITY 8192u
#define OUTPUT_CAPACITY 4096u
#define PATH_CAPACITY 512u
#define MAX_DURATION_SECONDS 180u

/* The sample loop performs no user-space allocation, fork, or exec. */
static char input_buffer[INPUT_CAPACITY];
static char output_buffer[OUTPUT_CAPACITY];
static char pid_path[PATH_CAPACITY];
static char stat_path[64];

struct output { size_t length; bool overflow; };
struct read_result { const char *stage; int error; size_t length; };
struct number { uint64_t value; bool valid; };

/** @brief Append a NUL-terminated literal to a bounded output record.
 * @param out Owned output cursor. @param text Borrowed NUL-terminated text.
 */
static void append_text(struct output *out, const char *text)
{
    while (*text) {
        if (out->length == sizeof(output_buffer)) { out->overflow = true; return; }
        output_buffer[out->length++] = *text++;
    }
}

/** @brief Append a base-10 unsigned value without an allocation.
 * @param out Owned output cursor. @param value Integer to render.
 */
static void append_u64(struct output *out, uint64_t value)
{
    char digits[20]; size_t count = 0;
    do { digits[count++] = (char)('0' + value % 10u); value /= 10u; } while (value);
    while (count) {
        if (out->length == sizeof(output_buffer)) { out->overflow = true; return; }
        output_buffer[out->length++] = digits[--count];
    }
}

/** @brief Append a named optional counter, marking unavailable values with '?'.
 * @param out Owned output cursor. @param name Borrowed field name.
 * @param value Parsed value and validity from this individual snapshot.
 */
static void append_number(struct output *out, const char *name, struct number value)
{
    append_text(out, " "); append_text(out, name); append_text(out, "=");
    if (value.valid) append_u64(out, value.value); else append_text(out, "?");
}

/** @brief Write one complete record to the already-open diagnostic stream.
 * @param out Owned output cursor. @return Zero on complete write, negative on
 * overflow, closed stream, or I/O failure. Interrupted writes are retried.
 */
static int emit(struct output *out)
{
    if (out->overflow) return -1;
    size_t done = 0;
    while (done < out->length) {
        ssize_t written = write(STDOUT_FILENO, output_buffer + done, out->length - done);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return -1;
        done += (size_t)written;
    }
    return 0;
}

/** @brief Open and fully read one fresh proc or PID-file snapshot.
 * @param path Borrowed path. @return Stage, errno, and byte count; data is in
 * the static input buffer. A too-long read is an error, never a truncated value.
 */
static struct read_result read_snapshot(const char *path)
{
    struct read_result result = {"ok", 0, 0};
    int fd = open(path, O_RDONLY);
    if (fd < 0) return (struct read_result){"open", errno, 0};
    for (;;) {
        if (result.length == sizeof(input_buffer) - 1) {
            char extra;
            ssize_t n = read(fd, &extra, 1);
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) { result.stage = "read"; result.error = errno; }
            else if (n) { result.stage = "truncated"; result.error = EOVERFLOW; }
            break;
        }
        ssize_t n = read(fd, input_buffer + result.length,
                         sizeof(input_buffer) - 1 - result.length);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { result.stage = "read"; result.error = errno; break; }
        if (!n) break;
        result.length += (size_t)n;
    }
    input_buffer[result.length] = '\0';
    if (close(fd) && !result.error) { result.stage = "close"; result.error = errno; }
    return result;
}

/** @brief Parse an unsigned decimal value and detect arithmetic overflow.
 * @param text Borrowed start of token. @param end Receives the first unparsed
 * character when non-NULL. @return Parsed value or invalid on empty/overflow.
 */
static struct number decimal(const char *text, const char **end)
{
    struct number result = {0, false};
    const char *p = text;
    if (*p < '0' || *p > '9') return result;
    result.valid = true;
    while (*p >= '0' && *p <= '9') {
        unsigned digit = (unsigned)(*p++ - '0');
        if (result.value > (UINT64_MAX - digit) / 10u) result.valid = false;
        if (result.valid) result.value = result.value * 10u + digit;
    }
    if (end) *end = p;
    return result;
}

/** @brief Find one exact named numeric field in the current text snapshot.
 * @param key Borrowed field name including ':' for meminfo, excluding whitespace.
 * @return Parsed value, or invalid for an absent/malformed field.
 */
static struct number named_number(const char *key)
{
    size_t length = strlen(key);
    const char *line = input_buffer;
    while (*line) {
        const char *next = strchr(line, '\n');
        if (!next) next = line + strlen(line);
        if ((size_t)(next - line) > length && !strncmp(line, key, length) &&
            (line[length] == ' ' || line[length] == '\t')) {
            const char *p = line + length;
            while (p < next && (*p == ' ' || *p == '\t')) ++p;
            const char *end = NULL;
            struct number result = decimal(p, &end);
            if (!result.valid || end > next ||
                (end < next && *end != ' ' && *end != '\t' && *end != '\r'))
                return (struct number){0, false};
            return result;
        }
        line = *next ? next + 1 : next;
    }
    return (struct number){0, false};
}

/** @brief Obtain monotonic milliseconds used for sample and duration bounds.
 * @param value Required output. @return Zero on success, negative on clock or
 * representability failure; no wall-clock assumptions are made.
 */
static int monotonic_ms(uint64_t *value)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) || now.tv_sec < 0 ||
        (uint64_t)now.tv_sec > (UINT64_MAX - 999u) / 1000u ||
        now.tv_nsec < 0 || now.tv_nsec >= 1000000000L) return -1;
    *value = (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
    return 0;
}

/** @brief Append the outcome of one open/read/close transaction.
 * @param out Owned record. @param prefix Borrowed short field prefix.
 * @param result Exact stage and errno, with zero indicating success.
 */
static void append_read_result(struct output *out, const char *prefix, struct read_result result)
{
    append_text(out, " "); append_text(out, prefix); append_text(out, "_stage=");
    append_text(out, result.stage); append_text(out, " "); append_text(out, prefix);
    append_text(out, "_errno="); append_u64(out, (uint64_t)result.error);
}

/** @brief Emit one fresh meminfo or vmstat snapshot and its parse validity.
 * @param elapsed Milliseconds since readiness. @param path Standard proc path.
 * @param label Short output identity. @param names Null-terminated list of keys.
 * @return Zero when the log write succeeds, negative on output failure; each
 * optional value records parsing separately from open/read/close status.
 */
static int sample_counters(uint64_t elapsed, const char *path, const char *label,
                           const char *const *names)
{
    struct read_result result = read_snapshot(path);
    struct output out = {0};
    append_text(&out, "PRESSURE_OBSERVER_SAMPLE t_ms="); append_u64(&out, elapsed);
    append_text(&out, " kind="); append_text(&out, label);
    append_read_result(&out, "io", result);
    for (size_t i = 0; names[i]; ++i) {
        struct number number = result.error ? (struct number){0, false} : named_number(names[i]);
        append_number(&out, names[i], number);
    }
    append_text(&out, "\n"); return emit(&out);
}

/** @brief Parse an absolute PID handoff once the controller has published it.
 * @param pid Receives a positive PID only on success. @return Read/parse stage
 * and errno; whitespace around one decimal PID is accepted, other text rejected.
 */
static struct read_result read_pid(unsigned *pid)
{
    struct read_result result = read_snapshot(pid_path);
    if (result.error) return result;
    const char *p = input_buffer, *end = NULL;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    struct number value = decimal(p, &end);
    if (value.valid) {
        while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') ++end;
    }
    if (!value.valid || !end || *end || !value.value || value.value > INT_MAX)
        return (struct read_result){"parse", EINVAL, result.length};
    *pid = (unsigned)value.value;
    return result;
}

/** @brief Build a proc stat path for a previously validated positive PID.
 * @param pid Numeric PID. The fixed buffer is large enough for all int PIDs.
 */
static void make_stat_path(unsigned pid)
{
    char digits[10]; size_t count = 0, pos = 0;
    const char *prefix = "/proc/";
    while (*prefix) stat_path[pos++] = *prefix++;
    do { digits[count++] = (char)('0' + pid % 10u); pid /= 10u; } while (pid);
    while (count) stat_path[pos++] = digits[--count];
    const char *suffix = "/stat";
    while (*suffix) stat_path[pos++] = *suffix++;
    stat_path[pos] = '\0';
}

/** @brief Parse standard stat state, start time, virtual bytes and RSS pages.
 * @param state Required state output. @param start Required field 22 output.
 * @param virtual_bytes Required field 23 output. @param rss Required field 24.
 * @return True only for complete well-formed required fields. Field 2 may
 * contain spaces or parentheses, so parsing begins after its last ')'.
 */
static bool parse_stat(char *state, uint64_t *start, uint64_t *virtual_bytes, uint64_t *rss)
{
    const char *p = strrchr(input_buffer, ')');
    if (!p || p[1] != ' ' || !p[2] || p[2] == ' ' || p[3] != ' ') return false;
    *state = p[2]; p += 4;
    for (unsigned field = 4; field <= 24; ++field) {
        while (*p == ' ') ++p;
        if (!*p || *p == '\n') return false;
        const char *token = p;
        while (*p && *p != ' ' && *p != '\n') ++p;
        if (field >= 22) {
            const char *end = NULL;
            struct number number = decimal(token, &end);
            if (!number.valid || end != p) return false;
            if (field == 22) *start = number.value;
            else if (field == 23) *virtual_bytes = number.value;
            else *rss = number.value;
        }
    }
    return true;
}

/** @brief Log PID liveness and periodically sample proc stat without forking.
 * @param elapsed Milliseconds since readiness. @param pid Numeric handoff PID.
 * @param read_stat True at the one-second stat sampling interval.
 * @param first_start In/out first observed start time for PID-reuse indication.
 * @param have_start In/out flag for the first start time. @return Zero on log
 * write success. kill(0) is liveness only and does not pin process identity.
 */
static int sample_pid(uint64_t elapsed, unsigned pid, bool read_stat,
                      uint64_t *first_start, bool *have_start)
{
    errno = 0;
    int alive = kill((pid_t)pid, 0), alive_error = alive < 0 ? errno : 0;
    struct output out = {0};
    append_text(&out, "PRESSURE_OBSERVER_PID t_ms="); append_u64(&out, elapsed);
    append_text(&out, " pid="); append_u64(&out, pid);
    append_text(&out, " kill_rc="); append_text(&out, alive ? "-1" : "0");
    append_text(&out, " kill_errno="); append_u64(&out, (uint64_t)alive_error);
    if (read_stat) {
        make_stat_path(pid);
        struct read_result result = read_snapshot(stat_path);
        char state = '?'; uint64_t start = 0, virtual_bytes = 0, rss = 0;
        if (!result.error && !parse_stat(&state, &start, &virtual_bytes, &rss))
            result = (struct read_result){"parse", EINVAL, result.length};
        append_read_result(&out, "stat", result);
        if (!result.error) {
            if (!*have_start) { *first_start = start; *have_start = true; }
            append_text(&out, " state=");
            char state_text[2] = {state, '\0'}; append_text(&out, state_text);
            append_text(&out, " start_ticks="); append_u64(&out, start);
            append_text(&out, " first_start_ticks="); append_u64(&out, *first_start);
            append_text(&out, " pid_reused="); append_u64(&out, start != *first_start);
            append_text(&out, " virtual_bytes="); append_u64(&out, virtual_bytes);
            append_text(&out, " rss_pages="); append_u64(&out, rss);
        }
    } else append_text(&out, " stat_stage=not_sampled stat_errno=0");
    append_text(&out, "\n"); return emit(&out);
}

/** @brief Parse a bounded positive CLI decimal argument.
 * @param text Borrowed argument. @param minimum Inclusive lower bound.
 * @param maximum Inclusive upper bound. @param out Required result.
 * @return True for a complete numeric value within the requested interval.
 */
static bool argument(const char *text, unsigned minimum, unsigned maximum, unsigned *out)
{
    const char *end = NULL; struct number value = decimal(text, &end);
    if (!value.valid || !end || *end || value.value < minimum || value.value > maximum) return false;
    *out = (unsigned)value.value; return true;
}

/** @brief Publish a fresh controller handshake after the ready record is written.
 * @param path Absolute caller-provided ready-file path. @return Zero only after
 * exclusive no-follow creation and close; errors are logged with stage/errno.
 */
static int publish_ready_file(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    const char *stage = "open";
    int error = fd < 0 ? errno : 0;
    if (fd >= 0 && close(fd)) { stage = "close"; error = errno; }
    if (!error) return 0;
    struct output out = {0};
    append_text(&out, "PRESSURE_OBSERVER_READY_FILE_ERROR stage="); append_text(&out, stage);
    append_text(&out, " errno="); append_u64(&out, (uint64_t)error); append_text(&out, "\n");
    (void)emit(&out); return -1;
}

/** @brief Run bounded observation only on an explicitly named ReliefNT guest.
 * @param argc Argument count. @param argv Program name, --guest-observe,
 * absolute PID file, duration seconds (1..180), sample interval ms (100..5000),
 * and optionally an absolute fresh ready-file path for controller handoff.
 * @return Zero after bounded observation, 1 on clock/output/sleep failure, or
 * 2 on guard/argument failure. A completed observation is not a test pass.
 */
int main(int argc, char **argv)
{
    struct utsname system;
    unsigned duration, interval;
    if ((argc != 5 && argc != 6) || strcmp(argv[1], "--guest-observe") || uname(&system) ||
        strcmp(system.sysname, "ReliefNT") || argv[2][0] != '/' ||
        strlen(argv[2]) >= sizeof(pid_path) ||
        !argument(argv[3], 1, MAX_DURATION_SECONDS, &duration) ||
        !argument(argv[4], 100, 5000, &interval) ||
        (argc == 6 && (argv[5][0] != '/' || strlen(argv[5]) >= PATH_CAPACITY))) {
        static const char usage[] = "usage on ReliefNT guest only: --guest-observe /absolute/pidfile SECONDS(1..180) INTERVAL_MS(100..5000) [/absolute/readyfile]\n";
        (void)write(STDERR_FILENO, usage, sizeof(usage) - 1); return 2;
    }
    memcpy(pid_path, argv[2], strlen(argv[2]) + 1);
    memset(input_buffer, 0, sizeof(input_buffer));
    memset(output_buffer, 0, sizeof(output_buffer));
    volatile unsigned char stack_warm[8192];
    for (size_t i = 0; i < sizeof(stack_warm); i += 4096) stack_warm[i] = 0;
    int lock_input = mlock(input_buffer, sizeof(input_buffer)), input_error = lock_input ? errno : 0;
    int lock_output = mlock(output_buffer, sizeof(output_buffer)), output_error = lock_output ? errno : 0;
    int lock_stack = mlock((const void *)stack_warm, sizeof(stack_warm)), stack_error = lock_stack ? errno : 0;
    int lock_pid = mlock(pid_path, sizeof(pid_path)), pid_error = lock_pid ? errno : 0;
    int lock_stat = mlock(stat_path, sizeof(stat_path)), stat_error = lock_stat ? errno : 0;
    static const char *const mem_names[] = {"MemTotal:", "MemFree:", "MemAvailable:", "SwapTotal:", "SwapFree:", NULL};
    static const char *const vm_names[] = {"pgfault", "pgmajfault", "pswpin", "pswpout", "pgscan_direct", "pgscan_kswapd", "pgsteal_direct", "pgsteal_kswapd", "oom_kill", NULL};
    uint64_t now, start;
    if (monotonic_ms(&now) || sample_counters(0, "/proc/meminfo", "prewarm_meminfo", mem_names) ||
        sample_counters(0, "/proc/vmstat", "prewarm_vmstat", vm_names)) return 1;
    unsigned prewarm_pid = 0;
    struct read_result prewarm_handoff = read_pid(&prewarm_pid);
    make_stat_path((unsigned)getpid());
    struct read_result prewarm_stat = read_snapshot(stat_path);
    char prewarm_state = '?'; uint64_t prewarm_start = 0, prewarm_size = 0, prewarm_rss = 0;
    if (!prewarm_stat.error &&
        !parse_stat(&prewarm_state, &prewarm_start, &prewarm_size, &prewarm_rss))
        prewarm_stat = (struct read_result){"parse", EINVAL, prewarm_stat.length};
    errno = 0;
    int prewarm_alive = kill(getpid(), 0), prewarm_alive_error = prewarm_alive ? errno : 0;
    struct output ready = {0};
    append_text(&ready, "PRESSURE_OBSERVER_READY pid="); append_u64(&ready, (uint64_t)getpid());
    append_text(&ready, " duration_s="); append_u64(&ready, duration);
    append_text(&ready, " interval_ms="); append_u64(&ready, interval);
    append_text(&ready, " mlock_input_errno="); append_u64(&ready, (uint64_t)input_error);
    append_text(&ready, " mlock_output_errno="); append_u64(&ready, (uint64_t)output_error);
    append_text(&ready, " mlock_stack_errno="); append_u64(&ready, (uint64_t)stack_error);
    append_text(&ready, " mlock_pid_errno="); append_u64(&ready, (uint64_t)pid_error);
    append_text(&ready, " mlock_stat_errno="); append_u64(&ready, (uint64_t)stat_error);
    append_read_result(&ready, "prewarm_pidfile", prewarm_handoff);
    append_read_result(&ready, "prewarm_stat", prewarm_stat);
    append_text(&ready, " prewarm_kill_errno="); append_u64(&ready, (uint64_t)prewarm_alive_error);
    append_text(&ready, "\n");
    if (emit(&ready) || (argc == 6 && publish_ready_file(argv[5])) || monotonic_ms(&start)) return 1;
    unsigned pid = 0; uint64_t first_start = 0, last_stat = 0; bool have_start = false, stat_once = false;
    for (;;) {
        if (monotonic_ms(&now) || now < start) return 1;
        uint64_t elapsed = now - start;
        if (elapsed >= (uint64_t)duration * 1000u) break;
        if (sample_counters(elapsed, "/proc/meminfo", "meminfo", mem_names) ||
            sample_counters(elapsed, "/proc/vmstat", "vmstat", vm_names)) return 1;
        if (!pid) {
            struct read_result handoff = read_pid(&pid);
            struct output out = {0};
            append_text(&out, "PRESSURE_OBSERVER_HANDOFF t_ms="); append_u64(&out, elapsed);
            append_read_result(&out, "pidfile", handoff);
            append_text(&out, " pid="); append_u64(&out, pid); append_text(&out, "\n");
            if (emit(&out)) return 1;
        }
        if (pid) {
            bool sample_stat = !stat_once || elapsed - last_stat >= 1000u;
            if (sample_pid(elapsed, pid, sample_stat, &first_start, &have_start)) return 1;
            if (sample_stat) { stat_once = true; last_stat = elapsed; }
        }
        struct timespec remaining = {.tv_sec = interval / 1000u,
                                     .tv_nsec = (long)(interval % 1000u) * 1000000L};
        while (nanosleep(&remaining, &remaining)) {
            if (errno != EINTR) return 1;
        }
    }
    struct output done = {0};
    append_text(&done, "PRESSURE_OBSERVER_DONE elapsed_ms="); append_u64(&done, now - start);
    append_text(&done, " pid="); append_u64(&done, pid); append_text(&done, "\n");
    return emit(&done) ? 1 : 0;
}
