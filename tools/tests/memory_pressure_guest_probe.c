/* SPDX-License-Identifier: Apache-2.0 */
/* Standalone x86-64 user probe; use an actual private anonymous working set. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static volatile sig_atomic_t resume_requested;
static pthread_mutex_t worker_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t worker_condition = PTHREAD_COND_INITIALIZER;
static bool worker_ready, worker_stop;

/** @brief Mark a waiting probe ready to resume after the controller's SIGUSR1.
 * @param signal_number Delivered signal; no ownership transfer or blocking.
 */
static void resume_handler(int signal_number)
{ (void)signal_number; resume_requested = 1; }

/** @brief Keep a second task sharing this process's MM until orderly release.
 * @param unused Unused pthread argument. @return NULL after the main thread stops it.
 */
static void *hold_worker(void *unused)
{
    (void)unused;
    pthread_mutex_lock(&worker_lock);
    worker_ready = true; pthread_cond_broadcast(&worker_condition);
    while (!worker_stop) pthread_cond_wait(&worker_condition, &worker_lock);
    pthread_mutex_unlock(&worker_lock);
    return NULL;
}

/** @brief Apply and read back the standard proc OOM policy when available.
 * @param value Requested adjustment in -1000..1000. @return Zero on matching
 * readback or -1 on open, I/O or value failure; this performs host/guest proc I/O.
 */
static int set_oom_adjustment(int value)
{
    FILE *file = fopen("/proc/self/oom_score_adj", "w");
    if (!file) { perror("open oom_score_adj for write"); return -1; }
    int wrote = fprintf(file, "%d\n", value);
    int closed = fclose(file);
    if (wrote < 0 || closed) { perror("write oom_score_adj"); return -1; }
    file = fopen("/proc/self/oom_score_adj", "r");
    if (!file) { perror("open oom_score_adj for read"); return -1; }
    int observed = 0, parsed = fscanf(file, "%d", &observed);
    closed = fclose(file);
    if (parsed != 1 || closed || observed != value) {
        fprintf(stderr, "PRESSURE_ADJ_FAIL requested=%d observed=%d\n", value, observed); return -1;
    }
    printf("PRESSURE_OOM_ADJ requested=%d observed=%d\n", value, observed);
    return 0;
}

/** @brief Derive one deterministic nonzero byte from its working-set offset.
 * @param offset Byte offset in the mapping. @return Reproducible value 1..255.
 */
static uint8_t pattern(size_t offset)
{ return (uint8_t)(((offset * 37u) ^ (offset >> 8) ^ (offset >> 19)) % 255u + 1u); }

/** @brief Exercise a single x86-64 load crossing a page boundary.
 * @param address Borrowed address of eight readable bytes. @return Loaded word.
 */
__attribute__((noinline)) static uint64_t cross_load(const void *address)
{
    uint64_t value;
    __asm__ volatile("movq (%1), %0" : "=r"(value) : "r"(address) : "memory");
    return value;
}

/** @brief Execute one REP MOVSQ with both operands crossing page boundaries.
 * @param destination Borrowed writable eight-byte destination.
 * @param source Borrowed readable eight-byte source; caller owns both ranges.
 */
__attribute__((noinline)) static void cross_rep_movsq(void *destination, const void *source)
{
    size_t count = 1;
    __asm__ volatile("rep movsq" : "+D"(destination), "+S"(source), "+c"(count) :: "memory");
}

/** @brief Return immediately from the indirect-call probe. */
__attribute__((naked, noinline)) static void cross_call_target(void)
{ __asm__ volatile("ret"); }

/** @brief Call through a cross-page pointer while its return push also crosses.
 * @param new_stack Borrowed stack address with eight writable bytes below it.
 * @param slot Borrowed eight-byte function pointer to the returning probe target.
 */
__attribute__((noinline)) static void cross_indirect_call(void *new_stack, const void *slot)
{
    __asm__ volatile("movq %%rsp, %%r12\n\t"
                     "movq %[stack], %%rsp\n\t"
                     "call *(%[pointer])\n\t"
                     "movq %%r12, %%rsp"
                     :: [stack] "r"(new_stack), [pointer] "r"(slot)
                     : "r12", "rax", "rcx", "rdx", "rsi", "rdi",
                       "r8", "r9", "r10", "r11", "memory", "cc");
}

/** @brief Fill, measure residency and verify every byte of a private mapping.
 * @param argc Argument count. @param argv Byte count and optional controller modes.
 * @return Zero after verification, 1 on runtime failure, or 2 for invalid CLI.
 * Optional hold waits for SIGUSR1; --oom-adjust requires the standard proc entry.
 */
int main(int argc, char **argv)
{
    bool expect_nonresident=false, hold=false, thread=false, have_adjustment=false;
    int adjustment=0;
    if (argc < 2) { fprintf(stderr, "usage: %s BYTES [--expect-nonresident] [--hold] [--thread] [--oom-adjust N]\n", argv[0]); return 2; }
    for (int i=2; i<argc; ++i) {
        if (!strcmp(argv[i],"--expect-nonresident")) expect_nonresident=true;
        else if (!strcmp(argv[i],"--hold")) hold=true;
        else if (!strcmp(argv[i],"--thread")) thread=true;
        else if (!strcmp(argv[i],"--oom-adjust") && i+1<argc) {
            char *tail=NULL; errno=0; long value=strtol(argv[++i],&tail,10);
            if (errno || tail==argv[i] || *tail || value < -1000 || value > 1000) {
                fprintf(stderr,"invalid OOM adjustment\n"); return 2;
            }
            adjustment=(int)value; have_adjustment=true;
        } else { fprintf(stderr,"invalid option: %s\n",argv[i]); return 2; }
    }
    char *end = NULL; errno = 0;
    unsigned long long requested = strtoull(argv[1], &end, 10);
    long raw_page = sysconf(_SC_PAGESIZE);
    if (errno || !end || end == argv[1] || argv[1][0] == '-' || *end || !requested || requested > SIZE_MAX ||
        raw_page != 4096 || requested % (unsigned long long)raw_page) {
        fprintf(stderr, "invalid byte count or page size\n"); return 2;
    }
    size_t bytes = (size_t)requested, pages = bytes / (size_t)raw_page;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (have_adjustment && set_oom_adjustment(adjustment)) return 1;
    sigset_t previous, blocked;
    if (hold) {
        struct sigaction action = {0}; action.sa_handler=resume_handler;
        sigemptyset(&action.sa_mask);
        sigemptyset(&blocked); sigaddset(&blocked,SIGUSR1);
        if (sigaction(SIGUSR1,&action,NULL) || pthread_sigmask(SIG_BLOCK,&blocked,&previous)) {
            perror("install hold signal"); return 1;
        }
    }
    pthread_t worker;
    if (thread) {
        int rc=pthread_create(&worker,NULL,hold_worker,NULL);
        if (rc) { fprintf(stderr,"pthread_create error=%d\n",rc); return 1; }
        pthread_mutex_lock(&worker_lock);
        while (!worker_ready) pthread_cond_wait(&worker_condition,&worker_lock);
        pthread_mutex_unlock(&worker_lock);
        puts("PRESSURE_THREAD ready=1 shared_mm=1");
    }
    unsigned char *residency = malloc(pages);
    if (!residency) { perror("malloc residency"); return 1; }
    memset(residency, 0xff, pages);
    volatile uint8_t *memory = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) { perror("mmap"); free(residency); return 1; }
    printf("PRESSURE_CONFIG bytes=%zu pages=%zu page_size=%ld expect_nonresident=%u hold=%u thread=%u\n",
           bytes, pages, raw_page, (unsigned)expect_nonresident,(unsigned)hold,(unsigned)thread);
    uint64_t expected = 0;
    for (size_t i = 0; i < bytes; ++i) {
        uint8_t value = pattern(i); memory[i] = value; expected += value;
    }
    printf("PRESSURE_WRITTEN bytes=%zu checksum=%" PRIu64 "\n", bytes, expected);
    if (mincore((void *)memory, bytes, residency)) {
        perror("mincore"); munmap((void *)memory, bytes); free(residency); return 1;
    }
    size_t resident = 0;
    for (size_t i = 0; i < pages; ++i) resident += !!(residency[i] & 1);
    size_t nonresident = pages - resident;
    printf("PRESSURE_MINCORE pages=%zu resident=%zu nonresident=%zu\n",
           pages, resident, nonresident);
    if (hold) {
        printf("PRESSURE_READY pid=%ld bytes=%zu nonresident=%zu\n",(long)getpid(),bytes,nonresident);
        while (!resume_requested) sigsuspend(&previous);
        if (pthread_sigmask(SIG_SETMASK,&previous,NULL)) {
            fprintf(stderr,"restore signal mask failed\n"); return 1;
        }
        puts("PRESSURE_RESUMED");
    }
    if (pages >= 6) {
        size_t source = (pages / 3) * (size_t)raw_page - 4;
        size_t destination = (pages * 2 / 3) * (size_t)raw_page - 4;
        uint8_t source_bytes[8], destination_bytes[8];
        for (size_t i = 0; i < 8; ++i) {
            source_bytes[i] = memory[source + i];
            destination_bytes[i] = memory[destination + i];
        }
        cross_rep_movsq((void *)(memory + destination), (const void *)(memory + source));
        for (size_t i = 0; i < 8; ++i) {
            if (memory[destination + i] != source_bytes[i]) {
                fprintf(stderr, "PRESSURE_MISMATCH rep_movsq byte=%zu\n", i);
                munmap((void *)memory, bytes); free(residency); return 1;
            }
            memory[destination + i] = destination_bytes[i];
        }
        uintptr_t target = (uintptr_t)&cross_call_target;
        uint8_t target_bytes[8]; memcpy(target_bytes, &target, sizeof(target_bytes));
        for (size_t i = 0; i < 8; ++i) memory[source + i] = target_bytes[i];
        cross_indirect_call((void *)(memory + destination + 8), (const void *)(memory + source));
        for (size_t i = 0; i < 8; ++i) {
            memory[source + i] = source_bytes[i];
            memory[destination + i] = destination_bytes[i];
        }
        puts("PRESSURE_MULTI rep_movsq=1 indirect_call=1");
    } else {
        puts("PRESSURE_MULTI rep_movsq=0 indirect_call=0 reason=too_few_pages");
    }
    for (size_t p = 1; p < pages; ++p) {
        size_t offset = p * (size_t)raw_page - 4; uint8_t actual[8];
        uint64_t word = cross_load((const void *)(memory + offset));
        memcpy(actual, &word, sizeof(actual));
        for (size_t j = 0; j < sizeof(actual); ++j) {
            if (actual[j] != pattern(offset + j)) {
                fprintf(stderr, "PRESSURE_MISMATCH cross offset=%zu\n", offset + j);
                munmap((void *)memory, bytes); free(residency); return 1;
            }
        }
    }
    printf("PRESSURE_CROSS cases=%zu bytes_per_case=8\n", pages - 1);
    uint64_t observed = 0;
    for (size_t i = 0; i < bytes; ++i) {
        uint8_t actual = memory[i]; observed += actual;
        if (actual != pattern(i)) {
            fprintf(stderr, "PRESSURE_MISMATCH offset=%zu got=%u expected=%u\n",
                    i, actual, pattern(i));
            munmap((void *)memory, bytes); free(residency); return 1;
        }
    }
    printf("PRESSURE_READBACK bytes=%zu checksum=%" PRIu64 "\n", bytes, observed);
    int failed = expect_nonresident && !nonresident;
    if (thread) {
        pthread_mutex_lock(&worker_lock);worker_stop=true;
        pthread_cond_broadcast(&worker_condition);pthread_mutex_unlock(&worker_lock);
        if (pthread_join(worker,NULL)) { fprintf(stderr,"pthread_join failed\n"); failed=1; }
        else puts("PRESSURE_THREAD joined=1");
    }
    if (munmap((void *)memory, bytes)) { perror("munmap"); failed = 1; }
    free(residency);
    if (failed) { fprintf(stderr, "PRESSURE_FAIL expected nonresident pages\n"); return 1; }
    puts("PRESSURE_PASS");
    return 0;
}
