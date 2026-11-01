/* SPDX-License-Identifier: Apache-2.0 */
/* Freestanding native x86-64 probe, suitable for rdinit without a libc/rootfs.
 * Build with -ffreestanding -fno-builtin -fno-stack-protector -nostdlib -static
 * -no-pie -Wl,-e,_start -Wl,-Ttext-segment=0x400000 -Iinclude/uapi.
 * PID 1 remains parked after its result; other invocations exit normally. */
#include <stddef.h>
#include <stdint.h>
#include <linux/syscall.h>

static char contents[65536];
static long output = 1;
static unsigned char thread_stack[32768] __attribute__((aligned(16)));
static int thread_complete;

/** @brief Enter the native Linux ABI with six scalar syscall arguments.
 * @param nr Number. @param a First. @param b Second. @param c Third.
 * @param d Fourth. @param e Fifth. @param f Sixth. @return Raw result/errno.
 */
static long call(long nr, long a, long b, long c, long d, long e, long f)
{
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    long result;
    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "a"(nr), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory", "cc");
    return result;
}

/** @brief Count a probe-owned NUL-terminated string. @param s Text. @return Bytes. */
static size_t length(const char *s)
{
    size_t n = 0;
    while (s[n])
        ++n;
    return n;
}

/** @brief Write a diagnostic with short-write handling. @param s Text. */
static void print(const char *s)
{
    size_t n = length(s);
    while (n) {
        long done = call(__NR_write, output, (long)s, (long)n, 0, 0, 0);
        if (done <= 0)
            return;
        s += done;
        n -= (size_t)done;
    }
}

/** @brief Exit a normal invocation or park the owned PID 1 after diagnostics.
 * @param status Zero on success; nonzero on failure. Does not return.
 */
__attribute__((noreturn)) static void finish(int status)
{
    if (call(__NR_getpid, 0, 0, 0, 0, 0, 0) != 1)
        call(__NR_exit_group, status, 0, 0, 0, 0, 0);
    for (;;)
        call(__NR_pause, 0, 0, 0, 0, 0, 0);
}

/** @brief Require a probe invariant. @param valid Boolean. @param what Description. */
static void require(int valid, const char *what)
{
    if (valid)
        return;
    print("FAIL raw proc ABI: ");
    print(what);
    print("\n");
    finish(1);
}

/** @brief Find a bounded literal in the current file snapshot.
 * @param text NUL-terminated buffer. @param needle Literal. @return Membership.
 */
static int contains(const char *text, const char *needle)
{
    for (; *text; ++text) {
        size_t i = 0;
        while (needle[i] && text[i] == needle[i])
            ++i;
        if (!needle[i])
            return 1;
    }
    return 0;
}

/** @brief Decode a proc hexadecimal digit in either kernel's letter case.
 * @param c Input byte. @return Digit or -1 for a separator.
 */
static int hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

/** @brief Find this probe's bound UDP endpoint and verify pending receive data.
 * Linux reports socket buffer memory; ReliefNT reports queued payload bytes.
 * @param text Complete UDP table. @return A matching row has a nonempty queue.
 */
static int udp_has_data(const char *text)
{
    const char *endpoint = "0100007f:d6d8";
    for (; *text; ++text) {
        size_t i = 0;
        while (endpoint[i] && text[i] &&
               (endpoint[i] == ':' ? text[i] == ':' :
                hex_digit(endpoint[i]) == hex_digit(text[i])))
            ++i;
        if (endpoint[i])
            continue;
        const char *p = text + i;
        for (unsigned field = 0; field < 3; ++field) {
            while (*p == ' ' || *p == '\t')
                ++p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\n')
                ++p;
        }
        /* The third field after local_address is tx_queue:rx_queue. */
        while (p > text && p[-1] != ':')
            --p;
        uint64_t queued = 0;
        while (hex_digit(*p) >= 0)
            queued = queued * 16 + (unsigned)hex_digit(*p++);
        return queued >= 5;
    }
    return 0;
}

/** @brief Open, read in seven-byte pieces, rewind and close an actual proc file.
 * @param path Intrinsic path. @return Bytes in contents, including binary data.
 */
static size_t read_file(const char *path)
{
    long fd = call(__NR_open, (long)path, 0, 0, 0, 0, 0);
    require(fd >= 0, path);
    size_t used = 0;
    for (;;) {
        require(used + 8 < sizeof(contents), "snapshot capacity");
        long n = call(__NR_read, fd, (long)(contents + used), 7, 0, 0, 0);
        require(n >= 0 && n <= 7, path);
        if (!n)
            break;
        used += (size_t)n;
    }
    contents[used] = 0;
    require(call(__NR_lseek, fd, 0, 0, 0, 0, 0) == 0, "seek to start");
    char first[7];
    long n = call(__NR_read, fd, (long)first, sizeof(first), 0, 0, 0);
    require(n >= 0 && n <= 7, "read after rewind");
    for (long i = 0; i < n; ++i)
        require(first[i] == contents[i], "stable snapshot");
    require(call(__NR_close, fd, 0, 0, 0, 0, 0) == 0, "close snapshot");
    return used;
}

/** @brief Construct a numeric descriptor leaf without a libc formatter.
 * @param path Caller buffer, at least 64 bytes. @param prefix Proc parent.
 * @param fd Nonnegative descriptor.
 */
static void descriptor_path(char *path, const char *prefix, unsigned fd)
{
    size_t n = 0;
    while (*prefix)
        path[n++] = *prefix++;
    char digits[10];
    unsigned count = 0;
    do {
        digits[count++] = '0' + fd % 10;
        fd /= 10;
    } while (fd);
    while (count)
        path[n++] = digits[--count];
    path[n] = 0;
}

/** @brief Validate thread-self against gettid in a separately cloned thread.
 * Publishes completion to the parent, then exits only this thread.
 */
__attribute__((used, noreturn)) static void child_probe(void)
{
    char expected[64], actual[64] = {0};
    descriptor_path(expected, "", (unsigned)call(__NR_getpid, 0, 0, 0, 0, 0, 0));
    size_t n = length(expected);
    descriptor_path(expected + n, "/task/", (unsigned)call(__NR_gettid, 0, 0, 0, 0, 0, 0));
    long bytes =
        call(__NR_readlink, (long)"/proc/thread-self", (long)actual, sizeof(actual), 0, 0, 0);
    require(bytes == (long)length(expected), "thread-self link length");
    for (long i = 0; i < bytes; ++i)
        require(actual[i] == expected[i], "thread-self TID");
    read_file("/proc/thread-self/status");
    descriptor_path(expected, "Pid:\t", (unsigned)call(__NR_gettid, 0, 0, 0, 0, 0, 0));
    require(contains(contents, expected), "thread status PID");
    __atomic_store_n(&thread_complete, 1, __ATOMIC_RELEASE);
    call(__NR_exit, 0, 0, 0, 0, 0, 0);
    for (;;) {
    }
}

/** @brief Clone onto a caller-provided stack and enter the child trampoline.
 * @param stack Required aligned stack top. @return TID or negative errno in parent.
 */
__attribute__((naked)) static long create_thread(void *stack __attribute__((unused)))
{
    __asm__ volatile(
        "mov %rdi,%rsi\n\tmov $0x50f00,%edi\n\txor %edx,%edx\n\t"
        "xor %r10d,%r10d\n\txor %r8d,%r8d\n\tmov $56,%eax\n\tsyscall\n\t"
        "test %rax,%rax\n\tjnz 1f\n\txor %ebp,%ebp\n\tcall child_probe\n\tud2\n1:\tret");
}

/** @brief Verify read-only proc nodes and real auxv/descriptor/socket ownership.
 * Invoked from the ELF entry point with no libc or dynamic-loader dependency.
 * Does not return; PID 1 emits a result and remains parked.
 */
__attribute__((used, noreturn)) static void probe_main(void)
{
    if (call(__NR_getpid, 0, 0, 0, 0, 0, 0) == 1) {
        long serial = call(__NR_open, (long)"/dev/ttyS0", 1, 0, 0, 0, 0);
        if (serial >= 0)
            output = serial;
    }
    print("START raw proc ABI\n");
    const char *paths[] = {"/proc/loadavg",
                           "/proc/stat",
                           "/proc/meminfo",
                           "/proc/vmstat",
                           "/proc/swaps",
                           "/proc/partitions",
                           "/proc/diskstats",
                           "/proc/interrupts",
                           "/proc/net/dev",
                           "/proc/net/route",
                           "/proc/net/arp",
                           "/proc/net/tcp",
                           "/proc/net/udp",
                           "/proc/net/unix",
                           "/proc/sys/fs/nr_open",
                           "/proc/sys/fs/file-nr",
                           "/proc/sys/fs/file-max",
                           "/proc/sys/fs/pipe-max-size",
                           "/proc/sys/fs/pipe-user-pages-soft",
                           "/proc/sys/fs/pipe-user-pages-hard",
                           "/proc/sys/kernel/pid_max",
                           "/proc/self/status",
                           "/proc/self/stat",
                           "/proc/self/statm",
                           "/proc/self/maps",
                           "/proc/self/smaps",
                           "/proc/self/smaps_rollup",
                           "/proc/self/limits",
                           "/proc/self/io"};
    for (size_t i = 0; i < sizeof(paths) / sizeof(*paths); ++i)
        read_file(paths[i]);
    print("PASS raw proc nodes, short reads, seek and EOF\n");
    read_file("/proc/self/maps");
    require(contains(contents, "r-xp"), "executable map");
    read_file("/proc/self/smaps_rollup");
    require(contains(contents, "Pss:"), "PSS rollup");
    read_file("/proc/thread-self/status");
    require(contains(contents, "Threads:"), "thread-self status");
    long tasks = call(__NR_open, (long)"/proc/self/task", 0, 0, 0, 0, 0);
    require(tasks >= 0, "task directory");
    require(call(__NR_getdents64, tasks, (long)contents, sizeof(contents), 0, 0, 0) > 0,
            "task entries");
    call(__NR_close, tasks, 0, 0, 0, 0, 0);
    size_t bytes = read_file("/proc/self/auxv");
    require(bytes && !(bytes % 16), "binary auxv size");
    int pages = 0, end = 0;
    for (size_t at = 0; at + 16 <= bytes; at += 16) {
        const uint64_t *pair = (const uint64_t *)(const void *)(contents + at);
        if (pair[0] == 6) {
            require(pair[1] == 4096, "AT_PAGESZ");
            pages = 1;
        }
        if (!pair[0]) {
            end = 1;
            break;
        }
    }
    require(pages && end, "auxv values and terminator");
    long fd = call(__NR_open, (long)"/dev/null", 2, 0, 0, 0, 0);
    require(fd >= 0, "null descriptor");
    long alias = call(__NR_dup, fd, 0, 0, 0, 0, 0);
    require(alias >= 0, "dup");
    call(__NR_close, fd, 0, 0, 0, 0, 0);
    char path[64];
    descriptor_path(path, "/proc/self/fdinfo/", (unsigned)alias);
    read_file(path);
    require(contains(contents, "pos:") && contains(contents, "flags:"), "fdinfo alias");
    call(__NR_close, alias, 0, 0, 0, 0, 0);
    long socket = call(__NR_socket, 2, 2 | 0x800, 0, 0, 0, 0);
    require(socket >= 0, "UDP socket");
    struct {
        uint16_t family, port;
        uint32_t ip;
        uint8_t zero[8];
    } address = {
        .family = 2, .port = __builtin_bswap16(55000), .ip = __builtin_bswap32(0x7f000001)};
    require(call(__NR_bind, socket, (long)&address, sizeof(address), 0, 0, 0) == 0, "UDP bind");
    require(call(__NR_sendto, socket, (long)"hello", 5, 0, (long)&address, sizeof(address)) == 5,
            "UDP local send");
    read_file("/proc/net/udp");
    require(udp_has_data(contents), "UDP live queue");
    descriptor_path(path, "/proc/self/fdinfo/", (unsigned)socket);
    read_file(path);
    require(contains(contents, "ino:"), "socket inode");
    call(__NR_close, socket, 0, 0, 0, 0, 0);
    require(create_thread(thread_stack + sizeof(thread_stack)) > 0, "clone thread");
    while (!__atomic_load_n(&thread_complete, __ATOMIC_ACQUIRE)) {
        const uint64_t delay[] = {0, 10000000};
        call(__NR_nanosleep, (long)delay, 0, 0, 0, 0, 0);
    }
    print("PASS raw proc ABI: mappings, auxv, task directory, cloned thread-self, fdinfo and UDP "
          "queue\n");
    finish(0);
}

/** @brief Align the native entry stack and enter the freestanding probe. */
__attribute__((naked, noreturn)) void _start(void)
{
    __asm__ volatile("xor %rbp,%rbp\n\tand $-16,%rsp\n\tcall probe_main\n\tud2");
}
