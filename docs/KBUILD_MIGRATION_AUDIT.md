# ReliefNT build migration audit

This comparison records the pre-migration build inputs at child revision `10b4899`. The old `mk/` implementation and configuration adapter are removed; this file preserves the source, flag, and product baseline for review. The checked-in Linux 6.12 engine byte inventory and SHA-256 values are in [`scripts/kbuild-source.json`](../scripts/kbuild-source.json).

## Kernel source inventory and order

The old `mk/kernel.mk` searched these roots and sorted the discovered translation units with `LC_ALL=C sort`:

```text
kernel/reliefnt drivers/console drivers/input/mouse drivers/tty/serial
drivers/net/e1000 drivers/sound/ac97 drivers/sound/es1371 drivers/sound/hda
drivers/video/vga drivers/video/framebuffer drivers/video/svga drivers/video/drm
drivers/usb/uhci drivers/storage/ide drivers/storage/ahci drivers/storage/nvme
firmware block arch mm fs net kernel/exec
```

At link time, the old graph placed the 157 C objects in global C-locale sort order, then the three assembly objects in global C-locale sort order, then the generated vDSO blob. These source paths are the archived C/assembly inventory; `Kbuild` contains the equivalent object entries in that exact order.

```text
arch/x86_64/apic.c
arch/x86_64/arch.c
arch/x86_64/cpuinfo.c
arch/x86_64/gdt.c
arch/x86_64/idt.c
arch/x86_64/irq.c
arch/x86_64/multiboot2.c
arch/x86_64/paging.c
arch/x86_64/pci.c
arch/x86_64/pci_irq.c
arch/x86_64/platform.c
arch/x86_64/power.c
arch/x86_64/smp.c
block/storage_block.c
block/storage_devlinks.c
block/storage_disk.c
block/storage_identity.c
block/storage_installer.c
block/storage_state.c
block/storage_state_data.c
block/storage_sync.c
drivers/console/console.c
drivers/console/ostui.c
drivers/input/mouse/mouse.c
drivers/net/e1000/e1000.c
drivers/sound/ac97/ac97.c
drivers/sound/es1371/es1371.c
drivers/sound/hda/codec.c
drivers/sound/hda/controller.c
drivers/sound/hda/controls.c
drivers/sound/hda/hda.c
drivers/sound/hda/stream.c
drivers/storage/ahci/storage_ahci.c
drivers/storage/ide/storage_ide.c
drivers/storage/nvme/storage_nvme.c
drivers/tty/serial/serial.c
drivers/usb/uhci/usb_uhci.c
drivers/video/drm/drm_bo.c
drivers/video/drm/drm_device.c
drivers/video/drm/drm_faildiag.c
drivers/video/drm/drm_file.c
drivers/video/drm/drm_ioctl.c
drivers/video/drm/drm_kms.c
drivers/video/drm/drm_prime.c
drivers/video/drm/drm_vmwgfx_caps.c
drivers/video/drm/drm_vmwgfx_execbuf.c
drivers/video/drm/drm_vmwgfx_execbuf_parser.c
drivers/video/drm/drm_vmwgfx_resource.c
drivers/video/framebuffer/fbdev.c
drivers/video/framebuffer/framebuffer.c
drivers/video/svga/batch_state.c
drivers/video/svga/borrowed.c
drivers/video/svga/device.c
drivers/video/svga/fifo.c
drivers/video/svga/gb.c
drivers/video/svga/gmr.c
drivers/video/svga/query.c
drivers/video/svga/render.c
drivers/video/svga/shader.c
drivers/video/svga/svga3d.c
drivers/video/svga/triangle.c
drivers/video/vga/vga.c
firmware/efi_fs.c
fs/object.c
fs/permissions.c
fs/procfs.c
fs/storage_exfat.c
fs/storage_ext2.c
fs/storage_ext2_cache.c
fs/storage_ext2_write.c
fs/storage_ext4_alloc.c
fs/storage_ext4_cache.c
fs/storage_ext4_checksum.c
fs/storage_ext4_dir.c
fs/storage_ext4_extent.c
fs/storage_ext4_format.c
fs/storage_ext4_journal.c
fs/storage_ext4_mount.c
fs/storage_ext4_ops.c
fs/storage_ext4_vfs.c
fs/storage_ext4_xattr.c
fs/storage_fat32.c
fs/storage_inode.c
fs/storage_iso.c
fs/storage_mount.c
fs/storage_mounts.c
fs/storage_permissions.c
fs/storage_sidecar.c
fs/storage_statfs.c
fs/storage_tmpfs.c
fs/storage_vfs.c
fs/sysfs.c
fs/tmpfs.c
kernel/exec/elf.c
kernel/exec/usercopy.c
kernel/exec/usercopy_task.c
kernel/exec/userland.c
kernel/exec/vdso.c
kernel/reliefnt/audio/alsa_control.c
kernel/reliefnt/audio/alsa_pcm.c
kernel/reliefnt/audio/core.c
kernel/reliefnt/audio/device.c
kernel/reliefnt/audio/mixer.c
kernel/reliefnt/audio/oss.c
kernel/reliefnt/audio/pcm.c
kernel/reliefnt/audio/storage_audio.c
kernel/reliefnt/audio/timer.c
kernel/reliefnt/driver_manager.c
kernel/reliefnt/driver_manager_phase.c
kernel/reliefnt/driver_resources.c
kernel/reliefnt/futex.c
kernel/reliefnt/gpu.c
kernel/reliefnt/input.c
kernel/reliefnt/kernel.c
kernel/reliefnt/kernel_debug.c
kernel/reliefnt/lib/bugcheck.c
kernel/reliefnt/lib/panic.c
kernel/reliefnt/lib/string.c
kernel/reliefnt/lib/text_utf16.c
kernel/reliefnt/lock.c
kernel/reliefnt/pty.c
kernel/reliefnt/random.c
kernel/reliefnt/sched/sched.c
kernel/reliefnt/shm.c
kernel/reliefnt/signal.c
kernel/reliefnt/signal_queue.c
kernel/reliefnt/signalfd.c
kernel/reliefnt/syscall.c
kernel/reliefnt/syscall_clone.c
kernel/reliefnt/syscall_device.c
kernel/reliefnt/syscall_file_io.c
kernel/reliefnt/syscall_fs.c
kernel/reliefnt/syscall_gui.c
kernel/reliefnt/syscall_ipc.c
kernel/reliefnt/syscall_locks.c
kernel/reliefnt/syscall_mm.c
kernel/reliefnt/syscall_process.c
kernel/reliefnt/syscall_process_vm.c
kernel/reliefnt/syscall_security.c
kernel/reliefnt/syscall_socket.c
kernel/reliefnt/syscall_socket_batch.c
kernel/reliefnt/syscall_sysv_msg.c
kernel/reliefnt/syscall_sysv_sem.c
kernel/reliefnt/syscall_sysv_shm.c
kernel/reliefnt/syscall_time.c
kernel/reliefnt/time.c
kernel/reliefnt/time_discipline.c
kernel/reliefnt/uts.c
kernel/reliefnt/version.c
kernel/reliefnt/wait.c
mm/heap.c
mm/mm.c
mm/page_cache.c
net/net.c
net/net_control.c
net/net_packet.c
net/net_udp.c
arch/x86_64/boot.S
arch/x86_64/scheduler_stack.S
arch/x86_64/smp_trampoline.S
```

The tail of the kernel archive is `arch/x86_64/boot.o`, `arch/x86_64/scheduler_stack.o`, `arch/x86_64/smp_trampoline.o`, then `vdso/vdso_blob.o`.

## Pre-migration compiler and linker policy

The default target toolchain is described by `configs/toolchains/llvm-x86_64.mk`: Clang targeting `x86_64-unknown-none`, `llvm-ar`, `ld.lld`, and `llvm-objcopy`. `PROFILE=release` selected `-O3`; `PROFILE=debug` selected `-O0 -g`. The old commands were:

```text
Kernel C:   clang -target x86_64-unknown-none [profile] -std=c11 -ffreestanding
            -fno-stack-protector -fno-pic -fno-pie -mno-red-zone
            -mgeneral-regs-only -mcmodel=kernel -Wall -Wextra -MMD -MP
            -I$(O)/include -I<source>/kernel/reliefnt/include
            -I<source>/include/uapi -I<source>/include
            -include $(O)/include/generated/autoconf.h $(KERNEL_CFLAGS)
Kernel ASM: clang -target x86_64-unknown-none [profile] -ffreestanding
            -mno-red-zone -mgeneral-regs-only -MMD -MP [same include roots]
Kernel LD:  ld.lld -nostdlib -z max-page-size=0x1000
            -T arch/x86_64/linker.ld $(KERNEL_LDFLAGS)
vDSO C:     clang -target x86_64-unknown-none [profile] -std=c11 -ffreestanding
            -fPIC -fvisibility=hidden -fno-builtin -fno-stack-protector
            -fno-plt -fno-unwind-tables -fno-asynchronous-unwind-tables
            -mno-red-zone -mgeneral-regs-only -Wall -Wextra -Werror
            -I<source>/kernel/reliefnt/include
vDSO LD:    ld.lld -m elf_x86_64 -shared -nostdlib -z max-page-size=0x1000
            --hash-style=both --no-undefined --build-id=none
            -soname linux-vdso.so.1 --version-script=vdso/vdso.map
```

Kernel C/ASM dependency flags were `-MMD -MP`. The kernel link consumed all C objects, then all assembly objects, then the embedded blob; `kernel.sys` was `--strip-debug` and `kernel.debug` was `--only-keep-debug` from the same unstripped ELF. `debug/kerneldebug.c` was compiled as freestanding target C and converted with `llvm-objcopy`, dropping `.llvm_addrsig`, `.comment`, and `.note.GNU-stack` and setting the ReliefOS debug note section flags.

## Generated inputs and products

| Purpose | Pre-migration output |
| --- | --- |
| Per-source objects and dependency files | `$(O)/obj/kernel/<source>.o` and `.d` |
| Native config outputs | `$(O)/config/.config`, `$(O)/config/autoconf.mk`, `$(O)/include/generated/autoconf.h` |
| Version input | `$(O)/include/generated/build_info.h` |
| Cached Unifont transform | `$(O)/include/generated/cjk_font.h` |
| vDSO ELF, assembly, and embedded object | `$(O)/generated/vdso/vdso.so`, `vdso_blob.S`; `$(O)/obj/kernel/vdso/vdso_blob.S.o` |
| Kernel link and images | `$(O)/generated/system/kernel.unstripped`, `kernel.sys`, `kernel.debug` |
| Kernel debugger | `$(O)/generated/system/kerneldebug.sys` |
| UAPI export | `$(O)/kernel-export/include/**`, `manifest.txt` |
| Install | `$(DESTDIR)/kernel.sys`, `kernel.debug`, `kerneldebug.sys`, `manifest.txt` |

The standalone kernel interface is still the three products `kernel.sys`, `kernel.debug`, and `kerneldebug.sys`; the explicit header whitelist and install manifest are retained. The native implementation uses Kbuild source-relative object directories and `.cmd` records, while user-facing artifact and export locations remain under the same `generated/system` and `kernel-export` paths. The vDSO test now inspects the native `$(O)/vdso/vdso.so` and `$(O)/vdso/vdso_blob.o` locations.
