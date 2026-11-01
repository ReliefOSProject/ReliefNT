#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <reliefnt/console.h>
#include <reliefnt/heap.h>
#include <reliefnt/lock.h>
#include <reliefnt/mm.h>

#define PAGE 4096u
#define PAGES 8192u
#define BASE 0x100000ULL
static unsigned char *ram;
static bool used[PAGES];
static uint32_t available = PAGES;
static unsigned alloc_calls;
static unsigned mapping_failure;
static unsigned checked_clears;
static pthread_mutex_t pmm_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local unsigned heap_lock_depth;
static _Thread_local unsigned publication_race_role;
static unsigned publication_race_waiting;
static unsigned publication_race_published;

/** @brief Check that production slot clearing occurs without the heap lock.
 * @param dest Slot payload. @param value Fill byte. @param bytes Fill length.
 * @return The supplied destination, after filling it.
 */
void *fixture_checked_memset(void *dest, int value, size_t bytes)
{
    assert(!heap_lock_depth);
    __atomic_add_fetch(&checked_clears, 1, __ATOMIC_RELAXED);
    return memset(dest, value, bytes);
}

/** @brief Ignore fixture diagnostics. @param level Log level. @param format Format string. */
void console_log(enum console_log_level level, const char *format, ...)
{ (void)level; (void)format; }
/** @brief Acquire the actual heap lock with host-thread exclusion.
 * @param lock Shared heap lock. @param flags Receives the fixture IRQ state.
 */
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{
    assert(!heap_lock_depth);
    while (__atomic_exchange_n(&lock->state, 1, __ATOMIC_ACQUIRE)) sched_yield();
    heap_lock_depth = 1;
    *flags = 0;
}
/** @brief Release the actual heap lock. @param lock Held lock. @param flags Fixture IRQ state. */
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{
    (void)flags;
    assert(heap_lock_depth == 1);
    heap_lock_depth = 0;
    __atomic_store_n(&lock->state, 0, __ATOMIC_RELEASE);
}
/** @brief Allocate contiguous fixture pages, preserving PMM zero-on-allocation semantics.
 * @param count Number of pages. @return Physical base, or zero on exhaustion.
 */
uint64_t mm_alloc_pages(uint32_t count)
{
    assert(!heap_lock_depth);
    pthread_mutex_lock(&pmm_lock);
    ++alloc_calls;
    if (publication_race_role == 2) {
        pthread_mutex_unlock(&pmm_lock);
        __atomic_store_n(&publication_race_waiting, 1, __ATOMIC_RELEASE);
        while (!__atomic_load_n(&publication_race_published, __ATOMIC_ACQUIRE)) sched_yield();
        return 0;
    }
    uint32_t start = 0, run = 0;
    if (count && count <= PAGES) {
        for (uint32_t i = 0; i < PAGES; ++i) {
            if (!used[i]) { if (!run) start = i; ++run; } else run = 0;
            if (run == count) break;
        }
    }
    if (!count || count > PAGES || run != count) {
        pthread_mutex_unlock(&pmm_lock);
        return 0;
    }
    for (uint32_t i = start; i < start + count; ++i) used[i] = true;
    available -= count;
    pthread_mutex_unlock(&pmm_lock);
    memset(ram + (size_t)start * PAGE, 0, (size_t)count * PAGE);
    if (publication_race_role == 1)
        while (!__atomic_load_n(&publication_race_waiting, __ATOMIC_ACQUIRE)) sched_yield();
    return BASE + (uint64_t)start * PAGE;
}
/** @brief Release an owned fixture page run.
 * @param phys Physical run base. @param count Number of pages.
 */
void mm_free_pages(uint64_t phys, uint32_t count)
{
    assert(!heap_lock_depth);
    assert(phys >= BASE && !((phys - BASE) % PAGE));
    uint64_t start = (phys - BASE) / PAGE;
    assert(count && start < PAGES && count <= PAGES - start);
    pthread_mutex_lock(&pmm_lock);
    for (uint64_t i = start; i < start + count; ++i) { assert(used[i]); used[i] = false; }
    available += count;
    pthread_mutex_unlock(&pmm_lock);
}
/** @brief Map a supported fixture physical byte. @param phys Physical address.
 * @return Host pointer in simulated RAM, or NULL.
 */
void *mm_phys_to_virt(uint64_t phys)
{
    assert(!heap_lock_depth);
    if (mapping_failure == 1) return NULL;
    if (mapping_failure == 2) return ram + (phys - BASE) + 1;
    return phys >= BASE && phys - BASE < (uint64_t)PAGES * PAGE ? ram + (phys - BASE) : NULL;
}
/** @brief Translate a fixture pointer. @param pointer Address in simulated RAM.
 * @return Fixture physical byte, or zero when outside RAM.
 */
uint64_t mm_virt_to_phys(const void *pointer)
{
    uintptr_t address = (uintptr_t)pointer, first = (uintptr_t)ram;
    return address >= first && address - first < (uint64_t)PAGES * PAGE ? BASE + address - first : 0;
}
/** @brief Snapshot fixture free pages. @return Number of currently unowned pages. */
static uint32_t free_pages(void)
{
    pthread_mutex_lock(&pmm_lock);
    uint32_t n = available;
    pthread_mutex_unlock(&pmm_lock);
    return n;
}
/** @brief Check that an allocation is aligned and fully zeroed.
 * @param p Borrowed payload. @param bytes Payload bytes.
 */
static void check_zero(const void *p, size_t bytes)
{
    assert(p && !((uintptr_t)p & 15u));
    const unsigned char *q = p;
    for (size_t i = 0; i < bytes; ++i) assert(q[i] == 0);
}
/** @brief Reject a page-per-object heap and verify page conservation. */
static void density(void)
{
    enum { N = 4000 };
    void *objects[N];
    uint32_t start = free_pages();
    for (unsigned i = 0; i < N; ++i) {
        objects[i] = kernel_malloc(16); check_zero(objects[i], 16);
        memset(objects[i], (int)(i % 255 + 1), 16);
    }
    unsigned consumed = start - free_pages();
    fprintf(stderr, "4000 x 16-byte allocations consumed %u pages (limit 24)\n", consumed);
    assert(consumed <= 24);
#ifdef HEAP_CHECK_CLEAR
    assert(__atomic_load_n(&checked_clears, __ATOMIC_RELAXED) == N);
#endif
    for (unsigned i = 0; i < N; ++i) kernel_free(objects[(i * 1597u) % N]);
    assert(free_pages() == start);
}
/** @brief Check the full payload at size-class boundaries without overlap. */
static void classes(void)
{
    static const size_t sizes[] = {1, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128,
                                   129, 255, 256, 257, 511, 512, 513, 1023, 1024,
                                   1025, 2047, 2048};
    enum { COPIES = 7, KINDS = sizeof(sizes) / sizeof(sizes[0]) };
    void *objects[KINDS][COPIES];
    uint32_t start = free_pages();
    for (unsigned i = 0; i < KINDS; ++i) for (unsigned j = 0; j < COPIES; ++j) {
        unsigned char *p = kernel_malloc(sizes[i]);
        check_zero(p, sizes[i]); memset(p, (int)(1 + i * COPIES + j), sizes[i]);
        objects[i][j] = p;
    }
    for (unsigned i = 0; i < KINDS; ++i) for (unsigned j = 0; j < COPIES; ++j) {
        const unsigned char *p = objects[i][j];
        for (size_t k = 0; k < sizes[i]; ++k) assert(p[k] == (unsigned char)(1 + i * COPIES + j));
        kernel_free(objects[i][j]);
    }
    assert(free_pages() == start);
}
/** @brief Reuse freed slots in live slabs and verify every returned byte is zero. */
static void reuse(void)
{
    enum { N = 320 };
    void *objects[N];
    uint32_t start = free_pages();
    for (unsigned i = 0; i < N; ++i) { objects[i] = kernel_malloc(32); check_zero(objects[i], 32); memset(objects[i], 0xa5, 32); }
    uint32_t populated = free_pages();
    for (unsigned i = 0; i < N; i += 2) kernel_free(objects[i]);
    for (unsigned i = 0; i < N; i += 2) { objects[i] = kernel_malloc(32); check_zero(objects[i], 32); }
    assert(free_pages() == populated);
    for (unsigned i = 0; i < N; ++i) kernel_free(objects[i]);
    assert(free_pages() == start);
}
/** @brief Ignore interior, duplicate, unknown and forged pointers without freeing live objects. */
static void invalid(void)
{
    uint32_t start = free_pages();
    unsigned char *a = kernel_malloc(64), *b = kernel_malloc(64);
    assert(a && b); memset(a, 0x55, 64); memset(b, 0x77, 64);
    uint32_t populated = free_pages();
    kernel_free(a + 1);
    uint32_t fake[] = {0x4c484541u, 1};
    memcpy(a, fake, sizeof(fake));
    kernel_free(a + 16);
    kernel_free((void *)((uintptr_t)a & ~(uintptr_t)(PAGE - 1)));
    assert(free_pages() == populated && b[0] == 0x77);
    kernel_free(a); kernel_free(a);
    assert(free_pages() == populated && b[0] == 0x77);
    kernel_free(b); kernel_free(b);
    assert(free_pages() == start);
    unsigned char *large = kernel_malloc(8193);
    assert(large);
    populated = free_pages();
    memcpy(large, fake, sizeof(fake));
    kernel_free(large + 16);
    kernel_free(large + 1);
    kernel_free((void *)(uintptr_t)1);
    assert(free_pages() == populated);
    kernel_free(large); kernel_free(large);
    assert(free_pages() == start);
    kernel_free((void *)(uintptr_t)1);
    kernel_free(NULL);
    assert(free_pages() == start);
}
/** @brief Check large contiguous allocations and overflow rejection. */
static void large(void)
{
    static const size_t sizes[] = {2049, 4096, 8193, 16384};
    uint32_t start = free_pages();
    assert(!kernel_malloc(0));
    unsigned calls = alloc_calls;
    assert(!kernel_malloc(SIZE_MAX));
    assert(!kernel_malloc(SIZE_MAX - 7));
    assert(!kernel_malloc((size_t)UINT32_MAX * PAGE + 1));
    assert(alloc_calls == calls);
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        size_t n = sizes[i];
        unsigned char *p = kernel_malloc(n); check_zero(p, n);
        uint64_t base = mm_virt_to_phys(p);
        for (size_t j = 0; j < n; j += PAGE) assert(mm_virt_to_phys(p + j) == base + j);
        memset(p, 0xcf, n); kernel_free(p);
        assert(free_pages() == start);
    }
}
/** @brief Detect ENOMEM and recover every page after freeing all live allocations. */
static void exhaustion(void)
{
    void *objects[PAGES];
    uint32_t start = free_pages();
    unsigned count = 0;
    while (count < PAGES && (objects[count] = kernel_malloc(2048)) != NULL) ++count;
    assert(count && !free_pages());
    assert(!kernel_malloc(2048));
    assert(!kernel_malloc(8193));
    for (unsigned i = 0; i < count; ++i) kernel_free(objects[i]);
    assert(free_pages() == start);
}

/** @brief Failed and invalid mappings return both slab and large page runs to PMM. */
static void mapping(void)
{
    uint32_t start = free_pages();
    for (mapping_failure = 1; mapping_failure <= 2; ++mapping_failure) {
        assert(!kernel_malloc(64));
        assert(free_pages() == start);
        assert(!kernel_malloc(8193));
        assert(free_pages() == start);
    }
    mapping_failure = 0;
    void *p = kernel_malloc(64), *q = kernel_malloc(8193);
    assert(p && q);
    kernel_free(p); kernel_free(q);
    assert(free_pages() == start);
}

struct publication_arg {
    pthread_barrier_t *barrier;
    unsigned id;
    void **objects;
    uint32_t initial;
};
/** @brief Synchronize first publication and concurrent final-slot frees repeatedly.
 * @param raw Shared test state and thread index. @return NULL to pthread.
 */
static void *publication_worker(void *raw)
{
    enum { THREADS = 8 };
    struct publication_arg *arg = raw;
    for (unsigned round = 0; round < 150; ++round) {
        pthread_barrier_wait(arg->barrier);
        arg->objects[arg->id] = kernel_malloc(16);
        check_zero(arg->objects[arg->id], 16);
        memset(arg->objects[arg->id], (int)(arg->id + 1), 16);
        pthread_barrier_wait(arg->barrier);
        if (!arg->id) {
            assert(free_pages() == arg->initial - 1);
            for (unsigned i = 0; i < THREADS; ++i) for (unsigned j = i + 1; j < THREADS; ++j)
                assert(arg->objects[i] != arg->objects[j]);
        }
        pthread_barrier_wait(arg->barrier);
        kernel_free(arg->objects[arg->id]);
        pthread_barrier_wait(arg->barrier);
        if (!arg->id) assert(free_pages() == arg->initial);
        pthread_barrier_wait(arg->barrier);
    }
    return NULL;
}
/** @brief Verify simultaneous first and final operations return all pages every round. */
static void publication(void)
{
    enum { THREADS = 8 };
    pthread_t threads[THREADS]; struct publication_arg args[THREADS];
    pthread_barrier_t barrier; void *objects[THREADS];
    uint32_t start = free_pages();
    assert(!pthread_barrier_init(&barrier, NULL, THREADS));
    for (unsigned i = 0; i < THREADS; ++i) {
        args[i] = (struct publication_arg){&barrier, i, objects, start};
        assert(!pthread_create(&threads[i], NULL, publication_worker, &args[i]));
    }
    for (unsigned i = 0; i < THREADS; ++i) assert(!pthread_join(threads[i], NULL));
    assert(!pthread_barrier_destroy(&barrier));
    assert(free_pages() == start);
}

struct publication_race_arg { unsigned role; void *result; };
/** @brief Force one contender's PMM failure to overlap another's first publication.
 * @param raw Thread role and result. @return NULL to pthread.
 */
static void *publication_race_worker(void *raw)
{
    struct publication_race_arg *arg = raw;
    publication_race_role = arg->role;
    arg->result = kernel_malloc(16);
    if (arg->role == 1) __atomic_store_n(&publication_race_published, 1, __ATOMIC_RELEASE);
    return NULL;
}
/** @brief A racing PMM failure must use the slab published while it was waiting. */
static void publication_enomem(void)
{
    uint32_t start = free_pages();
    pthread_t threads[2];
    struct publication_race_arg args[2] = {{1, NULL}, {2, NULL}};
    for (unsigned i = 0; i < 2; ++i)
        assert(!pthread_create(&threads[i], NULL, publication_race_worker, &args[i]));
    for (unsigned i = 0; i < 2; ++i) assert(!pthread_join(threads[i], NULL));
    check_zero(args[0].result, 16); check_zero(args[1].result, 16);
    assert(args[0].result != args[1].result && free_pages() == start - 1);
    kernel_free(args[0].result); kernel_free(args[1].result);
    assert(free_pages() == start);
}

struct duplicate_arg { pthread_barrier_t *barrier; void *target; };
/** @brief Race a duplicate free while another slot keeps the page live.
 * @param raw Shared barrier and target. @return NULL to pthread.
 */
static void *duplicate_worker(void *raw)
{
    struct duplicate_arg *arg = raw;
    pthread_barrier_wait(arg->barrier);
    kernel_free(arg->target);
    return NULL;
}
/** @brief A concurrent duplicate free cannot create two owners of the same slot. */
static void duplicate(void)
{
    uint32_t start = free_pages();
    for (unsigned round = 0; round < 200; ++round) {
        unsigned char *anchor = kernel_malloc(32), *target = kernel_malloc(32);
        assert(anchor && target);
        memset(anchor, 0x7a, 32);
        pthread_t threads[2]; pthread_barrier_t barrier;
        struct duplicate_arg arg = {&barrier, target};
        assert(!pthread_barrier_init(&barrier, NULL, 2));
        for (unsigned i = 0; i < 2; ++i) assert(!pthread_create(&threads[i], NULL, duplicate_worker, &arg));
        for (unsigned i = 0; i < 2; ++i) assert(!pthread_join(threads[i], NULL));
        assert(!pthread_barrier_destroy(&barrier));
        void *a = kernel_malloc(32), *b = kernel_malloc(32);
        assert(a && b && a != b && a != anchor && b != anchor && anchor[0] == 0x7a);
        kernel_free(a); kernel_free(b); kernel_free(anchor);
        assert(free_pages() == start);
    }
}

/** @brief Exercise interleaved insertion, arbitrary removal and address reuse in both trees. */
static void trees(void)
{
    enum { N = 1024 };
    unsigned char *small[N], *large[N];
    uint32_t start = free_pages();
    for (unsigned i = 0; i < N; ++i) {
        small[i] = kernel_malloc(2048); large[i] = kernel_malloc(2049);
        check_zero(small[i], 2048); check_zero(large[i], 2049);
        memset(small[i], 0x35, 2048); memset(large[i], 0x79, 2049);
    }
    for (unsigned i = 0; i < N; ++i) {
        unsigned index = (i * 997u) % N;
        assert(small[index][0] == 0x35 && small[index][2047] == 0x35);
        assert(large[index][0] == 0x79 && large[index][2048] == 0x79);
        kernel_free(small[index]); kernel_free(large[index]);
        if (i % 2 == 0) {
            small[index] = kernel_malloc(2048); large[index] = kernel_malloc(2049);
            check_zero(small[index], 2048); check_zero(large[index], 2049);
            memset(small[index], 0x35, 2048); memset(large[index], 0x79, 2049);
        } else small[index] = large[index] = NULL;
    }
    for (unsigned i = 0; i < N; ++i) {
        if (small[i]) {
            assert(small[i][2047] == 0x35 && large[i][2048] == 0x79);
            kernel_free(small[i]); kernel_free(large[i]);
        }
    }
    assert(free_pages() == start);
}

struct thread_arg { pthread_barrier_t *barrier; unsigned id; };
/** @brief Stress simultaneous first publication, random slot reuse and final release.
 * @param raw Thread argument. @return NULL to pthread.
 */
static void *worker(void *raw)
{
    struct thread_arg *arg = raw;
    void *slots[64] = {0};
    size_t sizes[64] = {0};
    uint32_t state = 1234567u + arg->id * 7919u;
    pthread_barrier_wait(arg->barrier);
    for (unsigned round = 0; round < 5000; ++round) {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        unsigned index = state % 64;
        if (slots[index]) {
            unsigned char *p = slots[index];
            for (size_t j = 0; j < sizes[index]; ++j) assert(p[j] == (unsigned char)(arg->id + 1));
            kernel_free(p); slots[index] = NULL;
        } else {
            size_t n = 1 + ((state >> 6) % 2048);
            unsigned char *p = kernel_malloc(n); check_zero(p, n);
            memset(p, (int)(arg->id + 1), n); slots[index] = p; sizes[index] = n;
        }
    }
    pthread_barrier_wait(arg->barrier);
    for (unsigned i = 0; i < 64; ++i) kernel_free(slots[i]);
    return NULL;
}
/** @brief Verify concurrent thread traffic never loses pages or contents. */
static void concurrency(void)
{
    enum { THREADS = 8 };
    pthread_t threads[THREADS]; struct thread_arg args[THREADS]; pthread_barrier_t barrier;
    uint32_t start = free_pages();
    assert(!pthread_barrier_init(&barrier, NULL, THREADS));
    for (unsigned i = 0; i < THREADS; ++i) {
        args[i] = (struct thread_arg){&barrier, i};
        assert(!pthread_create(&threads[i], NULL, worker, &args[i]));
    }
    for (unsigned i = 0; i < THREADS; ++i) assert(!pthread_join(threads[i], NULL));
    assert(!pthread_barrier_destroy(&barrier));
    assert(free_pages() == start);
}
/** @brief Run one independent case. @param argc Argument count. @param argv Case name.
 * @return Zero on success or 2 for an unknown case.
 */
int main(int argc, char **argv)
{
    assert(argc == 2);
    ram = mmap(NULL, (size_t)PAGES * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(ram != MAP_FAILED && !((uintptr_t)ram & (PAGE - 1)));
    if (!strcmp(argv[1], "density")) density();
    else if (!strcmp(argv[1], "classes")) classes();
    else if (!strcmp(argv[1], "reuse")) reuse();
    else if (!strcmp(argv[1], "invalid")) invalid();
    else if (!strcmp(argv[1], "large")) large();
    else if (!strcmp(argv[1], "exhaustion")) exhaustion();
    else if (!strcmp(argv[1], "mapping")) mapping();
    else if (!strcmp(argv[1], "publication")) publication();
    else if (!strcmp(argv[1], "publication_enomem")) publication_enomem();
    else if (!strcmp(argv[1], "duplicate")) duplicate();
    else if (!strcmp(argv[1], "trees")) trees();
    else if (!strcmp(argv[1], "concurrency")) concurrency();
    else return 2;
    puts("PASS");
    assert(!munmap(ram, (size_t)PAGES * PAGE));
    return 0;
}
