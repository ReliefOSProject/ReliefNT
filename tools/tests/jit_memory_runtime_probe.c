/* Linux ABI reference and ReliefOS guest probe for ordinary JIT permissions. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "[jit-memory] FAIL line=%d: %s errno=%d\n", \
            __LINE__, #condition, errno); return 1; } } while (0)

/* This benign x86_64 leaf does nothing except return the integer 42. */
static const unsigned char return_42[] = {0xb8, 42, 0, 0, 0, 0xc3};

static int execute(void *memory)
{
    int (*function)(void);
    _Static_assert(sizeof(function) == sizeof(memory), "x86_64 function pointer");
    memcpy(&function, &memory, sizeof(function));
    return function();
}

static int execution_denied(void *memory)
{
    pid_t pid = fork();
    if (pid == 0) { (void)execute(memory); _exit(99); }
    int status;
    return pid > 0 && waitpid(pid, &status, 0) == pid &&
           WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV;
}

int main(void)
{
    struct rlimit no_core = {0, 0};
    CHECK(setrlimit(RLIMIT_CORE, &no_core) == 0);
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    unsigned char *code = mmap(NULL, page, PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(code != MAP_FAILED);
    memcpy(code, return_42, sizeof(return_42));
    CHECK(mprotect(code, page, PROT_WRITE | PROT_EXEC) == 0);
    CHECK(execute(code) == 42);
    CHECK(mprotect(code, page, PROT_READ | PROT_EXEC) == 0);
    CHECK(execute(code) == 42);
    CHECK(mprotect(code, page, PROT_READ | PROT_WRITE | PROT_EXEC) == 0);
    code[1] = 43;
    CHECK(execute(code) == 43);
    CHECK(mprotect(code, page, PROT_READ | PROT_WRITE) == 0);
    CHECK(execution_denied(code));
    CHECK(code[1] == 43);
    CHECK(munmap(code, page) == 0);

    code = mmap(NULL, page, PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(code != MAP_FAILED);
    memcpy(code, return_42, sizeof(return_42));
    CHECK(execute(code) == 42);
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) { code[1] = 17; _exit(execute(code) == 17 ? 0 : 1); }
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(execute(code) == 42);
    CHECK(munmap(code, page) == 0);
    puts("[jit-memory] PASS W-to-WX, RWX mmap, RX/RW transitions, NX and fork COW");
    return 0;
}
