/*
 * ReliefOS kernel bootstrap: coordinates early platform initialization.
 * Starts memory, interrupts, drivers, storage and scheduling.
 */
#include <reliefnt/arch.h>
#include <reliefnt/apic.h>
#include <reliefnt/console.h>
#include <reliefnt/driver_manager.h>
#include <reliefnt/efi_fs.h>
#include <reliefnt/framebuffer.h>
#include <reliefnt/input.h>
#include <reliefnt/kernel_debug.h>
#include <reliefnt/kernel.h>
#include <reliefnt/mm.h>
#include <reliefnt/heap.h>
#include <reliefnt/multiboot2.h>
#include <reliefnt/net.h>
#include <reliefnt/platform.h>
#include <reliefnt/power.h>
#include <reliefnt/pty.h>
#include <reliefnt/page_cache.h>
#include <reliefnt/object.h>
#include <reliefnt/sched.h>
#include <reliefnt/smp.h>
#include <reliefnt/storage.h>
#include <reliefnt/syscall.h>
#include <reliefnt/svga.h>
#include <reliefnt/time.h>
#include <reliefnt/usb.h>
#include <reliefnt/userland.h>
#include <reliefnt/version.h>

#include "../arch/x86_64/idt.h"

static uint8_t kernel_ring0_stack[65536] __attribute__((aligned(16)));

/**
 * @brief Scratch for the early EFI memory-map capture. GRUB on UEFI keeps boot
 * services active and omits the Multiboot2 memory-map tags, so the kernel
 * captures the live firmware map itself before the allocator starts. The
 * buffer lives in kernel .bss, which mm.c reserves through the linker bounds.
 */
static uint8_t efi_mmap_buffer[262144] __attribute__((aligned(16)));

/**
 * @brief Halt the CPU forever; used when there is nothing left to run.
 */
void kernel_idle_loop(void)
{
    for (;;) {
        __asm__ volatile("hlt");
    }
}

/**
 * @brief Return 1 if the boot command line contains the substring needle.
 */
static int cmdline_has(const struct boot_info *boot, const char *needle)
{
    if (!boot || !boot->cmdline || !needle) {
        return 0;
    }
    for (const char *p = boot->cmdline; *p; ++p) {
        const char *a = p;
        const char *b = needle;
        while (*a && *b && *a == *b) {
            ++a;
            ++b;
        }
        if (*b == 0) {
            return 1;
        }
    }
    return 0;
}

/**
 * @brief Hold the boot console until Enter is pressed after discarding queued input.
 * Called with interrupts disabled after IRQ/USB initialization and before user
 * tasks or APs are started. Keyboard and timer IRQs run during each halt; returns
 * with interrupts disabled. Pointer events and key releases do not resume boot.
 */
static void boot_log_wait_for_enter(void)
{
    struct input_raw_event event;
    while (input_pop(&event)) {
    }
    console_printf("[boot] Boot log paused. Press Enter to continue.\n");
    for (;;) {
        while (input_pop(&event)) {
            if (event.type == INPUT_EVENT_KEYBOARD && event.pressed &&
                event.keycode == 28u) {
                return;
            }
        }
        __asm__ volatile("sti; hlt; cli" ::: "memory");
    }
}

/**
 * @brief Fill boot's EFI memory-map fields from a live boot-services capture.
 * Used when GRUB's Multiboot2 info carries neither memory-map tag.
 */
static void boot_capture_efi_memory_map(struct boot_info *boot)
{
    uint64_t entry_size = 0;
    uint32_t entry_count = 0;

    if (!boot || boot->mmap_entry_count || boot->efi_mmap_entry_count) {
        return;
    }
    if (efi_boot_memory_map(boot->efi_system_table, efi_mmap_buffer,
                            sizeof(efi_mmap_buffer), &entry_size,
                            &entry_count) < 0) {
        console_printf("[reliefnt] EFI memory map unavailable; using legacy estimate\n");
        return;
    }
    boot->efi_mmap_addr = (uint64_t)(uintptr_t)efi_mmap_buffer;
    boot->efi_mmap_entry_size = (uint32_t)entry_size;
    boot->efi_mmap_entry_count = entry_count;
    console_printf("[reliefnt] using EFI memory map entries=%u descriptor=%u\n",
                   entry_count, (uint32_t)entry_size);
}

/**
 * @brief Read the boot theme from the ESP display.conf, mirroring the value
 *        make_image.py stages there. Returns 1 (metro) when absent or unreadable.
 */
static uint32_t boot_load_ui_theme(void)
{
    static const char reliefos_path[] = "/reliefos/config/display.conf";
    static const char legacy_path[] = "/leonos/config/display.conf";
    static const char win95[] = "theme=win95";
    const void *data = 0;
    size_t len = 0;
    const char *config;

    if (efi_fs_read_file(reliefos_path, &data, &len) < 0 &&
        efi_fs_read_file(legacy_path, &data, &len) < 0) {
        return 1u;
    }
    config = (const char *)data;
    for (size_t index = 0; index + sizeof(win95) - 1u <= len; ++index) {
        size_t matched = 0;
        while (matched < sizeof(win95) - 1u &&
               config[index + matched] == win95[matched]) {
            ++matched;
        }
        if (matched == sizeof(win95) - 1u) {
            return 0u;
        }
    }
    return 1u;
}

/**
 * @brief Full boot sequence: parse Multiboot2/EFI info, then initialize every kernel subsystem in order and enter userland.
 */
static void kernel_start(uint32_t magic, uint32_t multiboot_info)
{
    bool boot_log_pause;

    __asm__ volatile("cli");
    /* The firmware left the legacy PIC unmasked with the PIT running, so IRQ0
     * is pending before the first kernel instruction.  Mask every input now:
     * any code path that restores IF would otherwise deliver that tick through
     * the firmware IDT, whose gates no longer fit the kernel GDT. */
    irq_mask_legacy_pic();
    console_init();
    const struct reliefos_system_info *system = reliefnt_system_info();
    console_printf("ReliefOS %s %s booting\n",
                   system->kernel_name,
                   system->kernel_version);

    struct boot_info boot;
    multiboot2_parse(magic, (uintptr_t)multiboot_info, &boot);
    /**
 * @brief UEFI GRUB keeps boot services active and may therefore omit the Multiboot2 memory-map tags; capture the live EFI map before the allocator falls back to the legacy 512 MiB estimate.
 */
    boot_capture_efi_memory_map(&boot);
    boot_log_pause = cmdline_has(&boot, "bootlog-pause=1");
    /**
 * @brief Parse ACPI before the physical allocator can reclaim ACPI memory.
 */
    power_init(&boot);
    platform_identity_init(&boot);

    arch_init();
    apic_init();
    ioapic_init();
    smp_init();
    framebuffer_init(&boot);
    mm_init(&boot);
    kernel_heap_init();
    page_cache_init();
    kernel_objects_init();
    time_init();
    input_init();
    pty_init();
    if (boot.efi_system_table) {
        efi_fs_init(boot.efi_system_table);
    }
    console_set_ui_theme(boot_load_ui_theme());
    console_enable_framebuffer();
    console_enable_vga_fallback();
    sched_init();
    sched_create_idle_task();
    syscall_init();
    syscall_trace_configure(boot.cmdline);
    arch_userland_init(kernel_ring0_stack + sizeof(kernel_ring0_stack));
    /* The kernel GDT and TSS are live now, so the kernel IDT must be loaded
     * before anything else can take a fault or restore IF.  Until this lidt
     * the CPU still holds the firmware IDT, whose gates select firmware GDT
     * segments and triple fault on delivery after the GDT switch. */
    idt_init();
    /* The bootstrap page tables are complete now, so SVGA BARs can be marked
     * UC before any 3D FIFO or guest-memory command is issued. */
    int svga_ret = svga3d_init();
    struct svga_info boot_svga_info;
    svga_get_info(&boot_svga_info);
    int triangle_ret = svga_ret;
    if (cmdline_has(&boot, "svga3d-triangle=1")) {
        triangle_ret = svga_ret == 0 ? svga3d_triangle_test() : svga_ret;
        console_printf("[svga3d] triangle-test=%d\n", triangle_ret);
    }
    irq_init();
    {
        bool ramdisk_root = cmdline_has(&boot, "mode=installer") ||
                            cmdline_has(&boot, "mode=live");
        storage_mount_boot_root(&boot, ramdisk_root);
        if (ramdisk_root && !storage_ready()) {
            console_printf("[reliefnt] installer ramdisk root not ready, retrying installer module\n");
            storage_init_installer_root(&boot);
        }
    }
    driver_manager_init();
    driver_manager_load_builtin();
    usb_init();
    net_init();
    if (kernel_debug_boot_requested()) {
        console_printf("[reliefnt] entering kernel debug tool before userland\n");
        (void)kernel_debug_run_module();
        console_printf("[reliefnt] kernel debug tool finished; continuing normal startup\n");
    }
    if (boot_log_pause) {
        console_printf("[svga3d] init=%d available=%u fifo-ready=%u\n",
                       svga_ret, (unsigned)boot_svga_info.available,
                       (unsigned)boot_svga_info.fifo_ready);
        console_printf("[svga3d] caps=0x%x fifo=0x%x\n",
                       boot_svga_info.device_caps, boot_svga_info.fifo_caps);
        console_printf("[svga3d] host=0x%x guest=0x%x\n",
                       boot_svga_info.host_version, boot_svga_info.guest_version);
        const struct svga_probe_info *probe = &boot_svga_info.probe;
        console_printf("[svga3d] probe=%s status=%d\n",
                       probe->stage ? probe->stage : "not-run", probe->status);
        console_printf("[svga3d] probe-fifo=0x%x min=%u mem-regs=%u\n",
                       probe->fifo_caps, probe->fifo_min, probe->mem_regs);
        console_printf("[svga3d] raw-hw=0x%x revised=0x%x enable=0x%x\n",
                       probe->host_legacy, probe->host_revised, probe->enable);
        console_printf("[svga3d] gb=%u devcap3d=%u\n",
                       (unsigned)probe->gb_objects, probe->devcap_3d);
        if (cmdline_has(&boot, "svga3d-triangle=1")) {
            console_printf("[svga3d] triangle-test=%d\n", triangle_ret);
        }
        boot_log_wait_for_enter();
    }
    if (pty_vt_init() < 0) {
        console_printf("[reliefnt] unable to initialize six virtual terminals\n");
        kernel_idle_loop();
    }
    userland_init(&boot);
    /* All initial task objects are now present. APs may enter the shared
     * scheduler without racing the bootstrap task construction above. */
    smp_start_aps();
    sched_dump();
    console_printf("[reliefnt] boot complete: version=%s root=/ fs=%s vt=tty1\n",
                   system->kernel_version, storage_root_filesystem_name());

    userland_enter_first();
    kernel_idle_loop();
}

/**
 * @brief Multiboot2 entry point called by the arch entry stub; starts the kernel.
 */
void kernel_main(uint32_t magic, uint32_t multiboot_info)
{
    kernel_start(magic, multiboot_info);
}
