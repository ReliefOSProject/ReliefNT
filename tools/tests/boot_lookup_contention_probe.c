/* Cold pathname pages racing an independent SATA read transaction.
 *
 * Build a static executable and copy it to a disk-backed guest filesystem:
 *   x86_64-linux-musl-gcc -O2 -Wall -Wextra -static -no-pie \
 *       tools/tests/boot_lookup_contention_probe.c -o boot-lookup-contention-probe
 * Run against a read-only mounted installed ESP (or its instance directory):
 *   /path/boot-lookup-contention-probe /mnt/esp /dev/sda 100 45
 *
 * The pathname bytes live in initialized, writable ELF pages. No userspace
 * load touches a pathname between MADV_DONTNEED and its syscall. This targets
 * checked kernel copyin, rather than an ordinary ring-3 load fault. A child
 * continuously reads the supplied disk/file to create foreign I/O ownership.
 * The probe never writes the installed filesystem or its read source.
 * A guest SIGALRM deadline kills the owned reader and exits 124. The VM harness
 * must also enforce a host deadline: no guest signal can bound a kernel hang.
 */
#define _GNU_SOURCE 1
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { PAGE = 4096, PATHS = 7, OPERATIONS = 5, READ_BYTES = 65536 };
static char cold_paths[PATHS][PAGE]
    __attribute__((aligned(PAGE), section(".data.cold_paths"), used)) = {
    ".", "grub", "EFI", "reliefos", "reliefos/config",
    "grub/grub.cfg", "reliefos/config/bootloaders.conf",
};
extern const unsigned char __ehdr_start[];
static volatile sig_atomic_t reader_pid;

struct shared_state { unsigned stop, ready; uint64_t reads, bytes; int error; };

static void timeout_handler(int signal_number)
{
    (void)signal_number;
    static const char message[] = "FAIL boot lookup probe: deadline exceeded\n";
    ssize_t ignored = write(STDERR_FILENO, message, sizeof(message) - 1);
    (void)ignored;
    if (reader_pid > 0) (void)kill(reader_pid, SIGKILL);
    _exit(124);
}

/* Return zero or errno, without reading pathname bytes in userspace. */
static int lookup(unsigned operation, const char *path)
{
    struct stat status;
    char link[512];
    long result;
    do {
        switch (operation) {
        case 0:
            result = syscall(SYS_newfstatat, AT_FDCWD, path, &status, 0);
            break;
        case 1:
            result = syscall(SYS_newfstatat, AT_FDCWD, path, &status, AT_SYMLINK_NOFOLLOW);
            break;
        case 2:
            result = syscall(SYS_openat, AT_FDCWD, path,
                O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC, 0);
            if (result >= 0) { (void)close((int)result); return 0; }
            break;
        case 3:
            result = syscall(SYS_access, path, R_OK);
            break;
        default:
            result = syscall(SYS_readlinkat, AT_FDCWD, path, link, sizeof(link));
            break;
        }
    } while (result < 0 && errno == EINTR);
    return result < 0 ? errno : 0;
}

static void reader(int fd, struct shared_state *state)
{
#ifdef PR_SET_PDEATHSIG
    (void)prctl(PR_SET_PDEATHSIG, SIGKILL);
#endif
    unsigned char buffer[READ_BYTES];
    memset(buffer, 0, sizeof(buffer));
    __atomic_store_n(&state->ready, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&state->stop, __ATOMIC_ACQUIRE)) {
        ssize_t count = read(fd, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { __atomic_store_n(&state->error, errno, __ATOMIC_RELEASE); break; }
        if (!count) {
            if (lseek(fd, 0, SEEK_SET) < 0) {
                __atomic_store_n(&state->error, errno, __ATOMIC_RELEASE); break;
            }
            continue;
        }
        __atomic_add_fetch(&state->bytes, (uint64_t)count, __ATOMIC_RELAXED);
        __atomic_add_fetch(&state->reads, 1, __ATOMIC_RELEASE);
    }
    (void)close(fd);
    _exit(__atomic_load_n(&state->error, __ATOMIC_ACQUIRE) ? 1 : 0);
}

/* Some kernels reject DONTNEED on executable VMAs. Retain the same initialized
 * file bytes through a private mapping in that case, and identify that mode in
 * the result. The normal ReliefNT path uses the original ELF data VMA. */
static char (*map_path_pages(int executable))[PAGE]
{
    const Elf64_Ehdr *header = (const Elf64_Ehdr *)__ehdr_start;
    if (memcmp(header->e_ident, ELFMAG, SELFMAG) || header->e_ident[EI_CLASS] != ELFCLASS64 ||
        header->e_phentsize != sizeof(Elf64_Phdr)) { errno = ENOEXEC; return MAP_FAILED; }
    uintptr_t base = header->e_type == ET_DYN ? (uintptr_t)__ehdr_start : 0;
    const Elf64_Phdr *segments = (const Elf64_Phdr *)(__ehdr_start + header->e_phoff);
    for (unsigned i = 0; i < header->e_phnum; ++i) {
        const Elf64_Phdr *segment = &segments[i];
        uintptr_t start = base + segment->p_vaddr;
        uintptr_t address = (uintptr_t)cold_paths;
        if (segment->p_type != PT_LOAD || address < start ||
            address - start > segment->p_filesz ||
            sizeof(cold_paths) > segment->p_filesz - (address - start)) continue;
        uint64_t offset = segment->p_offset + address - start;
        if (offset % PAGE) { errno = EINVAL; return MAP_FAILED; }
        return mmap(NULL, sizeof(cold_paths), PROT_READ | PROT_WRITE,
                    MAP_PRIVATE, executable, (off_t)offset);
    }
    errno = ENOEXEC;
    return MAP_FAILED;
}

static unsigned argument(const char *text, unsigned maximum)
{
    char *end;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno || !*text || *end || !value || value > maximum) return 0;
    return (unsigned)value;
}

int main(int argc, char **argv)
{
    if (argc < 3 || argc > 5) {
        fprintf(stderr, "Usage: %s BOOT_DIRECTORY READ_SOURCE [ROUNDS=100] [TIMEOUT=45]\n", argv[0]);
        return 2;
    }
    unsigned rounds = argc > 3 ? argument(argv[3], 100000) : 100;
    unsigned timeout = argc > 4 ? argument(argv[4], 3600) : 45;
    if (!rounds || !timeout) { fputs("Invalid rounds or timeout\n", stderr); return 2; }
    int executable = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    if (executable < 0) executable = open(argv[0], O_RDONLY | O_CLOEXEC);
    int input = open(argv[2], O_RDONLY | O_CLOEXEC);
    if (input < 0 || chdir(argv[1])) { perror("opening read source/boot directory"); return 2; }
    struct sigaction action = {.sa_handler = timeout_handler};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGALRM, &action, NULL)) { perror("sigaction"); return 2; }
    alarm(timeout);
    struct shared_state *state = mmap(NULL, sizeof(*state), PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (state == MAP_FAILED) { perror("mmap shared counters"); return 2; }
    memset(state, 0, sizeof(*state));
    char (*paths)[PAGE] = cold_paths;
    const char *mode = "elf-data";
    if (madvise(paths, sizeof(cold_paths), MADV_DONTNEED)) {
        if (executable < 0 || (paths = map_path_pages(executable)) == MAP_FAILED ||
            madvise(paths, sizeof(cold_paths), MADV_DONTNEED)) {
            perror("discarding file-backed pathname pages"); return 2;
        }
        mode = "private-file-map";
    }
    if (executable >= 0) close(executable);
    int expected[PATHS][OPERATIONS];
    for (unsigned path = 0; path < PATHS; ++path) {
        for (unsigned operation = 0; operation < OPERATIONS; ++operation) {
            expected[path][operation] = lookup(operation, paths[path]);
            int error = expected[path][operation];
            if (error && error != ENOENT && !(operation == 4 && error == EINVAL)) {
                fprintf(stderr, "FAIL baseline path=%u operation=%u errno=%d\n", path, operation, error);
                return 1;
            }
        }
    }
    if (expected[0][0]) { fputs("FAIL boot directory is unavailable\n", stderr); return 1; }
    reader_pid = fork();
    if (reader_pid < 0) { perror("fork"); return 2; }
    if (!reader_pid) reader(input, state);
    close(input);
    struct timespec pause = {0, 1000000};
    while (!__atomic_load_n(&state->ready, __ATOMIC_ACQUIRE) ||
           !__atomic_load_n(&state->reads, __ATOMIC_ACQUIRE)) {
        int error = __atomic_load_n(&state->error, __ATOMIC_ACQUIRE);
        if (error) { fprintf(stderr, "FAIL reader errno=%d\n", error); kill(reader_pid, SIGKILL); return 1; }
        nanosleep(&pause, NULL);
    }
    uint64_t completed = 0;
    int failed = 0;
    for (unsigned round = 0; round < rounds && !failed; ++round) {
        for (unsigned path = 0; path < PATHS && !failed; ++path) {
            for (unsigned operation = 0; operation < OPERATIONS; ++operation) {
                if (madvise(paths[path], PAGE, MADV_DONTNEED)) {
                    perror("discarding pathname"); failed = 1; break;
                }
                int error = lookup(operation, paths[path]);
                if (error != expected[path][operation]) {
                    fprintf(stderr, "FAIL round=%u path=%u operation=%u errno=%d expected=%d\n",
                            round, path, operation, error, expected[path][operation]);
                    failed = 1; break;
                }
                ++completed;
            }
        }
    }
    __atomic_store_n(&state->stop, 1, __ATOMIC_RELEASE);
    int status = -1;
    while (waitpid(reader_pid, &status, 0) < 0) {
        if (errno != EINTR) { perror("waitpid"); failed = 1; break; }
    }
    reader_pid = 0;
    alarm(0);
    int reader_error = __atomic_load_n(&state->error, __ATOMIC_ACQUIRE);
    if (reader_error || !WIFEXITED(status) || WEXITSTATUS(status)) failed = 1;
    printf("%s boot lookup probe mode=%s lookups=%llu reader_reads=%llu reader_bytes=%llu reader_errno=%d\n",
           failed ? "FAIL" : "PASS", mode, (unsigned long long)completed,
           (unsigned long long)__atomic_load_n(&state->reads, __ATOMIC_ACQUIRE),
           (unsigned long long)__atomic_load_n(&state->bytes, __ATOMIC_RELAXED), reader_error);
    if (paths != cold_paths) munmap(paths, sizeof(cold_paths));
    munmap(state, sizeof(*state));
    return failed ? 1 : 0;
}
