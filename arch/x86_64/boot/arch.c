/*
 * ReliefOS x86_64 architecture support: initializes processor control state.
 * Provides CPU feature setup and the transition into user execution.
 */
#include <reliefnt/console.h>
#include <reliefnt/arch.h>
#include <reliefnt/types.h>
#include <reliefnt/smp.h>

#define X86_CR0_MP (1ULL << 1)
#define X86_CR0_EM (1ULL << 2)
#define X86_CR0_TS (1ULL << 3)
#define X86_CR0_NE (1ULL << 5)
#define X86_CR0_WP (1ULL << 16)
#define X86_CR4_OSFXSR (1ULL << 9)
#define X86_CR4_OSXMMEXCPT (1ULL << 10)
#define X86_CR4_OSXSAVE (1ULL << 18)
#define X86_IA32_FS_BASE 0xc0000100u
#define X86_XCR0_X87 (1ULL << 0)
#define X86_XCR0_SSE (1ULL << 1)
#define X86_XCR0_AVX (1ULL << 2)
#define X86_CPUID_XSAVE (1u << 26)
#define X86_CPUID_AVX (1u << 28)

static void write_msr(uint32_t msr, uint64_t value)
{
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)value),
                     "d"((uint32_t)(value >> 32)) : "memory");
}

static uint64_t read_msr(uint32_t msr)
{
    uint32_t low;
    uint32_t high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

void arch_set_user_fs(uint64_t base)
{
    /* FS_BASE is a per-CPU MSR; skip the wrmsr (a VM-exit under KVM) when this
     * CPU already holds the value, e.g. the same task resuming after a syscall. */
    static uint64_t fs_base_loaded[SMP_MAX_CPUS];
    static uint8_t fs_base_valid[SMP_MAX_CPUS];
    uint32_t cpu = smp_current_cpu();
    if (fs_base_valid[cpu] && base == fs_base_loaded[cpu])
        return;
    write_msr(X86_IA32_FS_BASE, base);
    fs_base_loaded[cpu] = base;
    fs_base_valid[cpu] = 1;
}

uint64_t arch_get_user_fs(void)
{
    return read_msr(X86_IA32_FS_BASE);
}

/* XSAVE area size for x87/SSE/AVX.  It is a 64-byte aligned image, as
 * required by XSAVE/XRSTOR.  The legacy FXSAVE path only consumes 512 bytes. */
static uint8_t initial_fpu_state[RELIEFNT_FPU_STATE_MAX_SIZE]
    __attribute__((aligned(64)));
static uint32_t fpu_state_size = RELIEFNT_FPU_LEGACY_STATE_SIZE;
static uint64_t fpu_xstate_mask;
static uint32_t fpu_mxcsr_mask = 0xffbfU;
static bool fpu_policy_initialized;

/**
 * @brief Read a CPUID leaf/subleaf without relying on compiler feature flags.
 * @param leaf CPUID function number.
 * @param subleaf CPUID subfunction number.
 * @param out Caller-owned four-element EAX/EBX/ECX/EDX result array.
 */
static void arch_cpuid(uint32_t leaf, uint32_t subleaf, uint32_t out[4])
{
    __asm__ volatile("cpuid" : "=a"(out[0]), "=b"(out[1]),
                     "=c"(out[2]), "=d"(out[3])
                     : "a"(leaf), "c"(subleaf));
}

/**
 * @brief Write an XCR register after CR4.OSXSAVE has been enabled.
 * @param index XCR selector, normally zero for the user xstate mask.
 * @param value New 64-bit register value.
 */
static void arch_xsetbv(uint32_t index, uint64_t value)
{
    __asm__ volatile("xsetbv" : : "c"(index), "a"((uint32_t)value),
                     "d"((uint32_t)(value >> 32)) : "memory");
}

/** @brief Return the currently selected user FPU image size. */
uint32_t arch_fpu_state_size(void)
{
    return fpu_state_size;
}

/** @brief Return the XCR0 feature mask saved in user task images. */
uint64_t arch_fpu_xstate_mask(void)
{
    return fpu_xstate_mask;
}

/** @brief Return the MXCSR mask captured from the boot CPU. */
uint32_t arch_fpu_mxcsr_mask(void)
{
    return fpu_mxcsr_mask;
}

/**
 * @brief Copy the selected architectural FPU image and clear unused storage.
 */
static void copy_fpu_state(void *dst, const void *src)
{
    uint8_t *out = (uint8_t *)dst;
    const uint8_t *in = (const uint8_t *)src;
    for (uint32_t i = 0; i < fpu_state_size; ++i) {
        out[i] = in[i];
    }
    for (uint32_t i = fpu_state_size; i < RELIEFNT_FPU_STATE_MAX_SIZE; ++i)
        out[i] = 0;
}

/**
 * @brief Report that the architecture layer has initialized.
 */
void arch_init(void)
{
    console_info("[reliefnt] arch/x86_64 initialized\n");
}

/**
 * @brief Enable x87/SSE and a common x87/SSE/AVX XSAVE policy on each CPU.
 * The boot CPU owns the immutable clean image and MXCSR validation mask.
 */
void arch_fpu_init(void)
{
    uint64_t cr0;
    uint64_t cr4;
    uint32_t features[4];
    uint32_t maximum[4];
    uint32_t xstate[4] = {0};
    uint32_t ymm[4] = {0};
    bool first_cpu = !fpu_policy_initialized;

    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(X86_CR0_EM | X86_CR0_TS);
    cr0 |= X86_CR0_MP | X86_CR0_NE | X86_CR0_WP;
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= X86_CR4_OSFXSR | X86_CR4_OSXMMEXCPT;
    arch_cpuid(0, 0, maximum);
    arch_cpuid(1, 0, features);
    if (maximum[0] >= 0x0d &&
        (features[2] & (X86_CPUID_XSAVE | X86_CPUID_AVX)) ==
            (X86_CPUID_XSAVE | X86_CPUID_AVX)) {
        arch_cpuid(0x0d, 0, xstate);
        arch_cpuid(0x0d, 2, ymm);
    }
    bool avx = (xstate[0] & 7u) == 7u && ymm[0] == 256u && ymm[1] == 576u;
    if (first_cpu && avx) {
        fpu_xstate_mask = X86_XCR0_X87 | X86_XCR0_SSE | X86_XCR0_AVX;
        fpu_state_size = ymm[1] + ymm[0];
    }
    if (fpu_xstate_mask && !avx) {
        console_error("[reliefnt] CPU lacks the boot CPU XSAVE/AVX contract\n");
        for (;;) __asm__ volatile("cli; hlt");
    }
    cr4 = (cr4 & ~X86_CR4_OSXSAVE) | (fpu_xstate_mask ? X86_CR4_OSXSAVE : 0);
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
    if (fpu_xstate_mask) arch_xsetbv(0, fpu_xstate_mask);

    if (first_cpu) {
        uint8_t probe[RELIEFNT_FPU_LEGACY_STATE_SIZE] __attribute__((aligned(16)));
        __asm__ volatile("fninit\n\tfxsave64 (%0)" : : "r"(probe) : "memory");
        __builtin_memcpy(&fpu_mxcsr_mask, probe + 28, sizeof(fpu_mxcsr_mask));
        if (!fpu_mxcsr_mask) fpu_mxcsr_mask = 0xffbfU;
        __builtin_memset(initial_fpu_state, 0, sizeof(initial_fpu_state));
        uint16_t control = 0x037fU;
        uint32_t mxcsr = 0x1f80U;
        __builtin_memcpy(initial_fpu_state, &control, sizeof(control));
        __builtin_memcpy(initial_fpu_state + 24, &mxcsr, sizeof(mxcsr));
        __builtin_memcpy(initial_fpu_state + 28, &fpu_mxcsr_mask, sizeof(fpu_mxcsr_mask));
        /* An empty XSAVE bitmap initializes all enabled components, including
         * XMM and YMM registers which FNINIT alone does not clear. */
        fpu_policy_initialized = true;
        console_info("[reliefnt] FPU task state enabled size=%u xfeatures=0x%llx\n",
                     (unsigned)fpu_state_size, (unsigned long long)fpu_xstate_mask);
    }
    arch_fpu_restore(initial_fpu_state);
}

/**
 * @brief Initialize a new task's FPU area with the clean, saved FPU state.
 */
void arch_fpu_task_init(void *state)
{
    copy_fpu_state(state, initial_fpu_state);
}

/**
 * @brief Save all enabled user floating-point components into an aligned task image.
 */
void arch_fpu_save(void *state)
{
    if (fpu_xstate_mask) {
        __asm__ volatile("xsave64 (%0)" : : "r"(state),
                         "a"((uint32_t)fpu_xstate_mask),
                         "d"((uint32_t)(fpu_xstate_mask >> 32)) : "memory");
    } else {
        __asm__ volatile("fxsave64 (%0)" : : "r"(state) : "memory");
    }
}

/**
 * @brief Restore the task's enabled floating-point components with XRSTOR or FXRSTOR.
 */
void arch_fpu_restore(const void *state)
{
    if (fpu_xstate_mask) {
        __asm__ volatile("xrstor64 (%0)" : : "r"(state),
                         "a"((uint32_t)fpu_xstate_mask),
                         "d"((uint32_t)(fpu_xstate_mask >> 32)) : "memory");
    } else {
        __asm__ volatile("fxrstor64 (%0)" : : "r"(state) : "memory");
    }
}
