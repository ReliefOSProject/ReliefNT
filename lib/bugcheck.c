/* SPDX-License-Identifier: Apache-2.0 */
/* Fatal diagnostics use the current TTY or a black framebuffer text console.
 * No allocator, scheduler lock, console lock or GPU lock is used after claim. */
#include <reliefnt/bugcheck.h>
#include <reliefnt/console.h>
#include <reliefnt/apic.h>
#include <reliefnt/sched.h>
#include <reliefnt/smp.h>
#include <reliefnt/time.h>
#include <generated/build_info.h>
#include "../arch/x86_64/port.h"

static volatile int g_bugcheck_active;
static volatile uint32_t g_bugcheck_parked[SMP_MAX_CPUS];

#define PANIC_RED 0x00ff5555u
#define PANIC_YELLOW 0x00ffff55u
#define PANIC_WHITE 0x00ffffffu

struct bugcheck_info {
    const char *reason;
    const char *detail;
    bool has_exception;
    bool have_registers;
    bool have_runtime;
    bool header_printed;
    uint32_t cpu, apic, pid;
    uint32_t stopped, others;
    uint64_t cr0, cr2, cr3, cr4;
    uint64_t ticks, uptime_ms;
    char task_name[64];
    struct console_panic_context display;
    struct trap_frame frame;
};

struct panic_line {
    char bytes[192];
    size_t used;
};

/**
 * @brief Append bounded, literal diagnostic text, neutralizing terminal controls.
 * @param line Destination line, always leaving space for newline.
 * @param text NUL-terminated text; NULL is treated as unavailable.
 */
static void line_text(struct panic_line *line, const char *text)
{
    if (!text) text = "(unavailable)";
    while (*text && line->used + 2u < sizeof(line->bytes)) {
        unsigned char ch = (unsigned char)*text++;
        line->bytes[line->used++] = ch < 32u || ch == 127u ? '?' : (char)ch;
    }
}

/**
 * @brief Append a fixed-width, zero-padded 64-bit hexadecimal value.
 * @param line Destination line.
 * @param value Unsigned value to format.
 */
static void line_hex(struct panic_line *line, uint64_t value)
{
    static const char digits[] = "0123456789ABCDEF";
    line_text(line, "0x");
    for (int shift = 60; shift >= 0 && line->used + 2u < sizeof(line->bytes); shift -= 4)
        line->bytes[line->used++] = digits[(value >> shift) & 15u];
}

/**
 * @brief Append a decimal value without an allocator or libc formatter.
 * @param line Destination line.
 * @param value Unsigned value to format.
 */
static void line_dec(struct panic_line *line, uint64_t value)
{
    char digits[21];
    unsigned count = 0;
    do { digits[count++] = '0' + value % 10u; value /= 10u; } while (value);
    while (count && line->used + 2u < sizeof(line->bytes))
        line->bytes[line->used++] = digits[--count];
}

/**
 * @brief Print one line, then reset the reusable formatting buffer.
 * @param line Complete line to print.
 * @param color Logical RGB foreground color; background is always black.
 */
static void line_emit(struct panic_line *line, uint32_t color)
{
    line->bytes[line->used++] = '\n';
    console_panic_write(line->bytes, line->used, color);
    line->used = 0;
}

/**
 * @brief Format a short constant or bounded string as its own diagnostic line.
 * @param text Text to print.
 * @param color Logical RGB foreground color.
 */
static void panic_text(const char *text, uint32_t color)
{
    struct panic_line line = {0};
    line_text(&line, text);
    line_emit(&line, color);
}

/**
 * @brief Name an x86 architectural exception vector.
 * @param vector CPU exception number.
 * @return Resident descriptive string, including a generic fallback.
 */
static const char *exception_name(uint64_t vector)
{
    switch (vector) {
    case 0: return "Divide By Zero";
    case 1: return "Debug Exception";
    case 2: return "Non Maskable Interrupt";
    case 3: return "Breakpoint";
    case 4: return "Overflow";
    case 5: return "Bound Range Exceeded";
    case 6: return "Invalid Opcode";
    case 7: return "Device Not Available";
    case 8: return "Double Fault";
    case 10: return "Invalid TSS";
    case 11: return "Segment Not Present";
    case 12: return "Stack Segment Fault";
    case 13: return "General Protection Fault";
    case 14: return "Page Fault";
    case 16: return "x87 Floating Point Fault";
    case 17: return "Alignment Check";
    case 18: return "Machine Check";
    case 19: return "SIMD Floating Point Fault";
    case 20: return "Virtualization Exception";
    case 21: return "Control Protection Exception";
    case 28: return "Hypervisor Injection Exception";
    case 29: return "VMM Communication Exception";
    case 30: return "Security Exception";
    default: return "CPU Exception";
    }
}

/**
 * @brief Format two or three named registers on one terminal-width line.
 * @param a First register name.
 * @param av First register value.
 * @param b Second register name.
 * @param bv Second register value.
 * @param c Optional third name; NULL omits the third register.
 * @param cv Third register value.
 */
static void register_line(const char *a, uint64_t av, const char *b, uint64_t bv,
                          const char *c, uint64_t cv)
{
    struct panic_line line = {0};
    line_text(&line, a); line_text(&line, "="); line_hex(&line, av);
    line_text(&line, "  "); line_text(&line, b); line_text(&line, "="); line_hex(&line, bv);
    if (c) { line_text(&line, "  "); line_text(&line, c); line_text(&line, "="); line_hex(&line, cv); }
    line_emit(&line, PANIC_WHITE);
}

/**
 * @brief Decode architectural page-fault error bits without assuming feature support.
 * @param error CPU-supplied page-fault error code.
 */
static void page_fault_flags(uint64_t error)
{
    static const char *const names[] = {"P=", " W=", " U=", " RSVD=", " IF=", " PK=", " SS="};
    struct panic_line line = {0};
    line_text(&line, "PF: ");
    for (unsigned bit = 0; bit < 7; ++bit) {
        line_text(&line, names[bit]); line_dec(&line, (error >> bit) & 1u);
    }
    line_text(&line, " SGX="); line_dec(&line, (error >> 15) & 1u);
    line_emit(&line, PANIC_YELLOW);
    line_text(&line, error & 1u ? "Protection violation during " : "Non-present page during ");
    line_text(&line, error & 16u ? "instruction fetch" : error & 2u ? "write" : "read");
    line_text(&line, error & 4u ? " in user mode." : " in supervisor mode.");
    line_emit(&line, PANIC_YELLOW);
}

/**
 * @brief Render detailed diagnostics as terminal text rather than a GUI panel.
 * @param info Immutable trap/runtime snapshot; unavailable fields are explicit.
 */
static void render_bugcheck_text(const struct bugcheck_info *info)
{
    const struct trap_frame *f = &info->frame;
    struct panic_line line = {0};
    if (!info->header_printed) panic_text("!!! KERNEL PANIC - SYSTEM HALTED !!!", PANIC_RED);
    line_text(&line, "Reason: "); line_text(&line, info->reason); line_emit(&line, PANIC_RED);
    if (info->detail) { line_text(&line, "Detail: "); line_text(&line, info->detail); line_emit(&line, PANIC_YELLOW); }
    panic_text("ReliefNT " RELIEFOS_KERNEL_VERSION "  source=" RELIEFOS_SOURCE_ID, PANIC_WHITE);
    panic_text("Build: " RELIEFOS_BUILD_TIME "  x86_64", PANIC_WHITE);
    line_text(&line, "CPU "); line_dec(&line, info->cpu); line_text(&line, "  APIC "); line_dec(&line, info->apic);
    line_text(&line, "  other CPUs stopped="); line_dec(&line, info->stopped);
    line_text(&line, "/"); line_dec(&line, info->others); line_emit(&line, PANIC_WHITE);
    line_text(&line, "Display: ");
    if (info->display.vt) { line_text(&line, "tty"); line_dec(&line, info->display.vt); }
    else line_text(&line, "boot console");
    line_text(&line, info->display.graphical ? " (GUI -> panic text overlay)" : " (current text console)");
    line_emit(&line, PANIC_WHITE);
    if (info->have_runtime) {
        line_text(&line, "PID "); line_dec(&line, info->pid); line_text(&line, "  TASK "); line_text(&line, info->task_name);
        line_emit(&line, PANIC_WHITE);
        line_text(&line, "UPTIME_MS="); line_dec(&line, info->uptime_ms); line_text(&line, "  TICKS="); line_dec(&line, info->ticks);
        line_emit(&line, PANIC_WHITE);
    } else panic_text("Task/time: unavailable on emergency exception path", PANIC_YELLOW);
    line_text(&line, "Mode: "); line_text(&line, (f->cs & 3u) == 3u ? "user" : "kernel");
    if (info->has_exception) {
        line_text(&line, "  VECTOR="); line_dec(&line, f->vector);
        line_text(&line, "  ERROR="); line_hex(&line, f->error);
    } else line_text(&line, "  explicit panic (RIP = caller)");
    line_emit(&line, PANIC_YELLOW);
    register_line("RIP", f->rip, "RFLAGS", f->rflags, NULL, 0);
    if (f->rsp) register_line("RSP", f->rsp, "SS", f->ss, "CS", f->cs);
    else { register_line("CS", f->cs, "SS", f->ss, NULL, 0); panic_text("RSP: not saved by this exception entry", PANIC_YELLOW); }
    register_line("CR2", info->cr2, "CR3", info->cr3, NULL, 0);
    register_line("CR0", info->cr0, "CR4", info->cr4, NULL, 0);
    if (info->has_exception && f->vector == 14) page_fault_flags(f->error);
    if (info->have_registers) {
        register_line("RAX", f->rax, "RBX", f->rbx, "RCX", f->rcx);
        register_line("RDX", f->rdx, "RSI", f->rsi, "RDI", f->rdi);
        register_line("RBP", f->rbp, "R8 ", f->r8, "R9 ", f->r9);
        register_line("R10", f->r10, "R11", f->r11, "R12", f->r12);
        register_line("R13", f->r13, "R14", f->r14, "R15", f->r15);
    } else panic_text("General registers: not saved by this entry", PANIC_YELLOW);
    panic_text("Save this screen and serial log before restarting.", PANIC_WHITE);
    panic_text("Resolve RIP with the matching kernel.unstripped: addr2line -e <ELF> <RIP>", PANIC_WHITE);
    panic_text("!!! KERNEL PANIC - SYSTEM HALTED; RESTART REQUIRED !!!", PANIC_RED);
}

/** @brief Halt without touching console state, including on secondary CPUs. */
static __attribute__((noreturn)) void bugcheck_halt_forever(void)
{
    for (;;) __asm__ volatile("cli; hlt" : : : "memory");
}

/**
 * @brief Stop online peer CPUs via NMI, then wait a bounded time for their entry.
 * @param info Owner CPU identity; receives stopped/expected peer counts.
 */
static void stop_other_cpus(struct bugcheck_info *info)
{
    uint32_t count = smp_cpu_count();
    if (count > SMP_MAX_CPUS) count = SMP_MAX_CPUS;
    for (uint32_t cpu = 0; cpu < count; ++cpu) {
        if (cpu == info->cpu || !smp_cpu_online(cpu)) continue;
        ++info->others;
        const struct smp_cpu_info *other = smp_cpu_info(cpu);
        if (other) (void)apic_send_nmi(other->apic_id);
    }
    for (uint32_t attempt = 0; attempt < 1000000u; ++attempt) {
        info->stopped = 0;
        for (uint32_t cpu = 0; cpu < count; ++cpu)
            if (cpu != info->cpu && __atomic_load_n(&g_bugcheck_parked[cpu], __ATOMIC_ACQUIRE)) ++info->stopped;
        if (info->stopped >= info->others) break;
        __asm__ volatile("pause");
    }
}

/**
 * @brief Capture lock-free runtime fields; emergency faults omit scheduler data.
 * @param info Crash record whose frame and control registers are already saved.
 */
static void collect_bugcheck_info(struct bugcheck_info *info)
{
    if (info->has_exception && (info->frame.vector == 2 || info->frame.vector == 8 || info->frame.vector == 18)) return;
    info->ticks = time_ticks();
    info->uptime_ms = time_uptime_ms();
    info->pid = sched_current_pid();
    const struct task *task = sched_current_task();
    const char *name = task && task->name ? task->name : "(none)";
    size_t index = 0;
    while (index + 1u < sizeof(info->task_name) && name[index]) {
        info->task_name[index] = name[index]; ++index;
    }
    info->task_name[index] = 0;
    info->have_runtime = true;
}

/**
 * @brief Claim fatal output, stop peers, report through the emergency console, halt.
 * @param info Saved exception context, never allocated dynamically.
 */
static __attribute__((noreturn)) void bugcheck_commit(struct bugcheck_info *info)
{
    __asm__ volatile("cli" : : : "memory");
    uint32_t cpu = smp_current_cpu();
    if (__sync_lock_test_and_set(&g_bugcheck_active, 1)) {
        if (cpu < SMP_MAX_CPUS) __atomic_store_n(&g_bugcheck_parked[cpu], 1, __ATOMIC_RELEASE);
        bugcheck_halt_forever();
    }
    info->cpu = cpu;
    info->apic = apic_id();
    __asm__ volatile("mov %%cr0, %0" : "=r"(info->cr0));
    __asm__ volatile("mov %%cr3, %0" : "=r"(info->cr3));
    __asm__ volatile("mov %%cr4, %0" : "=r"(info->cr4));
    stop_other_cpus(info);
    info->display = console_panic_begin();
    panic_text("!!! KERNEL PANIC - SYSTEM HALTED !!!", PANIC_RED);
    console_panic_flush();
    info->header_printed = true;
    collect_bugcheck_info(info);
    render_bugcheck_text(info);
    console_panic_flush();
    bugcheck_halt_forever();
}

/**
 * @brief Report an explicit kernel panic, capturing caller RIP and current stack.
 * @param message Resident kernel diagnostic text; NULL is allowed.
 */
__attribute__((noreturn)) void bugcheck_panic(const char *message)
{
    struct bugcheck_info info = {.reason = "Explicit kernel panic", .detail = message};
    info.frame.rip = (uint64_t)__builtin_return_address(0);
    __asm__ volatile("mov %%rsp, %0" : "=r"(info.frame.rsp));
    __asm__ volatile("mov %%cs, %0" : "=r"(info.frame.cs));
    __asm__ volatile("mov %%ss, %0" : "=r"(info.frame.ss));
    __asm__ volatile("pushfq; popq %0" : "=r"(info.frame.rflags));
    __asm__ volatile("mov %%cr2, %0" : "=r"(info.cr2));
    bugcheck_commit(&info);
}

/**
 * @brief Report an unhandled scalar exception frame; do not invent GPR values.
 * @param vector CPU exception number.
 * @param error CPU exception error code.
 * @param rip Faulting instruction pointer.
 * @param cs Saved code selector.
 * @param rflags Saved CPU flags.
 * @param rsp Saved stack pointer, zero if absent from this entry.
 * @param ss Saved stack selector, zero if absent.
 * @param cr2 Fault address register captured by the dispatcher.
 */
__attribute__((noreturn)) void bugcheck_exception(uint64_t vector, uint64_t error,
    uint64_t rip, uint64_t cs, uint64_t rflags, uint64_t rsp, uint64_t ss, uint64_t cr2)
{
    struct bugcheck_info info = {
        .reason = exception_name(vector), .detail = "Unhandled CPU exception",
        .has_exception = true, .cr2 = cr2,
        .frame = {.vector = vector, .error = error, .rip = rip, .cs = cs,
                  .rflags = rflags, .rsp = rsp, .ss = ss},
    };
    bugcheck_commit(&info);
}

/**
 * @brief Preserve all registers from an unhandled architecture trap frame.
 * @param reason Resident diagnostic description, or NULL for a generic reason.
 * @param frame Saved CPU and general registers; NULL is explicitly unavailable.
 * @param cr2 Fault address captured at exception entry.
 */
__attribute__((noreturn)) void bugcheck_trap(const char *reason, const struct trap_frame *frame, uint64_t cr2)
{
    struct bugcheck_info info = {.reason = reason ? reason : "Kernel trap",
        .detail = frame ? "Trap frame captured" : "Trap frame unavailable",
        .has_exception = frame != NULL, .have_registers = frame != NULL, .cr2 = cr2};
    if (frame) info.frame = *frame;
    bugcheck_commit(&info);
}
