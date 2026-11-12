#include <reliefnt/moduleparam.h>
/*
 * ReliefOS kernel bootstrap: coordinates early platform initialization.
 * Starts memory, interrupts, drivers, storage and scheduling.
 */
#include <reliefnt/arch.h>
#include <reliefnt/apic.h>
#include <reliefnt/console.h>
#include <reliefnt/driver_manager.h>
#include <reliefnt/graphics_service.h>
#include <reliefnt/efi_fs.h>
#include <reliefnt/framebuffer.h>
#include <reliefnt/input.h>
#include <reliefnt/inventory.h>
#include <reliefnt/kernel.h>
#include <reliefnt/lock.h>
#include <reliefnt/mm.h>
#include <reliefnt/memory_topology.h>
#include <reliefnt/heap.h>
#include <reliefnt/multiboot2.h>
#include <reliefnt/module.h>
#include "../../fs/initramfs/boot.h"
#include <reliefnt/boot_block.h>
#include <reliefnt/panic.h>
#include <reliefnt/net.h>
#include <reliefnt/platform.h>
#include <reliefnt/power.h>
#include <reliefnt/paging.h>
#include <reliefnt/pat.h>
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

#include "../../arch/x86_64/idt.h"

static uint8_t kernel_ring0_stack[65536] __attribute__((aligned(16)));
/** @brief Entry stub marker set only after the i386 root has been installed. */
extern uint8_t boot_via_bios;

/**
 * @brief Scratch for the early EFI memory-map capture. GRUB on UEFI keeps boot
 * services active and omits the Multiboot2 memory-map tags, so the kernel
 * captures the live firmware map itself before the allocator starts. The
 * buffer lives in kernel .bss, which mm.c reserves through the linker bounds.
 */
static uint8_t efi_mmap_buffer[262144] __attribute__((aligned(16)));

/** @brief Apply UC policy to both retained display extents.
 * @param display Borrowed framebuffer descriptor from the early probe.
 * @param require_mapped True after publishing the complete sparse map.
 * @return True on success; a missing temporary mapping is deferred until publish.
 */
static bool boot_claim_framebuffer(const struct framebuffer *display, bool require_mapped)
{
    if (!display || !display->available) return true;
    const uint64_t starts[] = {display->reservation_start, display->auxiliary_reservation_start};
    const uint64_t sizes[] = {display->reservation_bytes, display->auxiliary_reservation_bytes};
    for (unsigned i = 0; i < 2; ++i) {
        if (!starts[i] || !sizes[i]) continue;
        if (!paging_kernel_direct_map_range(starts[i], sizes[i])) {
            if (require_mapped) return false;
            continue;
        }
        if (!paging_mmio_uncached(starts[i], sizes[i])) return false;
    }
    return true;
}

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
    console_info("[boot] Boot log paused. Press Enter to continue.\n");
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
 * @brief End live EFI boot services and publish their final memory map.
 * @param boot Writable decoded handoff; NULL is invalid and an ended handoff
 * performs no firmware calls or map changes.
 * @return 0 when services are ended, or a negative error requiring boot to stop.
 * Called after all early EFI protocol use, while firmware mappings are live.
 */
static int boot_capture_efi_memory_map(struct boot_info *boot)
{
    uint64_t entry_size = 0;
    uint32_t entry_count = 0;

    if (!boot) return -22;
    if (!boot->efi_boot_services_active) return 0;
    if (!boot->efi_system_table || !boot->efi_image_handle) return -22;
    int rc = efi_boot_exit_services(boot->efi_system_table, boot->efi_image_handle,
                                    efi_mmap_buffer, sizeof(efi_mmap_buffer),
                                    &entry_size, &entry_count);
    if (rc < 0) return rc;
    /* The topology uses final EFI usable ranges, while still honoring every
     * older Multiboot reservation that overlaps those ranges. */
    boot->efi_mmap_addr = (uint64_t)(uintptr_t)efi_mmap_buffer;
    boot->efi_mmap_entry_size = (uint32_t)entry_size;
    boot->efi_mmap_entry_count = entry_count;
    boot->efi_boot_services_active = false;
    console_info("[reliefnt] using EFI memory map entries=%u descriptor=%u\n",
                   entry_count, (uint32_t)entry_size);
    return 0;
}

/** @brief Snapshot live EFI address spaces and capture the ACPI RSDP before EBS.
 * @param boot Writable active-EFI handoff with a valid system table.
 * @return Zero or negative errno. Performs one GetMemoryMap and no exit; the
 * final handoff refreshes the same image-owned buffer immediately before EBS.
 * ACPI and SMBIOS consume only descriptor-proven identity spans in this window.
 */
static int boot_capture_efi_acpi(struct boot_info *boot)
{
    uint64_t stride = 0;
    uint32_t count = 0;
    if (!boot || !boot->efi_boot_services_active || !boot->efi_system_table) return -22;
    int rc = efi_boot_memory_map(boot->efi_system_table, efi_mmap_buffer,
                                 sizeof(efi_mmap_buffer), &stride, &count);
    if (rc || stride < sizeof(struct efi_memory_descriptor) || stride > UINT32_MAX || !count)
        return rc ? rc : -22;
    boot->efi_mmap_addr = (uintptr_t)efi_mmap_buffer;
    boot->efi_mmap_entry_size = (uint32_t)stride;
    boot->efi_mmap_entry_count = count;
    power_acpi_capture_efi_rsdp(boot);
    platform_identity_init(boot);
    return 0;
}

/**
 * @brief Read the boot theme from the ESP before replacing firmware page tables.
 * @return Zero for the Win95 theme, one for Metro or an absent/unreadable setting.
 */
static uint32_t boot_load_ui_theme(void)
{
    static const char reliefos_path[] = "/reliefos/config/display.conf";
    static const char legacy_path[] = "/leonos/config/display.conf";
    static const char win95[] = "theme=win95";
    char data[4096];
    size_t len = 0;
    const char *config;

    if (efi_fs_read_file_into(reliefos_path, data, sizeof(data), &len) < 0 &&
        efi_fs_read_file_into(legacy_path, data, sizeof(data), &len) < 0) {
        return 1u;
    }
    config = data;
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
 * @param magic Multiboot2 bootloader handoff magic.
 * @param multiboot_info Physical address of the bootloader-owned information block.
 * Builds the shared root namespace before driver/network/debug configuration
 * consumes filesystem paths. No user task or secondary CPU is running yet.
 */
static void kernel_start(uint32_t magic, uintptr_t multiboot_info)
{
    bool boot_log_pause;
    uint32_t boot_theme = 1u;

    __asm__ volatile("cli");
    /* The firmware left the legacy PIC unmasked with the PIT running, so IRQ0
     * is pending before the first kernel instruction.  Mask every input now:
     * any code path that restores IF would otherwise deliver that tick through
     * the firmware IDT, whose gates no longer fit the kernel GDT. */
    irq_mask_legacy_pic();
    console_init();
    struct boot_info boot;
    if (multiboot2_parse(magic, multiboot_info, &boot) < 0) {
        panic("Invalid Multiboot2 boot information");
    }
    boot.bios_identity_4g = boot_via_bios != 0;
    boot.efi_identity_amd64 = !boot.bios_identity_4g && boot.efi_boot_services_active;
    console_log_configure(boot.cmdline);
    const struct reliefos_system_info *system = reliefnt_system_info();
    console_info("ReliefOS %s %s booting\n",
                   system->kernel_name,
                   system->kernel_version);
    console_info("[boot] architecture=%s build=%s bootstrap_stack_bytes=%u\n",
                 system->architecture, system->build_time,
                 (unsigned)sizeof(kernel_ring0_stack));

    console_info("[reliefnt] GRUB handoff ok bootloader=%s cmdline=%s modules=%u\n",
                 boot.bootloader ? boot.bootloader : "(unknown)",
                 boot.cmdline ? boot.cmdline : "(none)", boot.module_count);
    console_info("[boot] firmware=%s acpi_rsdp=%p multiboot_mmap=%u efi_mmap=%u\n",
                 boot.efi_system_table ? "UEFI" : "legacy",
                 (void *)(uintptr_t)boot.rsdp_addr,
                 boot.mmap_entry_count, boot.efi_mmap_entry_count);
    for (uint32_t module_index = 0; module_index < boot.module_count; ++module_index) {
        const struct boot_module *module = &boot.modules[module_index];
        uint64_t bytes = module->end >= module->start ? module->end - module->start : 0;
        console_log(bytes ? CONSOLE_LOG_INFO : CONSOLE_LOG_WARN,
                    "[boot] module=%u name=%s start=0x%llx bytes=%llu\n",
                    module_index, module->name ? module->name : "(unnamed)",
                    (unsigned long long)module->start, (unsigned long long)bytes);
    }
    /* Firmware filesystem and GOP access is confined to this pre-paging
     * window. Snapshot retained data, close the root volume so its pools are
     * released, then replace stale GRUB maps with the final live EFI map. */
    bool platform_captured = boot.efi_boot_services_active;
    if (boot.efi_boot_services_active) {
        if (!boot.efi_identity_amd64)
            panic("Active EFI services without EFI amd64 identity mapping");
        if (!boot.efi_system_table || !boot.efi_image_handle)
            panic("EFI boot services active without system table or image handle");
        efi_fs_init(boot.efi_system_table);
        boot_theme = boot_load_ui_theme();
        framebuffer_capture_gop(boot.efi_system_table);
        efi_fs_shutdown();
        if (boot_capture_efi_acpi(&boot) < 0)
            panic("Unable to capture EFI ACPI mapping");
        /* GetMemoryMap uses the explicit table argument and does not need the
         * filesystem handle. Capture after close but before replacing firmware
         * mappings so any released pool pages are reflected in the allocator. */
        if (boot_capture_efi_memory_map(&boot) < 0)
            panic("Unable to exit EFI boot services");
    }
    boot_log_pause = cmdline_has(&boot, "bootlog-pause=1");
    /**
 * @brief Parse ACPI before the physical allocator can reclaim ACPI memory.
 */
    console_info("[boot] discovering ACPI power controls and SMBIOS identity\n");
    power_init(&boot);
    if (power_acpi_capture_resources() < 0)
        panic("Unable to preserve ACPI boot resources");
    if (!platform_captured) platform_identity_init(&boot);
    const struct platform_dmi_info *dmi = platform_dmi();
    if (dmi->values[0][0] || dmi->values[4][0]) {
        console_info("[boot] firmware_vendor=%s firmware_version=%s system_vendor=%s model=%s\n",
                     dmi->values[0], dmi->values[1], dmi->values[3], dmi->values[4]);
    }

    console_info("[boot] discovering interrupt controllers and CPU topology limit=%u\n", SMP_MAX_CPUS);
    arch_init();
    apic_init();
    ioapic_init();
    smp_init();
    const struct cpu_inventory *cpu_info = cpu_inventory_get(smp_current_cpu());
    if (cpu_info) {
        console_info("[cpu] vendor=%s model=%s family=%u model_id=%u stepping=%u apic=%u\n",
                     cpu_info->vendor, cpu_info->model_name, cpu_info->family,
                     cpu_info->model, cpu_info->stepping, cpu_info->apic_id);
        console_info("[cpu] cpuid_features ecx=0x%08x edx=0x%08x base_mhz=%u max_mhz=%u\n",
                     cpu_info->leaf1_ecx, cpu_info->leaf1_edx,
                     cpu_info->base_mhz, cpu_info->max_mhz);
    }
    console_info("[boot] initializing display and physical page allocator\n");
    /* Install the shared supervisor direct map before mm_init reads or writes
     * any physical page through its alias.  Initialize it before framebuffer
     * setup as that path also converts the firmware-provided physical surface. */
    paging_init_user_identity();
    framebuffer_init(&boot);
    /* Capture the controller claims while the temporary low map is live so
     * their exact cache policies transfer into the sparse map before CR3. */
    apic_map_kernel_aliases();
    const struct framebuffer *boot_framebuffer = framebuffer_get();
    if (!boot_claim_framebuffer(boot_framebuffer, false)) {
        panic("Unable to preserve framebuffer cache policy");
    }
    int memory_status = mm_bootstrap_prepare(&boot, paging_physical_bits());
    if (!memory_status) memory_status = paging_direct_map_init(mm_bootstrap_topology());
    if (memory_status) {
        console_fatal("[reliefnt] sparse direct-map initialization failed rc=%d\n", memory_status);
        panic("Unable to construct sparse direct map");
    }
    mm_init(&boot);
    if (!boot_claim_framebuffer(boot_framebuffer, true))
        panic("Unable to retain framebuffer mapping");
    if (boot.cmdline) {
        boot.cmdline = (const char *)mm_phys_to_virt((uint64_t)(uintptr_t)boot.cmdline);
    }
    if (boot.bootloader) {
        boot.bootloader = (const char *)mm_phys_to_virt((uint64_t)(uintptr_t)boot.bootloader);
    }
    for (uint32_t i = 0; i < boot.module_count; ++i) {
        if (boot.modules[i].name) {
            boot.modules[i].name = (const char *)mm_phys_to_virt(
                (uint64_t)(uintptr_t)boot.modules[i].name);
        }
    }
    /* Boot strings now use retained aliases. Only this BSP exists and no user
     * roots exist; later APs/users must inherit the same final PAT policy. */
    if (arch_pat_boot_init(!cmdline_has(&boot,"nopat")))
        (void)framebuffer_enable_writecombine_boot();
    console_info("[memory] total_kib=%llu free_kib=%llu page_bytes=4096\n",
                 (unsigned long long)mm_total_memory_kib(),
                 (unsigned long long)mm_free_memory_kib());
    kernel_heap_init();
    page_cache_init();
    kernel_objects_init();
    time_init();
    input_init();
    vga_console_configure(boot.cmdline, !boot.efi_system_table);
    pty_init();
    console_set_ui_theme(boot_theme);
    console_enable_framebuffer();
    console_enable_vga_fallback();
    console_info("[boot] kernel_console=%s theme=%s serial=%s\n",
                 console_vga_text_mode ? "VGA-text" : framebuffer_get()->available ? "framebuffer" : "VGA",
                 boot_theme == 0u ? "win95" : "metro",
                 serial_is_ready() ? "driver" : "early-COM1");
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
    console_info("[boot] GDT/TSS/IDT installed; probing VMware graphics acceleration\n");
    /* Bootstrap page tables are complete. svga3d_init drains the legacy FIFO,
     * retains the selected VRAM policy, and validates the FIFO BAR before
     * issuing 3D FIFO or guest-memory commands. */
    int svga_ret = svga3d_init();
    struct svga_info boot_svga_info;
    svga_get_info(&boot_svga_info);
    console_log(svga_ret < 0 && boot_svga_info.available ? CONSOLE_LOG_WARN : CONSOLE_LOG_INFO,
                "[graphics] svga3d_status=%d available=%u fifo_ready=%u probe=%s probe_status=%d\n",
                svga_ret, (unsigned)boot_svga_info.available,
                (unsigned)boot_svga_info.fifo_ready,
                boot_svga_info.probe.stage ? boot_svga_info.probe.stage : "not-run",
                boot_svga_info.probe.status);
    int triangle_ret = svga_ret;
    if (cmdline_has(&boot, "svga3d-triangle=1")) {
        triangle_ret = svga_ret == 0 ? svga3d_triangle_test() : svga_ret;
        console_log(triangle_ret < 0 ? CONSOLE_LOG_WARN : CONSOLE_LOG_INFO,
                    "[svga3d] triangle-test=%d\n", triangle_ret);
    }
    irq_init();
    driver_manager_init();
    {
        int parameter_ret=module_parameters_boot(boot.cmdline);
        if(parameter_ret<0)console_error("[boot] builtin parameter error ret=%d\n",parameter_ret);
    }
    {
        uint64_t memory_device;
        int memory_ret=block_boot_image_register(&boot,&memory_device);
        if(memory_ret<0) {
            console_fatal("[boot] reserved root image invalid ret=%d; boot stopped\n",memory_ret);
            kernel_idle_loop();
        }
    }
    {
        uint64_t block_flags;
        kernel_execution_lock_irqsave(&block_flags);
        int block_ret=module_run_block_initcalls();
        kernel_execution_unlock_irqrestore(block_flags);
        if(block_ret<0)console_error("[boot] block discovery failed ret=%d\n",block_ret);
    }
    {
        int filesystem_ret=module_run_fs_initcalls();
        if(filesystem_ret<0)
            console_error("[boot] filesystem registration failed ret=%d\n",filesystem_ret);
    }
    {
        int memory_root=storage_initramfs_boot(&boot);
        if(memory_root<0) {
            console_fatal("[boot] initial archive failed ret=%d; boot stopped\n",memory_root);
            kernel_idle_loop();
        }
        bool ramdisk_root = cmdline_has(&boot, "mode=installer") ||
                            cmdline_has(&boot, "mode=live");
        console_info("[boot] selecting root source=%s modules=%u\n",
                     memory_root ? "initramfs" : ramdisk_root ? "installer/live ramdisk" : "physical disk",
                     boot.module_count);
        if(!memory_root)storage_mount_boot_root(&boot, ramdisk_root);
        if (!memory_root && ramdisk_root && !storage_ready()) {
            console_warn("[reliefnt] installer ramdisk root not ready, retrying installer module\n");
            storage_init_installer_root(&boot);
        }
        console_log(storage_ready() ? CONSOLE_LOG_INFO : CONSOLE_LOG_ERROR,
                    "[boot] root_ready=%u filesystem=%s\n", (unsigned)storage_ready(),
                    storage_root_filesystem_name());
    }
    if(storage_ready()) {
        int namespace_ret=storage_namespace_bootstrap();
        if(namespace_ret<0) {
            console_fatal("[reliefnt] initial mount namespace failed ret=%d\n",namespace_ret);
            kernel_idle_loop();
        }
    }
    console_info("[boot] loading built-in mouse, serial, network and audio drivers\n");
    driver_manager_load_builtin();
    console_info("[boot] input providers initialized; initializing IPv4 networking\n");
    net_init();
    if (boot_log_pause) {
        console_info("[svga3d] init=%d available=%u fifo-ready=%u\n",
                       svga_ret, (unsigned)boot_svga_info.available,
                       (unsigned)boot_svga_info.fifo_ready);
        console_info("[svga3d] caps=0x%x fifo=0x%x\n",
                       boot_svga_info.device_caps, boot_svga_info.fifo_caps);
        console_info("[svga3d] host=0x%x guest=0x%x\n",
                       boot_svga_info.host_version, boot_svga_info.guest_version);
        const struct svga_probe_info *probe = &boot_svga_info.probe;
        console_info("[svga3d] probe=%s status=%d\n",
                       probe->stage ? probe->stage : "not-run", probe->status);
        console_info("[svga3d] probe-fifo=0x%x min=%u mem-regs=%u\n",
                       probe->fifo_caps, probe->fifo_min, probe->mem_regs);
        console_info("[svga3d] raw-hw=0x%x revised=0x%x enable=0x%x\n",
                       probe->host_legacy, probe->host_revised, probe->enable);
        console_info("[svga3d] gb=%u devcap3d=%u\n",
                       (unsigned)probe->gb_objects, probe->devcap_3d);
        if (cmdline_has(&boot, "svga3d-triangle=1")) {
            console_log(triangle_ret < 0 ? CONSOLE_LOG_WARN : CONSOLE_LOG_INFO,
                        "[svga3d] triangle-test=%d\n", triangle_ret);
        }
        boot_log_wait_for_enter();
    }
    if (pty_vt_init() < 0) {
        console_fatal("[reliefnt] unable to initialize six virtual terminals\n");
        kernel_idle_loop();
    }
    console_info("[boot] virtual terminals tty1-tty6 ready; preparing PID 1\n");
    userland_init(&boot);
    /* The PM clock and task table are ready, with no shared locks held.
     * Give the BSP the same local preemption checks as the APs while the PIT
     * remains the sole source of the public 100 Hz tick clock. */
    uint64_t preempt_flags = kernel_irq_save();
    apic_timer_init(0x40u, APIC_SCHED_TICK_HZ, APIC_SCHED_FALLBACK_COUNT);
    kernel_irq_restore(preempt_flags);
    /* All initial task objects are now present. APs may enter the shared
     * scheduler without racing the bootstrap task construction above. */
    console_info("[boot] starting secondary CPUs discovered=%u\n", smp_cpu_count());
    smp_start_aps();
    sched_dump();
    uint32_t online_cpus = 0;
    for (uint32_t cpu_index = 0; cpu_index < smp_cpu_count(); ++cpu_index) {
        if (smp_cpu_online(cpu_index)) ++online_cpus;
    }
    console_info("[reliefnt] boot complete: version=%s root=/ fs=%s vt=tty1 online_cpus=%u/%u free_kib=%llu\n",
                 system->kernel_version, storage_root_filesystem_name(),
                 online_cpus, smp_cpu_count(),
                 (unsigned long long)mm_free_memory_kib());

    console_boot_complete();
    console_vt_activate(1, false);
    userland_enter_first();
    kernel_idle_loop();
}

/**
 * @brief Multiboot2 entry point called by the arch entry stub; starts the kernel.
 */
void kernel_main(uint32_t magic, uintptr_t multiboot_info)
{
    kernel_start(magic, multiboot_info);
}
