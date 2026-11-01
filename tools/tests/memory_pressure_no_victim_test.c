/* Real trap, pressure engine, wait queue, selector and page tables. Task table,
 * physical quota, CPU handoff and storage transport are deterministic models. */
#define _POSIX_C_SOURCE 200809L
#include "../../kernel/sched/internal.h"
#define PRESSURE_REAL_FIXTURE 1
#define PRESSURE_FAULT_TEST 1
#define PRESSURE_CUSTOM_TASK_TABLE 1
#define SWAP_FAULT_GATE_FIXTURE_ENTRY inherited_gate_main
#include "swap_fault_gate_test.c"

static struct sched_task_object protected_slots[40];
static unsigned terminal_results, selector_calls;
static bool jitter_free;
static unsigned free_reads;
static void (*retirement_notify)(void);

/** @brief Model 40 protected task slots followed by the current user task.
 * @param index Scheduler slot. @return Borrowed slot or NULL at the table end.
 */
struct task *sched_task_at(uint32_t index)
{
    if (index < 40) return &protected_slots[index].task;
    return index == 40 ? current : NULL;
}
/** @brief Model installed memory. @return Fixed capacity in KiB. */
uint64_t mm_total_memory_kib(void) { return 8192 * 4; }
/** @brief Model the pressure reserve. @return Fixed reserve in pages. */
uint64_t mm_emergency_reserve_pages(void) { return 64; }
/** @brief Force pressure, optionally with an unrelated one-page fluctuation.
 * @return Free KiB at the reserve or, on alternate jitter reads, one above.
 */
uint64_t mm_free_memory_kib(void)
{ return 4 * (64 + (jitter_free ? (free_reads++ & 1u) : 0u)); }
/** @brief Model idle storage. @return False; this fixture performs no I/O. */
bool swap_storage_busy(void) { return false; }
/** @brief Retain the scheduler's resource-retirement callback in the model.
 * @param notify Borrowed callback installed by the actual pressure engine.
 */
void sched_set_memory_release_notify(void (*notify)(void)) { retirement_notify = notify; }
/** @brief Fail closed if the protected-only selector tries to send a kill.
 * @param task Borrowed target. @param process Process-directed flag.
 * @param signal Signal number. @param info Borrowed optional signal data.
 * @return Never; any invocation contradicts the fixture's no-victim setup.
 */
int kernel_signal_enqueue(struct task *task, bool process, int signal,
                          const struct linux_siginfo *info)
{ (void)task; (void)process; (void)signal; (void)info; abort(); }
/** @brief Linker-provided real selector entry for this instrumented fixture.
 * @param budget Maximum candidate work. @return Actual selector status.
 */
int __real_memory_oom_advance(uint32_t budget);
/** @brief Count real selector calls and terminal no-victim results.
 * @param budget Actual bounded work allowance. @return Unmodified real status.
 */
int __wrap_memory_oom_advance(uint32_t budget)
{
    assert(budget && budget <= 32);
    ++selector_calls;
    int result = __real_memory_oom_advance(budget);
    if (result == -ENOMEM) ++terminal_results;
    return result;
}

/** @brief Require a terminal no-victim selector result to signal once.
 * @param argc Argument count. @param argv Selects anonymous, COW, or jitter failure.
 * @return Zero after the actual trap exits its first terminal selection.
 */
int main(int argc, char **argv)
{
    assert(argc == 2 && (!strcmp(argv[1], "anon") || !strcmp(argv[1], "cow") ||
                         !strcmp(argv[1], "anon-jitter")));
    nx_enabled = true; page_cache_init();
    for (unsigned i = 0; i < 40; ++i) {
        struct task *slot = &protected_slots[i].task;
        slot->pid = i + 1000; slot->kind = TASK_KIND_USER;
        slot->state = TASK_READY; slot->flags = TASK_FLAG_SERVICE;
    }
    struct task *parent = new_task(), *task = parent;
    const uint64_t va = RELIEFNT_USER_BASE + 0x50000;
    parent->vmas[0] = (struct task_vma){.used=1, .start=va, .end=va+4096,
        .prot=LINUX_PROT_READ|LINUX_PROT_WRITE, .flags=TASK_VMA_FLAG_PRIVATE|TASK_VMA_FLAG_ANON};
    if (!strcmp(argv[1], "cow")) {
        uint64_t frame = mm_alloc_page(); assert(frame);
        memset(mm_phys_to_virt(frame), 0xa5, 4096);
        assert(address_space_map_user_page(sched_task_as(parent), va, frame, RELIEFNT_PAGE_WRITABLE));
        task = calloc(1, sizeof(struct sched_task_object)); assert(task);
        task->kind = TASK_KIND_USER; task->pid = 124; task->vmas[0] = parent->vmas[0];
        assert(address_space_clone_cow(sched_task_as(parent), sched_task_as(task)));
    }
    task->flags |= TASK_FLAG_SERVICE;
    struct fault_work work = {task, va, !strcmp(argv[1], "cow") ? 7 : 6, 0x23, 0, NULL, 0};
    jitter_free = !strcmp(argv[1], "anon-jitter"); free_reads = 0;
    atomic_store(&fail_alloc, true);
    unsigned attempts = 0;
    while (!terminal_results) {
        run_fault(&work);
        assert(++attempts < 2000);
    }
    printf("case=%s attempts=%u selector_calls=%u terminal=%u signals=%u\n", argv[1],
           attempts, selector_calls, terminal_results, atomic_load(&signals)); fflush(stdout);
    assert(terminal_results == 1 && atomic_load(&signals) == 1);
    assert(!task->waiting_queue && retirement_notify);
    atomic_store(&fail_alloc, false);
    if (task != parent) destroy_task(task);
    destroy_task(parent);
    assert(!live_pages);
    puts("PASS first real selector no-victim terminal result exits the actual user trap");
    return 0;
}
